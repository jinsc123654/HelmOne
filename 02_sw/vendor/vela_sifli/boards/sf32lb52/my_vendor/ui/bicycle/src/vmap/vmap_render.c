/**
 * @file vmap_render.c
 * @brief vmap render 模块。
 */

#include "vmap_render.h"

#include "vmap_alloc.h"   /* 【临时取证】自检要 vmap_malloc */
#include "vmap_blit.h"
#include "vmap_config.h"
#include "vmap_format.h"
#include "vmap_style.h"
#include <math.h>
#include <string.h>

/*
 * 搬运后端：整块矩形（拷贝/填充）交给它，CPU 只是它的兜底。
 *
 * 句柄全局一个（EPIC 是独占硬件），准入判定在 `vmap_blit.c` 里 —— 只有开机
 * 自检证过"这个方向的重叠搬运与 CPU 逐位一致"才会真的走硬件，否则同一个
 * 调用会退回 CPU。所以下面这些调用点**不需要**再判断走没走硬件。
 */
static vmap_blit_t * s_blit;

static vmap_blit_t * render_blit(void)
{
    if (!s_blit) {
        s_blit = vmap_blit_get();
    }
    return s_blit;
}

/** @brief 把画布包成搬运器认的 surface。 */
static void render_surface(const vmap_canvas_t * c, vmap_blit_surface_t * s)
{
    s->buf = c->buf;
    s->w = c->w;
    s->h = c->h;
    s->stride = c->stride;
}

#define RGB565(r, g, b) VMAP_STYLE_RGB565((r), (g), (b))

#define ROAD_STYLE_NUM VMAP_STYLE_ROAD_CLASSES

#if VMAP_RENDER_AA
#define VMAP_FP_SHIFT 8
#define VMAP_FP_ONE   (1 << VMAP_FP_SHIFT)
#endif

/*
 * 图元级周期拆分（多边形 / 面填充 / 圆盘）+ 三个工作量计数，配合 vmap_view.c 的
 * `[vperf] prim` 一行 —— 用来回答"`cyc_tile` 里多边形占多少、一帧要做多少次
 * 逐行软除、多少次 (行 × 边) 扫描"。**只读 CYCCNT、只加计数，不改任何像素**；
 * 宿主差分构建用 `-DVMAP_RENDER_PRIM=0` 关掉（DWT 在宿主机上不存在）。
 *
 * 实测（全量帧、240 MHz）：`polyfp ≈ 12M` / `poly ≈ 17M` / `disc ≈ 11M` 周期，
 * 三者合计约 `cyc_tile` 的 70%；`cross ≈ 2.3 万` 次/帧。数字与用法见
 * docs/map/perf/MEASUREMENTS.md §9。
 */
#ifndef VMAP_RENDER_PRIM
#define VMAP_RENDER_PRIM 1
#endif

/** 开机跑一次的多边形定点自测（默认关：一次要 ~0.5 s；只在测"多边形这条路"
 *  的变化时打开 —— 见 `[vperf] polyself`）。 */
#ifndef VMAP_RENDER_PRIM_SELF
#define VMAP_RENDER_PRIM_SELF 0
#endif

static uint32_t s_prim_poly_fp;      /**< fill_polygon_fp 的周期数 */
static uint32_t s_prim_poly;         /**< fill_polygon 的周期数 */
static uint32_t s_prim_disc;         /**< fill_disc_aa_fp 的周期数 */
static uint32_t s_prim_n_poly_fp;
static uint32_t s_prim_n_poly;
static uint32_t s_prim_n_disc;
static uint32_t s_prim_cross;        /**< 逐行软除次数（活动的边 × 行） */
static uint32_t s_prim_scan;         /**< (行 × 边) 扫描次数（无活动边表的代价） */
static uint32_t s_prim_hist[6];      /**< n 的直方图 */
static uint32_t s_prim_cyc_n[6];     /**< 按 n 分桶的周期数（判"上限该开多大"） */

void vmap_render_prim_get(vmap_render_prim_t * out)
{
    int i;

    out->poly_fp = s_prim_poly_fp;
    out->poly = s_prim_poly;
    out->disc = s_prim_disc;
    out->n_poly_fp = s_prim_n_poly_fp;
    out->n_poly = s_prim_n_poly;
    out->n_disc = s_prim_n_disc;
    out->cross = s_prim_cross;
    out->scan = s_prim_scan;
    for (i = 0; i < 6; i++) {
        out->hist[i] = s_prim_hist[i];
        out->cyc_n[i] = s_prim_cyc_n[i];
    }
}

#if VMAP_RENDER_PRIM
#define VMAP_PRIM_DEMCR    (*(volatile uint32_t *)0xe000edfcu)
#define VMAP_PRIM_DWT_LAR  (*(volatile uint32_t *)0xe0001fb0u)
#define VMAP_PRIM_CTRL     (*(volatile uint32_t *)0xe0001000u)
#define VMAP_PRIM_CYCCNT   (*(volatile uint32_t *)0xe0001004u)
#define VMAP_PRIM_T0()     uint32_t _prim_t0

static inline uint32_t vmap_prim_cyc(void)
{
    return VMAP_PRIM_CYCCNT;
}

static void vmap_prim_init(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    VMAP_PRIM_DWT_LAR = 0xc5acce55u;
    VMAP_PRIM_DEMCR |= (1u << 24);
    VMAP_PRIM_CTRL |= 1u;
}

/** 分桶：<=4 / <=8 / <=16 / <=32 / <=256 / >256（后两桶对着待定的上限） */
static inline void vmap_prim_note(int32_t n, uint32_t cyc)
{
    uint32_t b;

    if (n <= 4) {
        b = 0u;
    } else if (n <= 8) {
        b = 1u;
    } else if (n <= 16) {
        b = 2u;
    } else if (n <= 32) {
        b = 3u;
    } else if (n <= 256) {
        b = 4u;
    } else {
        b = 5u;
    }
    s_prim_hist[b]++;
    s_prim_cyc_n[b] += cyc;
}
#else
#define VMAP_PRIM_T0()     uint32_t _prim_t0 = 0u; (void)_prim_t0
#define vmap_prim_init()        do { } while (0)
#define vmap_prim_cyc()         0u
#define vmap_prim_note(n, c)    do { (void)(n); (void)(c); } while (0)
#endif

/*
 * 画布存储计数（性能取证；置 0 即消失）。
 *
 * 用来分辨成本模型：(a) ∝ 写入**字节数**，(b) ∝ **每次 store 一次总线事务**。
 * 支持 (b) 的两个数据点：4 字节 store 约 88 周期；16 字节的 stmia 约 357 ≈ 4×88
 * （宽写被拆成 4 字节事务）。若 (b) 成立，杠杆是"减少 store 次数"而不是字节数，
 * 而且 16 位单像素 store 与 32 位同价 —— 那才是真正被浪费的一半。
 */
#ifndef VMAP_RENDER_STAT
#define VMAP_RENDER_STAT 1
#endif

#if VMAP_RENDER_STAT
static uint32_t s_stat_stores;
static uint32_t s_stat_bytes;

void vmap_render_stat_reset(void)
{
    s_stat_stores = 0u;
    s_stat_bytes = 0u;
}

void vmap_render_stat_get(uint32_t * stores, uint32_t * bytes)
{
    if (stores) {
        *stores = s_stat_stores;
    }
    if (bytes) {
        *bytes = s_stat_bytes;
    }
}

#define STAT_STORE(nbytes)  do { s_stat_stores++; s_stat_bytes += (nbytes); } while (0)
#define STAT_STORE_N(n, nbytes) \
    do { s_stat_stores += (uint32_t)(n); s_stat_bytes += (uint32_t)(nbytes); } while (0)
#else
void vmap_render_stat_reset(void) { }
void vmap_render_stat_get(uint32_t * stores, uint32_t * bytes)
{
    if (stores) { *stores = 0u; }
    if (bytes) { *bytes = 0u; }
}
#define STAT_STORE(nbytes)  do { } while (0)
#define STAT_STORE_N(n, nbytes)  do { } while (0)
#endif

static const vmap_style_road_t * road_style_for(int cls)
{
    const vmap_style_t * style = vmap_style_current();

    if (cls < 1 || cls >= ROAD_STYLE_NUM) {
        cls = VMAP_ROAD_RESIDENTIAL;
    }

    return &style->road[cls];
}

void vmap_render_clip_reset(vmap_canvas_t * c)
{
    if (c) {
        c->clip_n = 0;
    }
}

void vmap_render_clip_add(vmap_canvas_t * c, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    uint8_t i;

    if (!c || c->clip_n >= VMAP_RENDER_DIRTY_MAX || x1 >= x2 || y1 >= y2) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > c->w) {
        x2 = c->w;
    }
    if (y2 > c->h) {
        y2 = c->h;
    }
    if (x1 >= x2 || y1 >= y2) {
        return;
    }

    i = c->clip_n;
    c->clip_x1[i] = x1;
    c->clip_y1[i] = y1;
    c->clip_x2[i] = x2;
    c->clip_y2[i] = y2;
    c->clip_n++;
}

/*
 * 逐像素调 canvas_in_clip() 的时代过去了。
 *
 * 旧写法对图元范围内的**每一个**像素都要过一次裁剪判定：函数调用 + 最多 8 个
 * 矩形的循环（而 -O1 不内联）。裁剪省下的是 PSRAM 存储，付出的却是全量像素都
 * 多这一笔 —— 实测（analysis/LIVEMAP_PERF_AUDIT.md §8.3 ②）加裁剪后每个瓦片
 * 反而贵 2.5~4.5 倍（不裁 21~27 ms、裁 61~95 ms，两组样本不重叠）。
 *
 * 现在改成：**先按行算出真正可写的 x 区间（裁剪并集），再只对这些区间落笔。**
 * 覆盖度仍然逐像素算（它只与 x 和 span 的两个 fp 端点有关，与裁剪无关），
 * 每个像素仍然只写一次（并集去重 ⇒ 半透明像素不会被混合两次），
 * 所以输出逐位不变。
 */
typedef struct {
    int32_t a;
    int32_t b;
} vmap_row_span_t;

