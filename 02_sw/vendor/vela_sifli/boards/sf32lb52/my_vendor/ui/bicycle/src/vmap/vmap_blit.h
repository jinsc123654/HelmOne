/**
 * @file vmap_blit.h
 * @brief 画布搬运后端抽象：把"填充 / 二维拷贝"从 CPU 换成硬件搬运。
 *
 * 起因（实测，见 docs/map/perf/MEASUREMENTS.md）：
 *   - 使用过程一帧 762 ms 里，`vmap_render_scroll()` 一个函数就占 **164 ms**
 *     （468 KB 的读 + 写），是 `render_begin` 的 92%；
 *   - PSRAM 写通路是 write-through、**每次 4 字节 store 就是一次总线事务**
 *     （实测 88 周期/次，块写 `blk/w=100%` 说明宽写也被拆开）。
 * 所以真正的问题不是"搬多少字节"，而是"**谁在发事务**"：CPU 的 store 路径一次
 * 只发 4 字节，而 DMA 主设备可以自己排事务。这一层就是为了验证并利用这一点。
 *
 * **为什么做成结构体 + 函数**：换后端（DMAC ↔ EPIC ↔ 纯 CPU）、换芯片都只动
 * 这一个文件；调用方只认 surface + 矩形，不认寄存器。同时自检可以直接对比
 * CPU 与硬件后端的周期数。
 *
 * 线程约定（必须遵守，否则会画错）：
 *   - **只在 UI 线程调用**。所有渲染路径（`vmap_view_render*`）的调用点都在
 *     `map_page.c` 的 LVGL 定时器里，与 LVGL 自身的绘制同一线程；EPIC 的
 *     绘制线程（`CONFIG_LV_USE_SIFLI_EPIC_DRAW_THREAD`）没有打开。
 *   - **返回前必须等到完成**（`vmap_blit_wait`）。不能把未完成的作业留给
 *     LVGL 接下来的绘制阶段 —— 那时画布会处于半搬运状态。
 */

#ifndef VMAP_BLIT_H
#define VMAP_BLIT_H

/**
 * @brief EPIC（GPU）搬运总开关。**默认 1 = 开**（用户 2026-09-27 定；09-26 曾置 0）。
 *
 * @details 置 1：先跑九方向开机自检（与 CPU 逐位比对），通过才用 EPIC；置 0：后端恒为
 *          CPU、自检不跑（方向表不填 ⇒ 一切走 CPU）、`[vblit]` 只留一行"已关闭"的说明。
 *
 * @note 为什么 09-26 关过：09-25 上这条路径与三次 WDT 饥饿复位在时间上无法排除关系，而
 *       当时的完成等待是**无界忙等**（`while (STATUS != 0x0);`）。隔离它让"少一个 GPU
 *       参与"成为对比基线。
 * @note 为什么现在能开回来：① 等待已**有界化**（`EPIC_WaitDone()` 超时即把本次当失败，
 *       上层 CPU 兜底会把这一块重做 —— 不卡死、也不在半搬完的画布上留错像素）；
 *       ② 实测同负载下 `cyc_begin` 再降 61%，且 `cyc_tile`、UI 阻塞计数、WDT 家族都没恶化。
 *
 * @warning ⚠ **一致性前提（开之前必读）**：EPIC 是绕过 CPU 缓存直接写内存的 master，而
 *          PSRAM 现在默认写回（`CONFIG_MYVENDOR_PSRAM_CACHE_WB=y`）⇒ EPIC 写过的像素在
 *          CPU 缓存里可能是旧行，必须由
 *          `myvendor_epic_blit.c::epic_invalidate_rect_rows()`（按**被写到的行**逐行作废，
 *          不整层作废，否则会丢掉矩形外 CPU 的脏行）兜住。**两件事要么都开、要么都关**；
 *          证据与缺口编号见 docs/psram_cache_wb_audit.md 的 G1。
 *
 * @warning ⚠ 这只关 **vmap 自己的搬运**；LVGL 自身的 EPIC 合成另有独立开关
 *          `CONFIG_LV_USE_SIFLI_EPIC`（横幅半透、箭头旋转、画布→条带合成走那条）。
 */
#ifndef MYVENDOR_BLIT_EPIC
#define MYVENDOR_BLIT_EPIC 1
#endif

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 搬运后端。
 *
 * 目前实装的是 **EPIC**（2D blitter，一条命令一整块矩形）+ CPU 兜底。
 * DMAC 的 mem2mem 仍是空位：它要逐行发（stride ≠ 矩形宽）且重叠方向得自己挑，
 * 而"搬运该不该交给硬件"这个判定 EPIC 用更少的命令就能问出来 —— 若 EPIC 的
 * 吞吐都不比 CPU 好（PSRAM 写通路对谁都是每次 4 字节事务），DMAC 也一样。
 */
typedef enum {
    VMAP_BLIT_CPU = 0,   /**< 纯 CPU 逐行（永远可用的兜底）。 */
    VMAP_BLIT_DMAC,      /**< 通用 DMA 的 memory-to-memory（未实装）。 */
    VMAP_BLIT_EPIC       /**< EPIC 2D blitter（`HAL_EPIC_BlendStartEx`/`FillStart`）。 */
} vmap_blit_backend_t;

