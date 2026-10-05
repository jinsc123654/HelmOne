/**
 * @file vmap_blit.c
 * @brief 画布搬运后端：CPU 兜底 + EPIC 2D blitter（见 vmap_blit.h 的说明）。
 *
 * 分三层：
 *   cpu_*   逐位可证的兜底实现，同时是自检的"参照答案"；
 *   vl_epic_* 硬件那一层在板级（`myvendor_epic_blit.h`），这里只做准入判定；
 *   vmap_blit_probe() 开机一次性：把 EPIC 与 CPU 在**同一块缓冲**上的
 *   结果逐位比对（含"源/目的重叠"的各方向），并把结论固化成运行期的准入位。
 */

#include "vmap_blit.h"

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "vmap_alloc.h"
/*
 * EPIC 那一半在板级：`myvendor_epic_blit.h`（`board` 目标）。
 *
 * **为什么应用侧不直接摆 EPIC 的 HAL**：`bf0_hal_epic.h` 会拉进 `register.h`
 * 那一串芯片层头文件，而 NSH 应用目标的 include 路径里没有 `chips` 那一层
 * （只有 `board` 目标有）；LVGL 的适配层头 `lv_sifli_epic_cfg.h` 同理。
 * 板级封装同时收敛了硬件约定（独占句柄的复用、起动前的准备、写前 clean、
 * 非负偏移/坐标上限的检查、以及"返回前必定等完"的同步语义）。
 */
#include "myvendor_epic_blit.h"

struct vmap_blit_s {
    vmap_blit_backend_t backend;
    /**
     * 自检结论：**同一块缓冲内源/目的重叠**的搬运，在各方向上是否与 CPU
     * 逐位一致。下标 = [平移 dx 的符号 + 1][平移 dy 的符号 + 1]（2 = 正、1 = 0、
     * 0 = 负），true = 该方向可信。全 false（自检没跑成）⇒ 重叠一律退回 CPU。
     */
    bool ovl_ok[3][3];
    /**
     * 自检结论：填充的**颜色回读**（RGB565 → RGB888 展开 → 硬件再打包回 RGB565）
     * 是否与 CPU 填出来的逐位相同。没跑成或对不上 ⇒ 填充一律退回 CPU ——
     * 颜色对不上比慢更不能接受。
     */
    bool fill_ok;
    /**
     * 自检结论：**不相交**拷贝是否与 CPU 逐位相同（自检的对照用例）。
     * 这是"两跳搬运"的前提 —— 两跳的每一跳都是不相交拷贝，所以只要这个成立，
     * 重叠方向也能吃到硬件（见 vl_epic_copy_via_scratch）。
     */
    bool nonovl_ok;
    /** 两跳搬运用的中转缓冲（只有在需要它的方向才会分配；约 = 一次画布的字节数）。 */
    uint8_t * scratch;
    size_t scratch_sz;
    /** 运行计数：各条路各走了多少次（[vperf] 逐帧取增量，用来把耗时归到路径上）。 */
    uint32_t n_1hop;
    uint32_t n_2hop;
    uint32_t n_cpu;
};

/**
 * @brief 全进程共享句柄：EPIC 是独占硬件，句柄不该有第二个。
 *
 * **寿命是进程级，别改成跟页走**：句柄里装着开机自检的结论（`ovl_ok`/`fill_ok`/
 * `nonovl_ok`）与两跳用的中转缓冲。自检一个会话只跑一次（约 0.7 s，且必须在画布
 * 尺寸已知之后），而 map 页会被反复 load/unload（`map_page_on_unload()` →
 * `vmap_view_destroy()`）—— 跟着页销毁重建就等于每次进地图重跑自检、重新分配
 * 中转缓冲。中转缓冲按需增长（只涨不缩），上限 = 一次画布（496×472×2 = 468 KB），
 * 实测一次 325 KB —— 这是 LiveMap 明确批过的开销。
 */
static vmap_blit_t * s_shared;

/** @brief 自检只跑一次（map 页可以反复 load/unload，见 vmap_blit_probe()）。 */
static bool s_probed;
static bool s_probe_ok;

static bool vl_epic_copy(const vmap_blit_surface_t * dst, int32_t dx, int32_t dy,
    const vmap_blit_surface_t * src, int32_t sx, int32_t sy, int32_t w, int32_t h);
static bool vl_epic_fill(const vmap_blit_surface_t * dst, int32_t x, int32_t y,
    int32_t w, int32_t h, uint16_t color);

/* ---------------------------------------------------------------- 公共入口 */

static const char * const k_names[] = { "cpu", "dmac", "epic" };

const char * vmap_blit_backend_name(vmap_blit_backend_t k)
{
    if ((unsigned)k >= sizeof(k_names) / sizeof(k_names[0])) {
        return "?";
    }
    return k_names[k];
}

vmap_blit_backend_t vmap_blit_backend(const vmap_blit_t * b)
{
    return b ? b->backend : VMAP_BLIT_CPU;
}

