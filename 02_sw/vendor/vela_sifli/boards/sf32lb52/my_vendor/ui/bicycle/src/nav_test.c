/**
 * @file nav_test.c
 * @brief `ctl nav` 的实现：脱离 App 复现/对比"直规 vs 走廊(rolling)"。
 *
 * 为什么要有它：现场（2026-09-19）发现**同一对起终点**上两条规划器给出的路线长度
 * 差 1.8~2.4 倍（直规 1600/1986 m，rolling 3576/3846 m），导航每秒用的是 rolling，
 * 于是屏幕上的线一直绕远、机动里还频繁出现"掉头"。要定位/回归这件事，必须能在
 * **不开 App、不点地图**的情况下，把两条规划器对同一对点各跑一遍并且看数字 ——
 * 那就是这里做的事：
 *
 *     ctl nav plan <from_lon> <from_lat> <to_lon> <to_lat>      # 直规（worker STANDARD）
 *     ctl nav roll <from_lon> <from_lat> <to_lon> <to_lat>      # 走廊（worker ROLLING）
 *     ctl nav both <from_lon> <from_lat> <to_lon> <to_lat>      # 先直规再 rolling，并给出比值
 *     ctl nav status | stop
 *
 * 两条都走 `vmap_route_worker_*`（与 App 同一条路径、同一个回调形状），所以打印出来的
 * pts/len/man 就是导航真正会拿到的东西；`both` 还会打一行 `nav: DIRECT vs ROLLING`，
 * 差超过阈值时标 **WORSE**，用来一眼判断 rolling 是否在绕路。
 *
 * 放在 UI 侧（`ui/bicycle/src/`）而不是 ctl 应用里：vmap 的头文件只在这个 target 的
 * include 路径上（见 docs/ 里"include 分层"那条）；ctl 只 extern 一个入口函数。
 */

#include <nuttx/config.h>

#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "myvendor_bicycle_ctl.h"

/* UI 侧（map_page.c）：把真导航用的路线折线打出来。 */
extern uint32_t map_page_debug_route_dump(void);

#include "vmap/vmap_config.h"
#include "vmap/vmap_map_catalog.h"
#include "vmap/vmap_route.h"
#include "vmap/vmap_route_graph.h"
#include "vmap/vmap_route_worker.h"

/** 等 worker 回调的上限（地图 I/O + Dijkstra，实测百毫秒级；给足余量）。 */
#define NAV_TEST_WAIT_S     20
/** 提交前等上一条 job 收尾的上限。 */
#define NAV_TEST_IDLE_WAIT_MS  5000
/** 打印折线坐标点的上限（够看一条城市路线；最多 2048）。 */
#define NAV_TEST_PT_MAX     64

typedef struct
{
  sem_t     done;
  /** 命令超时返回后置位：回调仍在 **UI 线程**（`lv_async_call`）上跑，
   * 此时 `out` 所在的 NSH 栈帧可能已经被改写 —— 回调只能"报告没人收"，
   * 绝不能再往 `out` 里写。 */
  volatile bool abandoned;
  bool      ok;
  uint32_t  pt_count;
  uint32_t  man_count;
  double    total_m;
  uint32_t  regions;
  vmap_route_job_kind_t kind;
  /** 路线折线（**MCU 自己算出来的坐标点**，供与参考路线/GPX 逐点比对）。 */
  struct
  {
    double lon;
    double lat;
  } pt[NAV_TEST_PT_MAX];
  /** 前几个机动点，按工人回调里的顺序打印（road_name 是图里的常量串）。 */
  struct
  {
    const char * road;
    double       along_m;
    float        turn;
  } man[6];
} nav_test_result_t;

