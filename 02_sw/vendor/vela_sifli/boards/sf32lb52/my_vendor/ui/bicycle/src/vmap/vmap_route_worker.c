/**
 * @file vmap_route_worker.c
 * @brief vmap route_worker 模块。
 */

#include "vmap_route_worker.h"

#include "vmap_alloc.h"
#include "vmap_config.h"
#include "vmap_route_log.h"
#include "board_malloc.h"
#include "lvgl/lvgl.h"
#include <nuttx/clock.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 提交前自愈的参数：
 *  - `VMAP_ROUTE_WORKER_STUCK_MS`：排队/运行中的 job 在这么久里**步进数不变**就判卡死。
 *    规划器每个 Dijkstra 步都 `vmap_route_plan_yield()`（约 4 ms），所以正常的慢规划
 *    步进数是持续增长的；15 s 不动只可能是线程没了或算法卡住。
 *  - `VMAP_ROUTE_WORKER_REVIVE_MAX`：重生次数上限（每次泄漏一块 PSRAM 旧栈，见
 *    `route_worker_spawn()`），到顶后只报错不再重生，避免把 PSRAM 漏干。 */
#define VMAP_ROUTE_WORKER_STUCK_MS   15000u
#define VMAP_ROUTE_WORKER_REVIVE_MAX 3

#ifndef CONFIG_VMAP_ROUTE_WORKER_STACK
/* 128 KiB BoardPSRAM 栈：规划帧 + corridor 局部表，不占 SRAM Umem。
 *
 * 2026-09-19 从 64 KiB 提到 128 KiB：跨区（corridor）路径把门户候选池、分段 scratch 和
 * 两层嵌套的 Dijkstra 帧全压在**同一个线程栈**上 —— 实测
 * `ctl nav plan 118.33493 32.24533 118.34908 32.24345` 会 **HardFault**
 * （`[coredump] assert ../../nuttx/arch/arm/src/arm_m/arm_hardfault.c:186 irq=1 writing kv`
 * ，连 panic 打印都没能跑完），是典型的栈溢出症状。 */
#define CONFIG_VMAP_ROUTE_WORKER_STACK 131072
#endif

/* 64 KiB BoardPSRAM 栈：规划帧 + corridor 局部表，不占 SRAM Umem。 */

typedef struct {
    vmap_route_worker_done_cb cb;
    void * user;
    bool ok;
    vmap_route_job_kind_t kind;
    uint32_t regions_planned;
    vmap_route_pt_t pts[VMAP_ROUTE_MAX_PTS];
    uint32_t pt_count;
    double total_m;
    vmap_route_maneuver_t maneuvers[VMAP_ROUTE_MAX_MANEUVERS];
    uint32_t maneuver_count;
} route_plan_result_t;

typedef struct {
    char map_dir[64];
    vmap_route_graph_t * graph;
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;
    vmap_route_job_kind_t kind;
    vmap_route_rolling_req_t rolling;
    vmap_route_worker_done_cb cb;
    void * user;
} route_plan_job_t;

typedef struct {
    sem_t wake;
    pthread_t thread;
    pthread_mutex_t job_lock;
    atomic_bool thread_run;
    atomic_bool cancel;
    atomic_bool has_job;
    atomic_bool running;
    /** 线程体是否还活着：进入时置 true、**退出时置 false**。
     *  线程死了（或被谁删掉）而 has_job 还挂着时，`busy()` 会永远为真 —— 这就是
     *  "导航再也不规划"的直接原因，靠它的 false 触发提交前自愈。 */
    atomic_bool alive;
    /** 线程自己的"我还在跑"时间戳（ticks）：循环顶、取件、交付各打一次。
     *  卡死判定取它与规划器进度时刻的最新值（见 route_worker_ready）。 */
    atomic_uint alive_ticks;
    route_plan_job_t job;
} route_worker_t;

static route_worker_t g_worker;
static atomic_bool g_worker_inited;
static uint8_t * g_worker_stack;
/** 自愈时换下来的旧栈：旧线程可能还在里面跑，**绝不能释放也别复用**；
 *  只在这里存着，等 shutdown 时统一释放。 */
static uint8_t * g_dead_stacks[VMAP_ROUTE_WORKER_REVIVE_MAX];
static int g_dead_stack_n;
static int g_revives;
/** 最近一次**观察到规划步进变化**的时刻/步进值：判死窗口从这里开始算。 */
static uint32_t g_job_progress_ticks;

