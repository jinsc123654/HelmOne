/**
 * @file vmap_view.c
 * @brief 地图视图：瓦片泵 + 画布叠层（底图 → 导航线 → 标签 → REC 线 → REC 数字）。
 */

#include "vmap_view.h"

#include "board_psram_layout.h"
#include "vmap_config.h"
#include "vmap_label.h"
#include "vmap_blit.h"
#include "vmap_render.h"
#include "vmap_tile_cache.h"
#include "vmap_tile_system.h"
#include "vmap_alloc.h"
#if VMAP_TRACK_ENABLE
#include "vmap_track.h"
#endif
#if VMAP_ROUTE_ENABLE
#include "vmap_route.h"
#endif
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
#include "bicycle_inval_probe.h"
#endif
#include "bicycle_c_debug.h"
#include <myvendor_mtp_lfs.h>
#include "myvendor_devctl.h"
#include "myvendor_watchdog.h"
#include "myvendor_schedmon.h"
#include "sf32lb_dvfs.h"
#include <limits.h>
#include <math.h>
#include <nuttx/clock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define VMAP_PAN_MIN_STEP_PX  1

static void render_begin(vmap_view_t * view, bool reset_view, bool force_clear);

static void dvfs_map_busy(bool busy)
{
    if (busy) {
        sf32lb_dvfs_hold(SF32LB_DVFS_HOLD_MAP);
    } else {
        sf32lb_dvfs_release(SF32LB_DVFS_HOLD_MAP);
    }
}

/*
 * 渲染分段计时（只加观测，不改任何行为；置 0 即整体消失）。
 *
 * 时间基准用 DWT_CYCCNT，不用 tick：`CONFIG_USEC_PER_TICK=10000` 只有 10 ms
 * 分辨率，而单段常常是亚毫秒级。DWT 的解锁/自检写法照抄 services/myvendor_gnss.c
 * （Armv8-M 的 DWT 是锁访问的，两个 LAR 都写；CYCCNT 是全系统共享的，
 * **不要清零**，chips/sf32lb52/myvendor_idle_stat.c 正在用它统计 WFI 占比）。
 *
 * ⚠ **一律只报原始周期数，不在这里换算成微秒。** `SystemCoreClock` 是被
 * `sifli_systick_reclock()` 按 SysTick 基准赋的值（实测 24 MHz），**不等于 CPU
 * 核心频率**，拿它换算会差一个未知倍数。所以每帧额外打一个 `tickd`（本帧跨了
 * 几个 10 ms tick，由 `clock_systime_ticks()` 得到）：帧内 CPU 是忙的，
 * `cyc_tot / (tickd × 10 ms)` 就是 CYCCNT 的真实频率，可以据此把全部数字反解成
 * 微秒，且不依赖任何时钟树假设。`hz` 只作为 SystemCoreClock 的对照值保留。
 */
#define VMAP_PERF_PROBE 1

/** 多边形定点自测（'[vperf] polyself'）—— 默认关：一次 ~0.5 s。 */
#ifndef VMAP_RENDER_PRIM_SELF
#define VMAP_RENDER_PRIM_SELF 0
#endif

#if VMAP_PERF_PROBE
#define VMAP_PERF_DWT_DEMCR   (*(volatile uint32_t *)0xe000edfcu)
#define VMAP_PERF_DWT_CTRL    (*(volatile uint32_t *)0xe0001000u)
#define VMAP_PERF_DWT_CYCCNT  (*(volatile uint32_t *)0xe0001004u)
#define VMAP_PERF_DWT_LAR     (*(volatile uint32_t *)0xe0001fb0u)
#define VMAP_PERF_ITM_LAR     (*(volatile uint32_t *)0xe0000fb0u)
#define VMAP_PERF_LAR_KEY     0xc5acce55u
/** @brief 同一标签最快多久打一行。 */
#define VMAP_PERF_LOG_GAP_MS  2000u
/** @brief 单拍泵调用间隔超过它 ⇒ **无条件** WARNING（见 SLOW gap 的注释）。 */
#define VMAP_PERF_SLOW_GAP_MS   500u
/** @brief 单帧超过它 ⇒ **无条件** WARNING。 */
#define VMAP_PERF_SLOW_FRAME_MS 3000u

/** SoC 缓存控制器（见 chips/drivers/cmsis/sf32lb52x/cache.h）。 */
#define VMAP_PERF_CACHE_CCR    (*(volatile uint32_t *)0xe0080000u)
#define VMAP_PERF_CACHE_IMCR   (*(volatile uint32_t *)0xe0080004u)
#define VMAP_PERF_CACHE_IACR   (*(volatile uint32_t *)0xe0080008u)
#define VMAP_PERF_CACHE_DMCR   (*(volatile uint32_t *)0xe0080010u)
#define VMAP_PERF_CACHE_DACR   (*(volatile uint32_t *)0xe0080014u)
#define VMAP_PERF_CACHE_CNTEN  (1u << 0)
#define VMAP_PERF_CACHE_CNTCLR (1u << 1)

extern uint32_t SystemCoreClock;

static bool s_pf_ready;
static bool s_pf_usable;
static bool s_pf_in_frame;
static uint32_t s_pf_t0;
static clock_t s_pf_tick0;
static uint32_t s_pf_begin;
static uint32_t s_pf_warm;
static uint32_t s_pf_tile;
static uint32_t s_pf_nav;
static uint32_t s_pf_label;
static uint32_t s_pf_track;
/* stamp_overlays 也可能在没有帧的情况下被调（导航进度刷新），单独记一份。 */
static uint32_t s_pf_s_nav;
static uint32_t s_pf_s_track;
static uint32_t s_pf_n_tiles;
static uint32_t s_pf_n_warms;
static uint32_t s_pf_n_labels;
static uint32_t s_pf_last_log;
static uint32_t s_pf_last_stamp;
/** @brief 上一次泵调用的时间戳（判"UI 线程被占住多久"）。 */
static uint32_t s_pf_last_pump_cyc;
/* 0b：SoC 缓存控制器的命中/miss 计数（按帧取增量）。 */
static uint32_t s_pf_iacc0;
static uint32_t s_pf_imiss0;
static uint32_t s_pf_dacc0;
static uint32_t s_pf_dmiss0;
/* 0a：本帧瓦片光栅化发出的 store 次数与字节数。 */
static uint32_t s_pf_stores;
static uint32_t s_pf_bytes;
/* 使用过程里最大的两块：render_begin 内部（scroll/label-scroll/invalidate）
 * 与泵里"非脏瓦片"那一支（cache 查找 + 解析 + 标签候选收集）。 */
static uint32_t s_pf_scroll;
static uint32_t s_pf_lscroll;
static uint32_t s_pf_inval;
static uint32_t s_pf_meta;
/* 搬运路径计数（单跳 / 两跳 / CPU）的本帧增量 —— 把 scr 归到具体路径上。 */
static uint32_t s_pf_hop1;
static uint32_t s_pf_hop2;
static uint32_t s_pf_cpuc;
static uint32_t s_pf_hop0[3];   /**< 帧起点的三个计数。 */
/* 【临时取证】图元级拆分：帧起点快照 + 本帧增量（见 vmap_render.c 的 PRIM 块）。 */
static vmap_render_prim_t s_prim0;
static vmap_render_prim_t s_prim;

static void vmap_perf_psram_probe(void);
static void vmap_perf_dump_runnable(void);
static void vmap_perf_cache_probe(void);

static void vmap_perf_init_once(void)
{
    uint32_t a;
    uint32_t b;
    int i;

    if (s_pf_ready) {
        return;
    }
    s_pf_ready = true;

    VMAP_PERF_ITM_LAR = VMAP_PERF_LAR_KEY;
    VMAP_PERF_DWT_LAR = VMAP_PERF_LAR_KEY;
    VMAP_PERF_DWT_DEMCR |= (1u << 24);   /* TRCENA */
    VMAP_PERF_DWT_CTRL |= 1u;            /* CYCCNTENA */

    a = VMAP_PERF_DWT_CYCCNT;
    for (i = 0; i < 64; i++) {
        __asm__ volatile("nop");
    }
    b = VMAP_PERF_DWT_CYCCNT;
    s_pf_usable = (b != a);

    if (!s_pf_usable) {
        syslog(LOG_WARNING, "[vperf] DWT CYCCNT 不可用，分段计时停用\n");
    } else {
        vmap_perf_psram_probe();
        vmap_perf_cache_probe();
#if VMAP_RENDER_PRIM_SELF
        {
            /* 多边形定点自测：确定性输入，两个固件的数字可直接对减 */
            vmap_render_polyself_t st;

            vmap_render_polyselftest(&st);
            syslog(LOG_INFO,
                "[vperf] polyself pass=%u fpcyc=%u fpcross=%u intcyc=%u intcross=%u",
                (unsigned)st.pass, (unsigned)st.fp_cyc, (unsigned)st.fp_cross,
                (unsigned)st.in_cyc, (unsigned)st.in_cross);
        }
#endif
    }
}

static inline uint32_t vmap_perf_cyc(void)
{
    return VMAP_PERF_DWT_CYCCNT;
}