/** @brief 一块可搬运的二维表面（RGB565）。 */
typedef struct {
    uint16_t * buf;      /**< 左上角起点。 */
    int32_t  w;          /**< 宽（像素），用于边界判断。 */
    int32_t  h;          /**< 高（像素）。 */
    uint32_t stride;     /**< 行字节数。 */
} vmap_blit_surface_t;

typedef struct vmap_blit_s vmap_blit_t;

/**
 * @brief 建立搬运器：探测可用后端。
 * @return 句柄；始终成功（最差退化成 CPU 后端）——返回 NULL 只可能是内存不足。
 */
vmap_blit_t * vmap_blit_create(void);
void vmap_blit_destroy(vmap_blit_t * b);

/**
 * @brief 取全进程共享的句柄（EPIC 是独占硬件，只应有一个），首次调用时建立。
 *
 * **寿命是进程级，刻意不跟 livemap 页走**：句柄里存着一次性自检的结论与两跳用的
 * 中转缓冲，跟着页 load/unload 会变成"每次进地图重跑自检 + 重分配缓冲"。
 * 详见 `vmap_blit.c` 里 `s_shared` 的注释。
 *
 * @return 句柄；内存不足时为 NULL（调用方需容忍）。
 */
vmap_blit_t * vmap_blit_get(void);

/**
 * @brief 上板一次性自检：**同一块缓冲内源/目的重叠**的搬运，EPIC 与 CPU 的
 *        结果是否逐位相同（逐方向），并顺带量一次同尺寸的 CPU vs EPIC 吞吐。
 *
 * 只在开机跑一次（[vblit] 行）。结论固化进句柄：没通过（或压根没跑成）的方向
 * 在 `vmap_blit_copy()` 里退回 CPU —— 见 `vl_overlap_allowed()`。
 *
 * @param canvas_w 真机画布宽（自检用真机行宽，横向流水行为才对得上）。
 * @param canvas_h 真机画布高（自检缓冲高度会封顶，见 vmap_blit.c 的说明）。
 * @return true = 自检跑完了（不代表全部方向通过，看 [vblit] 日志）。
 */
bool vmap_blit_probe(int32_t canvas_w, int32_t canvas_h);

/** @brief 当前生效的后端（日志/自检用）。 */
vmap_blit_backend_t vmap_blit_backend(const vmap_blit_t * b);
/** @brief 后端名（"cpu" / "dmac" / "epic"）。 */
const char * vmap_blit_backend_name(vmap_blit_backend_t k);

/**
 * @brief 二维拷贝：把 `src` 的 (sx,sy,w,h) 搬到 `dst` 的 (dx,dy)，尺寸相同。
 *
 * 语义与 `memmove` 一致（**允许同一块缓冲区内的重叠搬运**，方向由实现保证），
 * 越界部分自动夹到各自表面内；夹完为空则什么都不做。
 *
 * @return true = 走了硬件后端（并已等完成）；false = 本次由 CPU 完成。
 */
bool vmap_blit_copy(vmap_blit_t * b,
    const vmap_blit_surface_t * dst, int32_t dx, int32_t dy,
    const vmap_blit_surface_t * src, int32_t sx, int32_t sy,
    int32_t w, int32_t h);

/**
 * @brief 填充矩形（同样是 `vmap_render_clear` 那条路的热点）。
 * @return true = 走了硬件后端（并已等完成）。
 */
bool vmap_blit_fill(vmap_blit_t * b, const vmap_blit_surface_t * dst,
    int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);

/** @brief 等到硬件完成（阻塞）；CPU 后端是空操作。 */
void vmap_blit_wait(vmap_blit_t * b);

/**
 * @brief 运行计数：拷贝分别走了"硬件单跳 / 硬件两跳 / CPU"各多少次。
 *
 * 用来把一帧的搬运耗时归到具体路径上（光看 `scr` 分不清是哪一支在跑）。任一
 * 输出可为 NULL；句柄为 NULL 时全部写 0。
 */
void vmap_blit_stats(const vmap_blit_t * b, uint32_t * one_hop,
    uint32_t * two_hop, uint32_t * cpu);

/**
 * @brief 微基准：同尺寸的 CPU 拷贝 vs 硬件拷贝，比周期数（自检用）。
 *
 * 这就是"**DMA 主设备在 PSRAM 上能不能拿到比 CPU 每次 4 字节 store 更好的
 * 吞吐**"那一次判定：`hw` 明显小于 `cpu` 才值得把 scroll/clear 换成硬件。
 * 任一输出可为 NULL；后端不可用时把 `hw` 写 0。
 */
void vmap_blit_bench(vmap_blit_t * b, uint32_t bytes,
    uint32_t * cpu_cyc, uint32_t * hw_cyc);

#ifdef __cplusplus
}
#endif

#endif /* VMAP_BLIT_H */