/**
 * @brief route_worker_result_alloc 接口。
 */
static route_plan_result_t * route_worker_result_alloc(const route_plan_job_t * job,
    bool ok, uint32_t regions_planned, const vmap_route_pt_t * pts,
    uint32_t pt_count, double total_m, const vmap_route_maneuver_t * mans,
    uint32_t man_count)
{
    route_plan_result_t * local;

    if (!job || !job->cb) {
        return NULL;
    }

    local = (route_plan_result_t *)board_malloc_psram(sizeof(*local));
    if (local == NULL || !board_ptr_in_psram_pool(local)) {
        board_mem_free(local);
        VMAP_NAV_ERR("worker: result alloc failed");
        return NULL;
    }

    memset(local, 0, sizeof(*local));
    local->cb = job->cb;
    local->user = job->user;
    local->ok = ok;
    local->kind = job->kind;
    local->regions_planned = regions_planned;

    if (ok && pts != NULL && pt_count >= 2u) {
        local->pt_count = pt_count;
        local->total_m = total_m;
        local->maneuver_count = man_count;
        memcpy(local->pts, pts, (size_t)pt_count * sizeof(local->pts[0]));
        if (mans != NULL && man_count > 0u) {
            memcpy(local->maneuvers, mans,
                (size_t)man_count * sizeof(local->maneuvers[0]));
        }
    }

    return local;
}

/**
 * @brief route_worker_async_done 接口。
 */
static void route_worker_async_done(void * user_data)
{
    route_plan_result_t * r = (route_plan_result_t *)user_data;

    if (r != NULL) {
        if (r->cb != NULL) {
            r->cb(r->user, r->ok, r->kind, r->regions_planned,
                r->ok ? r->pts : NULL, r->pt_count, r->total_m,
                r->ok ? r->maneuvers : NULL, r->maneuver_count);
        }
        board_mem_free(r);
    }
}

/**
 * @brief route_worker_thread 接口。
 */