static void vmap_perf_frame_begin(void)
{
    if (!s_pf_usable) {
        return;
    }
    s_pf_t0 = vmap_perf_cyc();
    s_pf_tick0 = clock_systime_ticks();
    s_pf_begin = 0u;
    s_pf_warm = 0u;
    s_pf_tile = 0u;
    s_pf_nav = 0u;
    s_pf_label = 0u;
    s_pf_track = 0u;
    s_pf_n_tiles = 0u;
    s_pf_n_warms = 0u;
    s_pf_n_labels = 0u;
    s_pf_stores = 0u;
    s_pf_bytes = 0u;
    s_pf_scroll = 0u;
    s_pf_lscroll = 0u;
    s_pf_inval = 0u;
    s_pf_meta = 0u;
    s_pf_hop1 = 0u;
    s_pf_hop2 = 0u;
    s_pf_cpuc = 0u;
    vmap_blit_stats(vmap_blit_get(), &s_pf_hop0[0], &s_pf_hop0[1],
        &s_pf_hop0[2]);
    s_pf_iacc0 = VMAP_PERF_CACHE_IACR;
    s_pf_imiss0 = VMAP_PERF_CACHE_IMCR;
    s_pf_dacc0 = VMAP_PERF_CACHE_DACR;
    s_pf_dmiss0 = VMAP_PERF_CACHE_DMCR;
    vmap_render_prim_get(&s_prim0);
    s_pf_in_frame = true;
}

static void vmap_perf_frame_end(uint32_t zoom, uint32_t dirty)
{
    uint32_t total;
    uint32_t now;
    uint32_t tickd;
    uint32_t iacc;
    uint32_t imiss;
    uint32_t dacc;
    uint32_t dmiss;
    uint32_t cps;

    if (!s_pf_usable || !s_pf_in_frame) {
        return;
    }
    s_pf_in_frame = false;

    total = vmap_perf_cyc() - s_pf_t0;
    tickd = (uint32_t)(clock_systime_ticks() - s_pf_tick0);
    iacc = VMAP_PERF_CACHE_IACR - s_pf_iacc0;
    imiss = VMAP_PERF_CACHE_IMCR - s_pf_imiss0;
    dacc = VMAP_PERF_CACHE_DACR - s_pf_dacc0;
    dmiss = VMAP_PERF_CACHE_DMCR - s_pf_dmiss0;
    cps = (s_pf_stores != 0u) ? (s_pf_tile / s_pf_stores) : 0u;
    {
        uint32_t h1 = 0u;
        uint32_t h2 = 0u;
        uint32_t hc = 0u;

        vmap_blit_stats(vmap_blit_get(), &h1, &h2, &hc);
        s_pf_hop1 = h1 - s_pf_hop0[0];
        s_pf_hop2 = h2 - s_pf_hop0[1];
        s_pf_cpuc = hc - s_pf_hop0[2];
    }
    now = lv_tick_get();
    if (s_pf_last_log != 0u && lv_tick_elaps(s_pf_last_log) < VMAP_PERF_LOG_GAP_MS) {
        return;
    }
    s_pf_last_log = now;

    /* 全部是原始周期数，单位只在主机侧按 cyc_tot/(tickd*10ms) 反解。
     * dirty 是这一帧的脏区条数：0 = 全量重画，>0 = scroll 补边。
     * st/by = 瓦片光栅化发出的 store 次数与字节数；cps = cyc_tile/st —— 判成本模型：
     *   cps ≈ 88  ⇒ 每次 store 一次总线事务（成本 ∝ store 次数）
     *   cyc_tile/(by/4) ≈ 88 而 cps 不成 ... 见 docs/map/perf/MEASUREMENTS.md
     * i/d = SoC 缓存的 access/miss 增量（判取指命中率）。 */
    /* 超长帧无条件 WARNING；正常帧留 INFO（用户 2026-09-26）。 */
    syslog((total > (VMAP_PERF_SLOW_FRAME_MS * 240u * 1000u)) ? LOG_WARNING : LOG_INFO,
        "[vperf] frame z=%u dirty=%u tickd=%u cyc_begin=%u cyc_warm=%u cyc_tile=%u cyc_nav=%u cyc_label=%u cyc_track=%u cyc_tot=%u tiles=%u warms=%u labels=%u st=%u by=%u cps=%u i=%u/%u d=%u/%u scr=%u lscr=%u inv=%u meta=%u hop1=%u hop2=%u cpuc=%u hz=%u",
        (unsigned)zoom, (unsigned)dirty, (unsigned)tickd,
        (unsigned)s_pf_begin, (unsigned)s_pf_warm,
        (unsigned)s_pf_tile, (unsigned)s_pf_nav,
        (unsigned)s_pf_label, (unsigned)s_pf_track,
        (unsigned)total,
        (unsigned)s_pf_n_tiles, (unsigned)s_pf_n_warms,
        (unsigned)s_pf_n_labels,
        (unsigned)s_pf_stores, (unsigned)s_pf_bytes, (unsigned)cps,
        (unsigned)iacc, (unsigned)imiss, (unsigned)dacc, (unsigned)dmiss,
        (unsigned)s_pf_scroll, (unsigned)s_pf_lscroll,
        (unsigned)s_pf_inval, (unsigned)s_pf_meta,
        (unsigned)s_pf_hop1, (unsigned)s_pf_hop2, (unsigned)s_pf_cpuc,
        (unsigned)SystemCoreClock);

    /* 【临时取证】cyc_tile 按图元拆开：多边形 / 面填充 / 圆盘，外加两个工作量
     * 计数（cross = 逐行软除次数，scan = 行×边 扫描次数）。测完即撤。 */
    vmap_render_prim_get(&s_prim);
    syslog(LOG_DEBUG,
        "[vperf] prim polyfp=%u npfp=%u poly=%u np=%u disc=%u nd=%u cross=%u scan=%u h=%u,%u,%u,%u,%u,%u cn=%u,%u,%u,%u,%u,%u",
        (unsigned)(s_prim.poly_fp - s_prim0.poly_fp),
        (unsigned)(s_prim.n_poly_fp - s_prim0.n_poly_fp),
        (unsigned)(s_prim.poly - s_prim0.poly),
        (unsigned)(s_prim.n_poly - s_prim0.n_poly),
        (unsigned)(s_prim.disc - s_prim0.disc),
        (unsigned)(s_prim.n_disc - s_prim0.n_disc),
        (unsigned)(s_prim.cross - s_prim0.cross),
        (unsigned)(s_prim.scan - s_prim0.scan),
        (unsigned)s_prim.hist[0], (unsigned)s_prim.hist[1],
        (unsigned)s_prim.hist[2], (unsigned)s_prim.hist[3],
        (unsigned)s_prim.hist[4], (unsigned)s_prim.hist[5],
        (unsigned)s_prim.cyc_n[0], (unsigned)s_prim.cyc_n[1],
        (unsigned)s_prim.cyc_n[2], (unsigned)s_prim.cyc_n[3],
        (unsigned)s_prim.cyc_n[4], (unsigned)s_prim.cyc_n[5]);
}

static void vmap_perf_stamp_end(uint32_t zoom)
{
    uint32_t now;

    if (!s_pf_usable || s_pf_in_frame) {
        return;
    }
    if (s_pf_s_nav == 0u && s_pf_s_track == 0u) {
        return;
    }
    now = lv_tick_get();
    if (s_pf_last_stamp != 0u
        && lv_tick_elaps(s_pf_last_stamp) < VMAP_PERF_LOG_GAP_MS) {
        s_pf_s_nav = 0u;
        s_pf_s_track = 0u;
        return;
    }
    s_pf_last_stamp = now;

    syslog(LOG_DEBUG, "[vperf] stamp z=%u cyc_nav=%u cyc_track=%u hz=%u",
        (unsigned)zoom, (unsigned)s_pf_s_nav,
        (unsigned)s_pf_s_track, (unsigned)SystemCoreClock);
    s_pf_s_nav = 0u;
    s_pf_s_track = 0u;
}
/*
 * PSRAM 通路自检（开机一次，打一行）。
 *
 * 目的：把"渲染为什么慢"拆成**每次访问的固定开销**与**字节速率**两件事。
 * 手法：同样大小，分别用 4 字节逐字写、32 字节块写（stmia）、4 字节逐字读各跑
 * 一遍，比周期数。判据（blk_w / word_w 的百分比）：
 *
 *   ≈ 12%  ⇒ 写通路会把块写合并成突发：`vmap_render.c` 的 32 字节块写
 *            （fill_run_u16 / vmap_render_clear）已经赚到；
 *   ≈ 100% ⇒ 写通路不合并、时间花在**字节**上：块写白做（该回滚），真正的
 *            杠杆是减少要写的字节数，而不是换存储方式。
 *
 * 读一遍只作参照；注意它可能部分命中 D-cache，所以读数字是下界。
 */
#define VMAP_PERF_PSRAM_BYTES (128 * 1024)

typedef struct {
    uint32_t w[8];
} vmap_perf_blk32_t;

static void vmap_perf_psram_probe(void)
{
    uint8_t * buf = (uint8_t *)vmap_malloc(VMAP_PERF_PSRAM_BYTES);
    vmap_perf_blk32_t blk;
    uint32_t c0;
    uint32_t c_word;
    uint32_t c_blk;
    uint32_t c_read;
    uint32_t sum = 0;
    int i;

    if (buf == NULL) {
        syslog(LOG_WARNING, "[vperf] psram probe: 分配失败，跳过\n");
        return;
    }
    for (i = 0; i < 8; i++) {
        blk.w[i] = 0xa5a5a5a5u;
    }

    memset(buf, 0, VMAP_PERF_PSRAM_BYTES);   /* 先热一遍，别把首次触碰算进去 */

    c0 = vmap_perf_cyc();
    for (i = 0; i < VMAP_PERF_PSRAM_BYTES; i += 4) {
        *(uint32_t *)(void *)(buf + i) = 0xa5a5a5a5u;
    }
    __asm__ volatile("" ::: "memory");
    c_word = vmap_perf_cyc() - c0;

    c0 = vmap_perf_cyc();
    for (i = 0; i < VMAP_PERF_PSRAM_BYTES; i += (int)sizeof(blk)) {
        *(vmap_perf_blk32_t *)(void *)(buf + i) = blk;
    }
    __asm__ volatile("" ::: "memory");
    c_blk = vmap_perf_cyc() - c0;

    c0 = vmap_perf_cyc();
    for (i = 0; i < VMAP_PERF_PSRAM_BYTES; i += 4) {
        sum += *(const uint32_t *)(const void *)(buf + i);
    }
    __asm__ volatile("" ::: "memory");
    c_read = vmap_perf_cyc() - c0;

    syslog(LOG_INFO,
        "[vperf] psram %uKB word_w=%u blk_w=%u word_r=%u cyc blk/w=%u%% sum=%u",
        (unsigned)(VMAP_PERF_PSRAM_BYTES / 1024), (unsigned)c_word,
        (unsigned)c_blk, (unsigned)c_read,
        (unsigned)(c_word != 0u ? (c_blk * 100u / c_word) : 0u), (unsigned)sum);

    vmap_free(buf);
}