vmap_blit_t * vmap_blit_create(void)
{
    vmap_blit_t * b = (vmap_blit_t *)calloc(1, sizeof(*b));

    if (!b) {
        return NULL;
    }

    /*
     * 后端探测。
     *
     * 走 EPIC：它是 2D blitter，一条命令就是一整块矩形（源/目的各自带
     * `data`/`total_width`/矩形尺寸），而 DMAC 的 mem2mem 要**逐行**发
     * （stride 一般不等于矩形宽），重叠还得自己挑方向 —— 同样的
     * "DMA 主设备能不能在 PSRAM 上拿到比 CPU 每次 4 字节 store 更好的吞吐"
     * 这个问题，EPIC 用更少的命令就能问出来，所以先实装它。
     *
     * 探测本身**不做任何硬件动作**（不分配、不写寄存器）：句柄只是拿到
     * 硬件可用性；真正的"能不能用/用在哪个方向"由 vmap_blit_probe() 上板实测
     * 决定，默认（没跑过自检）是"重叠一律退回 CPU"。
     */
    /* 总开关关着 ⇒ 恒为 CPU（见 vmap_blit.h 里 MYVENDOR_BLIT_EPIC 的说明）。 */
    b->backend = (MYVENDOR_BLIT_EPIC && myvendor_epic_ready())
        ? VMAP_BLIT_EPIC : VMAP_BLIT_CPU;
    return b;
}

void vmap_blit_destroy(vmap_blit_t * b)
{
    if (b) {
        vmap_free(b->scratch);
        free(b);
    }
}

vmap_blit_t * vmap_blit_get(void)
{
    if (!s_shared) {
        s_shared = vmap_blit_create();
    }
    return s_shared;
}

/* ---------------------------------------------------------------- CPU 兜底 */

static void cpu_copy(const vmap_blit_surface_t * dst, int32_t dx, int32_t dy,
    const vmap_blit_surface_t * src, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    const uint8_t * sbase = (const uint8_t *)src->buf;
    uint8_t * dbase = (uint8_t *)dst->buf;
    int32_t i;

    /*
     * 行的处理次序决定对错：`memmove` 只保护**行内**重叠，跨行没有保护。
     * 目的行 dy+i 读源行 sy+i —— 源在下方（sy > dy）时从上往下写安全（要读的
     * 行还在下面没被盖），源在上方（sy < dy）必须从下往上。不相交时两种次序
     * 结果相同，所以这个判据可以无条件用。
     */
    if (sy < dy) {
        for (i = h - 1; i >= 0; i--) {
            const uint8_t * s = sbase + (size_t)(sy + i) * src->stride
                + (size_t)sx * sizeof(uint16_t);
            uint8_t * d = dbase + (size_t)(dy + i) * dst->stride
                + (size_t)dx * sizeof(uint16_t);

            memmove(d, s, (size_t)w * sizeof(uint16_t));
        }
        return;
    }

    for (i = 0; i < h; i++) {
        const uint8_t * s = sbase + (size_t)(sy + i) * src->stride
            + (size_t)sx * sizeof(uint16_t);
        uint8_t * d = dbase + (size_t)(dy + i) * dst->stride
            + (size_t)dx * sizeof(uint16_t);

        /* 同一行内可能有重叠（横向平移且同一行），方向交给 memmove。 */
        memmove(d, s, (size_t)w * sizeof(uint16_t));
    }
}

static void cpu_fill(const vmap_blit_surface_t * dst, int32_t x, int32_t y,
    int32_t w, int32_t h, uint16_t color)
{
    uint8_t * dbase = (uint8_t *)dst->buf;
    const uint32_t pair = ((uint32_t)color << 16) | (uint32_t)color;
    int32_t i;

    /*
     * 成对 32 位写（原 `vmap_render_clear` 的写法）。**不要再往上做 32 字节块**：
     * 上板实测块写与逐字写的周期/字节完全一样（`[vperf] psram ... blk/w=100%`），
     * 写通路不合并，块写只省指令不省访问。详见 docs/map/perf/MEASUREMENTS.md §2.5。
     * 对齐按**地址**判（x 是奇数、或行跨度不是 4 的倍数时都会落单）。
     */
    for (i = 0; i < h; i++) {
        uint16_t * row = (uint16_t *)(void *)(dbase
            + (size_t)(y + i) * dst->stride) + x;
        const int32_t end = x + w;
        int32_t j = x;

        if ((((uintptr_t)row) & 3u) != 0u) {
            row[0] = color;
            j++;
        }
        while (j + 1 < end) {
            *(uint32_t *)(void *)(row + (j - x)) = pair;
            j += 2;
        }
        if (j < end) {
            row[j - x] = color;
        }
    }
}

/* ------------------------------------------------------------- EPIC 后端 */

static bool vl_epic_copy(const vmap_blit_surface_t * dst, int32_t dx, int32_t dy,
    const vmap_blit_surface_t * src, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    return myvendor_epic_copy(dst->buf, dst->stride, dx, dy,
        src->buf, src->stride, sx, sy, w, h);
}

static bool vl_epic_fill(const vmap_blit_surface_t * dst, int32_t x, int32_t y,
    int32_t w, int32_t h, uint16_t color)
{
    return myvendor_epic_fill(dst->buf, dst->stride, x, y, w, h, color);
}

/** @brief 平移方向的分类下标：负 → 0、零 → 1、正 → 2。 */
static inline int vl_dir_idx(int32_t shift)
{
    return (shift > 0) ? 2 : ((shift < 0) ? 0 : 1);
}