static void * route_worker_thread(void * arg)
{
    (void)arg;

    atomic_store_explicit(&g_worker.alive, true, memory_order_release);

    for (;;) {
        route_plan_job_t job;

        atomic_store_explicit(&g_worker.alive_ticks, (unsigned)clock_systime_ticks(),
            memory_order_release);
        route_plan_result_t * local = NULL;
        vmap_route_pt_t * pts_tmp = NULL;
        vmap_route_maneuver_t mans[VMAP_ROUTE_MAX_MANEUVERS];
        uint32_t pt_count = 0;
        uint32_t man_count = 0;
        double total_m = 0.0;
        uint32_t regions_planned = 0;
        bool ok = false;
        bool cancel;

        while (atomic_load_explicit(&g_worker.thread_run, memory_order_acquire)
            && !atomic_load_explicit(&g_worker.has_job, memory_order_acquire)) {
            (void)sem_wait(&g_worker.wake);
        }

        if (!atomic_load_explicit(&g_worker.thread_run, memory_order_acquire)) {
            break;
        }

        pthread_mutex_lock(&g_worker.job_lock);
        job = g_worker.job;
        atomic_store_explicit(&g_worker.has_job, false, memory_order_release);
        pthread_mutex_unlock(&g_worker.job_lock);

        VMAP_NAV_OUT("worker: job take kind=%d from (%.6f,%.6f) to (%.6f,%.6f) dir=%s",
            (int)job.kind, job.from_lon, job.from_lat, job.to_lon, job.to_lat,
            job.map_dir);

        atomic_store_explicit(&g_worker.running, true, memory_order_release);
        atomic_store_explicit(&g_worker.alive_ticks, (unsigned)clock_systime_ticks(),
            memory_order_release);
        cancel = atomic_exchange_explicit(&g_worker.cancel, false, memory_order_acq_rel);
        vmap_route_plan_yield();

        if (!cancel && job.cb != NULL) {
            pts_tmp = (vmap_route_pt_t *)board_malloc_psram(
                (size_t)VMAP_ROUTE_MAX_PTS * sizeof(vmap_route_pt_t));
            if (pts_tmp == NULL || !board_ptr_in_psram_pool(pts_tmp)) {
                board_mem_free(pts_tmp);
                VMAP_NAV_ERR("worker: pts scratch alloc failed");
            } else if (job.map_dir[0] == '\0') {
                VMAP_NAV_ERR("worker: missing map_dir");
                board_mem_free(pts_tmp);
            } else if (job.kind == VMAP_ROUTE_JOB_ROLLING) {
                VMAP_NAV_OUT("worker: rolling compute begin");
                ok = vmap_route_compute_rolling(&job.rolling, pts_tmp, &pt_count,
                    &total_m, mans, VMAP_ROUTE_MAX_MANEUVERS, &man_count,
                    &regions_planned);
                VMAP_NAV_OUT("worker: rolling compute %s pts=%u regions=%u",
                    ok ? "ok" : "fail", pt_count, regions_planned);
            } else if (vmap_route_corridor_needed(job.map_dir, job.from_lon,
                    job.from_lat, job.to_lon, job.to_lat)) {
                VMAP_NAV_OUT("worker: corridor compute begin");
                ok = vmap_route_compute_corridor(job.map_dir, job.from_lon,
                    job.from_lat, job.to_lon, job.to_lat, pts_tmp, &pt_count,
                    &total_m, mans, VMAP_ROUTE_MAX_MANEUVERS, &man_count);
                VMAP_NAV_OUT("worker: corridor compute %s pts=%u man=%u",
                    ok ? "ok" : "fail", pt_count, man_count);
            } else if (job.graph != NULL) {
                double snap_src = 0.0;
                double snap_dst = 0.0;

                VMAP_NAV_OUT("worker: compute begin");
                ok = vmap_route_compute(job.graph, job.from_lon, job.from_lat,
                    job.to_lon, job.to_lat, pts_tmp, &pt_count, &total_m,
                    mans, VMAP_ROUTE_MAX_MANEUVERS, &man_count);
                vmap_route_last_snap_m(&snap_src, &snap_dst);
                VMAP_NAV_OUT("worker: compute %s pts=%u man=%u",
                    ok ? "ok" : "fail", pt_count, man_count);

                /* 交给我们的图不是这对点该用的图（骑行者已经换格、UI 还拿着上一格的图，
                 * 或点落在图 clip 之外）时，吸附会被迫拉远 —— 实测
                 * `snap expanded start=150m dest=800m`，并且算出来的路线**出发就掉头 180°**
                 * （方向都反了）。这时改走 corridor：它自己按坐标装区域图；corridor 成功
                 * 且不比直规长，就用它的结果，否则保留直规结果。 */
                if (snap_src > VMAP_ROUTE_SNAP_M || snap_dst > VMAP_ROUTE_SNAP_M) {
                    uint32_t c_pts = 0;
                    uint32_t c_man = 0;
                    double c_total = 0.0;

                    VMAP_NAV_OUT("worker: direct snap far (src=%.0fm dst=%.0fm)"
                        " -> corridor retry", snap_src, snap_dst);
                    if (vmap_route_compute_corridor(job.map_dir, job.from_lon,
                            job.from_lat, job.to_lon, job.to_lat, pts_tmp,
                            &c_pts, &c_total, mans, VMAP_ROUTE_MAX_MANEUVERS,
                            &c_man) && c_pts >= 2u
                        && (!ok || c_total <= total_m)) {
                        ok = true;
                        pt_count = c_pts;
                        total_m = c_total;
                        man_count = c_man;
                        VMAP_NAV_OUT("worker: corridor retry ok pts=%u len=%.0fm",
                            c_pts, c_total);
                    }
                }
            } else {
                VMAP_NAV_ERR("worker: no graph and not corridor");
                board_mem_free(pts_tmp);
                pts_tmp = NULL;
            }

            if (pts_tmp != NULL) {
                if (ok) {
                    local = route_worker_result_alloc(&job, true, regions_planned,
                        pts_tmp, pt_count, total_m, mans, man_count);
                } else {
                    local = route_worker_result_alloc(&job, false, 0, NULL, 0,
                        0.0, NULL, 0);
                }
                board_mem_free(pts_tmp);
            } else {
                local = route_worker_result_alloc(&job, false, 0, NULL, 0,
                    0.0, NULL, 0);
            }
        } else {
            VMAP_NAV_WARN("worker: skipped cancel=%d graph=%p cb=%p",
                cancel ? 1 : 0, (void *)job.graph, (void *)job.cb);
        }

        if (local != NULL) {
            bool skip_done = vmap_route_plan_aborted()
                || atomic_load_explicit(&g_worker.cancel, memory_order_acquire);

            pthread_mutex_lock(&g_worker.job_lock);
            if (g_worker.job.cb == NULL) {
                skip_done = true;
            }
            pthread_mutex_unlock(&g_worker.job_lock);
            if (skip_done) {
                VMAP_NAV_WARN("worker: job result dropped (abort=%d cancel=%d cb=%p)",
                    vmap_route_plan_aborted() ? 1 : 0,
                    atomic_load_explicit(&g_worker.cancel, memory_order_acquire) ? 1 : 0,
                    (void *)g_worker.job.cb);
                board_mem_free(local);
                local = NULL;
            }
        }

        atomic_store_explicit(&g_worker.running, false, memory_order_release);

        if (local == NULL) {
            continue;
        }

        VMAP_NAV_OUT("worker: job deliver kind=%d ok=%d pts=%u -> lv_async",
            (int)local->kind, local->ok ? 1 : 0, (unsigned)local->pt_count);

        (void)lv_async_call(route_worker_async_done, local);

        /* 这一行是**关键探针**：卡死现场（busy 恒真、job 排队却没人取）时若看不到它，
         * 就说明线程卡在 `lv_async_call()` 里（LVGL 堆 malloc / LVGL 锁），而不是规划算法。 */
        VMAP_NAV_OUT("worker: deliver returned");

        atomic_store_explicit(&g_worker.alive_ticks, (unsigned)clock_systime_ticks(),
            memory_order_release);
    }

    /* 走到这里说明线程体自己在退出（thread_run 被置 false）：把存活标记放掉，
     * 提交前自愈就会看到 alive=false 并重生一个新线程。 */
    atomic_store_explicit(&g_worker.alive, false, memory_order_release);
    VMAP_NAV_WARN("worker: thread exit (revives=%d)", g_revives);
    return NULL;
}