static void nav_test_done(void * user, bool ok, vmap_route_job_kind_t kind,
    uint32_t regions_planned, const vmap_route_pt_t * pts, uint32_t pt_count,
    double total_m, const vmap_route_maneuver_t * maneuvers,
    uint32_t maneuver_count)
{
  nav_test_result_t * r = (nav_test_result_t *)user;
  uint32_t i;

  (void)pts;

  if (r->abandoned)
    {
      printf("nav: result dropped (timed out) kind=%d ok=%d pts=%u len=%.0fm\n",
             (int)kind, ok ? 1 : 0, (unsigned)pt_count, total_m);
      return;
    }

  r->ok        = ok;
  r->kind      = kind;
  r->regions   = regions_planned;
  r->pt_count  = pt_count;
  r->total_m   = total_m;
  r->man_count = maneuver_count;

  for (i = 0; i < pt_count && i < NAV_TEST_PT_MAX; i++)
    {
      r->pt[i].lon = pts[i].lon;
      r->pt[i].lat = pts[i].lat;
    }

  for (i = 0; i < maneuver_count && i < 6u; i++)
    {
      r->man[i].road    = maneuvers[i].road_name;
      r->man[i].along_m = maneuvers[i].along_m;
      r->man[i].turn    = maneuvers[i].turn_deg;
    }

  sem_post(&r->done);
}

static void nav_test_print(const char * tag, const nav_test_result_t * r)
{
  uint32_t i;

  printf("nav %-7s ok=%d kind=%d regions=%u pts=%u man=%u len=%.0fm\n",
         tag, r->ok ? 1 : 0, (int)r->kind, (unsigned)r->regions,
         (unsigned)r->pt_count, (unsigned)r->man_count, r->total_m);

  for (i = 0; i < r->man_count && i < 6u; i++)
    {
      printf("nav   %-7s man[%u] along=%.0fm turn=%.0f road=%s\n",
             tag, (unsigned)i, r->man[i].along_m, (double)r->man[i].turn,
             r->man[i].road ? r->man[i].road : "-");
    }

  /* 折线坐标点：**MCU 自己算出来的路线**，一行一点，直接用 `nav pt` 前缀抓成 TSV
   * 跟参考路线（或 GPX 轨迹）逐点比。 */
  for (i = 0; i < r->pt_count && i < NAV_TEST_PT_MAX; i++)
    {
      printf("nav pt %-7s %u %.7f %.7f\n", tag, (unsigned)i,
             r->pt[i].lon, r->pt[i].lat);
    }
}