/*
 * 准入判定：**同一块缓冲内的重叠搬运**要不要交给 EPIC。
 *
 * 重叠（|dx−sx| < w 且 |dy−sy| < h）时，EPIC 内部是"边读边写"的流水，
 * 读指针落在写指针**后面**的那些方向（源在目的右侧 / 下方）会不会被自己
 * 写坏，只有上板实测知道 —— 这就是 vmap_blit_probe() 的产出。没测过、
 * 或测出来不一致的方向，一律退回 CPU（那条路本来就逐位正确，只是慢）。
 * 不重叠时任何方向都安全，直接放行。
 */
static bool vl_overlap_allowed(const vmap_blit_t * b, int32_t shift_x,
    int32_t shift_y, int32_t w, int32_t h)
{
    bool overlap = (shift_x < w) && (shift_x > -w)
        && (shift_y < h) && (shift_y > -h);

    if (!overlap) {
        return true;
    }
    if (!b) {
        return false;
    }
    return b->ovl_ok[vl_dir_idx(shift_x)][vl_dir_idx(shift_y)];
}

/**
 * @brief 确保中转缓冲够大（不够就按需增长，失败返回 false）。
 *
 * 上限就是"一次画布"的量（真机 496×472×2 = 468 KB）。它是**惰性分配**的：
 * 只在真有"重叠且不安全方向"的搬运时才要，而且一旦拿到就一直留着
 * （`vmap_blit_get()` 的句柄是全局的，下次还用得上）。
 */
static bool vl_scratch_ensure(vmap_blit_t * b, size_t bytes)
{
    uint8_t * p;

    if (b->scratch && b->scratch_sz >= bytes) {
        return true;
    }

    p = (uint8_t *)vmap_malloc(bytes);
    if (!p) {
        syslog(LOG_WARNING, "[vblit] 中转缓冲分配失败（%u KB）⇒ 该方向退回 CPU",
            (unsigned)(bytes / 1024u));
        return false;
    }

    vmap_free(b->scratch);
    b->scratch = p;
    b->scratch_sz = bytes;
    syslog(LOG_INFO, "[vblit] 中转缓冲 %u KB（两跳搬运用；只在需要它的方向占）",
        (unsigned)(bytes / 1024u));
    return true;
}

/**
 * @brief 两跳搬运：先 src → 中转，再 中转 → dst。
 *
 * **为什么需要它**：EPIC 内部是"边读边写"的流水，同一块缓冲内目的地址**高于**
 * 源地址时，写指针会超前去盖掉还没读的源像素（上板自检实测：`(0,+48)` 坏
 * 12288 px、`(+48,0)` 坏 12800 px，逐位比对不通过）。绕开的办法很直接 ——
 * 把每一跳都变成**不相交**拷贝：中转缓冲是另一块内存，两跳都与缓存/画布错开，
 * 于是任何方向都安全（前提是自检的对照用例证明"不相交拷贝逐位可信"）。
 *
 * 代价是两次拷贝、两倍内存带宽。按板上基准（186 KB：CPU 21.45M 周期、
 * EPIC 2.65M）折算，一次全画布矩形 ≈ 2×20 ms，仍远低于 CPU 的 130~200 ms。
 */
static bool vl_epic_copy_via_scratch(vmap_blit_t * b,
    const vmap_blit_surface_t * dst, int32_t dx, int32_t dy,
    const vmap_blit_surface_t * src, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    vmap_blit_surface_t tmp;
    size_t bytes = (size_t)w * sizeof(uint16_t) * (size_t)h;

    if (!b->nonovl_ok) {
        return false;   /* 连不相交拷贝都没被证明 ⇒ 别用硬件 */
    }
    if (!vl_scratch_ensure(b, bytes)) {
        return false;
    }

    tmp.buf = (uint16_t *)b->scratch;
    tmp.w = w;
    tmp.h = h;
    tmp.stride = (uint32_t)w * sizeof(uint16_t);

    if (!vl_epic_copy(&tmp, 0, 0, src, sx, sy, w, h)) {
        return false;   /* 第一跳没跑成：画布还没被动过，交回 CPU 是安全的 */
    }
    /* 第二跳失败的话画布就被改了一半 —— 但同一块芯片上第一跳成了第二跳
     * 不会失败（同一组寄存器、同样的矩形），真失败也只能说明硬件出事，
     * 这时调用方照样会走 CPU 把结果盖对。 */
    return vl_epic_copy(dst, dx, dy, &tmp, 0, 0, w, h);
}

static bool hw_copy(vmap_blit_t * b, const vmap_blit_surface_t * dst,
    int32_t dx, int32_t dy, const vmap_blit_surface_t * src, int32_t sx,
    int32_t sy, int32_t w, int32_t h)
{
    if (!b || b->backend != VMAP_BLIT_EPIC) {
        return false;
    }
    /* 自检的对照用例是所有硬件用法的**总闸**：它没过（或压根没跑）就纯 CPU，
     * 连不相交的单跳也不给 —— 那时关于这块硬件的任何假设都没被验证过。 */
    if (!b->nonovl_ok) {
        return false;
    }
    if (vl_overlap_allowed(b, dx - sx, dy - sy, w, h)) {
        if (vl_epic_copy(dst, dx, dy, src, sx, sy, w, h)) {
            b->n_1hop++;
            return true;
        }
        b->n_cpu++;
        return false;
    }
    /* 重叠且该方向自检没过 ⇒ 改走两跳（每一跳都不相交），仍然比 CPU 快得多。 */
    if (vl_epic_copy_via_scratch(b, dst, dx, dy, src, sx, sy, w, h)) {
        b->n_2hop++;
        return true;
    }
    b->n_cpu++;
    return false;
}