/** @brief 把 [a,b] 并入已排序、互不相交也不相邻的 out[]；返回新区间数。 */
static uint8_t row_span_merge(vmap_row_span_t * out, uint8_t n, uint8_t cap,
    int32_t a, int32_t b)
{
    uint8_t i;
    uint8_t m;

    for (i = 0; i < n; i++) {
        if (b < out[i].a - 1) {
            break;                        /* 整段落在 i 之前 ⇒ 插到 i 处 */
        }
        if (a > out[i].b + 1) {
            continue;                     /* 与 i 不相接 ⇒ 继续往后找 */
        }
        /* 与 i 相交或相邻：并进 i，再吞掉后面被它盖住的区间 */
        if (a < out[i].a) {
            out[i].a = a;
        }
        if (b > out[i].b) {
            out[i].b = b;
        }
        while (i + 1u < n && out[i + 1u].a <= out[i].b + 1) {
            if (out[i + 1u].b > out[i].b) {
                out[i].b = out[i + 1u].b;
            }
            for (m = i + 1u; m + 1u < n; m++) {
                out[m] = out[m + 1u];
            }
            n--;
        }
        return n;
    }

    if (n >= cap) {
        return n;                         /* 放不下就丢弃（cap = 脏区上限 8） */
    }
    for (m = n; m > i; m--) {
        out[m] = out[m - 1u];
    }
    out[i].a = a;
    out[i].b = b;
    return (uint8_t)(n + 1u);
}

/**
 * @brief 行 y 上真正可写的 x 区间（裁剪矩形与 [x0,x1] 求交后的并集）。
 * @return 区间数；0 表示这一行没有可写像素。clip_n==0 时就是 [x0,x1]。
 */
static uint8_t row_clip_spans(const vmap_canvas_t * c, int32_t y, int32_t x0,
    int32_t x1, vmap_row_span_t * out, uint8_t cap)
{
    uint8_t n = 0u;
    uint8_t i;

    if (x0 > x1 || cap == 0u) {
        return 0u;
    }
    if (c->clip_n == 0u) {
        out[0].a = x0;
        out[0].b = x1;
        return 1u;
    }

    for (i = 0u; i < c->clip_n; i++) {
        int32_t a;
        int32_t b;

        if (y < c->clip_y1[i] || y >= c->clip_y2[i]) {
            continue;
        }
        a = x0 > c->clip_x1[i] ? x0 : c->clip_x1[i];
        b = x1 < (c->clip_x2[i] - 1) ? x1 : (c->clip_x2[i] - 1);
        if (a > b) {
            continue;
        }
        n = row_span_merge(out, n, cap, a, b);
    }
    return n;
}

/** @brief 把 [x0,x1]（闭区间）填成 color；成对 32 位写。 */
static void fill_run_u16(uint16_t * row, int32_t x0, int32_t x1, uint16_t color)
{
    const uint32_t pair = ((uint32_t)color << 16) | (uint32_t)color;
    int32_t x = x0;

    if (!row || x0 > x1) {
        return;
    }

    /* 对齐按**地址**判，不能按下标判：vmap_render_scroll 会把 row 本身偏移
     * dx 再交给本函数，指针可能落在奇数元素上。
     *
     * 只做成对（32 位）写就停在这里 —— 不再上 32 字节块：上板实测
     * `[vperf] psram ... blk/w=100%`，即块写与逐字写的**周期/字节完全一样**
     * （都是 ~22 周期/字节），写通路不合并，块写只省指令不省访问。
     * 详见 docs/map/perf/MEASUREMENTS.md §2.5。 */
    if (((uintptr_t)(row + x) & 3u) != 0u) {
        row[x++] = color;
        STAT_STORE(2);
    }
    {
        const int32_t npair = (x1 - x + 1) / 2;

        if (npair > 0) {
            int32_t n = npair;

            while (n-- > 0) {
                *(uint32_t *)(void *)(row + x) = pair;
                x += 2;
            }
            STAT_STORE_N(npair, npair * 4);
        }
    }
    if (x <= x1) {
        row[x] = color;
        STAT_STORE(2);
    }
}

static inline bool canvas_in_clip(const vmap_canvas_t * c, int32_t x, int32_t y)
{
    uint8_t i;

    if (!c || c->clip_n == 0) {
        return true;
    }
    for (i = 0; i < c->clip_n; i++) {
        if (x >= c->clip_x1[i] && x < c->clip_x2[i]
            && y >= c->clip_y1[i] && y < c->clip_y2[i]) {
            return true;
        }
    }
    return false;
}