/** @brief 跑一次规划并等结果。@return true = 拿到结果（哪怕是失败的结果）。 */
static bool nav_test_run(bool rolling, double from_lon, double from_lat,
    double to_lon, double to_lat, nav_test_result_t * out)
{
  static bool inited;
  static vmap_route_graph_t * graph;
  int rc;

  memset(out, 0, sizeof(*out));
  sem_init(&out->done, 0, 0);

  if (!inited)
    {
      vmap_route_worker_init();
      inited = true;
    }

  /* 图只装一次（`VMAP_TILE_DIR` 是编译期常量，运行期不会换目录）。
   *
   * **照 UI 的做法走"按区域"那条**（map_page.c:795 起）：本板的图是每区域一个
   * `rNNN.vpk`，`load_from_map_dir()`（map.idx + 共享 vpk 那套）在这里是失败的
   * —— 2026-09-19 实测 `nav: graph load failed (/mnt/fat/map)`。所以先
   * `catalog_load` + `find_region(from)` 拿区域号，再 `load_from_region()`。 */
  if (graph == NULL)
    {
      vmap_cell_id_t rid = VMAP_CELL_NONE;

      if (!vmap_map_catalog_load(VMAP_TILE_DIR) ||
          !vmap_map_catalog_find_region(from_lon, from_lat, &rid))
        {
          printf("nav: catalog/region lookup failed (%s)\n", VMAP_TILE_DIR);
          return false;
        }

      if (!vmap_route_graph_load_from_region(&graph, VMAP_TILE_DIR, rid))
        {
          printf("nav: graph load failed (%s rid=%lu)\n", VMAP_TILE_DIR,
                 (unsigned long)rid);
          return false;
        }

      printf("nav: graph ready region=%lu\n", (unsigned long)rid);
    }

  /* **先等空闲，再提交**。`busy()` 是 `has_job || running`（vmap_route_worker.c:328），
   * 而 `cancel()` 只把**排队中**那条 job 的 cb 置空，正在跑的取消不掉 —— 所以
   * "cancel + 100 ms 再提交" 会直接撞上 `submit failed`，看不出到底是"上一条卡住"
   * 还是"抢不到"。这里改成有界等待 + 把状态打出来：超时说明上一条**真的没跑完**。 */
  if (vmap_route_worker_busy())
    {
      int waited_ms = 0;

      printf("nav: worker busy, waiting...\n");
      while (vmap_route_worker_busy() && waited_ms < NAV_TEST_IDLE_WAIT_MS)
        {
          usleep(50 * 1000);
          waited_ms += 50;
        }

      if (vmap_route_worker_busy())
        {
          printf("nav: worker still busy after %d ms -> cancel queued job\n",
                 waited_ms);
          vmap_route_worker_cancel();
          usleep(200 * 1000);
        }
      else
        {
          printf("nav: worker idle after %d ms\n", waited_ms);
        }
    }

  if (rolling)
    {
      vmap_route_rolling_req_t req;

      memset(&req, 0, sizeof(req));
      req.map_dir     = VMAP_TILE_DIR;
      req.from_lon    = from_lon;
      req.from_lat    = from_lat;
      req.to_lon      = to_lon;
      req.to_lat      = to_lat;
      req.region_begin = 0;
      req.max_regions  = VMAP_ROUTE_REFINE_BATCH;

      if (!vmap_route_worker_submit_rolling(&req, nav_test_done, out))
        {
          printf("nav: submit_rolling failed (worker busy=%d)\n",
                 vmap_route_worker_busy() ? 1 : 0);
          return false;
        }
    }
  else if (!vmap_route_worker_submit(VMAP_TILE_DIR, graph,
               from_lon, from_lat, to_lon, to_lat, nav_test_done, out))
    {
      printf("nav: submit failed (worker busy=%d)\n",
             vmap_route_worker_busy() ? 1 : 0);
      return false;
    }

  /* **有界**等待（绝不能无限等：NSH 会被挂死）。回调不在 worker 线程上跑 ——
   * worker 收尾走 `lv_async_call(route_worker_async_done, …)`
   * （vmap_route_worker.c:241），也就是**在 LVGL/UI 线程上**执行；UI 线程被占住
   * 时回调就不会来。所以超时要区分"规划没算完"和"算完了没人送"：
   * 每秒醒一次并打点，超时后置 `abandoned`（回调若晚到只打印、不再写栈）。 */
  {
    int waited_s = 0;

    while (waited_s < NAV_TEST_WAIT_S)
      {
        struct timespec ts;

        (void)clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        rc = sem_timedwait(&out->done, &ts);
        if (rc == 0)
          {
            break;
          }

        waited_s++;
        if (waited_s % 5 == 0)
          {
            printf("nav: still waiting %d s (worker busy=%d)\n", waited_s,
                   vmap_route_worker_busy() ? 1 : 0);
          }
      }

    if (rc != 0)
      {
        printf("nav: wait timeout after %d s (errno=%d worker busy=%d)\n",
               NAV_TEST_WAIT_S, errno, vmap_route_worker_busy() ? 1 : 0);
        out->abandoned = true;
        return false;
      }
  }

  return true;
}