static bool hw_fill(vmap_blit_t * b, const vmap_blit_surface_t * dst,
    int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    if (!b || b->backend != VMAP_BLIT_EPIC) {
        return false;
    }
    /* 颜色回读没被自检证实过就不用（见 fill_ok 的说明）。 */
    if (!b->fill_ok) {
        return false;
    }
    return vl_epic_fill(dst, x, y, w, h, color);
}

/* ------------------------------------------------------------------ 入口 */

static bool clip_rect(const vmap_blit_surface_t * s, int32_t * x, int32_t * y,
    int32_t * w, int32_t * h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > s->w) {
        *w = s->w - *x;
    }
    if (*y + *h > s->h) {
        *h = s->h - *y;
    }
    return *w > 0 && *h > 0;
}

void vmap_blit_stats(const vmap_blit_t * b, uint32_t * one_hop,
    uint32_t * two_hop, uint32_t * cpu)
{
    if (one_hop) {
        *one_hop = b ? b->n_1hop : 0u;
    }
    if (two_hop) {
        *two_hop = b ? b->n_2hop : 0u;
    }
    if (cpu) {
        *cpu = b ? b->n_cpu : 0u;
    }
}

bool vmap_blit_copy(vmap_blit_t * b, const vmap_blit_surface_t * dst,
    int32_t dx, int32_t dy, const vmap_blit_surface_t * src, int32_t sx,
    int32_t sy, int32_t w, int32_t h)
{
    int32_t cw = w;
    int32_t ch = h;
    int32_t cx = 0;   /* 相对 (sx,sy) 的裁剪偏移，源与目的共用 */
    int32_t cy = 0;

    if (!b || !dst || !dst->buf || !src || !src->buf || w <= 0 || h <= 0) {
        return false;
    }

    /* 先在"源坐标"里裁一次，再把这个偏移平移到目的坐标。 */
    if (sx < 0) { cx = -sx; sx = 0; cw -= cx; }
    if (sy < 0) { cy = -sy; sy = 0; ch -= cy; }
    if (sx + cw > src->w) { cw = src->w - sx; }
    if (sy + ch > src->h) { ch = src->h - sy; }
    if (cw <= 0 || ch <= 0) {
        return false;
    }

    dx += cx;
    dy += cy;
    if (!clip_rect(dst, &dx, &dy, &cw, &ch)) {
        return false;
    }

    if (MYVENDOR_BLIT_EPIC && hw_copy(b, dst, dx, dy, src, sx, sy, cw, ch)) {
        vmap_blit_wait(b);
        return true;
    }

    cpu_copy(dst, dx, dy, src, sx, sy, cw, ch);
    return false;
}

bool vmap_blit_fill(vmap_blit_t * b, const vmap_blit_surface_t * dst,
    int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    if (!b || !dst || !dst->buf || w <= 0 || h <= 0) {
        return false;
    }
    if (!clip_rect(dst, &x, &y, &w, &h)) {
        return false;
    }

    if (MYVENDOR_BLIT_EPIC && hw_fill(b, dst, x, y, w, h, color)) {
        vmap_blit_wait(b);
        return true;
    }

    cpu_fill(dst, x, y, w, h, color);
    return false;
}

void vmap_blit_wait(vmap_blit_t * b)
{
    /*
     * EPIC 这条路上其实没什么可等的：板级封装底下是 polling 版 HAL，
     * **返回时 `EPIC_WaitDone()` 已经等完**。这里仍然照约定走一遍
     * `myvendor_epic_wait()`（它只做 idle 记账），免得日后换后端
     * （比如 DMAC 或 _IT 版）时忘了这一步。
     */
    if (b && b->backend == VMAP_BLIT_EPIC) {
        myvendor_epic_wait();
    }
}

/* ------------------------------------------------------- 上板自检 / 基准 */

/*
 * 时间基准沿用 [vperf] 那套 DWT_CYCCNT（见 vmap_view.c 顶部）：
 * 同一个计数器、同一个写法（两个 LAR 都要写；**不清零** —— 全系统共享）。
 * 只报原始周期数，不换算微秒（SystemCoreClock 是 SysTick 基准的陈旧值）。
 */
#define VL_DWT_DEMCR    (*(volatile uint32_t *)0xe000edfcu)
#define VL_DWT_CTRL     (*(volatile uint32_t *)0xe0001000u)
#define VL_DWT_CYCCNT   (*(volatile uint32_t *)0xe0001004u)
#define VL_DWT_LAR      (*(volatile uint32_t *)0xe0001fb0u)
#define VL_ITM_LAR      (*(volatile uint32_t *)0xe0000fb0u)
#define VL_LAR_KEY      0xc5acce55u

static inline uint32_t vl_cyc(void)
{
    return VL_DWT_CYCCNT;
}