/**
 * @brief 起一个规划线程（每次都是**新的 PSRAM 栈**）。
 *
 * 自愈重生时旧线程可能还在跑（我们只是判它不动了），所以绝不复用旧栈 —— 只能泄漏，
 * 指针存进 `g_dead_stacks[]`，等 shutdown 统一释放。
 *
 * @return 0 成功，负 errno 失败。
 */
static int route_worker_spawn(void)
{
    pthread_attr_t attr;
    uint8_t * stack;
    int ret;

    stack = (uint8_t *)board_malloc_psram(CONFIG_VMAP_ROUTE_WORKER_STACK);
    if (stack == NULL || !board_ptr_in_psram_pool(stack)) {
        board_mem_free(stack);
        return -ENOMEM;
    }

    pthread_attr_init(&attr);
    ret = pthread_attr_setstack(&attr, stack, CONFIG_VMAP_ROUTE_WORKER_STACK);
    if (ret == 0) {
        ret = pthread_create(&g_worker.thread, &attr, route_worker_thread, NULL);
    }

    pthread_attr_destroy(&attr);
    if (ret != 0) {
        board_mem_free(stack);
        return -ret;
    }

    g_worker_stack = stack;
    atomic_store_explicit(&g_worker.alive, true, memory_order_release);

#ifndef CONFIG_DISABLE_PTHREAD
    pthread_setname_np(g_worker.thread, "route_worker");
#endif
    {
        struct sched_param sp;

        /* 低于 bicycle_ui（通常 100），规划时 UI 能抢占；idle 仍靠 usleep。 */
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 80;
        (void)pthread_setschedparam(g_worker.thread, SCHED_FIFO, &sp);
    }

    return 0;
}

/**
 * @brief vmap route worker init。
 * @return 0 成功，负 errno 失败。
 */