int myvendor_nav_test(int argc, char ** argv)
{
  const char * cmd;
  bool do_direct = false;
  bool do_rolling = false;
  double from_lon;
  double from_lat;
  double to_lon;
  double to_lat;
  nav_test_result_t direct;
  nav_test_result_t rolling;

  if (argc < 2)
    {
      printf("usage: ctl nav plan|roll|both <from_lon> <from_lat> <to_lon> <to_lat>\n");
      printf("       ctl nav ui <to_lon> <to_lat>   # 真导航（UI rolling/trip）\n");
      printf("       ctl nav dump | status | stop | wedge\n");
      return 1;
    }

  cmd = argv[1];

  if (strcmp(cmd, "status") == 0)
    {
      printf("nav: worker_busy=%d alive=%d revives=%d steps=%lu dir=%s\n",
             vmap_route_worker_busy() ? 1 : 0,
             vmap_route_worker_alive() ? 1 : 0,
             vmap_route_worker_revives(),
             (unsigned long)vmap_route_worker_steps(), VMAP_TILE_DIR);
      return 0;
    }

  /* **真正的导航**：把规划交给 UI（和 `bicycle_nsh nav` 走同一条路 ——
   * `map_page` 的 rolling/trip 规划 + `vmap_route_nav` 跟踪）。
   * `ctl nav plan|roll` 只是规划器单测，**不代表导航的真实路径**。 */
  if (strcmp(cmd, "ui") == 0 && argc >= 4)
    {
      char payload[64];

      if (!myvendor_bicycle_ctl_ui_alive())
        {
          printf("nav: UI not running\n");
          return 1;
        }

      snprintf(payload, sizeof(payload), "%s,%s", argv[2], argv[3]);
      if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN, payload) != 0)
        {
          printf("nav: post failed\n");
          return 1;
        }

      printf("nav: UI NAV_PLAN posted (%s)\n", payload);
      return 0;
    }

  /* 真导航的路线折线（和 `ctl nav ui` 配合看）：用来判断"轨迹线缺失"是规划路线
   * 点数太少/有断口，还是别的原因。 */
  if (strcmp(cmd, "dump") == 0)
    {
      (void)map_page_debug_route_dump();
      return 0;
    }

  if (strcmp(cmd, "stop") == 0)
    {
      vmap_route_worker_cancel();
      printf("nav: cancel requested\n");
      return 0;
    }

  /* 故障注入：造"有 job 挂着却没人取"的卡死现场，验证提交前的自愈。
   * 用法：`ctl nav wedge` → 下一次提交（或 15 s 后再 status）应看到
   * `worker: stalled/stalled cap ... -> revive` + `worker: revived #1`，
   * 之后规划恢复正常（不需要重启板子）。 */
  if (strcmp(cmd, "wedge") == 0)
    {
      vmap_route_worker_init();
      vmap_route_worker_test_stall();
      printf("nav: stall injected (busy=%d)\n",
             vmap_route_worker_busy() ? 1 : 0);
      return 0;
    }

  if (strcmp(cmd, "plan") == 0)
    {
      do_direct = true;
    }
  else if (strcmp(cmd, "roll") == 0)
    {
      do_rolling = true;
    }
  else if (strcmp(cmd, "both") == 0)
    {
      do_direct = true;
      do_rolling = true;
    }
  else
    {
      printf("nav: unknown subcmd %s\n", cmd);
      return 1;
    }

  if (argc < 6)
    {
      printf("nav: need 4 coordinates\n");
      return 1;
    }

  from_lon = atof(argv[2]);
  from_lat = atof(argv[3]);
  to_lon   = atof(argv[4]);
  to_lat   = atof(argv[5]);

  /* 裸坐标打一遍（和 App 下发的是同一对数；日志里对齐 nav 的 raw/snap 用）。 */
  printf("nav: request from (%.7f,%.7f) to (%.7f,%.7f)\n",
         from_lon, from_lat, to_lon, to_lat);
  syslog(LOG_INFO, "nav test: from (%.7f,%.7f) to (%.7f,%.7f)\n",
         from_lon, from_lat, to_lon, to_lat);

  if (do_direct && nav_test_run(false, from_lon, from_lat, to_lon, to_lat,
                                &direct))
    {
      nav_test_print("direct", &direct);
    }

  if (do_rolling && nav_test_run(true, from_lon, from_lat, to_lon, to_lat,
                                 &rolling))
    {
      nav_test_print("rolling", &rolling);
    }

  if (do_direct && do_rolling && direct.ok && rolling.ok && direct.total_m > 1.0)
    {
      const double ratio = rolling.total_m / direct.total_m;

      printf("nav: DIRECT vs ROLLING  %.0fm vs %.0fm  ratio=%.2f%s\n",
             direct.total_m, rolling.total_m, ratio,
             ratio > 1.25 ? "  **WORSE**" : "");
      syslog(LOG_WARNING, "nav: direct=%.0fm rolling=%.0fm ratio=%.2f%s\n",
             direct.total_m, rolling.total_m, ratio,
             ratio > 1.25 ? " worse" : "");
    }

  return 0;
}