static void vl_cyc_enable(void)
{
    VL_ITM_LAR = VL_LAR_KEY;
    VL_DWT_LAR = VL_LAR_KEY;
    VL_DWT_DEMCR |= (1u << 24);   /* TRCENA */
    VL_DWT_CTRL |= 1u;            /* CYCCNTENA */
}

/*
 * 自检缓冲的高度上限。
 *
 * 自检要问的是"**同一块缓冲内源/目的矩形重叠**时 EPIC 的结果还对不对"，
 * 所以几何必须让重叠真的发生、而且要比生产更深：
 *   - 行宽用真机画布宽度（496）⇒ 横向流水行为与生产一致；
 *   - 平移量 48 px、矩形高 80 px ⇒ **纵向重叠 48 行 = 60%**，
 *     而生产（画布 496×472、跟车单次平移 ≤ 88 px）只有约 16%；
 *   - 缓冲 2×186 KB，一次性分配、自检完立即释放。
 *
 * 覆盖不到的唯一窗口：引擎若"预取深度恰好 49~88 行"，则只有 49~88 px 的
 * 平移会坏 —— 对一颗 2D blitter 来说这个预取深度不现实（那意味着几十行
 * 的行缓冲），而且运行时真出现那种平移，本文件也会因为该方向没通过自检而
 * 退回 CPU（见 vl_overlap_allowed）。
 */
#define VL_PROBE_H       192
#define VL_PROBE_SHIFT   48
#define VL_PROBE_MARGIN  8

/** @brief 基准的行宽：真机画布行宽（496 px × 2 B）。理由见 vmap_blit_bench()。 */
#define VL_BENCH_STRIDE  992u

/** @brief EPIC 的坐标上限（`bf0_hal_epic.h` 的 EPIC_COORDINATES_MAX；板级封装另有检查）。 */
#define VL_EPIC_COORD_MAX 1010

/** @brief 自检图案：每个像素都不一样，且与"被写坏后读到的值"不同。 */
static uint16_t vl_pat(int32_t x, int32_t y)
{
    uint32_t v = (uint32_t)x * 2654435761u ^ (uint32_t)y * 40503u;

    v ^= v >> 15;
    v *= 2246822519u;
    return (uint16_t)(v ^ (v >> 7));
}

static void vl_pattern(uint16_t * buf, int32_t w, int32_t h, uint32_t stride)
{
    int32_t y;

    for (y = 0; y < h; y++) {
        uint16_t * row = (uint16_t *)(void *)((uint8_t *)buf + (size_t)y * stride);
        int32_t x;

        for (x = 0; x < w; x++) {
            row[x] = vl_pat(x, y);
        }
    }
}

/** @brief 比对结果：不只是"差多少"，还要能判"是没写、还是写错了"。 */
typedef struct {
    uint32_t n;        /**< 不同像素数。 */
    uint32_t n_pat;    /**< 其中 A 仍等于**原始图案**的（= 这一块没被写）。 */
    uint32_t n_zero;   /**< 其中 A 是 0 的。 */
    int32_t  fx;
    int32_t  fy;
    int32_t  bx0;      /**< 不同像素的包围盒。 */
    int32_t  by0;
    int32_t  bx1;
    int32_t  by1;
    uint16_t ga;
    uint16_t gb;
} vl_diff_t;

/**
 * @brief 整块逐位比对（`pat_cmp` 为真时顺带统计"没被写"的像素）。
 *
 * 分类的意义：`n_pat == n` ⇒ 那块**一个像素都没写**（纯跳过）；
 * `n_zero == n` ⇒ 写了，但写进去的是 0（源地址/跨度不对）；
 * 两者都不是 ⇒ 写到了别处或写错了内容。一次上板就能分清，不用猜。
 */
static void vl_diff(const uint16_t * a, const uint16_t * b, int32_t w,
    int32_t h, uint32_t stride, bool pat_cmp, vl_diff_t * out)
{
    int32_t y;

    out->n = 0;
    out->n_pat = 0;
    out->n_zero = 0;
    out->fx = -1;
    out->fy = -1;
    out->bx0 = w;
    out->by0 = h;
    out->bx1 = -1;
    out->by1 = -1;
    out->ga = 0;
    out->gb = 0;

    for (y = 0; y < h; y++) {
        const uint16_t * ra = (const uint16_t *)(const void *)
            ((const uint8_t *)a + (size_t)y * stride);
        const uint16_t * rb = (const uint16_t *)(const void *)
            ((const uint8_t *)b + (size_t)y * stride);
        int32_t x;

        for (x = 0; x < w; x++) {
            if (ra[x] != rb[x]) {
                if (out->n == 0) {
                    out->fx = x;
                    out->fy = y;
                    out->ga = ra[x];
                    out->gb = rb[x];
                }
                if (pat_cmp && ra[x] == vl_pat(x, y)) {
                    out->n_pat++;
                }
                if (ra[x] == 0u) {
                    out->n_zero++;
                }
                if (x < out->bx0) { out->bx0 = x; }
                if (x > out->bx1) { out->bx1 = x; }
                if (y < out->by0) { out->by0 = y; }
                if (y > out->by1) { out->by1 = y; }
                out->n++;
            }
        }
    }
}

typedef struct {
    const char * name;
    int32_t sx;
    int32_t sy;
    int32_t dx;
    int32_t dy;
    int32_t w;
    int32_t h;
    /** 本用例的源/目的矩形是否**真的重叠**。只有真重叠的用例才允许写准入位
     *  —— 对照用例（不相交）不能拿来证明任何重叠方向可用。 */
    bool overlap;
} vl_case_t;