static void vmap_perf_cache_probe(void)
{
    const uint32_t ccr = VMAP_PERF_CACHE_CCR;

    /* 实测原值：全树没有任何代码碰过这个外设（grep hwp_cache-> 零命中）。
     * 首次上板读到的是 **ccr=0x00000000（irange=0 drange=0）**，而且整帧
     * I/D 计数器都是 0 ⇒ 缓存既不覆盖代码也不覆盖数据，**代码是从 PSRAM 裸取指**。
     * 详见 docs/map/perf/MEASUREMENTS.md §2.6。 */
    syslog(LOG_INFO,
        "[vperf] cache ccr=0x%08x irange=%u drange=%u cnten=%u",
        (unsigned)ccr,
        (unsigned)((ccr >> 16) & 0x1fu), (unsigned)((ccr >> 24) & 0x1fu),
        (unsigned)(ccr & 1u));

    /* **只打开计数器，不改缓存行为。**
     *
     * 实验做过了（2026-09-25）：把 IRANGE 设成 MPI1（只缓存代码区、DRANGE 保持 0，
     * 因为代码只读、缓存只读数据没有一致性问题，而这个外设又没有 clean/invalidate
     * 接口 ⇒ 不该缓存数据）——结果是 **cyc_tile 90.56M vs 不缓存 89.69M，没有变化**，
     * 而计数器同时显示取指 miss 率只有 **0.068%**（391,198,596 次访问 / 266,570 次 miss）。
     * ⇒ **未开缓存时顺序预取已经把紧凑代码喂饱了，取指不是瓶颈**，所以不设 IRANGE。
     * 详见 docs/map/perf/MEASUREMENTS.md §2.6。 */
    VMAP_PERF_CACHE_CCR = (ccr & ~VMAP_PERF_CACHE_CNTCLR) | VMAP_PERF_CACHE_CNTEN
        | VMAP_PERF_CACHE_CNTCLR;
}

/**
 * @brief SLOW gap 时点名"谁在运行 / 谁就绪"。
 *
 * 形态 C（用户 2026-09-26 现场）缺的就是这一条：探针能说"停了 1.61 秒"，但说不出
 * 那 1.61 秒是谁占着 CPU。判据来自另两条探针的组合 —— 喂狗线程（prio 180）没报
 * late ⇒ 不是全局冻结/长时间关中断；而 idle 心跳陈旧 ⇒ CPU 从没空过 ⇒ 是某个
 * prio ≥ 100 的任务在持续空转。所以这里把 Ready/Running 的任务列出来点名
 * （`state`: 2=Ready 3=Running，与 coredump 快照的 `snap … st=` 同一口径）。
 */
static void vmap_perf_dump_runnable(void)
{
    struct myvendor_schedmon_snap_s snap;
    char buf[176];
    size_t n = 0u;
    uint16_t i;

    if (!myvendor_schedmon_snap_copy(&snap)) {
        syslog(LOG_WARNING, "[vperf] SLOW: schedmon 快照不可用\n");
        return;
    }

    for (i = 0u; i < snap.ntask && n + 24u < sizeof(buf); i++) {
        const struct myvendor_schedmon_task_s * t = &snap.task[i];
        const int w = snprintf(buf + n, sizeof(buf) - n, "%s%s(pid%d,pri%d)",
                               n ? "," : "", t->name, (int)t->pid, (int)t->pri);

        if (t->state != 2u && t->state != 3u) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        n += (size_t)w;
    }

    syslog(LOG_WARNING, "[vperf] SLOW: 运行/就绪 = %s\n", n ? buf : "(无)");
}

/** 段首取时间戳；必须与 VMAP_PERF_T1 成对，且在同一层花括号里。 */
#define VMAP_PERF_T0()      uint32_t _pf_t0 = vmap_perf_cyc()
/** 嵌套用（避免与 T0 的 _pf_t0 互相遮蔽）。 */
#define VMAP_PERF_T0S()     uint32_t _pf_ts = vmap_perf_cyc()
#define VMAP_PERF_T1S(f)    do { s_pf_##f += (vmap_perf_cyc() - _pf_ts); } while (0)
/** 段尾累加这一段的周期数到 s_pf_<field>。 */
#define VMAP_PERF_T1(f)     do { s_pf_##f += (vmap_perf_cyc() - _pf_t0); } while (0)
/** 计数 +1 / +n。 */
#define VMAP_PERF_N(f)      do { s_pf_##f++; } while (0)
#define VMAP_PERF_NB(f, n)  do { s_pf_##f += (uint32_t)(n); } while (0)
/** nav/track 段在"帧内"与"无帧的 stamp"两种情形下写入不同的桶。 */
#define VMAP_PERF_NAV_SLOT()   (s_pf_in_frame ? &s_pf_nav : &s_pf_s_nav)
#define VMAP_PERF_TRACK_SLOT() (s_pf_in_frame ? &s_pf_track : &s_pf_s_track)
/** 段尾按指针累加（用于上面两个运行期才能定的桶）。 */
#define VMAP_PERF_T1P(p)    do { *(p) += (vmap_perf_cyc() - _pf_t0); } while (0)
#else
#define VMAP_PERF_T0()      uint32_t _pf_t0 = 0u
#define VMAP_PERF_T1(f)     do { (void)_pf_t0; } while (0)
#define VMAP_PERF_T1P(p)    do { (void)_pf_t0; (void)(p); } while (0)
#define VMAP_PERF_N(f)      do { } while (0)
#define VMAP_PERF_NB(f, n)  do { } while (0)
#define VMAP_PERF_NAV_SLOT()   ((uint32_t *)0)
#define VMAP_PERF_TRACK_SLOT() ((uint32_t *)0)
#endif

#if !VMAP_PERF_PROBE
#define vmap_perf_init_once()   do { } while (0)
#define vmap_perf_frame_begin() do { } while (0)
#define vmap_perf_frame_end()   do { } while (0)
#define vmap_perf_stamp_end()   do { } while (0)
#endif

bool vmap_boot_ll_ok(double lon, double lat)
{
    if (!isfinite(lon) || !isfinite(lat)) {
        return false;
    }

    if (fabs(lon) < 0.001 && fabs(lat) < 0.001) {
        return false;
    }

    if (lon < VMAP_POS_LON_MIN || lon > VMAP_POS_LON_MAX ||
        lat < VMAP_POS_LAT_MIN || lat > VMAP_POS_LAT_MAX) {
        return false;
    }

    return true;
}

void vmap_boot_lonlat(double *lon, double *lat)
{
    int32_t lon_e7 = 0;
    int32_t lat_e7 = 0;
    double last_lon;
    double last_lat;

    if (lon == NULL || lat == NULL) {
        return;
    }

    if (myvendor_devctl_last_pos_get(&lon_e7, &lat_e7)) {
        last_lon = (double)lon_e7 / 10000000.0;
        last_lat = (double)lat_e7 / 10000000.0;
        if (vmap_boot_ll_ok(last_lon, last_lat)) {
            *lon = last_lon;
            *lat = last_lat;
            return;
        }
    }

    *lon = VMAP_DEFAULT_LON;
    *lat = VMAP_DEFAULT_LAT;
}