void vmap_route_worker_init(void)
{
    bool expected = false;
    int ret;

    if (!atomic_compare_exchange_strong(&g_worker_inited, &expected, true)) {
        return;
    }

    memset(&g_worker, 0, sizeof(g_worker));
    sem_init(&g_worker.wake, 0, 0);
    pthread_mutex_init(&g_worker.job_lock, NULL);
    atomic_store_explicit(&g_worker.thread_run, true, memory_order_release);

    g_revives = 0;
    g_dead_stack_n = 0;
    g_job_progress_ticks = clock_systime_ticks();

    ret = route_worker_spawn();
    if (ret < 0) {
        sem_destroy(&g_worker.wake);
        pthread_mutex_destroy(&g_worker.job_lock);
        atomic_store_explicit(&g_worker_inited, false, memory_order_release);
        VMAP_NAV_ERR("worker: thread create failed %d", ret);
    }
}

/**
 * @brief vmap route worker shutdown。
 */
void vmap_route_worker_shutdown(void)
{
    if (!atomic_load_explicit(&g_worker_inited, memory_order_acquire)) {
        return;
    }

    atomic_store_explicit(&g_worker.thread_run, false, memory_order_release);
    atomic_store_explicit(&g_worker.has_job, true, memory_order_release);
    sem_post(&g_worker.wake);
    pthread_join(g_worker.thread, NULL);
    sem_destroy(&g_worker.wake);
    pthread_mutex_destroy(&g_worker.job_lock);
    board_mem_free(g_worker_stack);
    g_worker_stack = NULL;
    while (g_dead_stack_n > 0) {
        board_mem_free(g_dead_stacks[--g_dead_stack_n]);
        g_dead_stacks[g_dead_stack_n] = NULL;
    }

    atomic_store_explicit(&g_worker_inited, false, memory_order_release);
}

/**
 * @brief vmap route worker busy。
 */
bool vmap_route_worker_busy(void)
{
    if (!atomic_load_explicit(&g_worker_inited, memory_order_acquire)) {
        return false;
    }

    return atomic_load_explicit(&g_worker.has_job, memory_order_acquire)
        || atomic_load_explicit(&g_worker.running, memory_order_acquire);
}

/** @brief 线程体是否还活着（见 vmap_route_worker.h）。 */
bool vmap_route_worker_alive(void)
{
    return atomic_load_explicit(&g_worker_inited, memory_order_acquire)
        && atomic_load_explicit(&g_worker.alive, memory_order_acquire);
}

/** @brief 自愈次数（见 vmap_route_worker.h）。 */
int vmap_route_worker_revives(void)
{
    return g_revives;
}

/**
 * @brief 故障注入：造出"有 job 挂着却没人取"的卡死现场（自愈路径的验证用）。
 *
 * 现场记录（2026-09-19）：`route_worker` 卡死时正是这个状态 —— `has_job` 恒真、
 * `busy()` 恒真、submit 恒失败。这里手动置位，等下一次提交触发 `route_worker_ready()`
 * 的判死 + 重生，不需要重启板子。
 */
void vmap_route_worker_test_stall(void)
{
    if (!atomic_load_explicit(&g_worker_inited, memory_order_acquire)) {
        return;
    }

    atomic_store_explicit(&g_worker.has_job, true, memory_order_release);
    VMAP_NAV_WARN("worker: test stall injected (has_job=1, worker not woken)");
}

/** @brief 规划步进总数（见 vmap_route_worker.h）。 */
uint32_t vmap_route_worker_steps(void)
{
    return vmap_route_plan_steps();
}

/**
 * @brief vmap route worker cancel。
 */
void vmap_route_worker_cancel(void)
{
    if (!atomic_load_explicit(&g_worker_inited, memory_order_acquire)) {
        return;
    }

    atomic_store_explicit(&g_worker.cancel, true, memory_order_release);
    vmap_route_plan_abort();
    pthread_mutex_lock(&g_worker.job_lock);
    g_worker.job.cb = NULL;
    g_worker.job.user = NULL;
    pthread_mutex_unlock(&g_worker.job_lock);
}

static bool route_worker_revive(void);

/**
 * @brief 提交前的自愈：worker 线程没了、或挂着 job 太久不动，就地重生一个。
 *
 * 背景（2026-09-19 实测）：`route_worker` 线程消失后 `has_job` 永远挂着，`busy()` 恒真、
 * `submit` 恒失败 —— 表现就是"**导航再也不规划**"（`ps` 里已经看不到 route_worker），
 * 只能重启设备。这里把"永久失效"降级成一次几百毫秒的重生。
 *
 * @return true = 现在可以提交。
 */