/** @brief 跑一个搬运用例（EPIC 打 A、CPU 打 B，再整块比对）。 */
static void vl_run_case(vmap_blit_t * b, const vl_case_t * c,
    vmap_blit_surface_t * sa, vmap_blit_surface_t * sb)
{
    vl_diff_t d;
    uint32_t ep;
    uint32_t cpu;
    uint32_t c0;
    bool hw;
    bool busy;

    vl_pattern(sa->buf, sa->w, sa->h, sa->stride);
    memcpy(sb->buf, sa->buf, (size_t)sa->stride * (size_t)sa->h);

    c0 = vl_cyc();
    hw = vl_epic_copy(sa, c->dx, c->dy, sa, c->sx, c->sy, c->w, c->h);
    ep = vl_cyc() - c0;
    busy = myvendor_epic_busy();

    c0 = vl_cyc();
    cpu_copy(sb, c->dx, c->dy, sb, c->sx, c->sy, c->w, c->h);
    cpu = vl_cyc() - c0;

    vl_diff(sa->buf, sb->buf, sa->w, sa->h, sa->stride, true, &d);

    if (hw && d.n == 0) {
        if (c->overlap) {
            b->ovl_ok[vl_dir_idx(c->dx - c->sx)][vl_dir_idx(c->dy - c->sy)] = true;
        } else {
            /* 对照用例通过 = "不相交拷贝逐位可信" ⇒ 两跳搬运算术上成立。 */
            b->nonovl_ok = true;
        }
    }

    /* 降噪（用户 2026-09-26）：通过、以及"重叠拷贝在该方向不可直接用"这种**能力
     * 发现**（它本来就该写进方向表、走两跳）都只在 debug 级留细节 ——
     * `ctl log debug` 可看回来。真正的问题才升级为 WARNING：
     *   ① 根本没走硬件（!hw）；② 不相交拷贝都不一致（!overlap && dif）——
     * 后者会让两跳搬运的算术前提失效。 */
    syslog((hw && d.n == 0) ? LOG_DEBUG
            : ((!hw || !c->overlap) ? LOG_WARNING : LOG_DEBUG),
        "[vblit] case %s ovl=%u shift=(%d,%d) hw=%u busy=%u dif=%u pat=%u zero=%u bbox=(%d,%d)-(%d,%d) first=(%d,%d) got=0x%04x want=0x%04x ep=%u cpu=%u",
        c->name, (unsigned)(c->overlap ? 1u : 0u), (int)(c->dx - c->sx),
        (int)(c->dy - c->sy), (unsigned)(hw ? 1u : 0u), (unsigned)(busy ? 1u : 0u),
        (unsigned)d.n, (unsigned)d.n_pat, (unsigned)d.n_zero,
        (int)d.bx0, (int)d.by0, (int)d.bx1, (int)d.by1,
        (int)d.fx, (int)d.fy, (unsigned)d.ga, (unsigned)d.gb,
        (unsigned)ep, (unsigned)cpu);
}

/** @brief 跑填充用例（颜色分量回读是否逐位一致）。 */
static void vl_run_fill(vmap_blit_t * b, vmap_blit_surface_t * sa,
    vmap_blit_surface_t * sb, int32_t x, int32_t y, int32_t w, int32_t h,
    uint16_t color)
{
    vl_diff_t d;
    uint32_t ep;
    uint32_t cpu;
    uint32_t c0;
    bool hw;
    bool busy;

    vl_pattern(sa->buf, sa->w, sa->h, sa->stride);
    memcpy(sb->buf, sa->buf, (size_t)sa->stride * (size_t)sa->h);

    c0 = vl_cyc();
    hw = vl_epic_fill(sa, x, y, w, h, color);
    ep = vl_cyc() - c0;
    busy = myvendor_epic_busy();

    c0 = vl_cyc();
    cpu_fill(sb, x, y, w, h, color);
    cpu = vl_cyc() - c0;

    vl_diff(sa->buf, sb->buf, sa->w, sa->h, sa->stride, true, &d);

    if (!(hw && d.n == 0)) {
        b->fill_ok = false;
    }

    /* 填充没有重叠语义：不一致就是真问题 ⇒ WARNING；通过只留 debug。 */
    syslog((hw && d.n == 0) ? LOG_DEBUG : LOG_WARNING,
        "[vblit] fill color=0x%04x hw=%u busy=%u dif=%u pat=%u zero=%u bbox=(%d,%d)-(%d,%d) first=(%d,%d) got=0x%04x want=0x%04x ep=%u cpu=%u",
        (unsigned)color, (unsigned)(hw ? 1u : 0u), (unsigned)(busy ? 1u : 0u),
        (unsigned)d.n, (unsigned)d.n_pat, (unsigned)d.n_zero,
        (int)d.bx0, (int)d.by0, (int)d.bx1, (int)d.by1,
        (int)d.fx, (int)d.fy, (unsigned)d.ga, (unsigned)d.gb,
        (unsigned)ep, (unsigned)cpu);
}