static inline void put_pixel(vmap_canvas_t * c, int32_t x, int32_t y, uint16_t color)
{
    uint16_t * row;

    if (!c || !c->buf) {
        return;
    }
    if ((uint32_t)x >= (uint32_t)c->w || (uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (!canvas_in_clip(c, x, y)) {
        return;
    }

    row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
    row[x] = color;
    STAT_STORE(2);
}

/* ——— 多边形扫描填充：逐行增量推进 —————————————————————————————
 *
 * 旧写法每个扫描行、每条活动边都算一次
 *
 *     x = xi + trunc((y_scan - yi) * dx / dy)
 *
 * 这里的 `(int64)/(int64)` 在这颗 M33 上没有硬件指令，走 `__aeabi_ldivmod`
 * （一次 150~300 周期），而多边形填充的内层就是**行数 × 活动边数**
 * （实测一帧 23120 次，见 `[vperf] prim cross=`）—— 是这条路上最大的单笔开销。
 *
 * 增量写法：每行 y_scan 只涨一个常量，分子每行只加一个常量 step。难点是必须
 * **逐位**复现 C 的**向零截断**除法：负数余数的符号与 floor 不同，而"每行加一个
 * 常量"对截断除法并不封闭 —— 只维护 `|余数| < |dy|` 会让商漂一位（余数的符号
 * 约定也是解的一部分，光靠"补一次 ±1"补不回来）。所以这里换成**全非负**的
 * "面积 + floor"形式：
 *
 *   M   = |dy|                                     > 0
 *   sgn = sign(dx)                                 （商的方向；dx == 0 时取 +1）
 *   P_k = |dx| * |y_scan_k - yi|                   ≥ 0，每行只加常量 G
 *   A_k = floor(P_k / M)，a_k = P_k - A_k * M      （0 ≤ a_k < M）
 *   x_k = xi + sgn * A_k                           （= 原式，见下）
 *
 * 于是每行的推进只剩：
 *
 *     a += gr;  if (a >= M) { a -= M; x += sgn; }  x += gm;
 *
 * （`gm = sgn*floor(G/M)`、`gr = G mod M` 在装配时算一次 —— 一次除法每条边而已。）
 * 一次加法、一次比较、最多一次 ±1，没有乘法、没有除法、也没有会累积误差的
 * 定点斜率：维护的是 floor 分解的恒等式本身。
 *
 * 为什么 `x_k = xi + sgn*A_k` 就是原式：活动窗口里 `y_scan - yi` 的符号是固定的
 * （dy > 0 时 ∈ [0, dy)，dy < 0 时 ∈ (dy, 0]），所以 n = (y_scan-yi)*dx 的符号也
 * 固定，等于 sgn*sign(dy)；于是 trunc(n/dy) 就等于"按 |dy| 取 floor 再补符号"。
 *
 * 首行（每条边只此一次）仍按上面的定义直接算，等价于原式的量。
 * 量级：画布局部坐标 ±1000 px ⇒ |dx| ≤ 512000 fp ⇒ P ≤ 2.6e11（int64，除法只有
 * 这一处）、A ≤ |dx| ≤ 512000（int32），都装得下。
 */
/** 逐行维护状态的边数上限；**下标 ≥ 上限的边照旧逐行算**（像素不变，只是继续吃
 *  那一次软除）。放 .bss 不放栈：实测 `bicycle_ui` 栈只剩 ~10 KB 余量，而按边的
 *  下标开数组要能被 n 覆盖 —— 静态数组的代价是 RAM，栈的代价是崩溃。
 *
 *  ⚠ 别改成"槽号 = i & (N-1)"那种取模复用：看着省内存，实测**反而更慢** ——
 *  大的面要素（n 上百、同时激活的边一二十条）下标相差 64 的整数倍会有多对撞在
 *  同一个槽上，于是每行都要重新装配 + 播种（两次除法）⇒ `poly` 段实测 +13%。
 *  撞车只有在**同时激活**的边之间才会发生，而大面要素恰恰是这个条件最容易满足的。 */
#define VMAP_POLY_DDA_CAP 256

typedef struct {
    uint32_t gen;   /**< 装配时那次调用的代号（静态数组的残留必须能认出来） */
    int32_t x;      /**< 当前扫描行的 x（fp 或整像素，量纲由调用者定） */
    int32_t a;      /**< P mod M，恒在 [0, M) */
    int32_t gr;     /**< G mod M，恒在 [0, M) */
    int32_t gm;     /**< sgn * floor(G / M)：每行 x 的常量增量 */
    int32_t sgn;    /**< sign(dx)：商的方向（±1） */
} vmap_poly_dda_t;

/** 槽号 == 边号；`gen` 用来区分"这一槽是不是本次调用装配的"（0 是 .bss 初值）。 */
static vmap_poly_dda_t s_poly_dda[VMAP_POLY_DDA_CAP];
static uint32_t s_poly_dda_gen;

/**
 * @brief 装配一条边的常量部分：每行推进用的 gm / gr 与符号。
 * @param step 每行分子的增量：fp 路径是 `VMAP_FP_ONE * dx`，整像素路径是 `dx`。
 * @param yi   边的起点 y；`yj - yi` 必须非 0（调用点已用 live 判据保证）。
 */
static void vmap_poly_dda_arm(vmap_poly_dda_t * e, int32_t yi, int32_t yj,
    int64_t step)
{
    const int32_t dy = yj - yi;

    e->gen = s_poly_dda_gen;
    e->a = 0;
    e->gr = 0;
    e->gm = 0;
    e->sgn = (step < 0) ? -1 : 1;       /* sign(dx)：step 与 dx 同号 */
    if (dy != 0) {
        const int64_t m = (dy < 0) ? -(int64_t)dy : (int64_t)dy;
        const int64_t g = (int64_t)e->sgn * ((dy > 0) ? 1 : -1) * step;
        /* 商与余数都要（= 截断除法的两半）。⚠ 本工具链**不合并**这两个
         * libcall：`g/m` 与 `g%m` 各调一次 __aeabi_ldivmod（写成 `g - q*m`
         * 也一样，会被规范化回 `%`）。所以这里每条边是 2 次软除 —— 但只在
         * **被激活那一次**发生（一帧 ~1.5~3k 次），可以忽略。 */
        int64_t q = g / m;
        int64_t r = g % m;

        if (r < 0) {                    /* 归一到 floor：0 ≤ r < m */
            r += m;
            q -= 1;
        }
        e->gm = (int32_t)((int64_t)e->sgn * q);
        e->gr = (int32_t)r;
    }
}

/**
 * @brief 按定义算这条边在当前扫描行上的 x（`A = floor(|dx|*|delta_y| / |dy|)`）。
 * @param x_base   xi（fp / 整像素路径）或 `xi * VMAP_FP_ONE`（AA 路径的定标）
 * @param dx       `xj - xi`，**已按调用者的量纲定标**（AA 路径传 `dx * 256`）
 *
 * 与原式逐位相同；一条边只在**被激活那一次**走这里，所以这一次 64 位除法不算什么。
 */
static inline void vmap_poly_dda_seed(vmap_poly_dda_t * e, int64_t x_base,
    int64_t dx, int32_t dy, int32_t delta_y)
{
    const int64_t m = (dy < 0) ? -(int64_t)dy : (int64_t)dy;
    const int64_t area = ((dx < 0) ? -dx : dx)
        * ((delta_y < 0) ? -(int64_t)delta_y : (int64_t)delta_y);
    const int64_t q = area / m;         /* 两者都非负 ⇒ 截断即 floor */

    e->x = (int32_t)(x_base + (int64_t)e->sgn * q);
    e->a = (int32_t)(area - q * m);     /* < m，见 arm 的量级说明 */
}

/** @brief 推进一行（m == |dy| ≥ 1）。 */
static inline void vmap_poly_dda_step(vmap_poly_dda_t * e, int32_t m)
{
    const int32_t a = e->a + e->gr;

    e->x += e->gm;
    if (a >= m) {
        e->a = a - m;
        e->x += e->sgn;
    } else {
        e->a = a;
    }
}

/** @brief 这条边在当前扫描行是否激活（与改动前的双条件逐字相同）。 */
static inline bool vmap_poly_dda_live(int32_t yi, int32_t yj, int32_t y_scan)
{
    return (yi <= y_scan && yj > y_scan) || (yj <= y_scan && yi > y_scan);
}

#if VMAP_RENDER_AA
static uint16_t mix_rgb565(uint16_t bg, uint16_t fg, uint8_t mix)
{
    uint32_t mix5;
    uint32_t bg32;
    uint32_t fg32;
    uint32_t r;
    uint32_t g;
    uint32_t b;

    if (mix >= 255) {
        return fg;
    }
    if (mix == 0) {
        return bg;
    }

    mix5 = ((uint32_t)mix + 4U) >> 3;
    bg32 = bg;
    fg32 = fg;
    r = ((bg32 & 0xF800U) + (((fg32 & 0xF800U) - (bg32 & 0xF800U)) * mix5 >> 5))
        & 0xF800U;
    g = ((bg32 & 0x07E0U) + (((fg32 & 0x07E0U) - (bg32 & 0x07E0U)) * mix5 >> 5))
        & 0x07E0U;
    b = ((bg32 & 0x001FU) + (((fg32 & 0x001FU) - (bg32 & 0x001FU)) * mix5 >> 5))
        & 0x001FU;
    return (uint16_t)(r | g | b);
}

/**
 * @brief 保守判断：这个画布坐标包围盒是否**可能**写到任何像素。
 *
 * 所有写路径（put_pixel / blend_pixel / blend_span / cover_span_fp）都同时受
 * 画布边界与裁剪矩形限制，所以"盒与（画布 ∩ 裁剪）不相交"⇒ 一个像素都不会写。
 * 于是完全在画布外、或完全在脏区外的**图元可以整条跳过**，连投影后的简化、
 * 描边、圆盘都不必做 —— 这是实测里 77% 那一块（每像素迭代/覆盖计算）的直接来源。
 *
 * 判据必须**保守**：盒要按描边半宽、AA 余量外扩后再比，宁可多画也不能漏画。
 * 这是纯前置闸门，不改变任何像素。
 */
static bool box_may_write(const vmap_canvas_t * c, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    uint8_t i;

    if (!c || x2 < 0 || y2 < 0 || x1 > c->w - 1 || y1 > c->h - 1) {
        return false;
    }
    if (c->clip_n == 0u) {
        return true;
    }

    /* 裁剪矩形是半开区间 [clip_x1, clip_x2)，且加入时已夹到画布内。 */
    for (i = 0u; i < c->clip_n; i++) {
        if (x2 >= c->clip_x1[i] && x1 < c->clip_x2[i]
            && y2 >= c->clip_y1[i] && y1 < c->clip_y2[i]) {
            return true;
        }
    }
    return false;
}

/** @brief 单像素混合（边界与裁剪已在调用方处理完毕）。 */
static inline void blend_row_pixel(uint16_t * row, int32_t x, uint16_t color,
    uint8_t alpha)
{
    if (alpha == 0u) {
        return;
    }
    if (alpha >= 255u) {
        row[x] = color;
        STAT_STORE(2);
        return;
    }
    row[x] = mix_rgb565(row[x], color, alpha);
    STAT_STORE(2);
}

/** @brief 对 [x0,x1] 内的像素按覆盖度落笔（覆盖 <=0 的自动跳过）。 */
static void cover_partial_px(uint16_t * row, int32_t x0, int32_t x1,
    int32_t x0_fp, int32_t x1_fp, uint16_t color)
{
    int32_t x;

    for (x = x0; x <= x1; x++) {
        const int32_t px0_fp = x << VMAP_FP_SHIFT;
        const int32_t px1_fp = px0_fp + VMAP_FP_ONE;
        const int32_t a_fp = x0_fp > px0_fp ? x0_fp : px0_fp;
        const int32_t b_fp = x1_fp < px1_fp ? x1_fp : px1_fp;
        const int32_t cov_fp = b_fp - a_fp;

        if (cov_fp <= 0) {
            continue;
        }
        if (cov_fp >= VMAP_FP_ONE) {
            row[x] = color;
            STAT_STORE(2);
            continue;
        }
        blend_row_pixel(row, x, color,
            (uint8_t)(((uint32_t)cov_fp * 255U) >> VMAP_FP_SHIFT));
    }
}

static void blend_pixel(vmap_canvas_t * c, int32_t x, int32_t y, uint16_t color,
    uint8_t alpha)
{
    uint16_t * row;

    if (!c || !c->buf || alpha == 0) {
        return;
    }
    if ((uint32_t)x >= (uint32_t)c->w || (uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (!canvas_in_clip(c, x, y)) {
        return;
    }
    if (alpha >= 255) {
        put_pixel(c, x, y, color);
        return;
    }

    row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
    row[x] = mix_rgb565(row[x], color, alpha);
}

typedef struct {
    int32_t x;
    int32_t y;
} vmap_fp_point_t;

static void cover_span_fp(vmap_canvas_t * c, int32_t y, int32_t x0_fp,
    int32_t x1_fp, uint16_t color)
{
    vmap_row_span_t sp[VMAP_RENDER_DIRTY_MAX];
    uint16_t * row;
    int32_t xa;
    int32_t xb;
    int32_t f_lo;
    int32_t f_hi;
    uint8_t ns;
    uint8_t s;

    if (!c || !c->buf || (uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (x0_fp > x1_fp) {
        int32_t t = x0_fp;
        x0_fp = x1_fp;
        x1_fp = t;
    }

    xa = x0_fp >> VMAP_FP_SHIFT;
    xb = (x1_fp + VMAP_FP_ONE - 1) >> VMAP_FP_SHIFT;
    if (xa < 0) {
        xa = 0;
    }
    if (xb > c->w - 1) {
        xb = c->w - 1;
    }
    if (xa > xb) {
        return;
    }

    ns = row_clip_spans(c, y, xa, xb, sp, VMAP_RENDER_DIRTY_MAX);
    if (ns == 0u) {
        return;
    }

    row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);

    /* 被整个像素覆盖的那一段用成对写；两端最多各一个部分覆盖像素走逐像素
     * 覆盖度。三段的并集与旧实现的"逐像素 cov>=1 就落笔"完全相同。 */
    f_lo = (x0_fp + VMAP_FP_ONE - 1) >> VMAP_FP_SHIFT;
    f_hi = (x1_fp - VMAP_FP_ONE) >> VMAP_FP_SHIFT;

    for (s = 0u; s < ns; s++) {
        const int32_t a = sp[s].a;
        const int32_t b = sp[s].b;
        int32_t r1 = f_lo > a ? f_lo : a;   /* 第一个被整像素覆盖的 x */
        int32_t r2 = f_hi < b ? f_hi : b;   /* 最后一个被整像素覆盖的 x */

        if (r1 > r2) {
            /* 整条跨度还不足一个像素：此时两端其实是同一个（或相邻）像素，
             * 必须当成**一段**处理，否则中间那个像素会被混合两次。 */
            cover_partial_px(row, a, b, x0_fp, x1_fp, color);
            continue;
        }
        cover_partial_px(row, a, r1 - 1, x0_fp, x1_fp, color);
        fill_run_u16(row, r1, r2, color);
        cover_partial_px(row, r2 + 1, b, x0_fp, x1_fp, color);
    }
}

static void fill_polygon_fp(vmap_canvas_t * c, const vmap_fp_point_t * pts,
    int32_t n, uint16_t color)
{
    const uint32_t gen = ++s_poly_dda_gen;
    int32_t min_y;
    int32_t max_y;
    int32_t y;
    VMAP_PRIM_T0();
    int32_t _prim_xc = 0;

    if (n < 3) {
        return;
    }
    vmap_prim_init();
    _prim_t0 = vmap_prim_cyc();

    min_y = pts[0].y >> VMAP_FP_SHIFT;
    max_y = pts[0].y >> VMAP_FP_SHIFT;
    for (int32_t i = 1; i < n; i++) {
        int32_t py = pts[i].y >> VMAP_FP_SHIFT;
        if (py < min_y) {
            min_y = py;
        }
        if (py > max_y) {
            max_y = py;
        }
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y > c->h - 1) {
        max_y = c->h - 1;
    }

    for (y = min_y; y <= max_y; y++) {
        int32_t xs_fp[64];
        int32_t cnt = 0;
        const int32_t y_scan_fp = (y << VMAP_FP_SHIFT) + (VMAP_FP_ONE / 2);

        for (int32_t i = 0; i < n; i++) {
            const int32_t j = (i == 0) ? (n - 1) : (i - 1);
            const int32_t yi_fp = pts[i].y;
            const int32_t yj_fp = pts[j].y;
            int32_t x_fp;

            if (!vmap_poly_dda_live(yi_fp, yj_fp, y_scan_fp)) {
                continue;
            }
            if (i < VMAP_POLY_DDA_CAP) {
                vmap_poly_dda_t * e = &s_poly_dda[i];

                if (e->gen != gen) {
                    const int32_t dx = pts[j].x - pts[i].x;

                    vmap_poly_dda_arm(e, yi_fp, yj_fp,
                        (int64_t)VMAP_FP_ONE * dx);
                    vmap_poly_dda_seed(e, pts[i].x, dx, yj_fp - yi_fp,
                        y_scan_fp - yi_fp);
                } else {
                    vmap_poly_dda_step(e, (yj_fp > yi_fp) ? (yj_fp - yi_fp)
                        : (yi_fp - yj_fp));
                }
                x_fp = e->x;
            } else {
                /* 超过上限的边：照旧按原式逐行算（像素不变） */
                x_fp = (int32_t)((int64_t)(y_scan_fp - yi_fp)
                    * (pts[j].x - pts[i].x) / (yj_fp - yi_fp) + pts[i].x);
            }

            _prim_xc++;
            if (cnt < (int32_t)(sizeof(xs_fp) / sizeof(xs_fp[0]))) {
                xs_fp[cnt++] = x_fp;
            }
        }

        for (int32_t a = 1; a < cnt; a++) {
            int32_t v = xs_fp[a];
            int32_t b = a - 1;

            while (b >= 0 && xs_fp[b] > v) {
                xs_fp[b + 1] = xs_fp[b];
                b--;
            }
            xs_fp[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < cnt; a += 2) {
            cover_span_fp(c, y, xs_fp[a], xs_fp[a + 1], color);
        }
    }

    s_prim_poly_fp += vmap_prim_cyc() - _prim_t0;
    s_prim_n_poly_fp++;
    s_prim_cross += (uint32_t)_prim_xc;
    s_prim_scan += (uint32_t)(n * ((max_y >= min_y) ? (max_y - min_y + 1) : 0));
    vmap_prim_note(n, vmap_prim_cyc() - _prim_t0);
}
#endif /* VMAP_RENDER_AA */

static void blend_span(vmap_canvas_t * c, int32_t x0, int32_t x1, int32_t y,
    uint16_t color)
{
    vmap_row_span_t sp[VMAP_RENDER_DIRTY_MAX];
    uint16_t * row;
    uint8_t ns;
    uint8_t s;

    if (!c || !c->buf || (uint32_t)y >= (uint32_t)c->h) {
        return;
    }
    if (x0 > x1) {
        int32_t t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (x1 > c->w - 1) {
        x1 = c->w - 1;
    }
    if (x0 > x1) {
        return;
    }

    ns = row_clip_spans(c, y, x0, x1, sp, VMAP_RENDER_DIRTY_MAX);
    if (ns == 0u) {
        return;
    }

    row = (uint16_t *)((uint8_t *)c->buf + (size_t)y * c->stride);
    for (s = 0u; s < ns; s++) {
        fill_run_u16(row, sp[s].a, sp[s].b, color);
    }
}

#if VMAP_RENDER_AA
/** @brief 行 y 的中点相对圆心的 dy（fp）。 */
static inline int32_t disc_dy_fp(int32_t y, int32_t cy_fp)
{
    return (y << VMAP_FP_SHIFT) + (VMAP_FP_ONE / 2) - cy_fp;
}

/** @brief 把 dx 收缩到满足 dx² <= v 的最大整数（v = outer2 - dy²）。 */
static inline int32_t disc_shrink_dx(int32_t dx, int64_t v)
{
    while (dx > 0 && (int64_t)dx * (int64_t)dx > v) {
        dx--;
    }
    return dx;
}

static void fill_disc_aa_fp(vmap_canvas_t * c, int32_t cx_fp, int32_t cy_fp,
    int32_t r_fp, uint16_t color)
{
    const int64_t outer2 = (int64_t)r_fp * (int64_t)r_fp;
    int32_t y0;
    int32_t y1;
    int32_t yc;
    int32_t dx0;
    int32_t dx;
    int64_t v;
    int32_t y;
    VMAP_PRIM_T0();

    if (r_fp < VMAP_FP_ONE / 4) {
        blend_pixel(c, cx_fp >> VMAP_FP_SHIFT, cy_fp >> VMAP_FP_SHIFT, color, 255);
        return;
    }
    vmap_prim_init();
    _prim_t0 = vmap_prim_cyc();

    y0 = (cy_fp - r_fp) >> VMAP_FP_SHIFT;
    y1 = (cy_fp + r_fp + VMAP_FP_ONE - 1) >> VMAP_FP_SHIFT;
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > c->h - 1) {
        y1 = c->h - 1;
    }
    if (y0 > y1) {
        return;
    }

    /*
     * 行半宽 dx(y) = floor(sqrt(outer2 - dy²)) 从"离真圆心最近的那一行"向两侧
     * 单调不增。所以只有那一行需要一次真 sqrt，其余行各自单调递减逼近即可：
     * v 是整数、double 尾数足够精确 ⇒ 原式截断就等于精确的 floor(sqrt(v))，
     * 于是结果**逐位相同**，但把每行一次**软双精度 sqrt** 全去掉了。
     * draw_road_polyline() 在每个折线顶点都画一个圆盘，这里正是它的热点。
     *
     * 起点必须取「最近的一行」（cy_fp >> 8）而不是 (cy_fp-128) >> 8：
     * 后者会落在真圆心下方一行，可能出现"起点半宽 < 下一行半宽"，
     * 那样单调逼近就会给出偏小的宽度。
     */
    yc = cy_fp >> VMAP_FP_SHIFT;
    if (yc < y0) {
        yc = y0;
    }
    if (yc > y1) {
        yc = y1;
    }

    v = outer2 - (int64_t)disc_dy_fp(yc, cy_fp) * disc_dy_fp(yc, cy_fp);
    if (v < 0) {
        return;                        /* 连最宽的一行都不覆盖 ⇒ 无像素 */
    }
    dx0 = (int32_t)sqrt((double)v);

    dx = dx0;
    for (y = yc; y <= y1; y++) {
        v = outer2 - (int64_t)disc_dy_fp(y, cy_fp) * disc_dy_fp(y, cy_fp);
        if (v < 0) {
            break;
        }
        dx = disc_shrink_dx(dx, v);
        cover_span_fp(c, y, cx_fp - dx, cx_fp + dx, color);
    }

    dx = dx0;                          /* 另一侧从圆心行的宽度重新起算 */
    for (y = yc - 1; y >= y0; y--) {
        v = outer2 - (int64_t)disc_dy_fp(y, cy_fp) * disc_dy_fp(y, cy_fp);
        if (v < 0) {
            break;
        }
        dx = disc_shrink_dx(dx, v);
        cover_span_fp(c, y, cx_fp - dx, cx_fp + dx, color);
    }

    s_prim_disc += vmap_prim_cyc() - _prim_t0;
    s_prim_n_disc++;
}

static void fill_disc_aa(vmap_canvas_t * c, int32_t cx, int32_t cy, int32_t r,
    uint16_t color)
{
    const int32_t r_fp = (r << VMAP_FP_SHIFT) + (VMAP_FP_ONE / 2);

    if (r < 1) {
        blend_pixel(c, cx, cy, color, 255);
        return;
    }

    fill_disc_aa_fp(c, cx << VMAP_FP_SHIFT, cy << VMAP_FP_SHIFT, r_fp, color);
}
#endif /* VMAP_RENDER_AA */

void vmap_render_fill_disc(vmap_canvas_t * c, int32_t cx, int32_t cy, int32_t r,
    uint16_t color)
{
    int32_t r2;

#if VMAP_RENDER_AA
    fill_disc_aa(c, cx, cy, r, color);
    return;
#endif

    if (r < 1) {
        put_pixel(c, cx, cy, color);
        return;
    }

    r2 = r * r;
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t y = cy + dy;
        int32_t yy;
        int32_t dx_max;

        if ((uint32_t)y >= (uint32_t)c->h) {
            continue;
        }

        yy = dy * dy;
        dx_max = 0;
        while (((dx_max + 1) * (dx_max + 1) + yy) <= r2) {
            dx_max++;
        }
        blend_span(c, cx - dx_max, cx + dx_max, y, color);
    }
}

static void fill_polygon(vmap_canvas_t * c, const vmap_point_t * pts, int32_t n,
    uint16_t color)
{
    const uint32_t gen = ++s_poly_dda_gen;
    int32_t min_y;
    int32_t max_y;
    int32_t y;
    VMAP_PRIM_T0();
    int32_t _prim_xc = 0;

    if (n < 3) {
        return;
    }
    vmap_prim_init();
    _prim_t0 = vmap_prim_cyc();

    min_y = pts[0].y;
    max_y = pts[0].y;
    for (int32_t i = 1; i < n; i++) {
        if (pts[i].y < min_y) {
            min_y = pts[i].y;
        }
        if (pts[i].y > max_y) {
            max_y = pts[i].y;
        }
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y > c->h - 1) {
        max_y = c->h - 1;
    }

    /* 与 fill_polygon_fp 同一套增量推进，只是这里的 x 是整像素、y 也是整行号
     * （AA 那一路按原式把 x 与 dx 都定标到 fp：x 是画布局部像素，远小于
     * int32 的 8.4M ⇒ 与原来的 int32 乘法逐位相同）。 */
    for (y = min_y; y <= max_y; y++) {
#if VMAP_RENDER_AA
        int32_t xs_fp[64];
#else
        int32_t xs[64];
#endif
        int32_t cnt = 0;

        for (int32_t i = 0; i < n; i++) {
            const int32_t j = (i == 0) ? (n - 1) : (i - 1);
            const int32_t yi = pts[i].y;
            const int32_t yj = pts[j].y;
#if VMAP_RENDER_AA
            int32_t x_fp;
#else
            int32_t x;
#endif

            if (!vmap_poly_dda_live(yi, yj, y)) {
                continue;
            }
            if (i < VMAP_POLY_DDA_CAP) {
                vmap_poly_dda_t * e = &s_poly_dda[i];

                if (e->gen != gen) {
                    const int32_t dx = pts[j].x - pts[i].x;

#if VMAP_RENDER_AA
                    /* 按原式把 x 与 dx 都定标到 fp（x 是画布局部像素，远小于
                     * int32 的 8.4M ⇒ 与原来的 int32 乘法逐位相同）。 */
                    vmap_poly_dda_arm(e, yi, yj, (int64_t)dx * VMAP_FP_ONE);
                    vmap_poly_dda_seed(e, (int64_t)pts[i].x * VMAP_FP_ONE,
                        (int64_t)dx * VMAP_FP_ONE, yj - yi, y - yi);
#else
                    vmap_poly_dda_arm(e, yi, yj, dx);
                    vmap_poly_dda_seed(e, pts[i].x, dx, yj - yi, y - yi);
#endif
                } else {
                    vmap_poly_dda_step(e, (yj > yi) ? (yj - yi) : (yi - yj));
                }
#if VMAP_RENDER_AA
                x_fp = e->x;
#else
                x = e->x;
#endif
            } else {
                /* 超过上限的边：照旧按原式逐行算（像素不变） */
                const int32_t xi = pts[i].x;
                const int32_t xj = pts[j].x;
#if VMAP_RENDER_AA
                x_fp = (int32_t)((int64_t)xi * VMAP_FP_ONE
                    + (int64_t)(y - yi) * (xj - xi) * VMAP_FP_ONE / (yj - yi));
#else
                x = xi + (int32_t)((int64_t)(y - yi) * (xj - xi) / (yj - yi));
#endif
            }

            _prim_xc++;
#if VMAP_RENDER_AA
            if (cnt < (int32_t)(sizeof(xs_fp) / sizeof(xs_fp[0]))) {
                xs_fp[cnt++] = x_fp;
            }
#else
            if (cnt < (int32_t)(sizeof(xs) / sizeof(xs[0]))) {
                xs[cnt++] = x;
            }
#endif
        }

#if VMAP_RENDER_AA
        for (int32_t a = 1; a < cnt; a++) {
            int32_t v = xs_fp[a];
            int32_t b = a - 1;

            while (b >= 0 && xs_fp[b] > v) {
                xs_fp[b + 1] = xs_fp[b];
                b--;
            }
            xs_fp[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < cnt; a += 2) {
            cover_span_fp(c, y, xs_fp[a], xs_fp[a + 1], color);
        }
#else
        for (int32_t a = 1; a < cnt; a++) {
            int32_t v = xs[a];
            int32_t b = a - 1;
            while (b >= 0 && xs[b] > v) {
                xs[b + 1] = xs[b];
                b--;
            }
            xs[b + 1] = v;
        }
        for (int32_t a = 0; a + 1 < cnt; a += 2) {
            blend_span(c, xs[a], xs[a + 1], y, color);
        }
#endif
    }

    s_prim_poly += vmap_prim_cyc() - _prim_t0;
    s_prim_n_poly++;
    s_prim_cross += (uint32_t)_prim_xc;
    s_prim_scan += (uint32_t)(n * ((max_y >= min_y) ? (max_y - min_y + 1) : 0));
    vmap_prim_note(n, vmap_prim_cyc() - _prim_t0);
}

#if VMAP_RENDER_PRIM_SELF
/* ——— 多边形定点自测（默认关；打开时开机跑一次）———————
 *
 * 为什么需要它：地图帧与帧之间的内容差异有 ~10%，而这次改动要看的效应是几个
 * 百分点 —— 拿真帧对比是"噪声盖信号"。这一组用**确定性**的多边形（固定种子、
 * 两个固件编出来逐点相同）分别压在 fp 路径与整像素路径上，报出周期与交叉点数，
 * 于是 `周期/交叉点` 这个比值可以直接在两个固件之间对减。
 *
 * 形状照搬真实负载：fp 路径是"上下各偏移半个线宽"的四边形（= draw_thick_line_aa
 * 的构造）+ 12 边形；整像素路径是随机点闭合多边形（= 面要素填充）。
 */
#define VMAP_PST_W      96
#define VMAP_PST_H      96
#define VMAP_PST_PASS   8

static uint32_t s_pst_rng;

static int32_t vmap_pst_rnd(int32_t lo, int32_t hi)
{
    s_pst_rng = s_pst_rng * 1664525u + 1013904223u;
    return lo + (int32_t)((s_pst_rng >> 9) % (uint32_t)(hi - lo + 1));
}

void vmap_render_polyselftest(vmap_render_polyself_t * out)
{
    uint16_t * fb = (uint16_t *)vmap_malloc((size_t)VMAP_PST_W * VMAP_PST_H
        * sizeof(uint16_t));
    vmap_canvas_t c;
    uint32_t pass;
    uint32_t t0;
    uint32_t c0;
    uint32_t fp_cyc;
    uint32_t fp_cross;
    uint32_t in_cyc;
    uint32_t in_cross;

    if (out == NULL) {
        return;
    }
    out->pass = VMAP_PST_PASS;
    out->fp_cyc = 0u;
    out->fp_cross = 0u;
    out->in_cyc = 0u;
    out->in_cross = 0u;
    if (fb == NULL) {
        return;
    }
    vmap_prim_init();
    for (uint32_t i = 0; i < (uint32_t)VMAP_PST_W * VMAP_PST_H; i++) {
        fb[i] = 0x1234u;
    }
    memset(&c, 0, sizeof(c));
    c.buf = fb;
    c.w = VMAP_PST_W;
    c.h = VMAP_PST_H;
    c.stride = VMAP_PST_W * 2;
    vmap_render_clip_reset(&c);
    vmap_render_clip_add(&c, 0, 0, VMAP_PST_W, VMAP_PST_H);

    /* —— fp 路径：32 条"道路带"四边形 + 8 个 12 边形 —— */
    c0 = s_prim_cross;
    t0 = vmap_prim_cyc();
    for (pass = 0; pass < VMAP_PST_PASS; pass++) {
        s_pst_rng = 0x51ed270bu;
        for (int32_t k = 0; k < 32; k++) {
            vmap_fp_point_t q[4];
            const int32_t x0 = vmap_pst_rnd(0, VMAP_PST_W * 256 - 1);
            const int32_t y0 = vmap_pst_rnd(0, VMAP_PST_H * 256 - 1);
            const int32_t x1 = vmap_pst_rnd(0, VMAP_PST_W * 256 - 1);
            const int32_t y1 = vmap_pst_rnd(0, VMAP_PST_H * 256 - 1);
            const int32_t hw = vmap_pst_rnd(1, 3) * 128;

            q[0].x = x0; q[0].y = y0 - hw;
            q[1].x = x1; q[1].y = y1 - hw;
            q[2].x = x1; q[2].y = y1 + hw;
            q[3].x = x0; q[3].y = y0 + hw;
            fill_polygon_fp(&c, q, 4, 0x07e0u);
        }
        for (int32_t k = 0; k < 8; k++) {
            vmap_fp_point_t p[12];
            const int32_t cx = vmap_pst_rnd(0, VMAP_PST_W * 256 - 1);
            const int32_t cy = vmap_pst_rnd(0, VMAP_PST_H * 256 - 1);
            const int32_t rr = vmap_pst_rnd(8, 40) * 256;

            for (int32_t i = 0; i < 12; i++) {
                p[i].x = cx + (int32_t)(((int64_t)rr * ((i * 37) % 100 - 50)) / 100);
                p[i].y = cy + (int32_t)(((int64_t)rr * ((i * 53) % 100 - 50)) / 100);
            }
            fill_polygon_fp(&c, p, 12, 0x001fu);
        }
    }
    fp_cyc = vmap_prim_cyc() - t0;
    fp_cross = s_prim_cross - c0;

    /* —— 整像素路径：8 个 16 点 + 4 个 64 点 + 2 个 256 点闭合多边形 —— */
    c0 = s_prim_cross;
    t0 = vmap_prim_cyc();
    for (pass = 0; pass < VMAP_PST_PASS; pass++) {
        static const int32_t nn[3] = { 16, 64, 256 };
        static const int32_t cnt[3] = { 8, 4, 2 };

        s_pst_rng = 0x2f1e3d4cu;
        for (int32_t b = 0; b < 3; b++) {
            for (int32_t k = 0; k < cnt[b]; k++) {
                vmap_point_t p[256];

                for (int32_t i = 0; i < nn[b]; i++) {
                    p[i].x = vmap_pst_rnd(-8, VMAP_PST_W + 8);
                    p[i].y = vmap_pst_rnd(-8, VMAP_PST_H + 8);
                }
                fill_polygon(&c, p, nn[b], 0xf800u);
            }
        }
    }
    in_cyc = vmap_prim_cyc() - t0;
    in_cross = s_prim_cross - c0;

    out->fp_cyc = fp_cyc;
    out->fp_cross = fp_cross;
    out->in_cyc = in_cyc;
    out->in_cross = in_cross;
    vmap_free(fb);
}
#endif /* VMAP_RENDER_PRIM_SELF */

#if VMAP_RENDER_AA
static void draw_thick_line_aa(vmap_canvas_t * c, int32_t x0, int32_t y0,
    int32_t x1, int32_t y1, int32_t width, uint16_t color)
{
    double dx;
    double dy;
    double len;
    double nx;
    double ny;
    double hw;
    vmap_fp_point_t quad[4];

    dx = (double)(x1 - x0);
    dy = (double)(y1 - y0);
    len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    nx = -dy / len;
    ny = dx / len;
    hw = (double)width * 0.5;

    quad[0].x = (int32_t)lround((x0 + nx * hw) * (double)VMAP_FP_ONE);
    quad[0].y = (int32_t)lround((y0 + ny * hw) * (double)VMAP_FP_ONE);
    quad[1].x = (int32_t)lround((x1 + nx * hw) * (double)VMAP_FP_ONE);
    quad[1].y = (int32_t)lround((y1 + ny * hw) * (double)VMAP_FP_ONE);
    quad[2].x = (int32_t)lround((x1 - nx * hw) * (double)VMAP_FP_ONE);
    quad[2].y = (int32_t)lround((y1 - ny * hw) * (double)VMAP_FP_ONE);
    quad[3].x = (int32_t)lround((x0 - nx * hw) * (double)VMAP_FP_ONE);
    quad[3].y = (int32_t)lround((y0 - ny * hw) * (double)VMAP_FP_ONE);
    fill_polygon_fp(c, quad, 4, color);
}
#endif /* VMAP_RENDER_AA */

void vmap_render_draw_thick_line(vmap_canvas_t * c, int32_t x0, int32_t y0, int32_t x1,
    int32_t y1, int32_t width, uint16_t color)
{
#if VMAP_RENDER_AA
    if (width < 1) {
        width = 1;
    }

    if (x0 == x1 && y0 == y1) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    draw_thick_line_aa(c, x0, y0, x1, y1, width, color);
#else
    double dx;
    double dy;
    double len;
    double nx;
    double ny;
    double hw;
    vmap_point_t quad[4];

    if (width < 1) {
        width = 1;
    }

    if (x0 == x1 && y0 == y1) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    dx = (double)(x1 - x0);
    dy = (double)(y1 - y0);
    len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) {
        vmap_render_fill_disc(c, x0, y0, (width + 1) / 2, color);
        return;
    }

    nx = -dy / len;
    ny = dx / len;
    hw = (double)width * 0.5;

    quad[0].x = (int32_t)lround(x0 + nx * hw);
    quad[0].y = (int32_t)lround(y0 + ny * hw);
    quad[1].x = (int32_t)lround(x1 + nx * hw);
    quad[1].y = (int32_t)lround(y1 + ny * hw);
    quad[2].x = (int32_t)lround(x1 - nx * hw);
    quad[2].y = (int32_t)lround(y1 - ny * hw);
    quad[3].x = (int32_t)lround(x0 - nx * hw);
    quad[3].y = (int32_t)lround(y0 - ny * hw);
    fill_polygon(c, quad, 4, color);
#endif
}

void vmap_render_fill_disc_f(vmap_canvas_t * c, double cx, double cy,
    double radius, uint16_t color)
{
#if VMAP_RENDER_AA
    int32_t r_fp;

    if (!c) {
        return;
    }
    if (radius < 0.35) {
        radius = 0.35;
    }
    r_fp = (int32_t)lround(radius * (double)VMAP_FP_ONE + (double)(VMAP_FP_ONE / 2));
    fill_disc_aa_fp(c, (int32_t)lround(cx * (double)VMAP_FP_ONE),
        (int32_t)lround(cy * (double)VMAP_FP_ONE), r_fp, color);
#else
    vmap_render_fill_disc(c, (int32_t)lround(cx), (int32_t)lround(cy),
        (int32_t)lround(radius), color);
#endif
}

void vmap_render_draw_thick_line_f(vmap_canvas_t * c, double x0, double y0,
    double x1, double y1, double width, uint16_t color)
{
#if VMAP_RENDER_AA
    double dx;
    double dy;
    double len;
    double nx;
    double ny;
    double hw;
    vmap_fp_point_t quad[4];

    if (!c) {
        return;
    }
    if (width < 1.0) {
        width = 1.0;
    }

    dx = x1 - x0;
    dy = y1 - y0;
    len = sqrt(dx * dx + dy * dy);
    if (len < 0.20) {
        vmap_render_fill_disc_f(c, x0, y0, width * 0.5, color);
        return;
    }

    nx = -dy / len;
    ny = dx / len;
    hw = width * 0.5;

    quad[0].x = (int32_t)lround((x0 + nx * hw) * (double)VMAP_FP_ONE);
    quad[0].y = (int32_t)lround((y0 + ny * hw) * (double)VMAP_FP_ONE);
    quad[1].x = (int32_t)lround((x1 + nx * hw) * (double)VMAP_FP_ONE);
    quad[1].y = (int32_t)lround((y1 + ny * hw) * (double)VMAP_FP_ONE);
    quad[2].x = (int32_t)lround((x1 - nx * hw) * (double)VMAP_FP_ONE);
    quad[2].y = (int32_t)lround((y1 - ny * hw) * (double)VMAP_FP_ONE);
    quad[3].x = (int32_t)lround((x0 - nx * hw) * (double)VMAP_FP_ONE);
    quad[3].y = (int32_t)lround((y0 - ny * hw) * (double)VMAP_FP_ONE);
    fill_polygon_fp(c, quad, 4, color);
#else
    vmap_render_draw_thick_line(c, (int32_t)lround(x0), (int32_t)lround(y0),
        (int32_t)lround(x1), (int32_t)lround(y1), (int32_t)lround(width), color);
#endif
}

#if VMAP_RENDER_AA
static void ribbon_offset_vertex(const vmap_point_t * pts, int32_t n, int32_t i,
    double hw, double * lx, double * ly, double * rx, double * ry)
{
    double n0x;
    double n0y;
    double n1x;
    double n1y;
    double mx;
    double my;
    double mlen;

    if (i == 0) {
        const double dx = (double)(pts[1].x - pts[0].x);
        const double dy = (double)(pts[1].y - pts[0].y);
        const double len = hypot(dx, dy);

        if (len < 1e-6) {
            n0x = 0.0;
            n0y = 1.0;
        } else {
            n0x = -dy / len;
            n0y = dx / len;
        }
        mx = n0x;
        my = n0y;
        mlen = hw;
    } else if (i == n - 1) {
        const double dx = (double)(pts[n - 1].x - pts[n - 2].x);
        const double dy = (double)(pts[n - 1].y - pts[n - 2].y);
        const double len = hypot(dx, dy);

        if (len < 1e-6) {
            n0x = 0.0;
            n0y = 1.0;
        } else {
            n0x = -dy / len;
            n0y = dx / len;
        }
        mx = n0x;
        my = n0y;
        mlen = hw;
    } else {
        const double dx0 = (double)(pts[i].x - pts[i - 1].x);
        const double dy0 = (double)(pts[i].y - pts[i - 1].y);
        const double dx1 = (double)(pts[i + 1].x - pts[i].x);
        const double dy1 = (double)(pts[i + 1].y - pts[i].y);
        const double len0 = hypot(dx0, dy0);
        const double len1 = hypot(dx1, dy1);
        double dot;

        if (len0 < 1e-6 || len1 < 1e-6) {
            mx = 0.0;
            my = 1.0;
            mlen = hw;
            goto out;
        }

        n0x = -dy0 / len0;
        n0y = dx0 / len0;
        n1x = -dy1 / len1;
        n1y = dx1 / len1;
        mx = n0x + n1x;
        my = n0y + n1y;
        mlen = hypot(mx, my);
        if (mlen < 1e-6) {
            mx = n0x;
            my = n0y;
            mlen = hw;
            goto out;
        }

        dot = (mx / mlen) * n0x + (my / mlen) * n0y;
        mx /= mlen;
        my /= mlen;
        if (fabs(dot) > 1e-4) {
            mlen = hw / dot;
            if (mlen > hw * 2.5) {
                mlen = hw * 2.5;
            } else if (mlen < -hw * 2.5) {
                mlen = -hw * 2.5;
            }
        } else {
            mlen = hw;
        }
    }

out:
    *lx = (double)pts[i].x + mx * mlen;
    *ly = (double)pts[i].y + my * mlen;
    *rx = (double)pts[i].x - mx * mlen;
    *ry = (double)pts[i].y - my * mlen;
}
#endif /* VMAP_RENDER_AA */

#if VMAP_RENDER_AA
static void stroke_ribbon_aa_chunk(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color, bool cap_start, bool cap_end)
{
    vmap_fp_point_t left[VMAP_RIBBON_CHUNK_MAX];
    vmap_fp_point_t right[VMAP_RIBBON_CHUNK_MAX];
    vmap_fp_point_t poly[VMAP_RIBBON_CHUNK_MAX * 2];
    const double hw = (double)width * 0.5;
    int32_t pn;
    int32_t i;

    if (!c || !pts || n < 2 || width < 1 || n > VMAP_RIBBON_CHUNK_MAX) {
        return;
    }

    for (i = 0; i < n; i++) {
        double lx;
        double ly;
        double rx;
        double ry;

        ribbon_offset_vertex(pts, n, i, hw, &lx, &ly, &rx, &ry);
        left[i].x = (int32_t)lround(lx * (double)VMAP_FP_ONE);
        left[i].y = (int32_t)lround(ly * (double)VMAP_FP_ONE);
        right[i].x = (int32_t)lround(rx * (double)VMAP_FP_ONE);
        right[i].y = (int32_t)lround(ry * (double)VMAP_FP_ONE);
    }

    pn = 0;
    for (i = 0; i < n; i++) {
        poly[pn++] = left[i];
    }
    for (i = n - 1; i >= 0; i--) {
        poly[pn++] = right[i];
    }

    fill_polygon_fp(c, poly, pn, color);

    {
        const int32_t join_r = (width + 1) / 2;

        if (cap_start) {
            vmap_render_fill_disc(c, pts[0].x, pts[0].y, join_r, color);
        }
        if (cap_end) {
            vmap_render_fill_disc(c, pts[n - 1].x, pts[n - 1].y, join_r, color);
        }
    }
}
#endif /* VMAP_RENDER_AA */

void vmap_render_stroke_ribbon(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color)
{
#if VMAP_RENDER_AA
    int32_t start;

    if (!c || !pts || n < 2 || width < 1) {
        return;
    }

    if (n <= VMAP_RIBBON_CHUNK_MAX) {
        stroke_ribbon_aa_chunk(c, pts, n, width, color, true, true);
        return;
    }

    start = 0;
    while (start < n - 1) {
        int32_t end = start + VMAP_RIBBON_CHUNK_MAX - 1;
        bool cap_start;
        bool cap_end;

        if (end >= n - 1) {
            end = n - 1;
        }

        cap_start = (start == 0);
        cap_end = (end == n - 1);
        stroke_ribbon_aa_chunk(c, pts + start, end - start + 1, width, color,
            cap_start, cap_end);

        if (end >= n - 1) {
            break;
        }

        start = end;
    }
#else
    int32_t i;

    if (!c || !pts || n < 2 || width < 1) {
        return;
    }

    for (i = 1; i < n; i++) {
        vmap_render_draw_thick_line(c, pts[i - 1].x, pts[i - 1].y,
            pts[i].x, pts[i].y, width, color);
    }

    {
        const int32_t join_r = (width + 1) / 2;
        vmap_render_fill_disc(c, pts[0].x, pts[0].y, join_r, color);
        vmap_render_fill_disc(c, pts[n - 1].x, pts[n - 1].y, join_r, color);
    }
#endif
}

static void stroke_polygon(vmap_canvas_t * c, const vmap_point_t * pts, int32_t n,
    int32_t width, uint16_t color)
{
    int32_t join_r;
    int32_t i;

    if (n < 2 || width < 1) {
        return;
    }

    join_r = (width + 1) / 2;

    for (i = 0; i < n; i++) {
        int32_t j = (i + 1) % n;
        vmap_render_draw_thick_line(c, pts[i].x, pts[i].y, pts[j].x, pts[j].y, width, color);
    }

    for (i = 0; i < n; i++) {
        vmap_render_fill_disc(c, pts[i].x, pts[i].y, join_r, color);
    }
}

static double pt_seg_dist2(double ax, double ay, double bx, double by, double cx,
    double cy)
{
    const double dx = cx - ax;
    const double dy = cy - ay;
    const double len2 = dx * dx + dy * dy;
    double t;
    double px;
    double py;
    double ex;
    double ey;

    if (len2 < 1e-6) {
        ex = bx - ax;
        ey = by - ay;
        return ex * ex + ey * ey;
    }

    t = ((bx - ax) * dx + (by - ay) * dy) / len2;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }

    px = ax + t * dx;
    py = ay + t * dy;
    ex = bx - px;
    ey = by - py;
    return ex * ex + ey * ey;
}

/**
 * @brief Douglas-Peucker 简化折线。
 * @param pts 点数组。
 * @param n 点数。
 */
int32_t vmap_render_simplify_polyline(vmap_point_t * pts, int32_t n,
    float min_seg_px, float max_dev_px)
{
    const double min_seg2 = (double)min_seg_px * (double)min_seg_px;
    const double max_dev2 = (double)max_dev_px * (double)max_dev_px;
    int32_t w;

    if (n < 3) {
        return n;
    }

    w = 1;
    for (int32_t i = 1; i < n - 1; i++) {
        const double dev2 = pt_seg_dist2((double)pts[w - 1].x, (double)pts[w - 1].y,
            (double)pts[i].x, (double)pts[i].y,
            (double)pts[i + 1].x, (double)pts[i + 1].y);
        const double dx = (double)pts[i].x - (double)pts[w - 1].x;
        const double dy = (double)pts[i].y - (double)pts[w - 1].y;
        const double seg2 = dx * dx + dy * dy;

        if (dev2 < max_dev2) {
            continue;
        }

        if (seg2 < min_seg2) {
            pts[w - 1] = pts[i];
        } else {
            pts[w++] = pts[i];
        }
    }

    {
        const double dx = (double)pts[n - 1].x - (double)pts[w - 1].x;
        const double dy = (double)pts[n - 1].y - (double)pts[w - 1].y;

        if (dx * dx + dy * dy < min_seg2 && w >= 2) {
            pts[w - 1] = pts[n - 1];
        } else {
            pts[w++] = pts[n - 1];
        }
    }

    return w;
}

#if VMAP_ROAD_SIMPLIFY
static int32_t simplify_road_polyline(vmap_point_t * pts, int32_t n,
    float min_seg_px, float max_dev_px)
{
    return vmap_render_simplify_polyline(pts, n, min_seg_px, max_dev_px);
}
#endif /* VMAP_ROAD_SIMPLIFY */

/**
 * Skip area:highway leftovers: a closed ring whose interior is much wider
 * than a schematic stroke (roundabouts stay — they are compact).
 */
static bool road_ring_is_area(const vmap_point_t * pts, int32_t n)
{
    int32_t i;
    int32_t dx;
    int32_t dy;
    double area2;
    double peri;
    double compactness;
    double mean_w;

    if (!pts || n < 5) {
        return false;
    }

    dx = pts[0].x - pts[n - 1].x;
    dy = pts[0].y - pts[n - 1].y;
    if (dx * dx + dy * dy > 16) {
        return false;
    }

    area2 = 0.0;
    peri = 0.0;
    for (i = 0; i < n - 1; i++) {
        area2 += (double)pts[i].x * (double)pts[i + 1].y
            - (double)pts[i + 1].x * (double)pts[i].y;
        peri += hypot((double)(pts[i + 1].x - pts[i].x),
            (double)(pts[i + 1].y - pts[i].y));
    }

    area2 = fabs(area2);
    if (peri < 8.0) {
        return false;
    }

    compactness = (2.0 * M_PI * area2) / (peri * peri);
    mean_w = area2 / peri;
    if (compactness > 0.55) {
        return false;
    }

    return mean_w > 10.0 && compactness < 0.45;
}

static void draw_road_polyline(vmap_canvas_t * c, const vmap_point_t * pts,
    int32_t n, int32_t width, uint16_t color)
{
    const int32_t join_r = (width + 1) / 2;
    int32_t i;

    if (!c || n < 2 || width < 1) {
        return;
    }

    if (road_ring_is_area(pts, n)) {
        return;
    }

    /* 早退：点已经是画布坐标，包围盒几乎免费。 */
    {
        int32_t bx1 = pts[0].x;
        int32_t by1 = pts[0].y;
        int32_t bx2 = pts[0].x;
        int32_t by2 = pts[0].y;

        for (i = 1; i < n; i++) {
            if (pts[i].x < bx1) { bx1 = pts[i].x; }
            if (pts[i].x > bx2) { bx2 = pts[i].x; }
            if (pts[i].y < by1) { by1 = pts[i].y; }
            if (pts[i].y > by2) { by2 = pts[i].y; }
        }
        if (!box_may_write(c, bx1 - join_r - 2, by1 - join_r - 2,
                bx2 + join_r + 2, by2 + join_r + 2)) {
            return;
        }
    }

    vmap_render_stroke_ribbon(c, pts, n, width, color);

    for (i = 0; i < n; i++) {
        vmap_render_fill_disc(c, pts[i].x, pts[i].y, join_r, color);
    }
}

static void draw_road_feature_line(vmap_canvas_t * c, const vmap_feature_t * f,
    float base_x, float base_y, float u, int32_t width, uint16_t color,
    vmap_point_t * scratch, int scratch_cap)
{
    int32_t n;
    int32_t i;

    if (!f || f->point_count < 2 || width < 1 || !scratch || scratch_cap < 2) {
        return;
    }

    n = f->point_count;
    if (n > scratch_cap) {
        n = scratch_cap;
    }

    for (i = 0; i < n; i++) {
        scratch[i].x = (int32_t)lroundf(base_x + vmap_feature_x(f, (uint16_t)i) * u);
        scratch[i].y = (int32_t)lroundf(base_y + vmap_feature_y(f, (uint16_t)i) * u);
    }

    /* 早退：完全写不到就不必简化、不必描边、不必点圆盘。外扩 = 描边半宽 + AA 余量
     * （保守方向），且必须在 VMAP_ROAD_SIMPLIFY 之前 —— 那一步也是纯浪费。 */
    {
        const int32_t pad = (width + 1) / 2 + 2;
        int32_t bx1 = scratch[0].x;
        int32_t by1 = scratch[0].y;
        int32_t bx2 = scratch[0].x;
        int32_t by2 = scratch[0].y;

        for (i = 1; i < n; i++) {
            if (scratch[i].x < bx1) { bx1 = scratch[i].x; }
            if (scratch[i].x > bx2) { bx2 = scratch[i].x; }
            if (scratch[i].y < by1) { by1 = scratch[i].y; }
            if (scratch[i].y > by2) { by2 = scratch[i].y; }
        }
        if (!box_may_write(c, bx1 - pad, by1 - pad, bx2 + pad, by2 + pad)) {
            return;
        }
    }

#if VMAP_ROAD_SIMPLIFY
    n = simplify_road_polyline(scratch, n, VMAP_ROAD_MIN_SEG_PX,
        VMAP_ROAD_MAX_DEV_PX);
    if (n >= 4) {
        n = simplify_road_polyline(scratch, n, VMAP_ROAD_MIN_SEG_PX,
            VMAP_ROAD_MAX_DEV_PX);
    }
#endif

    draw_road_polyline(c, scratch, n, width, color);
}

static void draw_feature_line(vmap_canvas_t * c, const vmap_feature_t * f,
    float base_x, float base_y, float u, int32_t width, uint16_t color)
{
    int32_t join_r;
    int32_t px;
    int32_t py;
    uint16_t i;

    if (f->point_count < 2 || width < 1) {
        return;
    }

    join_r = (width + 1) / 2;

    /* 早退：先只做一遍投影求包围盒（外扩半宽 + AA 余量），完全写不到就整条跳过，
     * 连下面两遍（铺线 + 每顶点圆盘）都省掉。 */
    {
        int32_t bx1 = 0;
        int32_t by1 = 0;
        int32_t bx2 = 0;
        int32_t by2 = 0;

        for (i = 0; i < f->point_count; i++) {
            int32_t x = (int32_t)(base_x + vmap_feature_x(f, i) * u);
            int32_t y = (int32_t)(base_y + vmap_feature_y(f, i) * u);

            if (i == 0u) {
                bx1 = bx2 = x;
                by1 = by2 = y;
            } else {
                if (x < bx1) { bx1 = x; }
                if (x > bx2) { bx2 = x; }
                if (y < by1) { by1 = y; }
                if (y > by2) { by2 = y; }
            }
        }
        if (!box_may_write(c, bx1 - join_r - 2, by1 - join_r - 2,
                bx2 + join_r + 2, by2 + join_r + 2)) {
            return;
        }
    }

    px = (int32_t)(base_x + vmap_feature_x(f, 0) * u);
    py = (int32_t)(base_y + vmap_feature_y(f, 0) * u);

    for (i = 1; i < f->point_count; i++) {
        int32_t nx = (int32_t)(base_x + vmap_feature_x(f, i) * u);
        int32_t ny = (int32_t)(base_y + vmap_feature_y(f, i) * u);
        vmap_render_draw_thick_line(c, px, py, nx, ny, width, color);
        px = nx;
        py = ny;
    }

    for (i = 0; i < f->point_count; i++) {
        int32_t x = (int32_t)(base_x + vmap_feature_x(f, i) * u);
        int32_t y = (int32_t)(base_y + vmap_feature_y(f, i) * u);
        vmap_render_fill_disc(c, x, y, join_r, color);
    }
}

static uint16_t land_fill_color(const vmap_style_t * style, uint8_t attr)
{
    switch (attr) {
    case VMAP_LAND_COMMERCIAL:
    case VMAP_LAND_INDUSTRIAL:
        return RGB565(style->land_com_r, style->land_com_g, style->land_com_b);
    case VMAP_LAND_EDU:
        return RGB565(style->land_edu_r, style->land_edu_g, style->land_edu_b);
    case VMAP_LAND_PARK:
        return RGB565(style->land_park_r, style->land_park_g, style->land_park_b);
    default:
        return RGB565(style->land_res_r, style->land_res_g, style->land_res_b);
    }
}

static void draw_land_layer(vmap_canvas_t * c, vmap_feature_iter_t it,
    uint16_t extent, float base_x, float base_y, float scale,
    vmap_point_t * scratch, int scratch_cap)
{
    const vmap_style_t * style = vmap_style_current();
    float u = 256.0f * scale / (float)extent;
    vmap_feature_t f;

    while (vmap_feature_iter_next(&it, &f)) {
        int32_t n;
        int32_t i;

        if (!vmap_feature_is_closed(&f) || f.point_count < 3) {
            continue;
        }
        n = f.point_count;
        if (n > scratch_cap) {
            n = scratch_cap;
        }
        {
            /* 顺带算包围盒（不额外投影），完全写不到就整条跳过 fill_polygon。 */
            int32_t bx1 = 0;
            int32_t by1 = 0;
            int32_t bx2 = 0;
            int32_t by2 = 0;

            for (i = 0; i < n; i++) {
                int32_t x = (int32_t)(base_x + vmap_feature_x(&f, (uint16_t)i) * u);
                int32_t y = (int32_t)(base_y + vmap_feature_y(&f, (uint16_t)i) * u);

                scratch[i].x = x;
                scratch[i].y = y;
                if (i == 0) {
                    bx1 = bx2 = x;
                    by1 = by2 = y;
                } else {
                    if (x < bx1) { bx1 = x; }
                    if (x > bx2) { bx2 = x; }
                    if (y < by1) { by1 = y; }
                    if (y > by2) { by2 = y; }
                }
            }
            if (!box_may_write(c, bx1 - 2, by1 - 2, bx2 + 2, by2 + 2)) {
                continue;
            }
        }
        fill_polygon(c, scratch, n, land_fill_color(style, f.attr));
    }
}

static void draw_poly_layer(vmap_canvas_t * c, vmap_feature_iter_t it,
    uint16_t extent, float base_x, float base_y, float scale, uint16_t fill_color,
    uint16_t stroke_color, int32_t stroke_w, vmap_point_t * scratch, int scratch_cap)
{
    float u = 256.0f * scale / (float)extent;
    vmap_feature_t f;

    while (vmap_feature_iter_next(&it, &f)) {
        int32_t n;
        int32_t i;

        if (!vmap_feature_is_closed(&f) || f.point_count < 3) {
            continue;
        }
        n = f.point_count;
        if (n > scratch_cap) {
            n = scratch_cap;
        }
        {
            /* 顺带算包围盒（不额外投影）；填充与描边共用同一道闸门，取较大的外扩量
             * （描边半宽 + AA 余量）—— 宁可多画。 */
            const int32_t pad = (stroke_w > 0 ? (stroke_w + 1) / 2 : 0) + 2;
            int32_t bx1 = 0;
            int32_t by1 = 0;
            int32_t bx2 = 0;
            int32_t by2 = 0;

            for (i = 0; i < n; i++) {
                int32_t x = (int32_t)(base_x + vmap_feature_x(&f, (uint16_t)i) * u);
                int32_t y = (int32_t)(base_y + vmap_feature_y(&f, (uint16_t)i) * u);

                scratch[i].x = x;
                scratch[i].y = y;
                if (i == 0) {
                    bx1 = bx2 = x;
                    by1 = by2 = y;
                } else {
                    if (x < bx1) { bx1 = x; }
                    if (x > bx2) { bx2 = x; }
                    if (y < by1) { by1 = y; }
                    if (y > by2) { by2 = y; }
                }
            }
            if (!box_may_write(c, bx1 - pad, by1 - pad, bx2 + pad, by2 + pad)) {
                continue;
            }
        }
        fill_polygon(c, scratch, n, fill_color);
        if (stroke_w > 0) {
            stroke_polygon(c, scratch, n, stroke_w, stroke_color);
        }
    }
}

static void draw_road_layer(vmap_canvas_t * c, const vmap_tile_t * tile,
    uint16_t extent, float base_x, float base_y, float scale,
    vmap_point_t * scratch, int scratch_cap)
{
    const vmap_style_t * style = vmap_style_current();
    float u = 256.0f * scale / (float)extent;
    int cls;
    const uint16_t case_color = RGB565(style->road_case_r, style->road_case_g,
        style->road_case_b);

    for (cls = VMAP_ROAD_PATH; cls >= VMAP_ROAD_MOTORWAY; cls--) {
        vmap_feature_iter_t it = vmap_tile_layer_iter(tile, VMAP_LAYER_ROAD);
        vmap_feature_t f;
        while (vmap_feature_iter_next(&it, &f)) {
            int raw;
            const vmap_style_road_t * st;
            int32_t case_w;

            if (f.point_count < 2 || vmap_feature_is_closed(&f)) {
                continue;
            }
            raw = f.attr;
            if (raw < 1 || raw >= ROAD_STYLE_NUM) {
                raw = VMAP_ROAD_RESIDENTIAL;
            }
            if (raw != cls) {
                continue;
            }
            st = road_style_for(raw);
            case_w = (int32_t)st->w + (int32_t)style->road_case_extra;
            if (case_w > (int32_t)st->w) {
                draw_road_feature_line(c, &f, base_x, base_y, u, case_w, case_color,
                    scratch, scratch_cap);
            }
        }
    }

    for (cls = VMAP_ROAD_PATH; cls >= VMAP_ROAD_MOTORWAY; cls--) {
        vmap_feature_iter_t it = vmap_tile_layer_iter(tile, VMAP_LAYER_ROAD);
        vmap_feature_t f;
        while (vmap_feature_iter_next(&it, &f)) {
            int raw;
            const vmap_style_road_t * st;
            uint16_t color;

            if (f.point_count < 2 || vmap_feature_is_closed(&f)) {
                continue;
            }
            raw = f.attr;
            if (raw < 1 || raw >= ROAD_STYLE_NUM) {
                raw = VMAP_ROAD_RESIDENTIAL;
            }
            if (raw != cls) {
                continue;
            }
            st = road_style_for(raw);
            color = RGB565(st->r, st->g, st->b);
            draw_road_feature_line(c, &f, base_x, base_y, u, (int32_t)st->w, color,
                scratch, scratch_cap);
        }
    }
}

static void draw_line_layer(vmap_canvas_t * c, vmap_feature_iter_t it,
    uint16_t extent, float base_x, float base_y, float scale)
{
    const vmap_style_t * style = vmap_style_current();
    float u = 256.0f * scale / (float)extent;
    const uint16_t color = RGB565(style->waterway_r, style->waterway_g,
        style->waterway_b);
    vmap_feature_t f;

    while (vmap_feature_iter_next(&it, &f)) {
        if (f.point_count < 2) {
            continue;
        }
        draw_feature_line(c, &f, base_x, base_y, u, (int32_t)style->waterway_w,
            color);
    }
}

/**
 * @brief 用纯色清画布。
 * @param c 画布。
 * @param color RGB565。
 */
void vmap_render_clear(vmap_canvas_t * c, uint16_t color)
{
    vmap_blit_surface_t surf;

    if (!c || !c->buf) {
        return;
    }

    /* 先问搬运后端（走 EPIC 时是一整块矩形一条命令）；它不可用或没通过自检
     * 时，同一个调用会把 CPU 版本做完 —— 见 vmap_blit.h。 */
    render_surface(c, &surf);
    vmap_blit_fill(render_blit(), &surf, 0, 0, c->w, c->h, color);
}

/*
 * 这里原有一个 `render_fill_span(row, x0, x1, color)`（成对写的一行填充）。
 * scroll 改成"矩形 + 两条边"之后它没有调用者了 —— 补边现在走
 * `vmap_blit_fill()`，CPU 那支的成对写等价物在 `vmap_blit.c` 的 `cpu_fill()`。
 */

void vmap_render_scroll(vmap_canvas_t * c, int32_t dx, int32_t dy, uint16_t fill)
{
    vmap_blit_surface_t surf;
    vmap_blit_t * blit;
    int32_t copy_w;
    int32_t copy_h;
    int32_t x0;
    int32_t y0;

    if (!c || !c->buf) {
        return;
    }

    if (dx == 0 && dy == 0) {
        return;
    }

    if (dx >= c->w || dx <= -c->w || dy >= c->h || dy <= -c->h) {
        vmap_render_clear(c, fill);
        return;
    }

    /*
     * 原来这条是逐行 memmove + 逐行补边；现在拆成**一块矩形 + 两条边**，
     * 这样整块矩形才有机会落到 EPIC 上（一条命令 vs 472 次函数调用）。
     * 逐行版的代价是实打实的：使用过程一帧的 `cyc_begin` 里 92% 就是它
     * （见 `[vperf] frame ... scr=`），单帧 164 ms。
     *
     * 语义与逐行版逐位相同：
     *   拷贝区 = 目的 (x0,y0) ← 源 (x0+dx, y0+dy)，尺寸 (w−|dx|, h−|dy|)；
     *   横向露出的 |dx| 列只覆盖**拷贝区那几行**，纵向露出的 |dy| 行整宽铺满。
     * 次序必须是**先拷贝后填充**：露出的边落在源矩形的范围内（横向边在
     * y 属于拷贝区时、纵向边在 x 属于拷贝区时），先填会把还没读的源像素盖掉。
     * 逐行版之所以没这个问题，是因为它在同一行里也是先 memmove 后补边、
     * 且行的方向按 dy 的符号选过（跨行的重叠只有那个方向是安全的）。
     */
    copy_w = c->w - (dx >= 0 ? dx : -dx);
    copy_h = c->h - (dy >= 0 ? dy : -dy);
    x0 = (dx > 0) ? 0 : -dx;
    y0 = (dy > 0) ? 0 : -dy;

    render_surface(c, &surf);
    blit = render_blit();

    (void)vmap_blit_copy(blit, &surf, x0, y0, &surf, x0 + dx, y0 + dy,
        copy_w, copy_h);

    if (dx > 0) {
        (void)vmap_blit_fill(blit, &surf, c->w - dx, y0, dx, copy_h, fill);
    } else if (dx < 0) {
        (void)vmap_blit_fill(blit, &surf, 0, y0, -dx, copy_h, fill);
    }

    if (dy > 0) {
        (void)vmap_blit_fill(blit, &surf, 0, c->h - dy, c->w, dy, fill);
    } else if (dy < 0) {
        (void)vmap_blit_fill(blit, &surf, 0, 0, c->w, -dy, fill);
    }
}

static void fill_rect(vmap_canvas_t * c, int32_t x1, int32_t y1, int32_t x2,
    int32_t y2, uint16_t color)
{
    int32_t y;

    if (!c || x1 >= x2 || y1 >= y2) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > c->w) {
        x2 = c->w;
    }
    if (y2 > c->h) {
        y2 = c->h;
    }
    if (x1 >= x2 || y1 >= y2) {
        return;
    }

    for (y = y1; y < y2; y++) {
        blend_span(c, x1, x2 - 1, y, color);
    }
}

/**
 * @brief 将瓦片绘制到画布。
 */
void vmap_render_draw_tile(vmap_canvas_t * c, const vmap_tile_t * tile,
    float base_x, float base_y, float scale, vmap_point_t * scratch, int scratch_cap)
{
    const vmap_style_t * style;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;

    if (!c || !tile) {
        return;
    }

    /* Vector layers do not cover every pixel. Track/route ribbons are stamped
     * on top; without a bg fill, restamping leaves those pixels behind. */
    x1 = (int32_t)floorf(base_x);
    y1 = (int32_t)floorf(base_y);
    x2 = (int32_t)ceilf(base_x + 256.0f * scale);
    y2 = (int32_t)ceilf(base_y + 256.0f * scale);
    fill_rect(c, x1, y1, x2, y2, vmap_style_bg565());

    style = vmap_style_current();
    uint16_t extent = vmap_tile_extent(tile);
    const uint16_t forest = RGB565(style->forest_r, style->forest_g,
        style->forest_b);
    const uint16_t water = RGB565(style->water_r, style->water_g, style->water_b);

    draw_land_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_LAND), extent,
        base_x, base_y, scale, scratch, scratch_cap);
    draw_poly_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_FOREST), extent,
        base_x, base_y, scale, forest, 0, 0, scratch, scratch_cap);
    draw_poly_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_WATER), extent,
        base_x, base_y, scale, water, 0, 0, scratch, scratch_cap);
    draw_line_layer(c, vmap_tile_layer_iter(tile, VMAP_LAYER_WATERWAY), extent,
        base_x, base_y, scale);
    draw_road_layer(c, tile, extent, base_x, base_y, scale, scratch, scratch_cap);
}