struct vmap_view {
    lv_obj_t * canvas;
    lv_obj_t * arrow_layer;
    lv_obj_t * arrow;
    void * cbuf;
    bool cbuf_arena; /**< cbuf 来自板级专属 arena（**不能 free**，见 board_psram_layout.h）。 */
    int32_t w;
    int32_t h;
    int32_t view_w;
    int32_t view_h;
    int32_t margin;
    uint32_t stride;
    double lon;
    double lat;
    double render_lon;
    double render_lat;
    double pan_dx;
    double pan_dy;
    int zoom;
    double scale;
    bool follow_mode;
    int32_t last_arrow_angle10;
    /** @brief 显示偏置（y）：只影响画布与箭头的屏幕位置，不进跟车/rebase 数学。 */
    int32_t y_bias;
    int32_t last_pan_px;
    int32_t last_pan_py;
    vmap_tile_cache_t * cache;
    vmap_point_t * scratch;
    vmap_label_cand_t * labels;
    vmap_label_retained_t * label_retained;
    vmap_label_collector_t label_col;
    const lv_font_t * font;
    uint32_t map_rev;
    uint32_t render_rev;
    int32_t pump_tx;
    int32_t pump_ty;
    int32_t pump_tx_min;
    int32_t pump_ty_min;
    int32_t pump_tx_max;
    int32_t pump_ty_max;
    int pump_cx;
    int pump_cy;
    uint8_t pump_phase;
    uint8_t restamp_pending;
    uint8_t pump_need_region;
    uint8_t pump_dirty_n;
    int paint_zoom;
    double paint_scale;
    int32_t pump_dirty_x1[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_y1[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_x2[VMAP_RENDER_DIRTY_MAX];
    int32_t pump_dirty_y2[VMAP_RENDER_DIRTY_MAX];
    struct vmap_track * track;
#if VMAP_ROUTE_ENABLE
    struct vmap_route * route;
#endif
};

static void tile_range(int32_t center_x, int32_t center_y, int32_t canvas_w,
    int32_t canvas_h, double scale, int32_t ring,
    int32_t * tx_min, int32_t * ty_min, int32_t * tx_max, int32_t * ty_max)
{
    const int32_t tile_size = 256;
    double half_w = (canvas_w / 2.0) / scale;
    double half_h = (canvas_h / 2.0) / scale;
    double pad = (double)ring * tile_size;
    double left = (double)center_x - half_w - pad;
    double top = (double)center_y - half_h - pad;
    double right = (double)center_x + half_w + pad;
    double bottom = (double)center_y + half_h + pad;

    *tx_min = (int32_t)(left / tile_size) - (left < 0 ? 1 : 0);
    *ty_min = (int32_t)(top / tile_size) - (top < 0 ? 1 : 0);
    *tx_max = (int32_t)(right / tile_size);
    *ty_max = (int32_t)(bottom / tile_size);
}

static void vmap_view_apply_canvas_pos(vmap_view_t * view, double dx, double dy)
{
    lv_coord_t px;
    lv_coord_t py;

    if (!view || !view->canvas) {
        return;
    }

    px = (lv_coord_t)lround(dx);
    py = (lv_coord_t)lround(dy);

    if (px == view->last_pan_px && py == view->last_pan_py) {
        return;
    }

    /*
     * Moving a canvas invalidates the complete visible map. Coalesce sub-pixel
     * GNSS movement; the centered arrow still updates normally.
     */
    if (view->last_pan_px != INT32_MIN && view->last_pan_py != INT32_MIN
        && abs((int)px - view->last_pan_px) < VMAP_PAN_MIN_STEP_PX
        && abs((int)py - view->last_pan_py) < VMAP_PAN_MIN_STEP_PX) {
        return;
    }

    view->pan_dx = (double)px;
    view->pan_dy = (double)py;
    view->last_pan_px = px;
    view->last_pan_py = py;
    view->map_rev++;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + px, view->h / 2 + py);
#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_tag("vmap_pan");
#endif
    /* lv_obj_set_pos already invalidates old + new areas; do not inval again. */
    /* y_bias 只加在屏幕位置上：pan_dy 与投影原点都不动，于是偏置与跟车/rebase
     * 的数学完全解耦（偏置变化既不触发重算，也不改变 rebase 时机）。 */
    lv_obj_set_pos(view->canvas,
        (lv_coord_t)(-view->margin - px),
        (lv_coord_t)(-view->margin - py + view->y_bias));
}

static bool vmap_view_geo_to_viewport_impl(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y, bool check_visible)
{
    int base_x = 0;
    int base_y = 0;
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;
    int32_t sx;
    int32_t sy;
    const int32_t pad = 24;

    if (!view) {
        return false;
    }

    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom, &base_x, &base_y);
    vmap_latlong_to_pixel(lat, lon, view->zoom, &geo_x, &geo_y);

    cx = (geo_x - base_x) * view->scale + (double)view->w / 2.0;
    cy = (geo_y - base_y) * view->scale + (double)view->h / 2.0;
    sx = (int32_t)lround(cx - (double)view->margin - view->pan_dx);
    sy = (int32_t)lround(cy - (double)view->margin - view->pan_dy);

    if (out_x) {
        *out_x = sx;
    }
    if (out_y) {
        *out_y = sy;
    }

    if (!check_visible) {
        return true;
    }

    return sx >= -pad && sy >= -pad
        && sx <= view->view_w + pad && sy <= view->view_h + pad;
}

/**
 * @brief 经纬度 → 视口像素（含 follow pan）。
 */
bool vmap_view_geo_to_viewport(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    return vmap_view_geo_to_viewport_impl(view, lon, lat, out_x, out_y, true);
}

/**
 * @brief 经纬度 → 视口像素，不做可见性裁剪。
 */
bool vmap_view_geo_to_viewport_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    return vmap_view_geo_to_viewport_impl(view, lon, lat, out_x, out_y, false);
}

/**
 * @brief 经纬度 → 画布整数像素。
 */
bool vmap_view_geo_to_canvas_raw(const vmap_view_t * view, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    int base_x = 0;
    int base_y = 0;
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;

    if (!view) {
        return false;
    }

    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom, &base_x, &base_y);
    vmap_latlong_to_pixel(lat, lon, view->zoom, &geo_x, &geo_y);

    cx = (geo_x - base_x) * view->scale + (double)view->w / 2.0;
    cy = (geo_y - base_y) * view->scale + (double)view->h / 2.0;

    if (out_x) {
        *out_x = (int32_t)lround(cx);
    }
    if (out_y) {
        *out_y = (int32_t)lround(cy);
    }
    return true;
}

bool vmap_view_geo_to_canvas_f(const vmap_view_t * view, double lon, double lat,
    double * out_x, double * out_y)
{
    vmap_view_proj_t proj;

    if (!view) {
        return false;
    }

    vmap_view_proj_begin(view, &proj);
    vmap_view_proj_point_f(&proj, lon, lat, out_x, out_y);
    return true;
}

void vmap_view_proj_begin(const vmap_view_t * view, vmap_view_proj_t * proj)
{
    if (!proj) {
        return;
    }
    if (!view) {
        memset(proj, 0, sizeof(*proj));
        return;
    }

    proj->zoom = view->zoom;
    proj->scale = view->scale;
    proj->half_w = (double)view->w / 2.0;
    proj->half_h = (double)view->h / 2.0;
    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom,
        &proj->base_x, &proj->base_y);
}

void vmap_view_proj_point(const vmap_view_proj_t * proj, double lon, double lat,
    int32_t * out_x, int32_t * out_y)
{
    int geo_x = 0;
    int geo_y = 0;
    double cx;
    double cy;

    if (!proj) {
        return;
    }

    vmap_latlong_to_pixel(lat, lon, proj->zoom, &geo_x, &geo_y);
    cx = (geo_x - proj->base_x) * proj->scale + proj->half_w;
    cy = (geo_y - proj->base_y) * proj->scale + proj->half_h;
    if (out_x) {
        *out_x = (int32_t)lround(cx);
    }
    if (out_y) {
        *out_y = (int32_t)lround(cy);
    }
}

void vmap_view_proj_point_f(const vmap_view_proj_t * proj, double lon, double lat,
    double * out_x, double * out_y)
{
    double geo_x = 0.0;
    double geo_y = 0.0;

    if (!proj) {
        return;
    }

    vmap_latlong_to_pixel_d(lat, lon, proj->zoom, &geo_x, &geo_y);
    if (out_x) {
        *out_x = (geo_x - (double)proj->base_x) * proj->scale + proj->half_w;
    }
    if (out_y) {
        *out_y = (geo_y - (double)proj->base_y) * proj->scale + proj->half_h;
    }
}

/**
 * @brief retained 画布宽高（含 overscan）。
 * @return 请求的值。
 */
void vmap_view_get_buffer_size(const vmap_view_t * view, int32_t * w, int32_t * h)
{
    if (!view) {
        return;
    }
    if (w) {
        *w = view->w;
    }
    if (h) {
        *h = view->h;
    }
}

/**
 * @brief 画布坐标点是否可安全描边。
 */
bool vmap_view_canvas_point_sane(const vmap_view_t * view, int32_t x, int32_t y)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!view) {
        return false;
    }

    vmap_view_get_buffer_size(view, &cw, &ch);
    if (cw < 1 || ch < 1) {
        return false;
    }

    return x >= -cw * 4 && x <= cw * 5 && y >= -ch * 4 && y <= ch * 5;
}

/**
 * @brief 画布点是否落在可视窗口外扩 pad 内。
 */
bool vmap_view_canvas_point_in_pad(const vmap_view_t * view, int32_t x, int32_t y,
    int32_t pad)
{
    int32_t cw = 0;
    int32_t ch = 0;

    if (!view || pad < 0 || !vmap_view_canvas_point_sane(view, x, y)) {
        return false;
    }

    vmap_view_get_buffer_size(view, &cw, &ch);
    return x >= -pad && y >= -pad && x <= cw + pad && y <= ch + pad;
}

/**
 * @brief 画布点是否落在可视窗口内。
 */
bool vmap_view_canvas_point_visible(const vmap_view_t * view, int32_t x, int32_t y)
{
    return vmap_view_canvas_point_in_pad(view, x, y, 48);
}

/**
 * @brief 取出当前 RGB565 画布描述。
 */
bool vmap_view_get_canvas(const vmap_view_t * view, vmap_canvas_t * canvas)
{
    if (!view || !canvas || !view->cbuf) {
        return false;
    }

    canvas->buf = (uint16_t *)view->cbuf;
    canvas->w = view->w;
    canvas->h = view->h;
    canvas->stride = view->stride;
    vmap_render_clip_reset(canvas);
    return true;
}