bool vmap_blit_probe(int32_t canvas_w, int32_t canvas_h)
{
    if (!MYVENDOR_BLIT_EPIC) {
        /* 开关关着：自检不跑。**留一行说明**，否则日志里 [vblit] 集体消失会被
         * 误读成"搬运路径坏了"，而下一句"为什么没走硬件"就无从判断。 */
        syslog(LOG_INFO,
            "[vblit] EPIC 搬运已关闭（MYVENDOR_BLIT_EPIC=0）⇒ 全部走 CPU\n");
        return false;
    }

    static const int8_t k_dir_x[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    static const int8_t k_dir_y[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const char * const k_dir_name[8] = {
        "dx-dy-", "dx-dy0", "dx-dy+", "dx0dy-",
        "dx0dy+", "dx+dy-", "dx+dy0", "dx+dy+"
    };
    vmap_blit_t * b = vmap_blit_get();
    vmap_blit_surface_t sa;
    vmap_blit_surface_t sb;
    vl_case_t cs[9];
    uint16_t * abuf;
    uint16_t * bbuf;
    int32_t h_probe;
    int32_t x0;
    int32_t y0;
    int32_t rw;
    int32_t rh;
    uint32_t stride;
    size_t bytes;
    uint32_t t0;
    uint32_t cpu_cyc = 0;
    uint32_t hw_cyc = 0;
    int i;

    if (!b || b->backend != VMAP_BLIT_EPIC) {
        syslog(LOG_WARNING, "[vblit] 无 EPIC 后端，自检跳过");
        return false;
    }

    /*
     * 一个会话只跑一次。map 页会被反复 load/unload（`map_page_on_unload()` →
     * `vmap_view_destroy()`），而结论只取决于**硬件行为**、与画布尺寸无关；
     * 重复跑只会每次白花掉约一秒的开机时间（约 12 个用例 × 186 KB 图案往返）。
     * 所以第一次试过就记下结果，后面直接返回，连失败也不再重试（失败时准入位
     * 保持全 0 ⇒ 重叠一律走 CPU，是安全侧）。
     */
    if (s_probed) {
        return s_probe_ok;
    }
    s_probed = true;

    h_probe = (canvas_h > VL_PROBE_H) ? VL_PROBE_H : canvas_h;
    rw = canvas_w - 2 * (VL_PROBE_MARGIN + VL_PROBE_SHIFT);
    rh = h_probe - 2 * (VL_PROBE_MARGIN + VL_PROBE_SHIFT);
    if (rw < 32 || rh < 24 || rh <= VL_PROBE_SHIFT) {
        syslog(LOG_WARNING,
            "[vblit] 自检几何装不下（画布 %dx%d），跳过", (int)canvas_w,
            (int)canvas_h);
        return false;
    }

    stride = (uint32_t)canvas_w * sizeof(uint16_t);
    bytes = (size_t)stride * (size_t)h_probe;
    abuf = (uint16_t *)vmap_malloc(bytes);
    bbuf = (uint16_t *)vmap_malloc(bytes);
    if (!abuf || !bbuf) {
        vmap_free(abuf);
        vmap_free(bbuf);
        syslog(LOG_WARNING, "[vblit] 自检缓冲分配失败（%u B ×2），跳过",
            (unsigned)bytes);
        return false;
    }

    sa.buf = abuf;
    sa.w = canvas_w;
    sa.h = h_probe;
    sa.stride = stride;
    sb.buf = bbuf;
    sb.w = canvas_w;
    sb.h = h_probe;
    sb.stride = stride;

    vl_cyc_enable();
    t0 = vl_cyc();

    /* 用例 0：**不相交**拷贝（对照）。位置放在缓冲顶部，与重叠用例的矩形分开，
     * 而且**不写准入位** —— 不相交能过，说明不了任何重叠方向可用。 */
    cs[0].name = "ctrl";
    cs[0].sx = VL_PROBE_MARGIN;
    cs[0].sy = VL_PROBE_MARGIN;
    cs[0].dx = VL_PROBE_MARGIN;
    cs[0].dy = VL_PROBE_MARGIN + rh + VL_PROBE_MARGIN;
    cs[0].w = rw;
    cs[0].h = rh;
    cs[0].overlap = false;

    /* 用例 1~8：八个平移方向，矩形居中，保证 ±SHIFT 后仍在缓冲内。
     * 平移量 |SHIFT| = 48 < 矩形高 rh（几何校验已保证 rh > SHIFT）
     * ⇒ 这八个用例的源/目的矩形**真的重叠**。 */
    x0 = VL_PROBE_MARGIN + VL_PROBE_SHIFT;
    y0 = VL_PROBE_MARGIN + VL_PROBE_SHIFT;
    for (i = 0; i < 8; i++) {
        cs[i + 1].name = k_dir_name[i];
        cs[i + 1].sx = x0;
        cs[i + 1].sy = y0;
        cs[i + 1].dx = x0 + k_dir_x[i] * VL_PROBE_SHIFT;
        cs[i + 1].dy = y0 + k_dir_y[i] * VL_PROBE_SHIFT;
        cs[i + 1].w = rw;
        cs[i + 1].h = rh;
        cs[i + 1].overlap = true;
    }

    syslog(LOG_DEBUG,
        "[vblit] probe begin canvas=%dx%d probe=%dx%d stride=%u buf=%uKBx2 shift=%d rect=%dx%d",
        (int)canvas_w, (int)canvas_h, (int)canvas_w, (int)h_probe,
        (unsigned)stride, (unsigned)(bytes / 1024), (int)VL_PROBE_SHIFT,
        (int)rw, (int)rh);

    for (i = 0; i < 9; i++) {
        vl_run_case(b, &cs[i], &sa, &sb);
    }

    /* 填充色回读：r/b 与 g 各挑一个"满值"的颜色，最能照出展开约定不对。
     * 先置 true，任一个用例对不上就翻掉（见 fill_ok 的说明）。 */
    b->fill_ok = true;
    vl_run_fill(b, &sa, &sb, x0, y0, rw, rh, 0xf81fu);
    vl_run_fill(b, &sa, &sb, x0, y0, rw, rh, 0x07ffu);

    vmap_blit_bench(b, bytes, &cpu_cyc, &hw_cyc);
    (void)cpu_cyc;
    (void)hw_cyc;

    {
        /* 准入位图：bit = dirx*3 + diry（dirx/diry：0 = 负、1 = 零、2 = 正）。
         * 0x1ff = 九个方向全通过；只打印位图 + 逐方向的名字，便于一眼看出
         * 哪一支还能走 EPIC。 */
        uint32_t bits = 0u;

        for (i = 0; i < 3; i++) {
            int j;

            for (j = 0; j < 3; j++) {
                if (b->ovl_ok[i][j]) {
                    bits |= (1u << (unsigned)(i * 3 + j));
                }
            }
        }

        syslog(LOG_INFO,
            "[vblit] probe done ovl_bits=0x%03x (bit=dirx*3+diry; dirx/diry: 0=-,1=0,2=+) fill=%u nonovl=%u total=%u cyc",
            (unsigned)bits, (unsigned)(b->fill_ok ? 1u : 0u),
            (unsigned)(b->nonovl_ok ? 1u : 0u), (unsigned)(vl_cyc() - t0));
    }

    vmap_free(abuf);
    vmap_free(bbuf);
    s_probe_ok = true;
    return true;
}

void vmap_blit_bench(vmap_blit_t * b, uint32_t bytes, uint32_t * cpu_cyc,
    uint32_t * hw_cyc)
{
    vmap_blit_surface_t sa;
    vmap_blit_surface_t sb;
    uint16_t * abuf;
    uint16_t * bbuf;
    uint32_t c0;
    uint32_t hw;
    uint32_t rows;
    uint32_t stride;

    if (cpu_cyc) {
        *cpu_cyc = 0u;
    }
    if (hw_cyc) {
        *hw_cyc = 0u;
    }
    if (!b || bytes < VL_BENCH_STRIDE * 8u || !cpu_cyc || !hw_cyc) {
        return;
    }

    /*
     * 几何：行宽固定取真机画布行宽（496 px = 992 B），字节数靠**行数**堆。
     * 不能靠加宽 —— EPIC 的宽/坐标上限是 1010 px，宽度一超就直接被板级封装
     * 拒掉（返回 false），基准会静默变成 0。
     */
    stride = VL_BENCH_STRIDE;
    rows = bytes / stride;
    if (rows < 8u || rows > VL_EPIC_COORD_MAX) {
        syslog(LOG_INFO, "[vblit] bench 几何不合适（%u B），跳过",
            (unsigned)bytes);
        return;
    }

    abuf = (uint16_t *)vmap_malloc((size_t)stride * rows);
    bbuf = (uint16_t *)vmap_malloc((size_t)stride * rows);
    if (!abuf || !bbuf) {
        vmap_free(abuf);
        vmap_free(bbuf);
        syslog(LOG_NOTICE, "[vblit] bench 缓冲分配失败，跳过");
        return;
    }

    sa.buf = abuf;
    sa.w = (int32_t)(stride / sizeof(uint16_t));
    sa.h = (int32_t)rows;
    sa.stride = stride;
    sb = sa;
    sb.buf = bbuf;

    vl_pattern(abuf, sa.w, sa.h, stride);

    /* CPU：整块 A → B（不相交，所以这里量的是**纯吞吐**，不含重叠方向问题）。 */
    c0 = vl_cyc();
    cpu_copy(&sb, 0, 0, &sa, 0, 0, sa.w, sa.h);
    *cpu_cyc = vl_cyc() - c0;

    /* 硬件：同样的矩形，同样不相交。 */
    c0 = vl_cyc();
    hw = vl_epic_copy(&sb, 0, 0, &sa, 0, 0, sa.w, sa.h) ? 1u : 0u;
    *hw_cyc = vl_cyc() - c0;

    if (!hw) {
        *hw_cyc = 0u;   /* 没跑成 ⇒ 明确报 0，别拿它当"硬件很快" */
    }

    syslog(LOG_NOTICE,
        "[vblit] bench %ux%u px (%u KB) cpu=%u ep=%u ratio=%u%% (ep=0 表示没跑成)",
        (unsigned)sa.w, (unsigned)sa.h, (unsigned)(stride * rows / 1024u),
        (unsigned)*cpu_cyc, (unsigned)*hw_cyc,
        (unsigned)(*cpu_cyc != 0u ? (*hw_cyc * 100u / *cpu_cyc) : 0u));

    vmap_free(abuf);
    vmap_free(bbuf);
}