static bool route_worker_ready(void)
{
    uint32_t last;
    uint32_t idle_ms;

    vmap_route_worker_init();
    if (!atomic_load_explicit(&g_worker_inited, memory_order_acquire)) {
        return false;
    }

    if (!atomic_load_explicit(&g_worker.has_job, memory_order_acquire)
        && !atomic_load_explicit(&g_worker.running, memory_order_acquire)) {
        return true;    /* 空闲 */
    }

    /* 卡死判定 = "最后进度时刻"离现在多久；三个来源取最新：
     *   1. worker 线程自己的循环 / 取件 / 交付时间戳（`alive_ticks`）；
     *   2. 规划器每次 yield（`vmap_route_plan_progress_ticks()`，约 4 ms 一次）；
     *   3. 本 job 排队或上次重生的时刻（`g_job_progress_ticks`）。
     *
     * 现场（2026-09-19）：卡住时三个都不再更新 —— job 排队了没人取、规划器一步没走，
     * 于是 15 s 后这里判死并重生；而**正常的慢规划**有来源 2 在更新，不会被误判。 */
    last = g_job_progress_ticks;
    if ((int32_t)(atomic_load_explicit(&g_worker.alive_ticks, memory_order_acquire)
                  - last) > 0) {
        last = atomic_load_explicit(&g_worker.alive_ticks, memory_order_acquire);
    }
    if ((int32_t)(vmap_route_plan_progress_ticks() - last) > 0) {
        last = vmap_route_plan_progress_ticks();
    }

    idle_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - last);
    if (idle_ms < VMAP_ROUTE_WORKER_STUCK_MS) {
        return false;   /* 占线但还在动（或还没到判死窗口） */
    }

    VMAP_NAV_ERR("worker: stalled %u ms (alive=%d up=%d run=%d steps=%lu) -> revive",
        (unsigned)idle_ms,
        atomic_load_explicit(&g_worker.alive, memory_order_acquire) ? 1 : 0,
        atomic_load_explicit(&g_worker.has_job, memory_order_acquire) ? 1 : 0,
        atomic_load_explicit(&g_worker.running, memory_order_acquire) ? 1 : 0,
        (unsigned long)vmap_route_plan_steps());

    return route_worker_revive();
}

/**
 * @brief 换一个新线程接管规划（见 route_worker_ready）。
 *
 * 旧线程可能还在跑（我们只是判它不动）：置 `thread_run=false` 让它自己退出，
 * 并把挂着的回调清掉 —— 它算完的结果会被既有的 `skip_done` 丢掉，不会回调到
 * 已经作废的栈上。旧栈登记进 `g_dead_stacks[]` 泄漏掉，绝不复用。
 *
 * @return true = 新线程已就位（调用方可以继续提交）。
 */
static bool route_worker_revive(void)
{
    uint8_t * old_stack;
    int ret;

    if (g_revives >= VMAP_ROUTE_WORKER_REVIVE_MAX) {
        VMAP_NAV_ERR("worker: revive cap reached (%d), giving up", g_revives);
        return false;
    }

    atomic_store_explicit(&g_worker.thread_run, false, memory_order_release);
    pthread_mutex_lock(&g_worker.job_lock);
    g_worker.job.cb = NULL;
    g_worker.job.user = NULL;
    pthread_mutex_unlock(&g_worker.job_lock);

    old_stack = g_worker_stack;
    g_worker_stack = NULL;
    if (old_stack != NULL && g_dead_stack_n < VMAP_ROUTE_WORKER_REVIVE_MAX) {
        g_dead_stacks[g_dead_stack_n++] = old_stack;
    }

    atomic_store_explicit(&g_worker.has_job, false, memory_order_release);
    atomic_store_explicit(&g_worker.running, false, memory_order_release);
    atomic_store_explicit(&g_worker.cancel, false, memory_order_release);
    atomic_store_explicit(&g_worker.thread_run, true, memory_order_release);

    ret = route_worker_spawn();
    if (ret < 0) {
        atomic_store_explicit(&g_worker.thread_run, false, memory_order_release);
        VMAP_NAV_ERR("worker: revive failed %d", ret);
        return false;
    }

    g_revives++;
    g_job_progress_ticks = clock_systime_ticks();
    VMAP_NAV_OUT("worker: revived #%d (leaked stacks=%d)",
        g_revives, g_dead_stack_n);
    return true;
}