void vmap_view_invalidate_canvas_area(const vmap_view_t * view,
    int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    lv_area_t area;
    lv_area_t coords;

    if (!view || !view->canvas || x1 > x2 || y1 > y2) {
        return;
    }

    if (x2 < 0 || y2 < 0 || x1 >= view->w || y1 >= view->h) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= view->w) {
        x2 = view->w - 1;
    }
    if (y2 >= view->h) {
        y2 = view->h - 1;
    }

    lv_obj_get_coords(view->canvas, &coords);
    area.x1 = coords.x1 + x1;
    area.y1 = coords.y1 + y1;
    area.x2 = coords.x1 + x2;
    area.y2 = coords.y1 + y2;
    lv_obj_invalidate_area(view->canvas, &area);
}

#if VMAP_TRACK_ENABLE
/**
 * @brief 绑定 REC 轨迹叠层。
 */
void vmap_view_attach_track(vmap_view_t * view, vmap_track_t * track)
{
    if (view) {
        view->track = track;
    }
}

#if VMAP_ROUTE_ENABLE
/**
 * @brief 把规划路线绑到本视图。
 */
void vmap_view_attach_route(vmap_view_t * view, vmap_route_t * route)
{
    if (view) {
        view->route = route;
        if (route) {
            vmap_route_bind_view(route, view);
        }
    }
}
#endif
#endif

/**
 * @brief 创建带 overscan 的 RGB565 大画布；canvas 隐藏到首帧完成。
 * @return 视图；失败为 NULL。
 */
vmap_view_t * vmap_view_create(lv_obj_t * parent, int32_t view_w, int32_t view_h,
    int32_t margin)
{
    vmap_view_t * view = (vmap_view_t *)calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }

    vmap_perf_init_once();

    view->view_w = view_w;
    view->view_h = view_h;
    view->margin = margin;
    view->w = view_w + margin * 2;
    view->h = view_h + margin * 2;
    view->zoom = VMAP_DEFAULT_ZOOM;
    view->scale = VMAP_DEFAULT_SCALE;
    vmap_boot_lonlat(&view->lon, &view->lat);
    view->render_lon = view->lon;
    view->render_lat = view->lat;
    view->follow_mode = true;
    view->last_arrow_angle10 = INT32_MIN;
    view->last_pan_px = INT32_MIN;
    view->last_pan_py = INT32_MIN;
    view->paint_zoom = -1;

    view->stride = lv_draw_buf_width_to_stride(view->w, LV_COLOR_FORMAT_RGB565);
    /* ⚠ 画布**优先用专属 arena**（板级 MPU 只给它开 write-back，见
     * `board_psram_layout.h` 的 `BOARD_PSRAM_CANVAS_ARENA_*`）：普通 PSRAM 池是
     * write-through ⇒ 每次像素 store 都打到 PSRAM（~90 周期/次 ≈ 10 MB/s），
     * 这是地图渲染慢的底层原因。arena 放不下就回退普通池（慢但能用）。 */
    {
        const size_t need = (size_t)view->stride * (size_t)view->h;
        size_t arena_bytes = 0;
        void * arena = board_psram_canvas_arena(&arena_bytes);

        if (arena != NULL && need > 0 && need <= arena_bytes) {
            view->cbuf = arena;
            view->cbuf_arena = true;
            /* 这里**不要**写 WB/WT：写入属性由板级 MPU 决定（见 board_psram_layout.h），
             * MPU 侧没接上时 arena 仍是普通的 WT —— 写死"WB"会让人误判。 */
            syslog(LOG_NOTICE,
                   "[vmap] canvas arena %p +%u KiB (need %u KiB)",
                   arena, (unsigned)(arena_bytes / 1024u),
                   (unsigned)(need / 1024u));
        } else {
            view->cbuf = vmap_malloc(need);
            syslog(LOG_WARNING,
                   "[vmap] canvas fallback pool (need %u KiB, arena %u KiB)",
                   (unsigned)(need / 1024u), (unsigned)(arena_bytes / 1024u));
        }
    }

    if (!view->cbuf) {
        free(view);
        return NULL;
    }

    memset(view->cbuf, 0, (size_t)view->stride * (size_t)view->h);

    /*
     * 搬运后端自检（一次性，`[vblit]` 行）：拿真机画布几何测"同一块缓冲内
     * 源/目的矩形重叠"的搬运能不能交给 EPIC。放在这里是因为画布尺寸刚定下、
     * 还没有任何一帧在跑，而且自检缓冲用完立即释放。详见 vmap_blit.h。
     */
    vmap_blit_probe(view->w, view->h);

    view->scratch = (vmap_point_t *)vmap_malloc(sizeof(vmap_point_t) * VMAP_SCRATCH_CAP);
    view->labels = (vmap_label_cand_t *)vmap_malloc(sizeof(vmap_label_cand_t) * VMAP_LABEL_CAP);
    view->label_retained = (vmap_label_retained_t *)vmap_malloc(
        sizeof(vmap_label_retained_t) * VMAP_LABEL_RETAIN_CAP);
    view->cache = vmap_tile_cache_create();
    if (!view->scratch || !view->labels || !view->label_retained || !view->cache) {
        vmap_view_destroy(view);
        return NULL;
    }

    vmap_label_collector_init(&view->label_col, view->labels, VMAP_LABEL_CAP,
        view->label_retained, VMAP_LABEL_RETAIN_CAP,
        view->w, view->h, view->view_w, view->view_h, view->margin);

    view->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(view->canvas, view->cbuf, view->w, view->h,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_style_all(view->canvas);
    lv_obj_set_size(view->canvas, view->w, view->h);
    lv_obj_set_pos(view->canvas, -margin, -margin);
    /* 首帧 render_finish 前不要把 PSRAM 噪声送到 LCD。 */
    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    vmap_view_set_tile_dir(view, VMAP_TILE_DIR);
    return view;
}

/**
 * @brief 销毁视图：释放画布与瓦片缓存。
 */
void vmap_view_destroy(vmap_view_t * view)
{
    if (!view) {
        return;
    }

    view->track = NULL;

    if (view->canvas) {
        lv_obj_delete(view->canvas);
        view->canvas = NULL;
    }
    if (view->arrow) {
        lv_obj_delete(view->arrow);
    }
    if (view->cbuf && !view->cbuf_arena) {
        vmap_free(view->cbuf);
    }
    if (view->scratch) {
        vmap_free(view->scratch);
    }
    if (view->labels) {
        vmap_free(view->labels);
    }
    if (view->label_retained) {
        vmap_free(view->label_retained);
    }
    vmap_tile_cache_destroy(view->cache);
    free(view);
}

/**
 * @brief 底层 LVGL canvas 对象。
 */
lv_obj_t * vmap_view_get_obj(const vmap_view_t * view)
{
    return view ? view->canvas : NULL;
}

/**
 * @brief 设置地图目录（网格 .vpk 根）。
 */
void vmap_view_set_tile_dir(vmap_view_t * view, const char * dir)
{
    if (view && view->cache) {
        vmap_tile_cache_set_dir(view->cache, dir);
    }
}

/**
 * @brief 设置中心与缩放；不打开 .vpk。
 */
void vmap_view_set_view(vmap_view_t * view, double lon, double lat, int zoom)
{
    if (!view) {
        return;
    }
    view->lon = lon;
    view->lat = lat;
    view->render_lon = lon;
    view->render_lat = lat;
    view->zoom = zoom;
    /*
     * set_view is an explicit coordinate-system jump (route review/zoom/boot),
     * not a follow rebase.  Never scroll pixels from the previous projection.
     */
    view->paint_zoom = -1;
    if (view->cache) {
        vmap_tile_cache_set_zoom(view->cache, zoom);
    }
}

/**
 * @brief 打开当前经纬度所在 cell 的 region pack。
 */
bool vmap_view_ensure_region(vmap_view_t * view, double lon, double lat)
{
    if (!view || !view->cache) {
        return false;
    }
    return vmap_tile_cache_ensure_region(view->cache, lon, lat);
}

/**
 * @brief 解析瓦片 LRU / region slot 缓存。
 */
vmap_tile_cache_t * vmap_view_tile_cache(vmap_view_t * view)
{
    return view ? view->cache : NULL;
}

/**
 * @brief 当前瓦片 zoom。
 */
int vmap_view_get_zoom(const vmap_view_t * view)
{
    return view ? view->zoom : 0;
}

/**
 * @brief 当前显示倍率。
 */
double vmap_view_get_scale(const vmap_view_t * view)
{
    return view ? view->scale : 1.0;
}

/**
 * @brief 当前跟车中心（经纬度）。
 */
void vmap_view_get_center(const vmap_view_t * view, double * lon, double * lat)
{
    if (!view) {
        if (lon) {
            *lon = 0.0;
        }
        if (lat) {
            *lat = 0.0;
        }
        return;
    }
    if (lon) {
        *lon = view->lon;
    }
    if (lat) {
        *lat = view->lat;
    }
}

/**
 * @brief 改显示倍率。
 */
void vmap_view_set_scale(vmap_view_t * view, double scale)
{
    if (!view) {
        return;
    }
    if (scale < 0.25) {
        scale = 0.25;
    } else if (scale > 8.0) {
        scale = 8.0;
    }
    view->scale = scale;
}

/**
 * @brief 路名标签字体。
 */
void vmap_view_set_font(vmap_view_t * view, const lv_font_t * font)
{
    if (view) {
        view->font = font;
    }
}