/**
 * @brief vmap route worker submit。
 */
bool vmap_route_worker_submit(const char * map_dir, vmap_route_graph_t * graph,
    double from_lon, double from_lat, double to_lon, double to_lat,
    vmap_route_worker_done_cb cb, void * user)
{
    if (!map_dir || map_dir[0] == '\0' || !cb) {
        VMAP_NAV_ERR("submit: bad args map_dir=%p cb=%p", (void *)map_dir, (void *)cb);
        return false;
    }

    if (!vmap_route_corridor_needed(map_dir, from_lon, from_lat, to_lon, to_lat)
        && graph == NULL) {
        VMAP_NAV_ERR("submit: graph required for same-region plan");
        return false;
    }

    if (!route_worker_ready()) {
        VMAP_NAV_ERR("submit: worker not ready");
        return false;
    }

    pthread_mutex_lock(&g_worker.job_lock);
    if (atomic_load_explicit(&g_worker.has_job, memory_order_acquire)
        || atomic_load_explicit(&g_worker.running, memory_order_acquire)) {
        pthread_mutex_unlock(&g_worker.job_lock);
        VMAP_NAV_WARN("submit: worker busy");
        return false;
    }

    g_worker.job.kind = VMAP_ROUTE_JOB_STANDARD;
    memset(&g_worker.job.rolling, 0, sizeof(g_worker.job.rolling));
    strncpy(g_worker.job.map_dir, map_dir, sizeof(g_worker.job.map_dir) - 1);
    g_worker.job.map_dir[sizeof(g_worker.job.map_dir) - 1] = '\0';
    g_worker.job.graph = graph;
    g_worker.job.from_lon = from_lon;
    g_worker.job.from_lat = from_lat;
    g_worker.job.to_lon = to_lon;
    g_worker.job.to_lat = to_lat;
    g_worker.job.cb = cb;
    g_worker.job.user = user;
    vmap_route_plan_abort_clear();
    atomic_store_explicit(&g_worker.cancel, false, memory_order_release);
    g_job_progress_ticks = clock_systime_ticks();
    atomic_store_explicit(&g_worker.has_job, true, memory_order_release);
    pthread_mutex_unlock(&g_worker.job_lock);

    sem_post(&g_worker.wake);
    return true;
}

/**
 * @brief vmap route worker submit rolling。
 */
bool vmap_route_worker_submit_rolling(const vmap_route_rolling_req_t * req,
    vmap_route_worker_done_cb cb, void * user)
{
    if (!req || !req->map_dir || req->map_dir[0] == '\0' || !cb) {
        return false;
    }

    if (!route_worker_ready()) {
        VMAP_NAV_ERR("submit_rolling: worker not ready");
        return false;
    }

    pthread_mutex_lock(&g_worker.job_lock);
    if (atomic_load_explicit(&g_worker.has_job, memory_order_acquire)
        || atomic_load_explicit(&g_worker.running, memory_order_acquire)) {
        pthread_mutex_unlock(&g_worker.job_lock);
        VMAP_NAV_WARN("submit_rolling: worker busy");
        return false;
    }

    memset(&g_worker.job, 0, sizeof(g_worker.job));
    strncpy(g_worker.job.map_dir, req->map_dir, sizeof(g_worker.job.map_dir) - 1);
    g_worker.job.map_dir[sizeof(g_worker.job.map_dir) - 1] = '\0';
    g_worker.job.kind = VMAP_ROUTE_JOB_ROLLING;
    g_worker.job.rolling = *req;
    g_worker.job.rolling.map_dir = g_worker.job.map_dir;
    g_worker.job.from_lon = req->from_lon;
    g_worker.job.from_lat = req->from_lat;
    g_worker.job.to_lon = req->to_lon;
    g_worker.job.to_lat = req->to_lat;
    g_worker.job.cb = cb;
    g_worker.job.user = user;
    vmap_route_plan_abort_clear();
    atomic_store_explicit(&g_worker.cancel, false, memory_order_release);
    g_job_progress_ticks = clock_systime_ticks();
    atomic_store_explicit(&g_worker.has_job, true, memory_order_release);
    pthread_mutex_unlock(&g_worker.job_lock);

    sem_post(&g_worker.wake);
    return true;
}