static double vmap_clamp_d(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void vmap_follow_log(const vmap_view_t * view, double lon, double lat)
{
    static uint32_t s_last;

    if (!view) {
        return;
    }
    if (s_last != 0u && lv_tick_elaps(s_last) < 2000u) {
        return;
    }
    s_last = lv_tick_get();
    /* 几何诊断（用户 2026-09-25 报"导航中箭头不居中"）：把视图对象的屏幕位置与尺寸
     * 一起打出来 —— 一眼看出是容器没上移，还是视图自己被别处摆了位置。 */
    {
        lv_area_t a;

        if (view->arrow != NULL) {
            lv_obj_get_coords(view->arrow, &a);
            syslog(LOG_DEBUG, "[map] arrow  y1=%d y2=%d cy=%d (地图可见窗口中心期望值见页面几何)",
                (int)a.y1, (int)a.y2, (int)((a.y1 + a.y2) / 2));
        }
    }
}

/**
 * @brief 跟车：改中心经纬度。未越 overscan 只平移 canvas。
 */
void vmap_view_set_center(vmap_view_t * view, double lon, double lat)
{
    int now_x = 0;
    int now_y = 0;
    int base_x = 0;
    int base_y = 0;
    double dx;
    double dy;
    double limit;

    if (!view) {
        return;
    }

    vmap_latlong_to_pixel(lat, lon, view->zoom, &now_x, &now_y);
    vmap_latlong_to_pixel(view->render_lat, view->render_lon, view->zoom, &base_x, &base_y);

    dx = (now_x - base_x) * view->scale;
    dy = (now_y - base_y) * view->scale;
    view->lon = lon;
    view->lat = lat;

    /*
     * Follow must not decode tiles. vmap_tile_cache_warm() is a blocking
     * LFS parse (~150ms) on this thread; doing it every GNSS tick once pan
     * crossed the old prefetch threshold pinned bicycle_ui at ~80% idle~10%.
     * New tiles load in vmap_view_render_pump() when the canvas rebases.
     */
    limit = (double)view->margin - (double)VMAP_PAN_GUARD;

    if (dx >= -limit && dx <= limit && dy >= -limit && dy <= limit) {
        bicycle_c_debug_map_soft_pan();
        vmap_view_apply_canvas_pos(view, dx, dy);
        vmap_follow_log(view, lon, lat);
        return;
    }

    /* Soft-pan even when tiles cannot be reloaded (BLE LFS hold / in-flight pump). */
    if (view->pump_phase != 0 || myvendor_mtp_lfs_quiesce()) {
        bicycle_c_debug_map_soft_pan();
        vmap_view_apply_canvas_pos(view,
            vmap_clamp_d(dx, -limit, limit),
            vmap_clamp_d(dy, -limit, limit));
        vmap_follow_log(view, lon, lat);
        return;
    }

    bicycle_c_debug_map_full_render();
    vmap_view_render(view);
}

/**
 * @brief 绑定定位箭头所在 LVGL 层。
 */
void vmap_view_set_y_bias(vmap_view_t * view, int32_t bias)
{
    if (!view || view->y_bias == bias) {
        return;
    }

    view->y_bias = bias;

    /* 清掉角度去重，让箭头下一次更新必定重新摆位；否则角度没变就早退，
     * 箭头会留在偏置之前的位置上。 */
    view->last_arrow_angle10 = INT32_MIN;

    /* 画布立刻按新偏置重摆一次：导航开始/结束时地图常常是静止的，
     * 不能等下一次 pan 才生效。还没摆过（last_pan_* 未初始化）就交给下一次。 */
    if (view->canvas && view->last_pan_px != INT32_MIN
        && view->last_pan_py != INT32_MIN) {
        lv_obj_set_pos(view->canvas,
            (lv_coord_t)(-view->margin - view->last_pan_px),
            (lv_coord_t)(-view->margin - view->last_pan_py + bias));
    }
}

void vmap_view_bind_arrow_layer(vmap_view_t * view, lv_obj_t * layer)
{
    if (view) {
        view->arrow_layer = layer;
    }
}

/**
 * @brief 设置箭头图源。
 */
void vmap_view_set_arrow_image(vmap_view_t * view, const void * src)
{
    lv_coord_t aw;
    lv_coord_t ah;

    if (!view || !view->arrow_layer || !src) {
        return;
    }

    if (!view->arrow) {
        view->arrow = lv_img_create(view->arrow_layer);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_img_set_src(view->arrow, src);
    lv_obj_update_layout(view->arrow_layer);
    lv_obj_update_layout(view->arrow);
    aw = lv_obj_get_width(view->arrow);
    ah = lv_obj_get_height(view->arrow);
    lv_img_set_pivot(view->arrow, aw / 2, ah / 2);
    view->last_arrow_angle10 = INT32_MIN;
    /* 新建 img 默认在父对象 (0,0)。跟随模式立刻居中，避免钉在左上角。 */
    if (view->follow_mode) {
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, view->y_bias);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 跟随模式：箭头钉在视口中心。
 */
void vmap_view_set_follow_mode(vmap_view_t * view, bool enable)
{
    if (view) {
        view->follow_mode = enable;
    }
}

/**
 * @brief 按经纬度与航向摆箭头。
 */
void vmap_view_update_arrow(vmap_view_t * view, double lon, double lat, float course_deg)
{
    int32_t sx;
    int32_t sy;
    int32_t angle10;

    if (!view || !view->arrow) {
        return;
    }

    angle10 = (int32_t)(course_deg * 10.0f);
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + (int32_t)lround(view->pan_dx),
        view->h / 2 + (int32_t)lround(view->pan_dy));

    if (view->follow_mode) {
        const bool hidden = lv_obj_has_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);

        if (angle10 == view->last_arrow_angle10 && !hidden) {
            return;
        }
        view->last_arrow_angle10 = angle10;
        lv_obj_align(view->arrow, LV_ALIGN_CENTER, 0, view->y_bias);
        lv_img_set_angle(view->arrow, angle10);
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(view->arrow);
        return;
    }

    if (!vmap_view_geo_to_viewport(view, lon, lat, &sx, &sy)) {
        lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (angle10 == view->last_arrow_angle10
        && sx == lv_obj_get_x(view->arrow) + lv_obj_get_width(view->arrow) / 2
        && sy == lv_obj_get_y(view->arrow) + lv_obj_get_height(view->arrow) / 2) {
        return;
    }

    view->last_arrow_angle10 = angle10;
    lv_obj_set_pos(view->arrow,
        sx - lv_obj_get_width(view->arrow) / 2,
        sy - lv_obj_get_height(view->arrow) / 2 + view->y_bias);
    lv_img_set_angle(view->arrow, angle10);
    lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view->arrow);
}

/**
 * @brief 显示或隐藏定位箭头。
 */
void vmap_view_set_arrow_visible(vmap_view_t * view, bool visible)
{
    if (!view || !view->arrow) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(view->arrow, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 把箭头提到 map_area 最前。
 */
void vmap_view_bring_arrow_to_front(vmap_view_t * view)
{
    if (view && view->arrow) {
        lv_obj_move_foreground(view->arrow);
    }
}

/**
 * @brief 丢弃瓦片索引/arena 并重绘。
 * @return 0 成功，负 errno 失败。
 */
void vmap_view_reload_storage(vmap_view_t * view)
{
    if (!view || !view->cache || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    vmap_view_set_tile_dir(view, VMAP_TILE_DIR);
    vmap_tile_cache_set_zoom(view->cache, view->zoom);
    render_begin(view, true, true);
    vmap_view_bring_arrow_to_front(view);
}

/**
 * @brief 关闭已打开的地图分片句柄。
 */
void vmap_view_release_storage(vmap_view_t * view)
{
    if (!view || !view->cache) {
        return;
    }
    vmap_tile_cache_release_lfs(view->cache);
    /* 句柄已关，半帧 rebase 不能再解算；MTP reload 会 force_clear 重画。 */
    if (view->pump_phase != 0) {
        view->pump_phase = 0;
        view->pump_need_region = 0;
        view->pump_dirty_n = 0;
        view->restamp_pending = 0;
        dvfs_map_busy(false);
        syslog(LOG_NOTICE, "[map] abort pump (lfs released)");
    }
}

static void view_canvas(const vmap_view_t * view, vmap_canvas_t * canvas,
    bool clip_dirty)
{
    uint8_t i;

    canvas->buf = (uint16_t *)view->cbuf;
    canvas->w = view->w;
    canvas->h = view->h;
    canvas->stride = view->stride;
    vmap_render_clip_reset(canvas);
    if (!clip_dirty) {
        return;
    }
    for (i = 0; i < view->pump_dirty_n; i++) {
        vmap_render_clip_add(canvas,
            view->pump_dirty_x1[i], view->pump_dirty_y1[i],
            view->pump_dirty_x2[i], view->pump_dirty_y2[i]);
    }
}

static void pump_dirty_add(vmap_view_t * view, int32_t x1, int32_t y1,
    int32_t x2, int32_t y2)
{
    uint8_t i;

    if (!view || x1 >= x2 || y1 >= y2) {
        return;
    }

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > view->w) {
        x2 = view->w;
    }
    if (y2 > view->h) {
        y2 = view->h;
    }
    if (x1 >= x2 || y1 >= y2 || view->pump_dirty_n >= VMAP_RENDER_DIRTY_MAX) {
        return;
    }

    i = view->pump_dirty_n;
    view->pump_dirty_x1[i] = x1;
    view->pump_dirty_y1[i] = y1;
    view->pump_dirty_x2[i] = x2;
    view->pump_dirty_y2[i] = y2;
    view->pump_dirty_n++;
}

static void pump_dirty_from_scroll(vmap_view_t * view, int32_t sx, int32_t sy)
{
    const int32_t pad = (int32_t)VMAP_RENDER_DIRTY_PAD;

    view->pump_dirty_n = 0;
    if (sx > 0) {
        pump_dirty_add(view, view->w - sx - pad, 0, view->w, view->h);
    } else if (sx < 0) {
        pump_dirty_add(view, 0, 0, -sx + pad, view->h);
    }
    if (sy > 0) {
        pump_dirty_add(view, 0, view->h - sy - pad, view->w, view->h);
    } else if (sy < 0) {
        pump_dirty_add(view, 0, 0, view->w, -sy + pad);
    }
}

static bool pump_rects_overlap(int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2,
    int32_t bx1, int32_t by1, int32_t bx2, int32_t by2)
{
    return ax1 < bx2 && ax2 > bx1 && ay1 < by2 && ay2 > by1;
}

static bool pump_tile_dirty(const vmap_view_t * view, int32_t tx, int32_t ty)
{
    const int32_t tile_size = 256;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint8_t i;

    if (!view || view->pump_dirty_n == 0) {
        return true;
    }

    x1 = (int32_t)lround((tx * tile_size - view->pump_cx) * view->scale
        + view->w / 2.0);
    y1 = (int32_t)lround((ty * tile_size - view->pump_cy) * view->scale
        + view->h / 2.0);
    x2 = (int32_t)lround(x1 + 256.0 * view->scale);
    y2 = (int32_t)lround(y1 + 256.0 * view->scale);

    for (i = 0; i < view->pump_dirty_n; i++) {
        if (pump_rects_overlap(x1, y1, x2, y2,
                view->pump_dirty_x1[i], view->pump_dirty_y1[i],
                view->pump_dirty_x2[i], view->pump_dirty_y2[i])) {
            return true;
        }
    }
    return false;
}

static bool pump_advance(vmap_view_t * view)
{
    view->pump_ty--;
    if (view->pump_ty < view->pump_ty_min) {
        view->pump_tx--;
        view->pump_ty = view->pump_ty_max;
    }
    return view->pump_tx >= view->pump_tx_min;
}

static void render_begin(vmap_view_t * view, bool reset_view, bool force_clear)
{
    vmap_canvas_t canvas;
    const double origin_lon = reset_view ? view->lon : view->render_lon;
    const double origin_lat = reset_view ? view->lat : view->render_lat;
    int32_t sx;
    int32_t sy;
    bool scrolled = false;

    /* fopen/.vpk index belongs on the first pump tick, not GNSS/REC/set_view. */

    vmap_perf_frame_begin();
    VMAP_PERF_T0();
    dvfs_map_busy(true);
    view->pump_dirty_n = 0;
    sx = (view->last_pan_px == INT32_MIN) ? (int32_t)lround(view->pan_dx)
                                         : view->last_pan_px;
    sy = (view->last_pan_py == INT32_MIN) ? (int32_t)lround(view->pan_dy)
                                         : view->last_pan_py;

    if (reset_view) {
        /*
         * A hidden page can jump much farther than the last on-screen pan.
         * Derive the pixel migration from the two geographic origins instead
         * of trusting last_pan_px.  Otherwise a 1 km jump with a 14 px old pan
         * scrolls only 14 px, then marks the whole interior as the new origin.
         */
        if (!force_clear && view->render_rev > 0
            && view->paint_zoom == view->zoom
            && view->paint_scale == view->scale) {
            int old_x = 0;
            int old_y = 0;
            int new_x = 0;
            int new_y = 0;

            vmap_latlong_to_pixel(view->render_lat, view->render_lon,
                view->zoom, &old_x, &old_y);
            vmap_latlong_to_pixel(origin_lat, origin_lon,
                view->zoom, &new_x, &new_y);
            sx = (int32_t)lround((new_x - old_x) * view->scale);
            sy = (int32_t)lround((new_y - old_y) * view->scale);
        }

        bool can_scroll = !force_clear && view->render_rev > 0
            && view->paint_zoom == view->zoom
            && view->paint_scale == view->scale
            && (sx != 0 || sy != 0)
            && sx > -view->w && sx < view->w
            && sy > -view->h && sy < view->h;

        view->render_lon = origin_lon;
        view->render_lat = origin_lat;
        view_canvas(view, &canvas, false);
        if (can_scroll) {
            {
                VMAP_PERF_T0S();
                vmap_render_scroll(&canvas, sx, sy, vmap_style_bg565());
                pump_dirty_from_scroll(view, sx, sy);
                VMAP_PERF_T1S(scroll);
            }
            {
                VMAP_PERF_T0S();
                vmap_label_retained_scroll(&view->label_col, sx, sy,
                    VMAP_RENDER_DIRTY_MAX, &view->pump_dirty_n,
                    view->pump_dirty_x1, view->pump_dirty_y1,
                    view->pump_dirty_x2, view->pump_dirty_y2);
                VMAP_PERF_T1S(lscroll);
            }
            scrolled = true;
            {
                VMAP_PERF_T0S();
                lv_obj_invalidate(view->canvas);
                VMAP_PERF_T1S(inval);
            }
        } else {
            VMAP_PERF_T0S();
            vmap_render_clear(&canvas, vmap_style_bg565());
            vmap_label_retained_reset(&view->label_col);
            VMAP_PERF_T1S(scroll);  /* 清屏路径复用同一格 */
        }

        view->pan_dx = 0.0;
        view->pan_dy = 0.0;
        view->last_pan_px = INT32_MIN;
        view->last_pan_py = INT32_MIN;
        vmap_view_apply_canvas_pos(view, 0.0, 0.0);
    } else {
        /* keep-pan redraw repaints every base tile, erasing all baked labels. */
        vmap_label_retained_reset(&view->label_col);
    }

    vmap_label_collector_reset(&view->label_col);
    view->label_col.clip_n = 0;
    vmap_label_collector_set_focus(&view->label_col,
        view->w / 2 + (int32_t)lround(view->pan_dx),
        view->h / 2 + (int32_t)lround(view->pan_dy));

    vmap_latlong_to_pixel(origin_lat, origin_lon, view->zoom,
        &view->pump_cx, &view->pump_cy);
    tile_range(view->pump_cx, view->pump_cy, view->w, view->h, view->scale, 1,
        &view->pump_tx_min, &view->pump_ty_min,
        &view->pump_tx_max, &view->pump_ty_max);
    view->pump_tx = view->pump_tx_max;
    view->pump_ty = view->pump_ty_max;
    view->pump_phase = 1;
    view->pump_need_region = 1;

    if (scrolled) {
        syslog(LOG_INFO,
            "[map] rebase scroll=%d,%d dirty=%u labels=%d z=%d",
            (int)sx, (int)sy, (unsigned)view->pump_dirty_n,
            (int)view->label_col.retained_count, view->zoom);
    }
    VMAP_PERF_T1(begin);
}

static bool render_next_tile(vmap_view_t * view)
{
    const int32_t tile_size = 256;
    const double scale = view->scale;
    vmap_canvas_t canvas;
    vmap_tile_t tile;
    float base_x;
    float base_y;
    int32_t tx;
    int32_t ty;
    int32_t ix1;
    int32_t iy1;
    int32_t ix2;
    int32_t iy2;

    if (view->pump_need_region) {
        VMAP_PERF_T0();
        view->pump_need_region = 0;
        if (view->cache) {
            myvendor_watchdog_ui_beat();
            myvendor_watchdog_busy_pump();
            vmap_tile_cache_ensure_region(view->cache,
                view->render_lon, view->render_lat);
            myvendor_watchdog_ui_beat();
        }
        VMAP_PERF_T1(warm);
        return true;
    }

    while (view->pump_tx >= view->pump_tx_min) {
        tx = view->pump_tx;
        ty = view->pump_ty;

        if (!pump_tile_dirty(view, tx, ty)) {
            VMAP_PERF_T0();
            if (view->font
                && vmap_tile_cache_cached(view->cache, view->zoom, (int)tx, (int)ty)
                && vmap_tile_cache_get(view->cache, view->zoom, (int)tx, (int)ty,
                    &tile)) {
                base_x = (float)((tx * tile_size - view->pump_cx) * scale
                    + view->w / 2.0);
                base_y = (float)((ty * tile_size - view->pump_cy) * scale
                    + view->h / 2.0);
                vmap_label_collector_add_tile(&view->label_col, &tile,
                    base_x, base_y, (float)scale);
            }
            VMAP_PERF_T1(meta);
            if (!pump_advance(view)) {
                return false;
            }
            continue;
        }

        if (!vmap_tile_cache_cached(view->cache, view->zoom, (int)tx, (int)ty)) {
            bool warmed;

            VMAP_PERF_T0();
            warmed = vmap_tile_cache_warm(view->cache, view->zoom,
                    (int)tx, (int)ty);
            VMAP_PERF_T1(warm);
            if (!warmed) {
                /* 邻格 heap 暂时放不下：跳过，继续画已在 PSRAM 的瓦片。 */
                if (!pump_advance(view)) {
                    return false;
                }
                continue;
            }
            VMAP_PERF_N(n_warms);
            return true;
        }

        view_canvas(view, &canvas, true);
        base_x = (float)((tx * tile_size - view->pump_cx) * scale
            + view->w / 2.0);
        base_y = (float)((ty * tile_size - view->pump_cy) * scale
            + view->h / 2.0);

        if (vmap_tile_cache_get(view->cache, view->zoom, (int)tx, (int)ty,
                &tile)) {
            uint32_t st = 0u;
            uint32_t by = 0u;

            VMAP_PERF_T0();
            vmap_render_stat_reset();
            vmap_render_draw_tile(&canvas, &tile, base_x, base_y, (float)scale,
                view->scratch, VMAP_SCRATCH_CAP);
            VMAP_PERF_T1(tile);
            VMAP_PERF_N(n_tiles);
            vmap_render_stat_get(&st, &by);
            s_pf_stores += st;
            s_pf_bytes += by;
            if (view->font) {
                vmap_label_collector_add_tile(&view->label_col, &tile,
                    base_x, base_y, (float)scale);
            }

            ix1 = (int32_t)floor((double)base_x);
            iy1 = (int32_t)floor((double)base_y);
            ix2 = (int32_t)ceil((double)base_x + 256.0 * scale);
            iy2 = (int32_t)ceil((double)base_y + 256.0 * scale);
            if (view->pump_dirty_n == 0) {
                vmap_view_invalidate_canvas_area(view, ix1, iy1, ix2, iy2);
            } else {
                uint8_t di;

                for (di = 0; di < view->pump_dirty_n; di++) {
                    int32_t vx1 = ix1 > view->pump_dirty_x1[di]
                        ? ix1 : view->pump_dirty_x1[di];
                    int32_t vy1 = iy1 > view->pump_dirty_y1[di]
                        ? iy1 : view->pump_dirty_y1[di];
                    int32_t vx2 = ix2 < (view->pump_dirty_x2[di] - 1)
                        ? ix2 : (view->pump_dirty_x2[di] - 1);
                    int32_t vy2 = iy2 < (view->pump_dirty_y2[di] - 1)
                        ? iy2 : (view->pump_dirty_y2[di] - 1);

                    if (vx1 <= vx2 && vy1 <= vy2) {
                        vmap_view_invalidate_canvas_area(view, vx1, vy1, vx2, vy2);
                    }
                }
            }
        }

        if (!pump_advance(view)) {
            return false;
        }
        return true;
    }

    return false;
}

/*
 * Canvas stack, bottom → top:
 *   map tiles (already in buffer) → nav → labels → REC line → REC numbers.
 * clip_dirty: follow-rebase only restamps the new edge, so interior ink is not
 * redrawn fat. Labels use the same dirty rects.
 */
static void stamp_overlays(vmap_view_t * view, bool labels)
{
    vmap_canvas_t canvas;
    lv_area_t label_drawn;
    bool labels_drawn = false;

    if (!view || !view->canvas || !view->cbuf) {
        return;
    }

    view_canvas(view, &canvas, true);

#if VMAP_ROUTE_ENABLE
    if (view->route) {
        VMAP_PERF_T0();
        vmap_route_paint_to_canvas(view->route, &canvas, view);
        VMAP_PERF_T1P(VMAP_PERF_NAV_SLOT());
    }
#endif

    if (labels && view->font && view->label_col.count > 0) {
        VMAP_PERF_T0();
        labels_drawn = vmap_label_paint_to_canvas(
            view->canvas, &view->label_col, view->font,
            view->scale, view->pump_dirty_n,
            view->pump_dirty_x1, view->pump_dirty_y1,
            view->pump_dirty_x2, view->pump_dirty_y2, &label_drawn);
        VMAP_PERF_T1(label);
        VMAP_PERF_NB(n_labels, view->label_col.count);
    }

#if VMAP_TRACK_ENABLE
    if (view->track) {
        if (labels_drawn && canvas.clip_n > 0u) {
            int32_t x1 = label_drawn.x1;
            int32_t y1 = label_drawn.y1;
            int32_t x2 = label_drawn.x2 + 1;
            int32_t y2 = label_drawn.y2 + 1;
            uint8_t i;

            /*
             * New labels may be outside the tile dirty edge. Expand the REC
             * clip to their union so track ink is restored above those words.
             */
            for (i = 0; i < canvas.clip_n; i++) {
                if (canvas.clip_x1[i] < x1) {
                    x1 = canvas.clip_x1[i];
                }
                if (canvas.clip_y1[i] < y1) {
                    y1 = canvas.clip_y1[i];
                }
                if (canvas.clip_x2[i] > x2) {
                    x2 = canvas.clip_x2[i];
                }
                if (canvas.clip_y2[i] > y2) {
                    y2 = canvas.clip_y2[i];
                }
            }
            vmap_render_clip_reset(&canvas);
            vmap_render_clip_add(&canvas, x1, y1, x2, y2);
        }
        VMAP_PERF_T0();
        vmap_track_paint_to_canvas(view->track, &canvas);
        VMAP_PERF_T1P(VMAP_PERF_TRACK_SLOT());
    }
#endif
}

void vmap_view_stamp_nav_then_track(vmap_view_t * view)
{
    stamp_overlays(view, false);
    if (view && view->canvas) {
        lv_obj_invalidate(view->canvas);
    }
    vmap_perf_stamp_end((uint32_t)(view ? view->zoom : 0));
}

static void render_finish(vmap_view_t * view)
{
    stamp_overlays(view, true);
    /* 必须在 dvfs_map_busy(false) 之前：MAP hold 期间 HCLK 才是被钉住的那档。
     * 也要在 pump_dirty_n 被清零之前取，脏区条数是判"全量重画 / scroll 补边"的依据。 */
    vmap_perf_frame_end((uint32_t)view->zoom, (uint32_t)view->pump_dirty_n);

#if defined(CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE) && CONFIG_MYVENDOR_BICYCLE_INVAL_PROBE
    bicycle_inval_probe_tag("vmap_render");
#endif
    view->render_rev++;
    view->paint_zoom = view->zoom;
    view->paint_scale = view->scale;
    view->pump_phase = 0;
    view->pump_need_region = 0;
    view->pump_dirty_n = 0;
    lv_obj_clear_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    if (view->restamp_pending && !myvendor_mtp_lfs_quiesce()) {
        view->restamp_pending = 0;
        lv_obj_invalidate(view->canvas);
        render_begin(view, false, false);
        return;
    }
    dvfs_map_busy(false);
    lv_obj_invalidate(view->canvas);
    vmap_view_bring_arrow_to_front(view);
}

/**
 * @brief 开始一帧栅格化（只 begin，由 pump 画完）。
 */
void vmap_view_render(vmap_view_t * view)
{
    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return;
    }

    view->restamp_pending = 0;
    render_begin(view, true, false);
}

void vmap_view_redraw_keep_pan(vmap_view_t * view)
{
    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return;
    }
    if (view->render_rev == 0) {
        return;
    }
    if (view->pump_phase != 0) {
        view->restamp_pending = 1;
        syslog(LOG_NOTICE, "[map] restamp queued (pump busy)");
        return;
    }

    view->restamp_pending = 0;
    render_begin(view, false, false);
    syslog(LOG_NOTICE, "[map] restamp keep_pan z=%d tiles tx=%d..%d ty=%d..%d",
        view->zoom, (int)view->pump_tx_min, (int)view->pump_tx_max,
        (int)view->pump_ty_min, (int)view->pump_ty_max);
}

bool vmap_view_render_pump(vmap_view_t * view, unsigned max_tiles)
{
    unsigned n;
    uint32_t t0;

    if (!view || !view->canvas || !view->cbuf || myvendor_mtp_lfs_quiesce()) {
        return true;
    }

    if (view->pump_phase == 0) {
        /* 没有帧在飞 ⇒ 泵本来就不该被调用（帧结束后定时器自己暂停）。
         * **基准必须作废**，否则下一帧的第一拍会把"这一整段空闲"算成"UI 线程被占住"
         * —— 第一版就是这么误报的：`SLOW pump gap 9471ms` / `2228ms` 其实都是
         * "这段时间没渲染"（同期 BLE 每 10 秒的心跳照打，证明系统是活的）。 */
        s_pf_last_pump_cyc = 0u;
        return true;
    }

    if (max_tiles == 0) {
        max_tiles = VMAP_RENDER_PUMP_MAX_TILES;
    }

    t0 = lv_tick_get();
    myvendor_watchdog_ui_beat();
    /* 单拍间隔超长 ⇒ **无条件** WARNING（正常帧不受影响）。只在这一帧确实在飞时量，
     * 所以"没渲染的空闲"不会被算进来；而帧内一次 2 秒以上的拍间隔就只能是系统级
     * 停顿（现场两次 WDT 饥饿复位都伴随 1.6~12 秒的这类间隔，且当时 [vperf] 正被
     * 降到 debug、整场不可诊断）。 */
    {
        const uint32_t now_cyc = vmap_perf_cyc();

        if (s_pf_last_pump_cyc != 0u && now_cyc - s_pf_last_pump_cyc
            > VMAP_PERF_SLOW_GAP_MS * 240u * 1000u) {
            syslog(LOG_WARNING,
                "[vperf] SLOW pump gap %ums (>%ums) —— 帧内拍间隔异常，UI 线程被占住；若同时 WDT 停喂即饥饿族",
                (unsigned)((now_cyc - s_pf_last_pump_cyc) / 240000u),
                (unsigned)VMAP_PERF_SLOW_GAP_MS);
            vmap_perf_dump_runnable();
        }
        s_pf_last_pump_cyc = now_cyc;
    }

    for (n = 0; n < max_tiles && view->pump_phase != 0; n++) {
        if (n > 0 && lv_tick_elaps(t0) >= VMAP_RENDER_PUMP_BUDGET_MS) {
            break;
        }
        if (!render_next_tile(view)) {
            render_finish(view);
            break;
        }
        if (lv_tick_elaps(t0) >= VMAP_RENDER_PUMP_BUDGET_MS) {
            break;
        }
    }

    return view->pump_phase == 0;
}

bool vmap_view_render_busy(const vmap_view_t * view)
{
    return view != NULL && view->pump_phase != 0;
}

/**
 * @brief 地图存储世代（MTP reload 后递增）。
 */
uint32_t vmap_view_map_revision(const vmap_view_t * view)
{
    return view ? view->map_rev : 0;
}

/**
 * @brief 已完成的渲染帧计数；0 表示从未 render_finish。
 */
uint32_t vmap_view_render_revision(const vmap_view_t * view)
{
    return view ? view->render_rev : 0;
}
