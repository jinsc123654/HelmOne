/**
 * @file myvendor_gnss.c
 * @brief 板载 u-blox MAX-M10S-00B-01 NMEA 缓存：后台读 UART，sys/UI 只取快照。
 *
 * 模组：MAX-M10S-00B-01。USART2（/dev/ttyS0）PA31 TX / PA32 RX；PA43 切 VCC
 * （GNSS_PWR）。USART2 用法对齐 NSH USART1：读线程生命周期内只 open 一次，
 * 一直 poll 收；低功耗按 Integration Manual 切模组 VCC，不 close 主机 UART。
 * close(/dev/ttyS0) 会 uart_reset_sem，读线程还在 uart_read 里就会
 * sem_post holder assert。
 *
 * 无定位时仍发布 GGA/GSA/GSV（搜星、可见卫星、HDOP），主界面才能区分
 * 「模组无数据 / 搜星无信号 / 搜星有星 / 2D / 3D」。
 *
 * 经纬度/速度/航向以 RMC 为准。GGA 只补海拔、星数、HDOP；同一秒里 GGA
 * 和 RMC 坐标常差几米，两边都发布地图就会 1 Hz 来回跳。开机把
 * MAX-M10S-00B-01 设成自行车动态模型，并打开静止速度门限，压小区多径晃动。
 * 轨迹均速已经出来且明显高于自行车包络（或 SOG 被 sanity 成 0）时再切车载。
 *
 * 开机（自动同步开着时）从 /mnt/lfs/eph 注入最新 mga_<utc>.ubx。
 * BLE 再写入后热加载。导航库 dump_out 必须让 NMEA 继续走：只发 UBX-MGA-DBD
 * poll，在读线程里旁路收集 0x13/0x80，ACK 或帧间隙后再写成 mga_<utc>.ubx。
 * 下发星历和 dump 同一套 mga_<utc>.ubx 命名，一共最多留 5 份（超了删最老的）。
 * 关 NMEA 独占 UART 会卡住 DMA。旧 dump.ubx / mga.ubx 仅作无名文件回退。
 * 实验室仍可用 `test gnss eph dump`（不要和 GNSS 线程同时开 UART）。
 *
 * UART2 用 DMA 收包，和 NSH 一样一直 poll；模组静默后 poll 超时。无成功
 * publish 数秒则切 PA43 给模组断电再上电（UART 保持 open 并继续收）。任意
 * $… 行不能当活着：坏校验/TXT 会把 fail 清零，断电恢复再也走不到。诊断
 * 重启也只切 VCC。静止休眠同样不关串口，只把 RX 丢掉，不进 NMEA 状态机。
 * 读线程堵在灌星历 / dump / poll 里吃不到 kick 时，diag 按心跳重拉该线程。
 * 重拉必须等读线程真正退出再 pthread_join，禁止在同一块 g_gnss_stack 上
 * 叠第二个读线程（现场 4 个 gnss 同栈把 idle 饿死，IWDT 停喂复位）。
 * 读线程现在**阻塞**在 poll() 上（不再非阻塞自旋），退出靠两样东西：poll 的
 * 超时（只是记账节奏）和 gnss_reader_wake() 发的 SIGUSR1（真正的保证）。
 * 信号不经过 tick，所以 poll() 的 watchdog 即便因 DVFS/SysTick 失效而不再
 * 到期，reap / diag kick 也一定能把线程捅回循环顶。禁止无限 pthread_join
 * （会把 diag 线程一起卡死，BLE 也无法恢复），也不 close USART2。
 * 室内搜星无 fix 不要断电。
 *
 * UART RX 用 circular DMA：IDLE/HT/TC 只把 bounce 新字节拷进软件环，不停
 * DMAR。Normal DMA 在 1 Mbps 调试口上会在 USB 分包间隙丢字节。
 *
 * 禁止在本线程栈上 poll(POLLOUT)。USART2 TX 完成走 uart_datasent →
 * poll_notify(fds->cb)。poll() 返回后栈上的 pollfd 已被复用（现场里
 * 复用成 CFG-NAVSPG-DYNMODEL 0x20110021），ISR 仍拿着旧指针就会
 * HardFault（IBUSERR，PC=0x20110020）。写走 O_NONBLOCK + 短睡；
 * 读用静态 pollfd。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_gnss.h"
#include "myvendor_gnss_log.h"
#include "myvendor_diag.h"
#include "myvendor_schedmon.h"
#include "myvendor_devctl.h"
#include "companion_bridge.h"
#include "myvendor_identity.h"
#include "myvendor_mono.h"
#include "myvendor_gnss_uart.h"   /* 板级直连数据面（ISR 推环形 + 信号量） */
#include "myvendor_watchdog.h"
#include "board_malloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <nuttx/sched.h>
#include <nuttx/wdog.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
#  include "myvendor_mtp.h"
#endif

#ifdef CONFIG_BOARD_L96_GNSS
#  include <minmea/minmea.h>
#  include "sf32lb52_l96.h"
/** 本板模组订货号。低功耗切 VCC（PA43），不 close USART2。 */
#  define GNSS_MOD_PN SF32LB52_GNSS_MOD_PN
#  ifndef CONFIG_BOARD_L96_GNSS_DEVPATH
#    define CONFIG_BOARD_L96_GNSS_DEVPATH "/dev/ttyS0"
#  endif
#  ifndef CONFIG_BOARD_L96_GNSS_BAUD
#    define CONFIG_BOARD_L96_GNSS_BAUD 38400
#  endif
#  ifndef CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS
#    define CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS 1000
#  endif
#  ifndef CONFIG_BOARD_L96_GNSS_PWR_OFF_MS
#    define CONFIG_BOARD_L96_GNSS_PWR_OFF_MS 2500
#  endif
#endif

#define GNSS_STALE_MS            3000u
#define GNSS_SILENCE_MS          5000u
#define GNSS_EPH_GRACE_MS        15000u
#define GNSS_PWR_FAILS           2u
#define GNSS_LINE_MAX       160
#define GNSS_RX_BATCH       128
#define GNSS_RETRY_MS       2000u

/** 静止休眠期（已断电、fd 已关）读线程的轮询间隔。
 *
 *  休眠时读线程不再 poll 串口（PX4 式：fd 已 close），所以必须自己睡一下 ——
 *  主循环是"一轮一个字节"的紧循环，不睡就是空转烧满 CPU。200 ms 是
 *  "唤醒响应够快（人眼看不出延迟）"与"不烧 CPU"之间的取值：唤醒由
 *  `g_idle_sleep` 翻假驱动，最坏多等一个 200 ms 就开始上电。 */
#define GNSS_IDLE_NAP_MS     200u

/* 读线程栈上限（单槽，见下面 g_gnss_stack_sram 的注释）：
 * 实测水位 3,260 B，取 2.5× —— 栈底还有金丝雀 + MPU 只读页，
 * 溢出会当场 fault 而不是静默写坏别人。 */
#define GNSS_STACK_SIZE     8192
#define GNSS_THREAD_PRIO    50
#define GNSS_THREAD_STALL_MS 8000u
#define GNSS_REAP_WAIT_MS   8000u
#define GNSS_REAP_GIVEUP_MS 30000u  /* 连续收不回来多久后升级为显式故障 */
#define GNSS_SLEEP_SPIN_MAX 200000u   /* 已废：自旋上限改按预算缩放，见 gnss_sleep_us() */
#define GNSS_SPIN_YIELD_EVERY 1024u   /* 每 N 次自旋做一次 gnss_yield_all() */
/** @brief 自旋时**一次迭代至少**消耗的周期数，用于把"次数上限"换算成"时间上限"。
 *         取 64 偏保守（实际一次迭代含 DWT 读 + sched_yield，通常几十到几百周期），
 *         宁可上限偏大也不能截断合法的睡眠。 */
#define GNSS_SPIN_MIN_CYCLES_PER_TURN 64u
/** @brief 空读退避：连续多少次"就绪但读空"之后开始让出。 */
#define GNSS_EMPTY_BACKOFF_AFTER   3u
/** @brief 退避档位上限（每档 `GNSS_EMPTY_BACKOFF_STEP_MS` 毫秒）。 */
#define GNSS_EMPTY_BACKOFF_MAX     8u
/** @brief 退避步长（毫秒）。受 100 Hz 时基粒度限制，实际每档约 10 ms。 */
#define GNSS_EMPTY_BACKOFF_STEP_MS 10u
/** @brief "poll 很快就返回"的判据（毫秒）：小于它说明 poll 没起到 pacing 作用，
 *         属于空转，该退避；大于它说明是正常超时，本身已经是节奏。 */
#define GNSS_EMPTY_BACKOFF_FAST_MS 50u
/** @brief 自测记账里"单轮间隔"的合理上界（毫秒）。超过它只计数不累加 ——
 *         因为 Σdt ≡ 窗口长度，超限说明累计器的前提被打破（例如两个读循环
 *         并发，各自读到对方更早的时间戳）。见 g_rd_work_ms 的注释。 */
#define GNSS_WORK_DT_MAX_MS 2000u
/** @brief 读线程 housekeeping（星历/倒库/动态模型/链路报告/静默判定）的节奏（毫秒）。
 *
 *  @details
 *  状态机写在主循环体里，所以 `rx_run` 那一段原本是**每收一个字节**跑一遍
 *  （NMEA ~800 B/s ⇒ ~800 次/秒）。那里每个分支都要过 mutex、几个定时器判断，
 *  而**任何漏了限频的日志都会按字节率刷屏**（2026-09-17 现场：一行刷满屏、
 *  二十多秒）。
 *
 *  加一道时间闸之后这段按 20 Hz 跑。段内每一项都是时间驱动的 —— `link_report`
 *  15/5 s、`dbd_tick` 400 ms/10 s、静默判定 5/15 s、`dyn_apply` 重试 2 s ——
 *  50 ms 的节奏对它们的时间精度没有影响；省下的是每字节一次的锁与判断。
 */
#define GNSS_HOUSEKEEP_MS   50u

/* 叫醒读线程的信号，见 gnss_reader_wake()。 */
#define GNSS_WAKE_SIG       SIGUSR1
#define GNSS_RECOVER_MS     45000u
#define GNSS_LINK_OK_MS     15000u
#define GNSS_LINK_BAD_MS     5000u

/* 读线程栈：**单槽**。
 *
 * 为什么当初是两块：`gnss_reader_stranded()` 判出"僵尸"时线程**还活着**（st=RUNNING
 * 卡在 poll 里、永远不退出、join 会永久阻塞）。旧代码判到僵尸后只 `return 0`，
 * `g_gnss_has_thread`/`g_gnss_stack_busy` 一个都没清，于是 spawn 永远被
 * `busy=1 has=1` 拒掉 —— 现场 2026-09-18 15:56：判成僵尸之后每 30 s 重复一次
 * `spawn refused busy=1 has=1`，GNSS 一直死到重启。
 * 僵尸收不回来就必然占着它那块栈，所以重生成只能换一块 —— 那是"槽位永不复用"
 * 时代的设计。
 *
 * **2026-09-25 改成单槽**：`gnss_thread_spawn()` 现在会在重新生成前回收槽位
 * （pid 已死、或被调度器丢掉，两种可回收情形，见那里的注释），所以不再需要
 * 第二块备用栈来换。SRAM 账见 analysis/SRAM_BUDGET.md：这里是 32 KB → 8 KB。
 *
 * ⚠ 代价要讲清楚：**回收判据是唯一的恢复路径**。如果某次 stranded 线程
 * "还活着、又判不出被调度器丢掉"，这个槽就收不回来，GNSS 会一直死到重启 ——
 * 原来有第二块栈时这种情形还能再撑一次。所以判据若改动，先复验这一条。 */
#define GNSS_STACK_SLOTS 1
/* 这块 SRAM 是 g_gnss_stack[] 的兜底：从前的注释写"优先放 PSRAM"，但代码里
 * 只有 `g_gnss_stack[s] = g_gnss_stack_sram[s]` 一条路径（PSRAM 优先从未实现），
 * 实测水位 3,260 B（`test cpu -t 2`，见 analysis/SRAM_BUDGET.md）⇒ 按 2.5× 取 8 KB。 */
static uint8_t g_gnss_stack_sram[GNSS_STACK_SLOTS][GNSS_STACK_SIZE]
    __attribute__((aligned(16)));
static uint8_t *g_gnss_stack[GNSS_STACK_SLOTS];
static uint8_t g_gnss_stack_slot;
static uint32_t g_gnss_canary_tick;    /* 当前读线程用的槽位 */
static uint8_t g_gnss_stack_zombie;  /* 位掩码：被僵尸占住的槽 */
/* 每个被隔离槽位"当初放弃的是哪个线程"。用来判断它还活着没有 ——
 * 只要确认它确实不在了，这个槽就可以回收（见 gnss_thread_spawn 的选槽处）。
 * 原设计是"隔离后本开机内永不复用"，但槽位只有两个 ⇒ 两次 stranded 之后
 * spawn 永久失败、GNSS 永久死、只有重启能救（2026-09-21 现场实证）。
 * ⇒ 2026-09-25 起只剩**一个**槽，回收从"省着用的优化"变成**唯一**的复生路径。 */
static pid_t g_gnss_zombie_pid[GNSS_STACK_SLOTS];
static void gnss_reader_release_zombie(pid_t pid);   /* 定义在后面 */
#define GNSS_UBX_PAY_MAX         2048u
#define GNSS_EPH_INJECT_MAX      256u
#define GNSS_EPH_ACK_MS          80
#define GNSS_EPH_GAP_US          12000

/** @brief 两次成功 read 间隔的告警阈值（ms）。
 *
 *  **必须严格大于模组输出周期**：1 Hz 的多星座模组是"一秒吐一串、然后静默到
 *  下一秒"，静默期就是 ~1000 ms。阈值设成 1000 会把**每个正常周期**都报成
 *  异常 —— 2026-09-17 现场就是模组从 2 Hz 降到 1 Hz 后 `reader gap 1000 ms`
 *  每周期刷一次，真被饿死反而淹没在里面（判据见 `bad`/`junk` 计数器：
 *  真丢字节时它们必涨，而不是只看 gap）。
 */
#define GNSS_RD_GAP_WARN_MS      1800u
#define GNSS_EPH_PATH_MAX        96
#define GNSS_EPH_KEEP            5u
/* 位置辅助（MGA-INI-POS_LLH）取哪份定位、以及如实报多大精度。 */
#define GNSS_EPH_POS_PHONE_MS    (5u * 60u * 1000u)  /**< 手机位置给 5 分钟窗口。 */
#define GNSS_EPH_POS_SELF_MS     (10u * 60u * 1000u) /**< 本机自己的定位给 10 分钟。 */
#define GNSS_EPH_POS_SELF_ACC_CM 2000000             /**< 本机那份：20 km（见函数注释）。 */
/** @brief 解算好到这个程度（hAcc，mm）就不再需要外部位置辅助。 */
#define GNSS_ASSIST_POS_OK_MM    15000u
/** @brief 位置辅助补发的最小间隔（毫秒）：手机位置更新了也得等这么久再发。 */
#define GNSS_ASSIST_POS_MIN_MS   60000u
#define GNSS_EPH_DUMP_MAX        (32u * 1024u)
#define GNSS_DBD_MIN_FRAMES      4
#define GNSS_DBD_CAPTURE_MS      10000u
#define GNSS_DBD_IDLE_MS         400u
#define GNSS_DBD_NODATA_MS       5000u
#define GNSS_DBD_AFTER_FIX_MS    20000u
/** @brief "这次做不了"之后的重试间隔（毫秒）。
 *
 *  @details
 *  **倒库刻意不抢**（2026-09-17 定策）：它的价值只是"把导航库留给下次开机"，
 *  没有任何实时性要求，所以让它做让位的一方 —— App/UI 在用 LFS、或还在保鲜期
 *  内的 hold 挡着，就安静地等下一轮，不催、也不去挤。真正需要快的是**星历注入**
 *  （决定本次开机多久有定位），那条路有自己的退避（`GNSS_EPH_RETRY_*`，2~30 s），
 *  并且泄漏的 hold 会被 `myvendor_mtp_lfs_quiesce()` 在源头回收 —— 回收是为了
 *  所有消费方（注入首当其冲），不是为了让倒库插队。
 *
 *  这个常量同时是日志上限：挡下之后最多每 30 s 问一次 quiesce、打一行。
 *
 *  之所以必须有限频：本函数是**逐字节**被调到的（状态机写在主循环体里），
 *  历史上这里一行刷了二十多秒（~800 行/秒）。见 GNSS_HOUSEKEEP_MS 那道结构闸。 */
#define GNSS_DBD_RETRY_MS        30000u
#define GNSS_RX_WIN_MS           1000u
#define GNSS_RX_DEAD_MS          1500u
#define GNSS_CFG_ACKAIDING       0x10110025u
#define GNSS_CFG_DYNMODEL        0x20110021u
/** @brief CFG-SIGNAL-BDS_ENA / GLO_ENA（U1，1 = 开）。
 *
 *  **依据是 GSV 发话者普查**（`link extra … gsv gp/gl/ga/bd/gq`）：本固件不回
 *  UBX-CFG-VALGET，星座开关读不回来，只能这样证。现场（2026-09-17）只有
 *  `gp/ga/gq` 有句子，**`bd` 与 `gl` 恒为 0** —— 两路的信号根本没在解。
 *  北斗在国内是最该有的一路（室内/城市峡谷多一路就是多几颗星），所以显式打开：
 *  不支持的组合会回 NAK（`cfg ack … sig bds=-1`），支持就会重启 GNSS 子系统
 *  并开始出 `$BDGSV`，下一份日志的 `gsv` 那一项就能验证。 */
#define GNSS_CFG_SIGNAL_BDS      0x10310024u
#define GNSS_CFG_SIGNAL_GLO      0x10310025u
/** @brief CFG-MSGOUT-UBX_NAV-PVT_UART1 / UART2（U1，值 1 = 每个历元输出一帧）。
 *
 *  **探针阶段**（docs/gnss_speed_filter.md 步骤 2）：只打开模块输出、在读线程
 *  数帧（`link` 行的 `pvt=`），不解析。给的是 NMEA 拿不到的 `gSpeed`（模块侧
 *  滤波过的多普勒速度，mm/s）、`sAcc` 与 `velN`/`velE`。
 *
 *  UART1 是模组默认输出口 —— 现有 NMEA 就在这个口上，所以先试它；固件不认这个
 *  键（NAK）或口不对时退到 UART2，与 dynmodel 的 want/alt 同一套写法。 */
#define GNSS_CFG_MSGOUT_PVT_UART1 0x20910007u
#define GNSS_CFG_MSGOUT_PVT_UART2 0x20910008u
#define GNSS_DYN_BIKE            MYVENDOR_GNSS_DYN_BIKE
#define GNSS_DYN_AUTO            MYVENDOR_GNSS_DYN_AUTO
#define GNSS_DYN_RETRY_MS        2000u
/* 星历注入 0 帧后的重试退避：2 s 起、每次翻倍、封顶 30 s。
 * 0 帧最常见的原因是瞬态的 —— BLE 正在写 LittleFS（companion_fs 会
 * xfer_lfs_acquire，使 quiesce 为真）导致注入让路，几秒后就该重试。
 * 封顶 30 s 是为了别在长期缺文件时反复扫目录（每次重试都要短暂持有 LFS，
 * 会让地图的 quiesce 检查顺带跳一次 tile 读）。 */
#define GNSS_EPH_RETRY_MIN_MS    2000u
#define GNSS_EPH_RETRY_MAX_MS   30000u
#define GNSS_DYN_FAIL_MAX        3u
/** @brief CFG-NAVSPG-DYNMODEL：静止（零速约束）。停车+解算差时用，见 gnss_dyn_feed。 */
#define GNSS_DYN_STATIONARY      2u
/** @brief 低于它（0.01 km/h）算"没动"。 */
#define GNSS_DYN_STILL_KPH       300u
/** @brief 停车要持续这么久才切 stationary（20 s）。 */
#define GNSS_DYN_STILL_HOLD_MS   20000u
/** @brief 速度起来后持续这么久就切回来（3 s）—— 退出必须灵敏，见 gnss_dyn_feed。 */
#define GNSS_DYN_MOVE_HOLD_MS    3000u
/** @brief 认为"位置够用"的 hAcc（mm）：比它好的解算不需要零速约束。 */
#define GNSS_DYN_POS_OK_MM       8000u
#define GNSS_EPH_BOOT_WAIT_MS    12000u
#ifndef SF32LB52_GNSS_EPH_DIR
#  define SF32LB52_GNSS_EPH_DIR  "/mnt/lfs/eph"
#  define SF32LB52_GNSS_EPH_FILE SF32LB52_GNSS_EPH_DIR "/mga.ubx"
#  define SF32LB52_GNSS_EPH_DUMP SF32LB52_GNSS_EPH_DIR "/dump.ubx"
#  define SF32LB52_GNSS_EPH_TMP  SF32LB52_GNSS_EPH_DIR "/mga.tmp"
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static myvendor_sys_gnss_t g_fix;
static uint32_t g_fix_ms;
static uint32_t g_rx_win_ms;
static uint16_t g_rx_win_n;
static uint8_t g_rx_hz;
static uint32_t g_rx_last_ms;
static bool g_started;
static bool g_eph_injected;

/** 模块自己报的时间是否可信 —— **是否需要灌星历的判据**（用户规则）。
 *
 * 模块有备份电源：`VCC off PA43` 的 idle park 不会让它丢星历，冷启动之后它
 * 也会先用备份里的时基/历书对时。所以**只要它报出来的时间正常，就没有必要
 * 强制灌星历** —— 灌一遍要读 9.8 KB 的 LFS、发 106 帧 UBX，还会打断它刚开始
 * 的首次定位。反过来，报不出正常时间才是"备份掉电/真冷启动"的证据，那才该灌。
 *
 * 判据取 `gnss_try_set_utc()` 成功的那一刻：它只在日期合法且
 * `>= GNSS_TIME_MIN_UNIX` 时才落地，比另起一套解析便宜也更可信。
 */
static uint32_t g_mod_time_ms;
static bool     g_eph_time_skip;     /**< 这次是因"模块时间正常"跳过的；时间转坏可重开。 */

#define GNSS_MOD_TIME_TTL_MS 600000u   /* 10 min：期间一直有 1 Hz 的 ZDA/RMC 刷新。 */

/* 本文件的时间助手定义在后面（~line 600），这里先用。 */
static uint32_t gnss_now_ms(void);
static uint32_t gnss_elapsed_ms(uint32_t now, uint32_t then);

/** 上次成功注入的源文件指纹（同源反复重灌要挡住）。 */
static char     g_eph_sig_name[64];
static uint32_t g_eph_sig_size;
static uint32_t g_eph_sig_mtime;
static bool     g_eph_sig_valid;
static bool     g_eph_skip_same_src; /**< 本次 run 因"同源"跳过 —— 按完成处理。 */

/** 模块"刚上电/刚重配"的时刻。用于给"等模块报时间"一个窗口。
 *
 * 现场（2026-09-18）：idle 唤醒后 1.4 s 就判"模块时间丢了"→ 重开闸 → 白灌 43 帧
 * /3.9 s。判据本身是对的，**问得太早**：模块备份活着时通常 1 s 内就报时间
 * （同一份日志里开机那次 `eph skip (module time ok)` 就是这么跳过的）。 */
static uint32_t g_mod_up_ms;

#define GNSS_EPH_MOD_TIME_WAIT_MS 10000u

static bool gnss_mod_time_ok(void)
{
  return g_mod_time_ms != 0 &&
         gnss_elapsed_ms(gnss_now_ms(), g_mod_time_ms) < GNSS_MOD_TIME_TTL_MS;
}

static bool g_eph_hold;
static uint8_t g_uart_fails;
static bool g_dump_req;
static bool g_dump_halt;
static bool g_dump_done;
static bool g_dbd_run;
static bool g_dbd_halt;
static bool g_dbd_ack;
static bool g_dbd_saved;
/** @brief 上次"倒库暂时做不了"的时刻；用于给自动重试限频（见 GNSS_DBD_RETRY_MS）。 */
static uint32_t g_dbd_quiet_ms;
static int g_dbd_used;
static int g_dbd_n;
static uint32_t g_dbd_t0;
static uint32_t g_dbd_tlast;

static void gnss_dbd_abort(void);

#ifdef CONFIG_BOARD_L96_GNSS

static volatile uint32_t g_nmea_life_ms;
static volatile uint32_t g_heartbeat_ms;
static volatile bool g_diag_kick;
static volatile bool g_recovering;
static volatile bool g_gnss_stop;
static volatile bool g_idle_sleep;
static bool g_idle_parked;
static uint32_t g_recover_ms;
static int g_uart_fd = -1;

/* 读线程取证（只写 SRAM，由 reap / diag 的日志一次性打出）。
 *
 * 存在的理由：现场日志里 "phase=read-poll"、"pid=0"、"hb_age 一直涨" 这三条
 * 同时出现时，有**两种完全相反**的解释，而当时的日志无法区分：
 *   (A) 线程在 poll() 里等，只是那次等待永远不返回；
 *   (B) 线程根本没跑起来（TCB 建了没被激活），于是 phase 是**上一代线程**
 *       留下的陈旧字符串，pid 也从没被写过。
 * 区分它们只需要知道"入口跑过没有"和"循环在不在推进"：
 *   - g_rd_enter_ms == 0        → 入口一次都没执行过（(B)）
 *   - g_rd_loop_seq 停止增长    → 入口跑过，循环体停了（(A)）
 *   - g_rd_self == 0 而 pid 有值 → 是"读"的问题（可见性），不是"写"的问题
 * 这三个量都是单调累加/一次性赋值，读侧不需要锁。 */
static volatile uint32_t      g_rd_enter_ms;
static volatile uint32_t      g_rd_loop_seq;
static volatile unsigned long g_rd_self;

/* poll 进出取证：g_poll_enter_ms 重要 —— 它非 0 且不再推进，就说明
 * "有一次 poll 进去了再没出来"，这是 (A) 的直接证据；若它一直是 0，
 * 说明线程压根没进过 poll，那就是 (B)。 */
static volatile uint32_t g_poll_n;
static volatile uint32_t g_poll_enter_ms;
static volatile uint32_t g_poll_last_ms;

/* 自测工作量：**不依赖内核那套 200 Hz 瞬时采样**（ps 的 CPU% 就是它算的，
 * 对短促突发的线程噪声极大 —— 现场同一个线程 4 秒内从 17.0% 掉到 5.8%，
 * 工作量并没变）。这三个量由读线程自己用 DWT 累加，直接回答"它到底烧了多少"：
 *
 *   g_rd_loop_seq 循环轮数（已有）—— 轮数 × 每轮耗时才谈得上 CPU 占比
 *   g_rd_poll_ms  poll 阻塞累计（正常 ≈ 轮数 × 超时值 200 ms，不烧 CPU）
 *   g_rd_read_ms  read() 自身累计 —— read 里若藏着忙等（NuttX poll 看不到
 *                 bounce，uart_read 会 rxint(true) 触发 harvest），这一项会顶起来
 *   g_rd_work_ms  "每轮迭代墙钟 − poll"累计，也就是解析/发布/记账那一坨
 *
 * 注意 g_rd_work_ms 是**上界**：被高优先级任务抢占、等锁的时间都会被算进去。
 * 所以要和轮数一起读：轮数多 + work 大 = 真在烧自己的 CPU；轮数少 + work 大
 * = 主要在被抢占或等锁，那是另一回事。 */
static volatile uint32_t g_rd_work_ms;
static volatile uint32_t g_rd_read_ms;
static volatile uint32_t g_rd_poll_ms;
static volatile uint32_t g_rd_prev_loop_ms;
/** @brief 自旋上限被触发的次数（正常恒为 0；非 0 = 预算/周期计数出了问题）。 */
static volatile uint32_t g_rd_spin_capped;
/** @brief `gnss_yield_all()` 里"恢复优先级失败"的次数。
 *         一次就足以把读线程永久压在 pri=1（见该函数的注释），所以必须可见。 */
static volatile uint32_t g_prio_restore_fail;
/** @brief 连续"poll 就绪但 read 为空"的次数与退避档位（见主循环 rc==0 分支）。 */
static volatile uint32_t g_rd_empty_run;
static volatile uint32_t g_rd_backoff_n;
/** @brief 自测记账里出现"异常大的单轮间隔"的次数。非 0 = 累计器的前提被打破
 *         （最可能是两个读循环并发），此时 `busy`/`busy_pm` 不可信。 */
static volatile uint32_t g_rd_dt_huge;
static volatile uint8_t g_dyn_want = GNSS_DYN_BIKE;
static uint8_t g_dyn_have;
/** @brief 停车判定与"stationary 不被这颗固件支持"的闩锁（见 gnss_dyn_feed）。 */
static bool g_dyn_still;
static bool g_dyn_no_still;
/** @brief 停车切 stationary 的运行时开关（默认关，`ctl gnss still on|off`）。
 *         只在 GNSS 线程读，NSH 线程写，所以 volatile。 */
static volatile bool g_dyn_still_on;
static uint32_t g_dyn_still_since;
static uint32_t g_dyn_move_since;
/** @brief 本上电周期已经读过一次 CFG（见 gnss_cfg_dump_signal）。 */
static bool g_cfg_dumped;
/** @brief 星座实验请求（见 gnss_probe_tick / myvendor_gnss_probe）。 */
static volatile int g_probe_req;
/** @brief 模组支持的星座位图 / 星座串原文 / 固件串（MON-VER，见 gnss_mon_ver）。 */
static uint8_t g_mod_const;
static char g_mod_gnss[24];
static char g_mod_fw[24];
static uint32_t g_dyn_try_ms;
static uint8_t g_dyn_fails;
static uint8_t g_dyn_try_want;
static volatile bool g_ui_ready;
static bool g_eph_pending;
static bool g_mod_pwr;
static uint32_t g_boot_ms;
#if defined(CONFIG_MYVENDOR_GNSS_LOG) || defined(CONFIG_MYVENDOR_GNSS_TRACE)
static uint16_t g_poll_to_run;
#endif
static pid_t g_gnss_pid = (pid_t)-1;
static pthread_t g_gnss_thread;
static bool g_gnss_has_thread;
static volatile bool g_gnss_stack_busy;
static uint32_t g_reap_stuck_ms;        /* 连续收不回来的起点（0 = 没卡） */
static bool g_reap_giveup_logged;       /* 升级日志只打一次 */
/** @brief diag 触发过的重启次数（上屏用，见 gnss_fill_health）。 */
static uint32_t g_diag_restarts;
static bool g_gnss_spawning;

/** @brief 读者的 pollfd，**按栈槽各一份**（不跨线程共用）。
 *
 *  2026-09-23 修（转储 n587：`kind=assert` + `semaphore.h:1236`
 *  `DEBUGASSERT(!NXSEM_IS_MUTEX(sem))` + `irq=1`，被打断的任务是 `gnss`）：
 *
 *  原来这里只有**一个** `static struct pollfd g_uart_pfd`，新旧两个读者共用，
 *  而 `nuttx/fs/vfs/fs_poll.c` 的 `fs_poll_setup()` 每次调用都会**重置**同一份
 *  pollfd 的内部字段 —— 那段代码自己的 `REVISIT` 原文就写着：
 *
 *      "In a multi-threaded environment, one use case might be to share a
 *       single, array of struct pollfd in poll() calls on different threads.
 *       That use case is not supportable here because the non-standard
 *       internal fields get reset here on each call to poll() on each thread."
 *
 *  ⇒ 我们正好在用它明说不支持的用法。失效链：
 *   1. 读者 A（槽 0）进 `poll()`：`fds->arg = semA`（semA 就在 `poll()` 内部
 *      `fs_heap_malloc` 那块里，返回时 free），驱动把 `dev->fds[i]` 指向这份 pollfd；
 *   2. A 被判 stranded（"lost by scheduler"）—— **它还在 poll 里**，那块内存还活着；
 *   3. 新读者 B 起来，poll 同一份 pollfd ⇒ 内部字段被重置成 semB，
 *      **A 的注册被顶掉**；
 *   4. A 终于被调度、`poll()` 返回 ⇒ 它拆掉的是 **B 的**注册，并 free 掉自己那块
 *      ⇒ 驱动手里那份注册的 `arg` 指向**已释放内存**；
 *   5. USART2 **TX 完成中断**（`serial.c:2197 uart_datasent → uart_poll_notify
 *      → poll_default_cb`）读到那一块被复用成互斥量的内存 ⇒ 断言 ⇒ 重启。
 *
 *  按线程隔离后 A、B 各用各的，第 3、4 步不再发生。
 *  ⇒ 修法：**每个线程各持一份 pollfd**（见 `gnss_uart_pfd()`），从根上不存在
 *  "两个读者共用一份"这件事，也不必假设槽有没有被复用。
 *
 *  为什么用 TLS 而不是"按槽分"：这条路能 poll 的函数不止读者一个 ——
 *  `gnss_read_some → gnss_read_byte → {gnss_drain, gnss_ubx_read_frame}`，
 *  而 `gnss_drain`/`gnss_ubx_read_frame` 全树各有 7 / 6 个调用点（含 eph 注入、
 *  diag 那些**非读者线程**）。把槽号顺着这么多层穿下去既啰嗦又容易漏一处，
 *  而漏一处就又变回共享。TLS 按"谁在 poll"天然隔离，调用点一处都不用改。
 *
 *  代价：每个 poll 过的线程多占一个 `struct pollfd`（16 B），且**不回收**
 *  （线程数有限、都是长命线程）；分配失败时调用方退回非阻塞单次读。 */
static pthread_key_t  g_uart_pfd_key;
static pthread_once_t g_uart_pfd_once = PTHREAD_ONCE_INIT;

static void gnss_uart_pfd_key_make(void)
{
  (void)pthread_key_create(&g_uart_pfd_key, NULL);
}

/** @brief 取**本线程**的 pollfd（首次调用时分配并挂到 TLS；失败返回 NULL）。 */
static struct pollfd *gnss_uart_pfd(void)
{
  struct pollfd *pfd;

  (void)pthread_once(&g_uart_pfd_once, gnss_uart_pfd_key_make);

  pfd = (struct pollfd *)pthread_getspecific(g_uart_pfd_key);
  if (pfd == NULL)
    {
      /* 用板级 SRAM 分配器（本文件已 include board_malloc.h）：16 B、长命，
       * 随线程一起存活，不回收 —— poll 过的线程数有限，是可控的常量。 */
      pfd = (struct pollfd *)board_malloc_sram(sizeof(*pfd));
      if (pfd == NULL)
        {
          syslog(LOG_ERR, "gnss: pollfd alloc failed, poll disabled on this thread\n");
          return NULL;
        }

      (void)pthread_setspecific(g_uart_pfd_key, pfd);
    }

  return pfd;
}

/**
 * @brief 唤醒信号的 handler：只负责打断阻塞，不做任何实质工作。
 *
 * 它在**被中断线程的上下文**里跑，而那个线程可能正持有 LittleFS 占用或
 * UART 驱动内部锁。退出动作统一由读线程回到循环顶后自己判 `g_gnss_stop` /
 * `g_diag_kick`，这里连日志都不打。
 */
static void gnss_wake_handler(int signo)
{
  (void)signo;
}

/**
 * @brief 把读线程从阻塞的 `poll()` / `read()` 里叫回来。
 *
 * 这是恢复路径原先缺的那颗"牙齿"（见 docs/gnss_ble_diag_log.md §2.4）：
 * 读线程显式关掉了 cancellation（避免留"持锁的尸体"），所以它**唯一**的
 * 正常退出途径就是自己走到循环顶看 `g_gnss_stop`。原先"一定能走到循环顶"
 * 靠的是读等待的有界超时 —— 而那个超时由 tick 驱动，一旦计时基准被打断就
 * 永远走不到，线程和恢复一起死（§2.2）。
 *
 * 信号**不经过 tick**：`pthread_kill()` 直接把目标线程的等待打断，
 * `nxsem_tickwait()` / `nxsem_wait()` 返回 `-EINTR`。所以 reap 置位
 * `g_gnss_stop`、diag 置位 `g_diag_kick` 之后，必须调它一次。
 *
 * @note 选 `SIGUSR1` 是因为本配置没开 `CONFIG_SIG_SIGUSR1_ACTION`，
 *       `nxsig_default()`（sched/signal/sig_default.c）对表外信号的默认动作
 *       是 `SIG_IGN` —— 即使 handler 没装上，这个信号也只是被忽略，不会把
 *       读线程打死。若哪天开了那个 Kconfig，默认动作会变成异常终止，就不能
 *       再拿它当唤醒信号了。
 */
static void gnss_reader_wake(void)
{
  unsigned long self;
  uint32_t      seq;
  uint32_t      enter;
  int           rc;

  if (!g_gnss_has_thread) {
    return;
  }

  /* 快照取证字段：失败时要连"线程是谁、跑过没有"一起打出来，
   * 否则区分不了"句柄已失效"和"信号发给了另一具线程"。 */
  self  = g_rd_self;
  seq   = g_rd_loop_seq;
  enter = g_rd_enter_ms;

  rc = pthread_kill(g_gnss_thread, GNSS_WAKE_SIG);
  if (rc != 0) {
    /* **ESRCH 也要报**（原来被静默吞掉）：
     *   - ESRCH → 这个 pthread 句柄已经不对应任何线程，即"叫不醒是因为人不在"，
     *     此时应清掉句柄而不是继续重试；
     *   - 其它非 0 → 投递本身失败。
     * 两种情况都等于"恢复没有牙齿"，必须留痕。 */
    syslog(LOG_ERR, "gnss: reader wake rc=%d self=%lx seq=%u entered=%u%s\n",
           rc, self, (unsigned)seq, enter != 0u ? 1u : 0u,
           rc == ESRCH ? " (ESRCH: 句柄已无对应线程)" : "");

    if (rc == ESRCH) {
      g_gnss_has_thread = false;
      g_gnss_stack_busy = false;
    }
  }
}

struct gnss_link_s
{
  uint16_t poll_to;
  uint16_t poll_err;
  uint16_t rd;
  uint16_t byte;
  uint16_t nmea;
  uint16_t nmea_bad;
  uint16_t ubx;
  uint16_t pvt;      /* 校验通过的整帧 UBX-NAV-PVT，见 GNSS_CFG_MSGOUT_PVT_UART1 */
  uint16_t junk;
  uint16_t pub;
  uint16_t wr_fail;
};

static struct gnss_link_s g_link;
static char g_link_last[192];
static uint32_t g_link_ms;
static bool g_link_got_nmea;
static bool g_link_got_fix;
static bool g_link_got_pub;
static bool g_get_ok;

static uint8_t s_ubx_tx[GNSS_EPH_INJECT_MAX + 8u];
static uint8_t *s_ubx_pl;
static uint8_t *s_dbd_buf;

/**
 * @brief UBX 解析缓冲放 BoardPSRAM（2 KiB）。
 */
static bool gnss_ubx_pl_ensure(void)
{
  if (s_ubx_pl != NULL) {
    return true;
  }

  s_ubx_pl = board_malloc_psram(GNSS_UBX_PAY_MAX);
  return s_ubx_pl != NULL;
}

static char g_eph_src_path[GNSS_EPH_PATH_MAX];
static char g_dump_last_path[GNSS_EPH_PATH_MAX];
static char g_inject_last_path[GNSS_EPH_PATH_MAX];
static uint8_t g_gsv_view_acc;
static uint8_t g_gsv_heard_acc;
/** @brief 本窗口按星座分组的 GSV 句子数：gp/gl/ga/bd/gq（见 gnss_apply_gsv）。 */
static uint8_t g_gsv_talk[5];
static uint32_t g_rmc_pos_ms;
static struct minmea_date g_last_date =
{
  .day = -1,
  .month = -1,
  .year = -1,
};

#define GNSS_TIME_MIN_UNIX       1704067200  /* 2024-01-01 UTC */
/** GPS 历元（1980-01-06 00:00 UTC）的 unix 秒。算"周内秒"**必须从它起算**：
 *  unix 0 与 GPS 周起点相差 3 天（315964800 不是 604800 的整数倍，差 259200 s），
 *  直接 `unix % 604800` 会把每个周的偏移算歪 3 天（主机侧校验器踩过这个坑）。 */
#define GNSS_GPS_EPOCH_SEC       315964800
/** 当前 GPS-UTC 闰秒（MGA-INI-TIME_UTC 的 leapS 也是这个值）。 */
#define GNSS_LEAP_SEC            18
/* 星历有效 2–4 h：既用来算"下次该同步"的提示（last+4 h），也用作**注入前的年龄
 * 门限**（见 gnss_eph_inject()：星历没过期就注入，过期才丢 EPH 帧）。
 *
 * 辅助帧（时间/位置）另有处理：不丢，而是按年龄把精度字段改成如实值，
 * 见 gnss_eph_assist_fixup()。 */
#define GNSS_EPH_VALID_SEC       (4u * 3600u)

/** 辅助帧年龄算不出来（RTC 还没对时、文件名也没时间）时用的上限值：
 *  tAcc 字段满量程 ≈18 h，等于告诉模组"这一刻时间帧没有意义"。 */
#define GNSS_EPH_ASSIST_AGE_MAX  65535u
/** 位置随年龄增长的不确定度上界：50 m/s（180 km/h，厘米/秒）。
 *
 *  位置辅助帧里写的是"生成那一刻"的坐标，模组只能信帧里的 posAcc。年龄越大，设备
 *  可能跑得越远，所以按这个上界把不确定度放宽 —— 宁可报松（丢一点乐观启动），
 *  也不报紧（u-blox：精度报得比实际好会明显劣化甚至起不来）。 */
#define GNSS_EPH_ASSIST_DRIFT_CMS 5000u
/** posAcc 上限 1000 km（厘米），与 App 侧夹取一致。 */
#define GNSS_EPH_ASSIST_PACC_MAX  100000000u
/** 文件里的时间帧超过这个年龄、而 RTC 又有效时就不发了。
 *
 *  固件自己那帧（tAcc=5 s，来自 RTC）此时严格更准；再补一帧几分钟前的时间只会把
 *  模组刚拿到的好时间搅浑。RTC 无效时照发（如实报大 tAcc）——那是它唯一的时间来源。 */
#define GNSS_EPH_ASSIST_TIME_FRESH_SEC 60u
#define GNSS_EPH_TIMES_CACHE_MS  2000u

/** idle park 唤醒后，最多推迟这么久不重灌星历（手机来要则立刻灌）。 */
#define GNSS_EPH_WAKE_DEFER_MS   300000u

static uint32_t g_eph_last_utc;
static uint32_t g_eph_times_ms;
static uint32_t g_eph_times_cached;
static bool g_eph_reload;
static bool g_eph_busy;
static uint32_t g_eph_grace_ms;
static volatile uint32_t g_wake_hold_ms;

/** 外部（ctl/UI/diag）申请"唤醒保持窗"的**唯一**入口。
 *
 * 以前 `myvendor_gnss_idle_sleep(false)` 直接写 `g_wake_hold_ms`，而读线程在
 * publish 路径里清它 —— 两个写者、无锁：刚申请的窗口可能被那一行立刻抹掉。
 * 现在外部只投递这个电平，时间戳由读线程落地（单写者）。 */
static volatile bool g_wake_hold_req;

/** idle park 唤醒后"不急着重灌星历"的起点（0 = 没有推迟）。见
 *  `GNSS_EPH_WAKE_DEFER_MS` 与 `gnss_reconfig_after_vcc()` 里的说明。 */
static uint32_t g_eph_wake_defer_ms;

/**
 * @brief "这段间隔是我们自己按住的" 标记，由下一次 gap 判定消费（清 0）。
 *
 * 为什么不能只看判定时刻的 hold 标志：注入收尾设的 `g_eph_grace_ms` 会被紧接着的
 * 一次 `gnss_publish()` 清掉（publish 成功即认为模组活着，宽限不再需要），而读线程
 * 那时还没读回第一包 —— 2026-09-18 现场就是 `eph grace 15000 ms` 之后 200 ms 打印
 * 了 `reader gap 33840 ms`，判定时 grace 已经是 0。这个标记只在窗口收尾置一次、
 * 只由 gap 判定清零，所以与"谁先跑"无关。
 */
static uint32_t g_rd_selfblock_ms;
static bool g_rd_selfblock_logged;

static uint32_t gnss_now_ms(void)
{
  return myvendor_mono_ms();
}

static uint32_t gnss_elapsed_ms(uint32_t now, uint32_t then)
{
  return myvendor_mono_elapsed_ms(now, then);
}

/* ===== DWT 周期计数器：给读线程用的、与 tick 无关的时间基准 ==============
 *
 * 背景（docs/gnss_ble_diag_log.md §2.2）：读线程原先用 `poll(…, timeout_ms)`
 * 等数据，而 `poll()` 的超时是**由 tick 驱动的看门狗**。一旦计时基准被打断
 * （§2.2 点名的 `up_timer_set_lowerhalf()` 型故障），这个超时不再到期，
 * 线程就永久停在 `poll()` 里：`pthread_cancel` 唤不醒它（阻塞在系统调用），
 * reap 只能放弃 join，于是 `g_gnss_stack_busy` 永远为 1、spawn 永远被拒 ——
 * GNSS 直到关机都不恢复（现场 hb_age 一路涨到 261 s）。
 *
 * DWT_CYCCNT 是 CPU 自由运行的周期计数器，**不经过 SysTick，也不挂 wdog**，
 * 所以拿它做等待的上界不会被上面那类故障打断。它只用于"到点该返回了"的判断，
 * 不参与任何状态机；`sched_yield()` 让出 CPU，睡眠仍由 idle 的 WFI 承担。
 *
 * CYCCNT 需要 TRCENA + CYCCNTENA，不保证每颗芯片都真的在跑；所以初始化时
 * 做一次自检（两次读数必须不同），跑不起来就整体退回 `usleep()` 老路径 ——
 * 有降级，但不会因为读一个不动的时间戳而算错等待。
 */
#define GNSS_DWT_DEMCR   (*(volatile uint32_t *)0xe000edfcu)
#define GNSS_DWT_CTRL    (*(volatile uint32_t *)0xe0001000u)
#define GNSS_DWT_CYCCNT  (*(volatile uint32_t *)0xe0001004u)
/* Armv8-M（本板是 Cortex-M33）的 DWT 是锁访问的：不解锁的话往 CTRL/CYCCNT
 * 写全被忽略，CYCCNT 永远不动。解锁值固定是 0xC5ACCE55。
 *
 * 两个 LAR 都写：0xe0001fb0 是 Armv8-M 架构规定的 **DWT_LAR**，0xe0000fb0 是
 * ITM_LAR。本板既有的 DWT 用户（chips/sf32lb52/myvendor_idle_stat.c 的
 * myvendor_dwt_init()）只写了 ITM_LAR 且实测可用（up_idle() 每次进 idle 都
 * 用 CYCCNT 记 WFI 时长，统计是正常的），说明这颗片子两个锁要么共享、
 * 要么根本没锁。两个都写一遍，无论哪种情况都能解锁，且都是幂等的。 */
#define GNSS_DWT_LAR     (*(volatile uint32_t *)0xe0001fb0u)
#define GNSS_ITM_LAR     (*(volatile uint32_t *)0xe0000fb0u)
#define GNSS_DWT_LAR_KEY 0xc5acce55u

/* CMSIS 定义在别处（sifli_systick_reclock() 会跟着 HCLK 更新它）。 */
extern uint32_t SystemCoreClock;

static bool g_dwt_ready;
static bool g_dwt_usable;   /* CYCCNT 自检通过，可当时间基准 */

static void gnss_dwt_init_once(void)
{
  uint32_t a;
  uint32_t b;
  int i;

  if (g_dwt_ready)
    {
      return;
    }

  g_dwt_ready = true;

  GNSS_ITM_LAR = GNSS_DWT_LAR_KEY;   /* 与本板既有 idle_stat 的写法一致 */
  GNSS_DWT_LAR = GNSS_DWT_LAR_KEY;   /* Armv8-M 架构规定的 DWT_LAR */
  GNSS_DWT_DEMCR |= (1u << 24);      /* TRCENA —— 使能跟踪子系统 */
  GNSS_DWT_CTRL |= 1u;               /* CYCCNTENA */

  /* 注意：**不要**清零 GNSS_DWT_CYCCNT。
   *
   * CYCCNT 是全系统共享的：chips/sf32lb52/myvendor_idle_stat.c 用它统计 WFI
   * 占比，窗口起点存在 g_win_start_cyc 里，用 `now - start` 的 uint32 回绕差
   * 算时长。在它的窗口中间把计数器清零，会让那一个窗口的统计直接失准。
   * 本文件只用差值（now - cyc0），不需要从 0 开始，所以没有理由去动它。 */

  /* 自检：读数必须会变，否则它只是个恒定的 MMIO 寄存器。 */
  a = GNSS_DWT_CYCCNT;
  for (i = 0; i < 64; i++)
    {
      __asm__ volatile("nop");
    }

  b = GNSS_DWT_CYCCNT;
  g_dwt_usable = (b != a);

  if (!g_dwt_usable)
    {
      syslog(LOG_WARNING,
             "gnss: DWT CYCCNT 不可用，读等待退回 usleep\n");
    }
}

static inline uint32_t gnss_cycles_now(void)
{
  return GNSS_DWT_CYCCNT;
}

/** @return 微秒预算对应的周期数；DWT / HCLK 不可用时返回 0（表示"别用 DWT 判超时"）。 */
static uint32_t gnss_cycles_budget_us(uint32_t us)
{
  uint32_t hz = SystemCoreClock;

  if (!g_dwt_usable || hz == 0)
    {
      return 0;
    }

  return (uint32_t)(((uint64_t)us * (uint64_t)hz) / 1000000u);
}

/** 让出 CPU 给所有就绪任务（含更低优先级），定义见下。 */
static void gnss_yield_all(const struct sched_param *restore, int policy,
                           bool have);

/**
 * @brief 与 tick 无关的短睡眠（微秒）。
 *
 * `usleep()` 内部同样要挂一个 wdog 超时，所以 §2.2 那类故障下它也会一起失灵：
 * 线程卡在睡眠里，连 `g_gnss_stop` 都看不到，reap 照样只能放弃。读线程上的
 * 每一处等待都必须是"一定会醒"的，所以这些短睡眠统一走 DWT 预算 +
 * `sched_yield()`：睡眠由 idle 的 WFI 承担，上界由 DWT 保证，全程不碰 wdog。
 *
 * @param us 微秒。0、或 DWT/HCLK 不可用时退回原来的 `usleep()`。
 * @note 语义是"至少睡够"。按时基粒度（100 Hz）取整，只会偏长不会偏短 ——
 *       调用方需要的是最小间隔（星历帧间隙、上电稳定时间），偏长是安全的。
 * @note 这是自旋实现，**会烧 CPU**：调用点都是低频路径（写退避、星历帧
 *       间隙、VCC 稳定时间），所以代价可接受；但像
 *       `gnss_sleep_ms(CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS)`（可达 1 s）
 *       这样的长睡眠仍是实打实的一秒 CPU。**稳态等待路径不走这里**
 *       —— 那一条已经改成阻塞 `poll()`，见 `gnss_read_some()`。
 */
static void gnss_sleep_us(uint32_t us)
{
  uint32_t budget;
  uint32_t cyc0;
  uint32_t spins = 0;
  struct sched_param prio0;
  int policy0 = 0;
  bool have0;

  if (us == 0)
    {
      return;
    }

  budget = gnss_cycles_budget_us(us);
  if (budget == 0)
    {
      usleep(us);
      return;
    }

  /* 恢复目标里**优先级用编译期常量**，不用当前值。
   *
   * 原来这里把 `pthread_getschedparam()` 读到的当前优先级当恢复目标，注释说
   * "入口取一次就不会自我引用"—— 那只在**一次都没泄漏**时成立。只要
   * `gnss_yield_all()` 里那次恢复失败过一次（它的返回值被 `(void)` 丢掉、
   * 也没有日志），线程就永久停在 `SCHED_PRIORITY_MIN`(=1)；之后每次"入口取"
   * 取到的都是 1，于是固化下去，再也回不到 50。
   *
   * 现场 `ps` 里抓到的 `gnss pri=1` 就是这么来的，后果有两层：
   *   1. 读线程被压到只比 IDLE 高一级 → 任何就绪任务都能抢它 → NMEA 被饿死
   *      （`gnss: reader gap …`);
   *   2. 它一有空就在 IDLE 之上自旋（那段 1 kHz 的空 poll 循环）→ CPU 被吃掉。
   * 两者是同一个原因。
   *
   * 用常量当恢复目标，泄漏就变成"下一次调用自动纠正"，不会累积。policy 仍取
   * 当前值（线程创建时用的是 RR，改 policy 不是这里的事）。 */
  have0 = (pthread_getschedparam(pthread_self(), &policy0, &prio0) == 0);
  if (have0)
    {
      prio0.sched_priority = GNSS_THREAD_PRIO;
    }

  cyc0 = gnss_cycles_now();
  for (;;)
    {
      if ((uint32_t)(gnss_cycles_now() - cyc0) >= budget)
        {
          return;
        }

      /* 自旋上限必须**跟着预算缩放**，不能用固定次数。
       *
       * 固定 200000 次在 100 Hz 时基下大约只够跑几十毫秒；而
       * `gnss_sleep_ms(2500)`（VCC 断电）的预算在 240 MHz 上是 6e8 周期，
       * 每次迭代只花几十周期 —— 于是长睡眠被上限截断到约 1/30，"至少睡够"
       * 的契约直接失效：模组还没断电完/没稳定就被重配，重试退避也变成
       * 约 20 倍快。这里按 `budget / 每次迭代的最小周期数` 反推上限，
       * 保证预算永远先到，上限只用于"周期计数不前进"这种异常兜底。 */
      if (spins > (budget / GNSS_SPIN_MIN_CYCLES_PER_TURN) + 1024u)
        {
          g_rd_spin_capped++;
          return;
        }

      spins++;
      if ((spins % GNSS_SPIN_YIELD_EVERY) == 0u)
        {
          gnss_yield_all(&prio0, policy0, have0);  /* 别饿住 diag / BT LW WQ */
        }
      else
        {
          (void)sched_yield();
        }
    }
}

static void gnss_sleep_ms(uint32_t ms)
{
  gnss_sleep_us(ms * 1000u);
}

/**
 * @brief 把 CPU 让给**所有**其它就绪任务，包括优先级比本线程低的。
 *
 * `sched_yield()` 做不到这一点：它等价于把本线程重新排到"同优先级"的队尾
 * （`nuttx/sched/sched_yield.c` 的注释写得很清楚），所以优先级低于本线程的
 * 任务在自旋期间一个都跑不到。
 *
 * 本 fork 里**数值越大优先级越高**（`SCHED_PRIORITY_MAX`=255 是最高，
 * `SCHED_PRIORITY_MIN`=1 是最低；佐证：hpwork=224 > lpwork=100，
 * "BT LW WQ"=10，diag=9，gnss=50，ui_flush=100）。读线程 prio=50，
 * 所以它的自旋会饿住优先级比它低的两条关键路径：
 *   - `diag`     prio 9  —— GNSS 的判死 / 重启全靠它；
 *   - `BT LW WQ` prio 10 —— zblue 的延迟工作队列，蓝牙的收尾工作靠它。
 * 这两条都必须能秒级被调度到（diag 被饿住就等于自愈能力被拿掉，
 * 正是「连上了但一直未连接」那类问题的放大版），所以自旋期间必须定期
 * 把 CPU 真正交出去。
 *
 * 做法：临时降到最低优先级再让出 —— 这样所有就绪任务都会排到我们前面，
 * 等我们下次被调度回来时立刻恢复原优先级。
 *
 * @param restore 调用方在**自旋之前**取好的原优先级。
 * @param policy  与 `restore` 配套的调度策略。
 * @param have    `false` 表示取原优先级失败，此时只做 `sched_yield()`。
 *
 * @warning **不要在这里用 `pthread_getschedparam()` 取原优先级。**
 *          循环里读到的值是"我们刚降下去的最低优先级"，于是恢复也回最低 ——
 *          一次恢复失败（或读到被降过的值）就会把最低优先级当成原值永久
 *          固化，`ps` 里看到读线程常年 `pri=1` 就是这么来的。
 *          原值必须由调用方在进入自旋前捕获一次。
 */
static void gnss_yield_all(const struct sched_param *restore, int policy,
                           bool have)
{
  struct sched_param low;

  if (!have || restore == NULL)
    {
      (void)sched_yield();
      return;
    }

  low = *restore;
  low.sched_priority = SCHED_PRIORITY_MIN;

  /* 降优先级本身就会触发重调度：只要 diag / BT LW WQ 就绪，我们马上被换下。
   * 那句 sched_yield() 只是兜底（万一同优先级还有别的就绪任务）。 */
  if (pthread_setschedparam(pthread_self(), policy, &low) == 0)
    {
      int rc;

      (void)sched_yield();

      /* **恢复的返回值必须看**：以前被 `(void)` 吞掉，于是"降下去没恢复回来"
       * 这件事完全无声，读线程就永久停在 pri=1（现场 `ps` 抓到过）。
       * 只报前几次（这是自旋热路径，不能刷屏），但要报出来。 */
      rc = pthread_setschedparam(pthread_self(), policy, restore);
      if (rc != 0)
        {
          g_prio_restore_fail++;
          if (g_prio_restore_fail <= 3u)
            {
              syslog(LOG_ERR, "gnss: prio restore FAILED %d (want %d), prio=%d\n",
                     rc, restore->sched_priority,
                     running_task() != NULL ? (int)running_task()->sched_priority
                                            : -1);
            }
        }
    }
  else
    {
      (void)sched_yield();
    }
}

/* ===== 诊断插桩 ==========================================================
 * 下面这些状态只被 syslog 读取，不参与任何判断：不改变阈值、控制流、
 * 返回值，也不影响恢复策略。目的只有两件事：
 *   1) 异常成因  —— 注入为什么 0 帧、读线程为什么退出、看门狗为什么判死；
 *   2) 恢复为什么慢 —— 从触发到重新出数据，每个阶段各花了多久。
 */

/* 一轮"从恢复动作到重新有数据"的时间线。 */
static uint32_t g_rt_t0_ms;
static uint32_t g_rt_last_ms;
static uint8_t g_rt_seq;
static uint16_t g_rt_episodes;
static bool g_rt_wait_nmea;
static bool g_rt_wait_fix;
static uint32_t g_rt_nmea_ms;
static uint32_t g_rt_fix_ms;

/* 读线程活跃度：判断 1024 字节接收环有没有被撑爆的风险。 */
static uint32_t g_rd_gap_max_ms;
static uint32_t g_rd_last_ok_ms;
static uint16_t g_rd_stall_1s;
static uint32_t g_rd_loop_max_ms;
static uint32_t g_rd_last_loop_ms;
static uint32_t g_rd_session_ms;        /* 本读线程会话起点 */
static uint16_t g_rd_poll_close;        /* 本会话 read 返回 <=0 的次数 */
/* 2026-09-18 停用手机位置辅助（见 gnss_assist_tick 的空实现）：
 * 下面两个静态量与 `GNSS_EPH_POS_PHONE_MS`/`GNSS_ASSIST_POS_MIN_MS` 随之退休，
 * 保留定义只为让"曾经有过这条逻辑"可查。 */
/** 本会话已经报过 `first pvt`。判据是 UBX-NAV-PVT 到没到，报一次就够。 */
static bool g_pvt_first_done;

/** @brief 上次跑 housekeeping 段的时刻；0 = 还没跑过（新会话要立刻跑一次）。 */
static uint32_t g_house_ms;

/**
 * @brief 最近一帧 UBX-NAV-PVT 的关键值，供 `gnss_publish()` 贴进快照。
 *
 * @details
 * 见 docs/gnss_speed_filter.md 步骤 2。读线程写、`gnss_publish()` 读，两者都在
 * 读线程上下文，所以不另外加锁。
 *
 * 这里存的是**原始单位**（centi-km/h、mm/s）：`gSpeed` 与 `speed_centi_kmh`
 * 同单位便于直接比，`sAcc` 保持 u-blox 的 mm/s 不换算，免得和文档对不上。
 */
struct gnss_pvt_s
{
  uint32_t ms;                /**< 该帧到达时刻；0 = 本会话还没收到过。 */
  uint16_t gspeed_centi_kmh;  /**< gSpeed：模块滤波过的多普勒地面速度。 */
  uint16_t sacc_mm_s;         /**< sAcc：速度精度 1σ。 */
  uint32_t hacc_mm;           /**< hAcc：水平位置精度 1σ。 */
  uint8_t  fix_type;          /**< 0 无 / 2 2D / 3 3D。 */
  uint8_t  flags;             /**< bit0 = gnssFixOK。 */
};

static struct gnss_pvt_s g_pvt_last;
/** @brief 超过这个年龄的 PVT 不再贴进快照（PVT 是 1 Hz）。 */
#define GNSS_PVT_FRESH_MS 2500u

/* 星历注入取证：gnss_eph_inject() 写，gnss_eph_run() 读，仅用于日志。 */
static int g_eph_last_frames;
static int g_eph_last_skip;
static int g_eph_last_badck;
static int g_eph_last_bytes;
static int g_eph_last_stale;  /* 因文件超龄丢掉的 EPH 帧，见 gnss_eph_inject() */

/**
 * @brief 最近一次"真的写出了一帧星历"的单调时刻（ms；0 = 从未）。
 *
 * @note 用户 2026-09-27：状态栏据此让**卫星图标黄灯闪烁**（判据：年龄 < 3 s）。
 *       选"逐帧打点 + 短窗口"而不是"进入/退出标志"：注入函数里所有早退都走 `goto done`，
 *       用标志就得在每个出口清、漏一个就一直闪；用帧时间戳则天然自愈（没有帧就不闪）。
 */
static volatile uint32_t g_eph_frame_ms;

/**
 * @brief 是否正在"星历下发"（最近 3 s 内真的写出去过帧）。
 */
bool myvendor_gnss_eph_active(void)
{
  const uint32_t now = (uint32_t)TICK2MSEC(clock_systime_ticks());

  if (g_eph_frame_ms == 0u)
    {
      return false;
    }

  return (now - g_eph_frame_ms) < 3000u;
}
static uint32_t g_eph_last_cost_ms;
/** 上次注入是否因 LittleFS 被占（BLE/MTP 正在写）而整段跳过。 */
static bool g_eph_last_busy;

/* 星历注入 0 帧后的重试退避状态（详见 GNSS_EPH_RETRY_MIN_MS 注释）。 */
static uint32_t g_eph_retry_ms;
static uint32_t g_eph_retry_gap_ms;
static uint16_t g_eph_retry_n;

/* 读线程重启触发者，只被 gnss_thread_restart() 的日志读取。 */
static const char *g_restart_why = "?";

/**
 * @brief 读线程当前所处阶段（"卡在哪一段"）。
 *
 * @details
 * 只写字符串字面量的地址，读者只取指针，所以不存在撕裂；读线程被阻塞在
 * 系统调用里时，独立线程仍然能读到它停在哪一段。
 */
static volatile const char *g_gnss_phase = "init";

/** @brief 读线程入口 pid 的副本（`gettid()`，不是 getpid()，见入口注释）。 */
static pid_t g_gnss_pid_boot = (pid_t)-1;

/** @brief 标记读线程阶段；参数必须是字符串字面量。 */
#define GNSS_PHASE(p)  (g_gnss_phase = (p))

static void gnss_rt_end(const char *why)
{
  if (g_rt_t0_ms == 0) {
    return;
  }

  syslog(LOG_WARNING,
         "gnss: rt end why=%s total=%u nmea_ok=%d nmea_ms=%u fix_ok=%d fix_ms=%u\n",
         why, (unsigned)gnss_elapsed_ms(gnss_now_ms(), g_rt_t0_ms),
         g_rt_wait_nmea ? 0 : 1,
         g_rt_wait_nmea ? 0u
                        : (unsigned)gnss_elapsed_ms(g_rt_nmea_ms, g_rt_t0_ms),
         g_rt_wait_fix ? 0 : 1,
         g_rt_wait_fix ? 0u
                       : (unsigned)gnss_elapsed_ms(g_rt_fix_ms, g_rt_t0_ms));
  g_rt_t0_ms = 0;
}

static void gnss_rt_begin(const char *why)
{
  if (g_rt_t0_ms != 0) {
    gnss_rt_end("superseded");
  }

  g_rt_episodes++;
  g_rt_seq = 0;
  g_rt_wait_nmea = true;
  g_rt_wait_fix = true;
  g_rt_nmea_ms = 0;
  g_rt_fix_ms = 0;
  g_rt_t0_ms = gnss_now_ms();
  g_rt_last_ms = g_rt_t0_ms;
  syslog(LOG_WARNING, "gnss: rt begin #%u why=%s\n",
         (unsigned)g_rt_episodes, why);
}

/** 阶段打点：+距今阶段耗时，tot=距本轮恢复起点的总耗时。 */
static void gnss_rt_mark(const char *phase)
{
  uint32_t now;

  if (g_rt_t0_ms == 0) {
    gnss_rt_begin("auto");
  }

  now = gnss_now_ms();
  g_rt_seq++;
  syslog(LOG_WARNING, "gnss: rt %u %s +%u tot=%u\n",
         (unsigned)g_rt_seq, phase,
         (unsigned)gnss_elapsed_ms(now, g_rt_last_ms),
         (unsigned)gnss_elapsed_ms(now, g_rt_t0_ms));
  g_rt_last_ms = now;
}

static void gnss_heartbeat(void)
{
  g_heartbeat_ms = gnss_now_ms();
}

/**
 * @brief 交给 diag 时基的进度源（见 include/myvendor_diag.h）。
 *
 * 就返回读线程的心跳时间戳：它**只在读循环里**更新，所以"心跳不动"等价于
 * "读线程没在转"。无锁、无阻塞（一次 volatile 读），可以从 diag 线程调。
 *
 * 为什么需要它：`poll()` 的超时挂在本线程 TCB 的 waitdog 上，而 wdog 链会断
 * （板级 `vela_override/sched/wd_start.c` 剪环保命）—— 剪掉之后 200 ms 超时
 * 永不到期，线程只能靠信号救。2026-09-19 实机卡了 20 s，就是这条。
 */
/* 栈金丝雀自查（读线程每 ~256 轮跑一次）：栈底 16 B 不是 0xA5 就报出来。
 * 地址直接打进日志 —— 拿去 `ctl wt <addr>` 挂 DWT 数据观察点，就能抓到**是谁写的**
 * （观察点命中时记录的 PC 就是写者）。 */
static void gnss_stack_canary_check(void)
{
  const uint8_t *stk = g_gnss_stack[g_gnss_stack_slot];
  uint32_t i;

  if (stk == NULL) {
    return;
  }

  for (i = 0; i < 16u; i++) {
    if (stk[i] != 0xA5u) {
      syslog(LOG_ERR, "gnss: STACK CANARY BROKEN at %p (byte %lu = 0x%02x)\n",
             (const void *)&stk[i], (unsigned long)i, (unsigned)stk[i]);
      return;
    }
  }
}

static uint32_t gnss_reader_progress_ms(void)
{
  return g_heartbeat_ms;
}

static void gnss_recover_begin(void)
{
  g_recovering = true;
  g_recover_ms = gnss_now_ms();
}

static bool gnss_hold_unlocked(uint32_t now)
{
  if (g_idle_sleep || g_idle_parked) {
    return true;
  }

  return g_wake_hold_ms != 0 &&
         gnss_elapsed_ms(now, g_wake_hold_ms) < GNSS_EPH_GRACE_MS;
}

static bool gnss_eph_grace_locked(uint32_t now)
{
  return g_eph_grace_ms != 0 &&
         gnss_elapsed_ms(now, g_eph_grace_ms) < GNSS_EPH_GRACE_MS;
}

static bool gnss_hold_locked(uint32_t now)
{
  return gnss_hold_unlocked(now) || g_eph_busy || g_dbd_run ||
         g_dump_req || gnss_eph_grace_locked(now);
}

static void gnss_eph_grace_begin(void)
{
  uint32_t now = gnss_now_ms();

  pthread_mutex_lock(&g_lock);
  g_eph_grace_ms = now;
  pthread_mutex_unlock(&g_lock);
  /* 注入刚把读线程按住过（几秒到几十秒，见 eph done 的 cost），这段 gap 是自家
   * 造成的：留个标记给 gap 判定，免得它把这次沉默报成"读线程卡住"。 */
  g_rd_selfblock_ms = now;
  g_nmea_life_ms = now;
}

/** @brief GNSS 模组挂在 USART2（/dev/ttyS0），与 CONFIG_BOARD_L96_GNSS_DEVPATH 一致。
 *  只给驱动器锁诊断用 —— sifli_uart_diag() 收的是 1 起的 USART 序号。 */
#define GNSS_UART_NUM 2

/* 驱动器锁诊断（chips/sf32lb52/sifli_uart.c）：读线程卡在 poll() 里时，
 * "谁握着 USART2 的 recv/xmit 互斥量"和"这段时间有没有人 close 过这个端口"
 * 是两个直接证据（机制见 sifli_uart_diag 的注释）。 */
extern int sifli_uart_diag(int uart_num, int *open_count, int *rx_holder,
                           int *tx_holder, unsigned int *setup_n,
                           unsigned int *shutdown_n, int *last_shutdown_pid);

#ifdef CONFIG_MYVENDOR_GNSS_LOG
static uint32_t g_diag_why_ms;

/**
 * @brief 打印读线程"卡在哪一段"。
 *
 * @details
 * g_gnss_phase 只写字符串字面量地址，读者只取指针 —— 所以**读线程被阻塞时，
 * 独立线程（diag）照样能读到它卡在哪一段**，不需要读线程配合。这与
 * companion_bridge_phase_set / _get 是同一套手法。
 *
 * 现场故障里读线程一次循环占了 488 秒、心跳停了、但任务一直活着，光看心跳
 * 只知道"卡住了"，不知道卡在 poll / LittleFS / 写串口 中的哪一个。加这一段
 * 就是为了下次一眼看出。
 *
 * @param why 调用来源。
 * @param extra 附加数值（各调用点含义不同）。
 */
static void gnss_diag_why(const char *why, uint32_t extra)
{
  uint32_t now = gnss_now_ms();

  if (g_diag_why_ms != 0 && gnss_elapsed_ms(now, g_diag_why_ms) < 2000u) {
    return;
  }

  g_diag_why_ms = now;
  /* **用 WARN 而不是 INFO**：这三行（not ok / tcb / uart）只在"不正常"时才打
   * （本函数 2 s 一次的节流），而 diag 通道的过滤线是 WARN+ —— 用 INFO 的话
   * 现场**落不了盘**，只能靠人守在控制台前面。现场教训：卡死复现时人不在，
   * 文件里只有 restart 那几行 ERROR，缺的正是"谁握着 USART2 的两把锁"。 */
  syslog(LOG_WARNING,
         "gnss: diag not ok (%s) extra=%u phase=%s fd=%d recov=%d eph=%d dbd=%d "
         "busy=%d pid=%d/%d hb_age=%u\n",
         why, (unsigned)extra, g_gnss_phase, g_uart_fd,
         g_recovering ? 1 : 0, g_eph_busy ? 1 : 0, g_dbd_run ? 1 : 0,
         g_gnss_stack_busy ? 1 : 0, (int)g_gnss_pid, (int)g_gnss_pid_boot,
         g_heartbeat_ms != 0
             ? (unsigned)gnss_elapsed_ms(now, g_heartbeat_ms) : 0u);

  /* 同时在 diag 路径打一次 TCB 现场：schedmon 是 15 s 一次的全量 dump，
   * 这条能把"卡住那一刻"的 state/waitobj/waitdog 与 phase 放在同一行，
   * 不必再去 15 s 的 dump 里对时间。
   * 注意 nxsched_get_tcb/put_tcb 必须成对，否则会把 TCB 引用计数钉住。
   *
   * 必须 guard `> 0`：pid 0 是合法的（IDLE 任务），nxsched_get_tcb(0) 会
   * 成功返回 IDLE 的 TCB —— 于是 pid=0 时这条会打出 IDLE 的状态（st=2
   * wait=0 wd=0），看起来像 GNSS 线程，实际完全不是。现场就踩过。 */
  {
    pid_t want = (g_gnss_pid > 0) ? g_gnss_pid : g_gnss_pid_boot;

    if (want > 0)
      {
        FAR struct tcb_s *tcb = nxsched_get_tcb(want);

        if (tcb != NULL && tcb->pid == want)
          {
            /* 「还在不在就绪链上」—— 把两种完全不同的死法分开。
             *
             * 现场第 4 次：`st=3`(TSTATE_TASK_RUNNING) + `wait=0` + `wd=0` +
             * `lock=0`，同时 `tx_hold/rx_hold=-1`（没人持锁）、`shutdown_n=0`
             * （没人 close 过）。四个字段合起来只有一个解释：**唤醒路径跑完了
             * （waitobj 被清、任务被加进就绪链、状态标成 RUNNING），但 CPU
             * 再也没切到它** —— 不是"卡在等什么"，所以 SIGUSR1 / poll 超时 /
             * cancel 全都使不上劲。
             *
             *   on_rdy=1 → 它确实挂在就绪链上：调度没能切过去的路径；
             *   on_rdy=0 → **状态说 RUNNING、链上却没有它**：被某条唤醒/摘链
             *              路径漏掉了（`dq_rem` 与 `nxsched_add_readytorun` 的配平）。
             *
             * 遍历用公开字段：单 CPU 下 `this_task()` 就是就绪链头（sched.h:257）。
             * 有界步数，链在变也不会转圈。 */
            FAR struct tcb_s *t = this_task();
            int on_rdy = 0;
            int hops;

            for (hops = 0; t != NULL && hops < 64; hops++, t = t->flink)
              {
                if (t == tcb)
                  {
                    on_rdy = 1;
                    break;
                  }
              }

            /* `self` / `blink` 是**给下一次复现准备的指纹**（2026-09-21 加）：
             * 现场已两次读到"st=RUNNING 但不在就绪链上"（`on_rdy=0`），
             * 而全树只有 6 处写 `TSTATE_TASK_RUNNING`、每一处都在把线程**放进**
             * 链表的那一段里 ⇒ 说明它是**先被挂上、之后又被摘下来**的。
             * 摘链是谁干的：`dq_rem()` 会把 flink/blink 都清成 NULL
             * （对照：本板 wdog 表那次损坏现场也是 `00000000 00000000` 形状）；
             * 只把 flink 指向自己则是另一种（自环）；两项都原样指向邻居则说明
             * 它**还在某条链上**（那 on_rdy=0 就得另找解释）。
             * 所以这三个指针一起看，才能把"从未挂上 / 被摘掉 / 还在别的链上"分开。 */
            syslog(LOG_WARNING,
                   "gnss: diag tcb pid=%d self=%p st=%u lock=%d wait=%p wd=%u "
                   "on_rdy=%d flink=%p blink=%p\n",
                   (int)tcb->pid, (void *)tcb, (unsigned)tcb->task_state,
                   (int)tcb->lockcount, (void *)tcb->waitobj,
                   WDOG_ISACTIVE(&tcb->waitdog) ? 1u : 0u,
                   on_rdy, (void *)tcb->flink, (void *)tcb->blink);
          }
        else
          {
            syslog(LOG_WARNING, "gnss: diag tcb gone pid=%d\n", (int)want);
          }

        if (tcb != NULL)
          {
            nxsched_put_tcb(tcb);
          }
      }
    else
      {
        /* 两个 pid 都不是正值：说明线程入口没把 getpid() 存进来（或存的就是
         * 0），此时无法定位 TCB。宁可说不知道，也不要打 IDLE 的状态。 */
        syslog(LOG_WARNING, "gnss: diag tcb unknown (pid=%d/%d)\n",
               (int)g_gnss_pid, (int)g_gnss_pid_boot);
      }
  }

  /* 驱动器侧现场：谁握着 USART2 的两把互斥量、这期间有没有 close/open 过。
   *
   * `poll()` 卡的就是 `nxmutex_lock(&dev->xmit.lock)` / `(&dev->recv.lock)`
   * （serial.c:1847/1869，无超时、信号免疫），所以 holder 的 pid 加上面那行
   * 自己的 TCB 状态，就能定位"是谁把读线程按住的"。
   * `setup_n`/`shutdown_n` 相比上一轮增长 = 有人 close/open 过这个端口 ——
   * **最后一次 close 会走 `uart_reset_sem()`（serial.c:2274），那是唯一能把
   * 这两把锁弄成永久不可获取的路径**（`nxmutex_reset` 会把等待者丢掉，
   * 之后谁再 lock 就一直等下去）。 */
  {
    int rx_holder = -2;
    int tx_holder = -2;
    int open_count = -2;
    int last_shutdown = -2;
    unsigned int setup_n = 0;
    unsigned int shutdown_n = 0;

    if (sifli_uart_diag(GNSS_UART_NUM, &open_count, &rx_holder, &tx_holder,
                        &setup_n, &shutdown_n, &last_shutdown) == 0)
      {
        syslog(LOG_WARNING,
               "gnss: diag uart tx_hold=%d rx_hold=%d open_cnt=%d setup_n=%u shutdown_n=%u last_shutdown_pid=%d\n",
               tx_holder, rx_holder, open_count, setup_n, shutdown_n,
               last_shutdown);
      }
  }
}
#else
#  define gnss_diag_why(w, e) ((void)0)
#endif

static bool gnss_thread_ok(uint32_t stall_ms)
{
  uint32_t hb = g_heartbeat_ms;

  if (hb == 0)
    {
      return false;
    }

  if (stall_ms == 0)
    {
      stall_ms = GNSS_THREAD_STALL_MS;
    }

  return gnss_elapsed_ms(gnss_now_ms(), hb) < stall_ms;
}

static bool gnss_pid_alive(pid_t pid)
{
  FAR struct tcb_s *tcb;
  bool alive;

  if (pid <= 0)
    {
      /* pid 0 是 IDLE 任务：nxsched_get_tcb(0) 会**成功**返回它的 TCB，
       * 把它当成读线程就完全错了（现场踩过，见 gnss_diag_why 的注释）。 */
      return false;
    }

  /* 用 TCB 查找，不用 `kill(pid, 0)`。后者是 POSIX 的"探测进程是否存在"写法，
   * 但 NuttX 这边没有这条语义：`nxsig_kill()` 里 `GOOD_SIGNO(0)` 为真
   * （`(unsigned)0 <= MAX_SIGNO`），于是它会真的往目标投递一个"信号 0"，
   * 后面按下标算 pending 位时用的是 si_signo-1。不赌这个。
   * `nxsched_get_tcb` 是文档化的查法；**必须成对 put**，否则 TCB 引用被钉住。 */
  tcb = nxsched_get_tcb(pid);
  alive = (tcb != NULL && tcb->pid == pid);
  if (tcb != NULL)
    {
      nxsched_put_tcb(tcb);
    }

  return alive;
}

static void gnss_link_add(uint16_t *slot)
{
  if (slot != NULL && *slot < 0xffffu)
    {
      (*slot)++;
    }
}

static void gnss_link_note(uint16_t *slot, uint16_t n)
{
  uint32_t sum;

  if (slot == NULL || n == 0)
    {
      return;
    }

  sum = (uint32_t)*slot + n;
  *slot = (sum > 0xffffu) ? 0xffffu : (uint16_t)sum;
}

/**
 * @brief 周期链路摘要。无 NMEA / 快照过期用 5 s，正常 15 s。
 *
 * UI「无数据」= myvendor_gnss_get 3 s 内无 publish。星历灌入/倒库/
 * 静止下电/唤醒上电窗口内保持上次快照，不把预期静默判成无数据。
 */
static void gnss_link_report(int fd, uint8_t rx_st, uint32_t silent_ms)
{
  uint32_t now = gnss_now_ms();
  uint32_t pub_age = 0;
  uint32_t nmea_age = 0;
  uint32_t period;
  uint32_t win_ms = 0;
  bool snap_ok;

  if (g_fix_ms != 0)
    {
      pub_age = gnss_elapsed_ms(now, g_fix_ms);
    }

  if (g_nmea_life_ms != 0)
    {
      nmea_age = gnss_elapsed_ms(now, g_nmea_life_ms);
    }

  snap_ok = (g_fix.alive && g_fix_ms != 0 && pub_age < GNSS_STALE_MS);
  period = (!snap_ok || silent_ms >= 1500u || g_link.nmea == 0) ?
           GNSS_LINK_BAD_MS : GNSS_LINK_OK_MS;
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
  if (period > 5000u)
    {
      period = 5000u;
    }
#endif
  if (g_link_ms != 0 && gnss_elapsed_ms(now, g_link_ms) < period)
    {
      return;
    }

  /* 窗口长度：两次真正打印之间的毫秒数（首窗口从开机算）。 */
  win_ms = (g_link_ms != 0) ? gnss_elapsed_ms(now, g_link_ms) : now;
  g_link_ms = now;
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
  {
    /* 自测工作量的**窗口增量**。窗口长度 win 一起打出来，占比就能自己算：
     *   busy/win  = 读线程非 poll 的驻留占比（上界，含被抢占）
     *   read/win  = 花在 read() 里的占比 → 顶起来说明烧在 uart_read 的 harvest
     *   pollw/win = 阻塞在 poll 里的占比 → 健康时这一项应该占大头
     *   loop      = 本窗口迭代轮数 → 与 busy 一起区分"真在烧"和"被抢占/等锁"
     *
     * 这几个量全部由读线程自己用 DWT 累加，不经过内核的 200 Hz 瞬时采样器
     * （ps 的 CPU% 是那个算的，对短促突发噪声极大）。 */
    static uint32_t p_loop;
    static uint32_t p_work;
    static uint32_t p_read;
    static uint32_t p_poll;
    static uint32_t p_close;
    uint32_t win = (win_ms != 0) ? win_ms : 1u;
    uint32_t loop_n = g_rd_loop_seq;
    uint32_t work = g_rd_work_ms;
    uint32_t rd_ms = g_rd_read_ms;
    uint32_t pw_ms = g_rd_poll_ms;

    syslog(LOG_DEBUG,
           "gnss: link cpu win=%u loop=%u busy=%u read=%u pollw=%u busy_pm=%u close=%u empty=%u backoff=%u prio_bad=%u capped=%u dthuge=%u\n",
           (unsigned)win, (unsigned)(loop_n - p_loop),
           (unsigned)(work - p_work), (unsigned)(rd_ms - p_read),
           (unsigned)(pw_ms - p_poll),
           (unsigned)(((work - p_work) * 1000u) / win),
           (unsigned)(g_rd_poll_close - p_close),
           (unsigned)g_rd_empty_run, (unsigned)g_rd_backoff_n,
           (unsigned)g_prio_restore_fail, (unsigned)g_rd_spin_capped,
           (unsigned)g_rd_dt_huge);

    p_loop = loop_n;
    p_work = work;
    p_read = rd_ms;
    p_poll = pw_ms;
    p_close = g_rd_poll_close;
  }
#endif /* CONFIG_MYVENDOR_GNSS_TRACE */

  /* 这一条是多行 format 的老语句，用编译期常量而不是 GNSS_TRACE 宏（宏插不进去
   * 又会撞上安全扫描对跨行 format 的判定）：关掉时整段被优化掉，不执行。 */
  if (GNSS_TRACE_ON)
    syslog(LOG_DEBUG,
           "gnss: link fd=%d st=%u silent=%u nmea_age=%u pub_age=%u "
         "hz=%u alive=%d recov=%d fail=%u "
         "rd=%u byte=%u nmea=%u bad=%u ubx=%u pvt=%u junk=%u "
         "poll_to=%u poll_err=%u wr_fail=%u pub=%u "
         "semnull=%u lock=%d\n",
         fd, (unsigned)rx_st, (unsigned)silent_ms, (unsigned)nmea_age,
         (unsigned)pub_age, (unsigned)g_rx_hz, snap_ok ? 1 : 0,
         g_recovering ? 1 : 0, (unsigned)g_uart_fails,
         (unsigned)g_link.rd, (unsigned)g_link.byte, (unsigned)g_link.nmea,
         (unsigned)g_link.nmea_bad, (unsigned)g_link.ubx,
         (unsigned)g_link.pvt, (unsigned)g_link.junk, (unsigned)g_link.poll_to,
         (unsigned)g_link.poll_err, (unsigned)g_link.wr_fail,
         (unsigned)g_link.pub,
         (unsigned)myvendor_schedmon_null_count(),
         running_task() != NULL ? (int)running_task()->lockcount : 0);
  snprintf(g_link_last, sizeof(g_link_last),
           "fd=%d st=%u silent=%u nmea_age=%u pub_age=%u hz=%u "
           "alive=%d recov=%d fail=%u rd=%u nmea=%u poll_to=%u pub=%u "
           "semnull=%u",
           fd, (unsigned)rx_st, (unsigned)silent_ms, (unsigned)nmea_age,
           (unsigned)pub_age, (unsigned)g_rx_hz, snap_ok ? 1 : 0,
           g_recovering ? 1 : 0, (unsigned)g_uart_fails,
           (unsigned)g_link.rd, (unsigned)g_link.nmea,
           (unsigned)g_link.poll_to, (unsigned)g_link.pub,
           (unsigned)myvendor_schedmon_null_count());
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
  GNSS_TRACE("link extra idle=%d parked=%d eph=%d dbd=%d hold=%d kick=%d "
           "poll_run=%u gsv %u/%u/%u/%u/%u",
           g_idle_sleep ? 1 : 0, g_idle_parked ? 1 : 0,
           g_eph_busy ? 1 : 0, g_dbd_run ? 1 : 0,
           gnss_hold_unlocked(now) ? 1 : 0, g_diag_kick ? 1 : 0,
           (unsigned)g_poll_to_run,
           /* 按星座分组的 GSV 句子数（gp/gl/ga/bd/gq）：哪几项非 0 就说明
            * 哪几个星座真的在跑 —— 本固件读不回星座开关，这是唯一的证据。 */
           (unsigned)g_gsv_talk[0], (unsigned)g_gsv_talk[1],
           (unsigned)g_gsv_talk[2], (unsigned)g_gsv_talk[3],
           (unsigned)g_gsv_talk[4]);
#endif
  memset(&g_link, 0, sizeof(g_link));
  memset(g_gsv_talk, 0, sizeof(g_gsv_talk));
}

static void gnss_rx_reset(void)
{
  g_rx_win_ms = 0;
  g_rx_win_n = 0;
  g_rx_hz = 0;
  g_rx_last_ms = 0;
}

static void gnss_rx_note(uint32_t now)
{
  uint32_t dt;

  g_rx_last_ms = now;
  if (g_rx_win_ms == 0) {
    g_rx_win_ms = now;
    g_rx_win_n = 1;
    return;
  }

  dt = gnss_elapsed_ms(now, g_rx_win_ms);
  g_rx_win_n++;
  if (dt >= GNSS_RX_WIN_MS) {
    unsigned hz = (g_rx_win_n * 1000u + dt / 2u) / dt;

    if (hz > 99u) {
      hz = 99u;
    }

    g_rx_hz = (uint8_t)hz;
    g_rx_win_ms = now;
    g_rx_win_n = 0;
  }
}

static uint8_t gnss_rx_hz_now(uint32_t now)
{
  if (g_rx_last_ms == 0 || gnss_elapsed_ms(now, g_rx_last_ms) >= GNSS_RX_DEAD_MS) {
    gnss_rx_reset();
    return 0;
  }

  if (g_rx_hz == 0 && g_rx_win_n > 0) {
    return 1;
  }

  return g_rx_hz;
}

static int32_t gnss_to_e7(float deg)
{
  double v;

  if (!isfinite(deg)) {
    return 0;
  }

  v = (double)deg * 10000000.0;
  if (v >= 0.0) {
    v += 0.5;
  } else {
    v -= 0.5;
  }

  return (int32_t)v;
}

static uint8_t gnss_hdop_x10(const struct minmea_float *f)
{
  float h;
  unsigned v;

  if (f == NULL) {
    return 0;
  }

  h = minmea_tofloat(f);
  if (!isfinite(h) || h < 0.1f || h >= 50.0f) {
    return 0;
  }

  v = (unsigned)(h * 10.0f + 0.5f);
  return (uint8_t)(v > 255u ? 255u : v);
}

static uint8_t gnss_quality_from_gsa(int fix_type)
{
  if (fix_type >= MINMEA_GPGSA_FIX_3D) {
    return 2;
  }

  if (fix_type >= MINMEA_GPGSA_FIX_2D) {
    return 1;
  }

  return 0;
}

static void gnss_remember_date(const struct minmea_date *date)
{
  if (date != NULL && date->year != -1 && date->month > 0 && date->day > 0) {
    g_last_date = *date;
  }
}

static void gnss_try_set_utc(myvendor_sys_gnss_t *fix,
                             const struct minmea_date *date,
                             const struct minmea_time *t)
{
  struct timespec ts;
  const struct minmea_date *d = date;

  if (fix == NULL || t == NULL || t->hours < 0) {
    return;
  }

  if (d == NULL || d->year == -1) {
    d = &g_last_date;
  } else {
    gnss_remember_date(d);
  }

  if (d->year == -1) {
    return;
  }

  if (minmea_gettime(&ts, d, t) != 0 || ts.tv_sec < (time_t)GNSS_TIME_MIN_UNIX) {
    return;
  }

  fix->utc_sec = (uint32_t)ts.tv_sec;

  /* 模块报出了正常时间 —— 这就是"不需要强制灌星历"的判据（见 g_mod_time_ms）。 */
  g_mod_time_ms = gnss_now_ms();

  /* 无定位也会出 GPS 时；直接改系统钟，顶栏不必等 valid。 */
  {
    struct timespec now;
    struct timeval tv;

    clock_gettime(CLOCK_REALTIME, &now);
    {
      long long delta = (long long)now.tv_sec - (long long)ts.tv_sec;

      if (delta < 0) {
        delta = -delta;
      }

      if (now.tv_sec < (time_t)GNSS_TIME_MIN_UNIX || delta >= 2) {
        tv.tv_sec = ts.tv_sec;
        tv.tv_usec = 0;
        (void)settimeofday(&tv, NULL);
      }
    }
  }
}

static void gnss_ubx_cksum(const uint8_t *data, size_t n, uint8_t *cka,
                           uint8_t *ckb)
{
  size_t i;
  uint8_t a = 0;
  uint8_t b = 0;

  for (i = 0; i < n; i++) {
    a = (uint8_t)(a + data[i]);
    b = (uint8_t)(b + a);
  }

  *cka = a;
  *ckb = b;
}

/* 搜星阶段 RMC 常无日期；打开 NMEA ZDA 才能对时。 */
static void gnss_enable_zda(int fd)
{
  uint8_t frame[11];
  uint8_t cka;
  uint8_t ckb;

  frame[0] = 0xb5;
  frame[1] = 0x62;
  frame[2] = 0x06;
  frame[3] = 0x01;
  frame[4] = 0x03;
  frame[5] = 0x00;
  frame[6] = 0xf0;
  frame[7] = 0x08;
  frame[8] = 0x01;
  gnss_ubx_cksum(frame + 2, 6, &cka, &ckb);
  frame[9] = cka;
  frame[10] = ckb;
  {
    /* 发送也走板级直连（同一个 USART2 HAL 句柄，阻塞发小帧）。 */
    int wr = myvendor_gnss_uart_write(frame, sizeof(frame));

    if (wr != 0) {
      gnss_link_add(&g_link.wr_fail);
      syslog(LOG_WARNING, "gnss: zda enable wr=%d\n", wr);
    }
  }
}

static void gnss_configure_nav(int fd);
static void gnss_eph_run(int fd);
static void gnss_snapshot_dead(void);
/* 定义在 PVT 解码那一段（读线程附近），但 gnss_publish() 先用它。 */
static void gnss_pvt_attach(myvendor_sys_gnss_t *fix, uint32_t now);
/* 定义在配置/星历那两段里，但 gnss_assist_tick() 与 gnss_configure_nav() 先用。 */
static void gnss_eph_send_pos(int fd);
static void gnss_cfg_dump_signal(int fd);

static int gnss_open(void)
{
  int fd = open(CONFIG_BOARD_L96_GNSS_DEVPATH, O_RDWR | O_NONBLOCK);

  if (fd < 0) {
    return fd;
  }

  /** **UART 的"上电"仍由这次 open 完成**，但数据面从此走板级直连
   *  （`myvendor_gnss_uart.c`）：驱动在这次 open 里做 HAL init + DMA 环形 +
   *  IDLE 中断，并把收到的字节同时喂给板级环形（见 `sifli_uart_rx_put()` 的钩子）；
   *  本服务**不再 poll、不再 read 这个 fd** —— 于是没有 pollfd 注册、
   *  没有 tick 驱动的 poll 超时、没有 recv.lock 等待（n587/n588/n590 那族消失）。 */
  if (myvendor_gnss_uart_start() != 0) {
    syslog(LOG_WARNING, "gnss: uart direct start failed\n");
  }

  return fd;
}

static void gnss_link_reset(void)
{
  g_link_got_nmea = false;
  g_link_got_fix = false;
  g_link_got_pub = false;
  g_link_ms = 0;
  memset(&g_link, 0, sizeof(g_link));
}

static void gnss_mod_ram_lost(void)
{
  gnss_dbd_abort();
  g_dyn_have = 0;
  g_dyn_try_ms = 0;
  /* 模组掉电 → 配置回默认，读回的那次就作废（见 gnss_cfg_dump_signal）。 */
  g_cfg_dumped = false;
}

/** 仅读线程退出时 close。运行中 close 会 uart_reset_sem，把 recv.lock 搞坏。
 *  USART2 和 NSH 一样保持 open 并继续 poll。 */
static void gnss_fd_release(int *fd)
{
  if (fd != NULL && *fd >= 0) {
    if (g_uart_fd == *fd) {
      g_uart_fd = -1;
    }

    /* 先停直连数据面（读者会被唤醒并看到 0 字节），再关 fd（那会 uart_reset_sem）。 */
    myvendor_gnss_uart_stop();
    close(*fd);
    *fd = -1;
  }
}

static void gnss_module_power_cycle(void)
{
  unsigned off_ms = (unsigned)CONFIG_BOARD_L96_GNSS_PWR_OFF_MS;
  unsigned on_ms = (unsigned)CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS;

  /* MAX-M10S-00B-01：切 VCC（PA43）。USART2 保持 open。 */

  /* 这 3.5 s 读线程不会更新心跳，对 diag 时基明说"我在忙"：被踢的话
   * `gnss_sleep_ms()` 会按 EINTR 提前返回，模组的上电稳定时间就不够了。
   * 放在这里而不是调用方，是因为开机第一次 `fd < 0` 也会走这条路。 */
  myvendor_diag_watch_hold(pthread_self(), true);

  syslog(LOG_WARNING,
         "gnss: " GNSS_MOD_PN " VCC cycle PA43 (off %u ms, settle %u ms)\n",
         off_ms, on_ms);
  gnss_rt_mark("vcc-off");
  sf32lb52_l96_uart_pins();
  (void)sf32lb52_l96_power(false);
  g_mod_pwr = false;
  gnss_sleep_ms(off_ms);
  gnss_rt_mark("vcc-power-down");
  (void)sf32lb52_l96_power(true);
  gnss_sleep_ms(on_ms);
  g_mod_pwr = true;
  gnss_rt_mark("vcc-settle");

  myvendor_diag_watch_hold(pthread_self(), false);
}

static void gnss_flush_host_rx(int fd)
{
  char dump[64];
  int n;
  int loops = 0;

  if (fd < 0) {
    return;
  }

  /* 切 VCC 时 USART2 仍 open，丢掉断电期间浮空 RX 攒下的字节。
   * 直连数据面之后这一步是**原子**的：直接把环形读游标追平写游标。 */
  myvendor_gnss_uart_flush();
}

static void gnss_reconfig_after_vcc(int fd, bool cycle)
{
  bool hold_eph = g_eph_hold;

  if (fd < 0) {
    return;
  }

  /* 这一段会长时间不更新心跳（7 次 cfg 各等 250 ms ACK，灌注星历还要读 LFS +
   * 写模组，十几秒），对 diag 时基明说"我在忙"，免得被当成卡死踢。 */
  myvendor_diag_watch_hold(pthread_self(), true);

  /* 模块刚上电/刚重配：给"等它报时间"的窗口计时（见 g_mod_up_ms）。 */
  g_mod_up_ms = gnss_now_ms();

  gnss_flush_host_rx(fd);
  gnss_rt_mark("cfg-flush");
  gnss_enable_zda(fd);
  gnss_rt_mark("cfg-zda");
  gnss_configure_nav(fd);
  gnss_rt_mark("cfg-nav");
  if (hold_eph) {
    g_eph_hold = false;
    g_eph_pending = false;
    syslog(LOG_WARNING, "gnss: skip eph inject (recover without mga)\n");
  } else if (cycle) {
    gnss_eph_run(fd);
  } else if (!g_eph_injected &&
             g_eph_wake_defer_ms != 0 &&
             gnss_elapsed_ms(gnss_now_ms(), g_eph_wake_defer_ms) <
               GNSS_EPH_WAKE_DEFER_MS) {
    /* 刚从 idle park 醒来时**不急着重灌**：模组确实掉电了（星历没了），但板子
     * 此刻还静止停着，定位不是当下的需求；而那次灌注会把读线程按住十几秒
     * （读 LFS + 写模组，见 eph done 的 cost）。推迟到"手机来要"（TIME_SYNC
     * 触发的 reload 走 gnss_eph_try_reload，不经这里）或超过
     * GNSS_EPH_WAKE_DEFER_MS —— 后者是兜底，免得真上路了还在等。 */
    static bool wake_defer_logged;

    if (!wake_defer_logged) {
      wake_defer_logged = true;
      syslog(LOG_INFO, "gnss: eph inject deferred (woke from idle park)\n");
    }
  } else if (!g_eph_injected) {
    g_eph_wake_defer_ms = 0;

    if (g_ui_ready) {
      gnss_eph_run(fd);
    } else if (myvendor_devctl_eph_auto_get()) {
      g_eph_pending = true;
      syslog(LOG_INFO, "gnss: eph inject deferred until ui\n");
    } else {
      g_eph_injected = true;
    }
  }

  gnss_rt_mark("cfg-done");
  myvendor_diag_watch_hold(pthread_self(), false);
}

/**
 * 静默 / 诊断：只切模组 VCC，不 close UART。
 *
 * @param why 仅用于诊断日志，说明本次恢复由谁触发。
 */
static void gnss_vcc_recover(int fd, bool force, const char *why)
{
  /* 断电重启 + 重配全程 3~4 s 不更新心跳（两段 sleep），对 diag 时基明说"我在
   * 忙"——否则它会在断电中途踢一脚，把那两段 sleep 打断，模组上电稳定时间就
   * 不够了（`gnss_sleep_ms` 会按 EINTR 提前返回）。 */
  myvendor_diag_watch_hold(pthread_self(), true);

  gnss_mod_ram_lost();
  gnss_snapshot_dead();
  gnss_link_reset();
  if (force) {
    g_uart_fails = GNSS_PWR_FAILS;
  } else {
    g_uart_fails++;
  }

  GNSS_LOG("vcc recover force=%d fails=%u recov=%d eph=%d dbd=%d fd=%d",
           force ? 1 : 0, (unsigned)g_uart_fails,
           g_recovering ? 1 : 0, g_eph_busy ? 1 : 0, g_dbd_run ? 1 : 0, fd);

  syslog(LOG_WARNING,
         "gnss: vcc recover why=%s force=%d fails=%u sess=%u gap_max=%u "
         "stall1s=%u poll_close=%u\n",
         why, force ? 1 : 0, (unsigned)g_uart_fails,
         g_rd_session_ms != 0
           ? (unsigned)gnss_elapsed_ms(gnss_now_ms(), g_rd_session_ms) : 0u,
         (unsigned)g_rd_gap_max_ms, (unsigned)g_rd_stall_1s,
         (unsigned)g_rd_poll_close);

  gnss_rt_begin(why);
  gnss_module_power_cycle();
  g_uart_fails = 0;
  gnss_reconfig_after_vcc(fd, true);
  myvendor_diag_watch_hold(pthread_self(), false);
}

/** 静止休眠：切 MAX-M10S-00B-01 VCC。USART2 保持 open，读线程继续 poll。 */
static void gnss_module_park(void)
{
  gnss_mod_ram_lost();
  g_uart_fails = 0;
  gnss_link_reset();
  sf32lb52_l96_uart_pins();
  (void)sf32lb52_l96_power(false);
  g_mod_pwr = false;
}

static void gnss_snapshot_dead(void)
{
  bool was_alive;

  pthread_mutex_lock(&g_lock);
  was_alive = g_fix.alive;
  g_fix.alive = false;
  g_fix.valid = false;
  g_fix.rx_hz = 0;
  g_fix_ms = 0;
  gnss_rx_reset();
  pthread_mutex_unlock(&g_lock);
  if (was_alive) {
    syslog(LOG_WARNING, "gnss: snapshot dead (UI 无数据)\n");
  }
}

/**
 * @brief 从 GNSS UART 读一次（有界超时的 poll + read），返回读到的字节数。
 *
 * @param[in]  fd         GNSS UART 文件描述符。
 * @param[out] buf        目标缓冲。
 * @param[in]  max        缓冲长度；<=0 时直接返回 0。
 * @param[in]  timeout_ms poll 超时。
 * @return 读到/搬出的字节数；<=0 表示超时或错误（错误已计入链路统计）。
 *
 * @note 超时**硬钳位**在 (0, 1000]，越界一律按 200 ms 处理。
 *       理由是 POSIX 里 `timeout < 0` 是**永久阻塞**；即便现在有
 *       `gnss_reader_wake()` 的信号兜底，也不值得为了省一个参数把"永久"
 *       引进来 —— 钳位保证每次调用最多 200 ms 就回到主循环顶一次，心跳、
 *       kick / stop 的响应都挂在这个节奏上。
 *
 * @warning 这个 `timeout_ms` 现在**就是 `poll()` 的超时参数**，但它已经
 *          **不再是唯一的退出途径** —— 这点是本次改动的全部要点。
 *
 *          历史：带钳位的版本日志里卡死依旧（`hb_age=80840`、`stop_before=1`、
 *          `busy=1`，卡了 80 秒），所以"timeout 本身无界"的推断是错的。
 *          真正的机制是 `poll(…, timeout)` 内部走 `nxsem_tickwait()`，而
 *          `poll()` 的超时**由 tick 驱动**；时间基准一被打断，所有基于 tick
 *          的超时（poll / usleep / nxsem_tickwait）会一起失灵，线程就永远
 *          停在 `poll()` 里 —— reap 的 `g_gnss_stack_busy` 也就永远是 1。
 *
 *          上一版据此把等待换成了 `poll(…, 0)` + DWT 周期预算自旋（不依赖
 *          tick），代价是**忙等**：实测读线程 51.8% CPU，而 CPU0 IDLE 是
 *          0.0%（自旋一次都没让 IDLE 进去）。
 *
 *          现在换成"阻塞 poll + 信号唤醒"：tick 只影响**记账节奏**，不影响
 *          能否退出 —— `gnss_reader_wake()` 发的信号不经过 tick，reap /
 *          diag kick 一定能把线程从 `nxsem_tickwait()` 里打断。
 *          见 docs/gnss_ble_diag_log.md §2.2 / §2.5。
 */
/** @brief 周期报表用：**解析/链路侧**的计数（`ctl cnt gnss` 的第二行）。
 *
 *  为什么必须单列一条：`reader gap` 只说明"读得慢"，**分不出"没收到"还是
 *  "收到但没解析出来"**。这两组计数一起看就分得开：
 *    · `rd`/`byte` 不涨                ⇒ 根本没读到（模组/中断侧没数据）；
 *    · `byte` 涨而 `nmea`/`ubx` 不涨 且 `bad`/`junk` 涨 ⇒ 收到了但**校验/解析异常**；
 *    · `nmea`/`ubx`/`pvt` 正常涨       ⇒ 链路与解析都好，问题只在那几秒的节奏。
 *  注：这些是"本开机累计"的 uint16（**到 0xffff 会饱和**），相邻两条相减就是增量；
 *  真要看窗口值请用 `ctl gnss`（那里有 win_* 一套）。
 */
void myvendor_gnss_link_cnt_report(void)
{
  /* 周期报表 = INFO（信息级）；异常另有 ERROR 直报，不受 `ctl cnt` 开关影响。 */
  syslog(LOG_INFO,
         "gnss: cnt link rd=%u byte=%u nmea=%u bad=%u ubx=%u pvt=%u junk=%u "
         "pub=%u wr_fail=%u gap_max=%u\n",
         (unsigned)g_link.rd, (unsigned)g_link.byte, (unsigned)g_link.nmea,
         (unsigned)g_link.nmea_bad, (unsigned)g_link.ubx, (unsigned)g_link.pvt,
         (unsigned)g_link.junk, (unsigned)g_link.pub,
         (unsigned)g_link.wr_fail, (unsigned)g_rd_gap_max_ms);
}

/** @brief 读路径已切到**板级直连数据面**（2026-09-24，`myvendor_gnss_uart.c`）。
 *
 *  运行期的"走的哪个驱动"由模块自己打：`gnss_uart: DRIVER=direct start(…)`
 *  / `first-data` / `tick …`（带 rx/feed/drop/wait/eintr/tx 计数），而旧的
 *  `gnss: DRIVER=poll …` 那一行**从此不再出现** —— 新旧成对可辨。
 *
 *  于是本函数本体不再有：pollfd 注册、`fds->arg` 指向已释放内存、tick 驱动的
 *  poll 超时（n587/n588/n590 那族的三个必要条件一起消失）。 */
static int gnss_read_some(int fd, char *buf, int max, int timeout_ms)
{
  ssize_t n;
  int pr;
  bool read_inline = false;

  if (buf == NULL || max <= 0) {
    return 0;
  }

  /* timeout_ms < 0 ⇒ 主读循环的"一直等"；>= 0 ⇒ 有界等待（旧的 poll 超时语义）。
   * 负数不能被下面的钳位吞掉，所以先放行它。 */
  if (timeout_ms >= 0 && (timeout_ms == 0 || timeout_ms > 1000)) {
    timeout_ms = 200;
  }

  /* （旧的 TLS pollfd 与 poll 注册在这条路上已经不需要了：数据面走
   *  `myvendor_gnss_uart` 的环形+信号量，不再把任何 pollfd 交给驱动。） */

  /* poll 与 read 分开打点：现场那次是"一次循环占 488 s"，光看心跳分不出卡在等待
   * 本身还是等待之后的取数。直连之后两者合并，阶段名沿用 `read-poll*` 以免打断
   * 既有日志对照。 */
  GNSS_PHASE("read-poll");
  /* 阻塞式 poll：线程真正睡在这里，由 UART RX 中断（或超时到点）唤醒。
   *
   * **这一行是修 CPU 占用的关键。** 原先是 `poll(…, 0)` 非阻塞探测 + DWT
   * 周期预算自旋，那是**忙等**：GNSS 的 NMEA 是 1 Hz，而每次调用预算是
   * 200 ms —— 一个窗口里绝大多数时候没有数据，于是每次都把预算自旋满。
   * 实测读线程占 51.8% CPU、而 CPU0 IDLE 是 0.0%（自旋一次都没让 IDLE
   * 进去），见 docs/gnss_ble_diag_log.md §2.5。
   *
   * 那原先为什么不敢阻塞？因为 `poll()` 的超时走 `nxsem_tickwait()`
   * （nuttx/fs/vfs/fs_poll.c），是**由 tick 驱动**的：计时基准一被打断就不再
   * 到期，线程永远停在 `poll()` 里（§2.2）。现在这条风险被
   * `gnss_reader_wake()` 兜住了 —— 信号**不经过 tick**，reap / diag 一定能把
   * 线程从 `nxsem_tickwait()` 里打断（返回 -EINTR）。于是这个超时退化成单纯的
   * **记账节奏**（心跳、silence 检测、dbd_tick、星历重载都挂在它上面），
   * 而不再是唯一的退出途径，可以放心阻塞。
   *
   * `pr` 的语义没变：0 = 本轮没数据（调用方据此记 poll_to），>0 = 可读。
   * EINTR 时返回 0，让调用方回到循环顶重看 `g_gnss_stop` / `g_diag_kick`。 */
  {
    /* 取证：poll 期间 `g_poll_enter_ms` 保持非 0，返回即清 0。
     * reap 时看到"非 0 且已经很旧"就是"有一次 poll 进去没出来"的直接证据；
     * 若它一直是 0，说明线程从没进过 poll —— 那是"TCB 建了没跑起来"那一类，
     * 与"卡在 poll 里"是完全相反的结论，靠这一行区分。
     * 用 `t_poll ? t_poll : 1u` 是因为 gnss_now_ms() 在刚初始化时可能正好是 0，
     * 而 0 必须无歧义地表示"不在 poll 里"。 */
    uint32_t t_poll = gnss_now_ms();

    g_poll_n++;
    g_poll_enter_ms = t_poll ? t_poll : 1u;
    /** **板级直连数据面：把原来的 `poll()` 换成"等 + 取"一次调用**
     *  （2026-09-24，`myvendor_gnss_uart.c`）。
     *
     *  ISR 把 USART2 收到的字节推环形并 `sem_post`，读者阻塞在这个信号量上；
     *  diag 的 `SIGUSR1` 踢进来时模块返回 0（它不区分 EINTR），语义与旧的
     *  "poll 超时、本轮无数据"完全一致 ⇒ 下游记账（`g_link.poll_to`、空读退避、
     *  silence 判定）本来就不用改。
     *
     *  这里**不再注册 pollfd**：驱动侧从此没有指向本线程的注册，
     *  `uart_datasent()`（TX 完成中断）也就没有回调可打 ——
     *  n587/n588/n590 那条"陈旧注册 → `nxsem_get_value(已复用成 mutex 的内存)`"
     *  的链**从缺了必要条件**（下面那段 `pfd->cb = NULL` 的对象仍是我们自己的
     *  内存、且已不再被 poll 装过，纯属留档）。 */
    {
      uint32_t t_read = gnss_now_ms();

      GNSS_PHASE("read-direct");
      /** 两条语义都要保留（2026-09-24 现场教训）：
       *    · 主读循环（timeout_ms < 0）→ **阻塞等**（不烧 CPU，靠 diag 的 SIGUSR1
       *      提供周期记账）；
       *    · 其它调用点（ACK 等待、`gnss_drain` 收尾）⇒ **有界** —— 它们原先靠
       *      "poll 超时 + 非阻塞读"在 2 ms~几百 ms 后返回，若在这里改成阻塞等待，
       *      配置/收尾流程会被拖到下一个字节才动（实测表现为开机后迟迟不定位）。 */
      if (timeout_ms < 0) {
        n = (ssize_t)myvendor_gnss_uart_read(buf, (size_t)max);
      } else {
        n = (ssize_t)myvendor_gnss_uart_read_timeout(buf, (size_t)max,
                                                     (uint32_t)timeout_ms);
      }
      g_rd_read_ms += gnss_elapsed_ms(gnss_now_ms(), t_read);
      pr = (n > 0) ? 1 : 0;
      read_inline = true;
    }

    /** **poll() 一返回就把回调摘掉**（2026-09-23，修 n587 那条断言）。
     *
     *  `nuttx/fs/vfs/fs_poll.c:270 poll_notify()` 遍历驱动侧的注册表时
     *  **只看 `fds != NULL` 和 `fds->cb != NULL`，不看 `fds->priv`** ——
     *  也就是说：只要驱动那边还留着一个指向本 pollfd 的注册，之后每一次
     *  `uart_datasent()`（USART2 **TX 完成中断**）都会回调进来。
     *
     *  而 `fs_poll_setup()` 把 `fds->arg` 设成 `poll()` 内部那块
     *  `fs_heap_malloc` 的等待信号量，**`poll()` 返回时那块就被 free 了**。
     *  于是"返回之后、下一次注册之前"这个窗口里，任何一次陈旧回调都会拿
     *  已释放的 `arg` 去 `nxsem_get_value()` —— 那块内存一旦被复用成
     *  `nxmutex_t`，count 字段带 mutex 位 ⇒
     *  `DEBUGASSERT(!NXSEM_IS_MUTEX(sem))` ⇒ panic ⇒ 重启。
     *  （现场两次都停在同一处：`gnss: rt 4 cfg-flush` 之后、`irq=1`。）
     *
     *  pollfd 是**我们自己**的内存（本线程一份、不释放），所以这里能单方面
     *  把 `cb` 清成 NULL：`poll_notify()` 看到 `cb == NULL` 直接跳过，
     *  陈旧注册就变成空操作。下一次 `poll()` 由 `fs_poll_setup()` 重新装回
     *  `cb`，正常路径完全不受影响。
     *
     *  治的是"回调还能打进来"这个必要条件，不是把断言改成告警 —— 不掩盖问题。 */
    /* 历史：这里原来清 `pfd->cb`/`pfd->priv`，用来掐断 n587 那条"陈旧 poll 注册
     * 被 TX 完成中断回调"的链。直连数据面之后**根本不再有 poll 注册**，
     * 这两行随之删除；上面那段注释留作来龙去脉。 */

    g_poll_last_ms = gnss_elapsed_ms(gnss_now_ms(), t_poll);
    g_poll_enter_ms = 0;

    /* 自测：poll 的阻塞时间累计（正常占满每个窗口的大部分，但它不烧 CPU）。 */
    g_rd_poll_ms += g_poll_last_ms;
  }

  GNSS_PHASE("read-poll-done");
  if (pr < 0) {
    if (errno != EINTR) {
      gnss_link_add(&g_link.poll_err);
    }

    return (errno == EINTR) ? 0 : -errno;
  }

  /* poll 超时也 read：NuttX poll 看不到 bounce，uart_read 会 rxint(true)
   * 触发 harvest。空读才记 poll_to。
   */
  GNSS_PHASE("read-syscall");
  if (!read_inline)
  {
    /* 自测：read() 自身耗时。这一项若在每窗口里顶起来（比如 5 次调用花掉
     * 上百毫秒），就说明 CPU 是在 uart_read 的 harvest 路径里烧掉的，
     * 而不是在解析或发布上。 */
    uint32_t t_read = gnss_now_ms();

    n = read(fd, buf, (size_t)max);
    g_rd_read_ms += gnss_elapsed_ms(gnss_now_ms(), t_read);
  }

  if (n > 0)
    {
      /* 真拿到数据：空读退避立刻归零（在唯一一处读数据的地方做，
       * 所有调用方都覆盖得到）。 */
      g_rd_empty_run = 0;
      g_rd_backoff_n = 0;
    }

  GNSS_PHASE("read-done");
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) {
      if (pr == 0) {
        gnss_link_add(&g_link.poll_to);
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
        g_poll_to_run++;
        if ((g_poll_to_run % 10u) == 0u) {
          GNSS_TRACE("poll_to run=%u fd=%d", (unsigned)g_poll_to_run, fd);
        }
#endif
      }

      return 0;
    }

    return -errno;
  }

  if (n == 0) {
    if (pr == 0) {
      gnss_link_add(&g_link.poll_to);
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
      g_poll_to_run++;
      if ((g_poll_to_run % 10u) == 0u) {
        GNSS_TRACE("poll_to run=%u fd=%d", (unsigned)g_poll_to_run, fd);
      }
#endif
      return 0;
    }

    return -EIO;
  }

#ifdef CONFIG_MYVENDOR_GNSS_LOG
  g_poll_to_run = 0;
#endif
  gnss_link_add(&g_link.rd);
  gnss_link_note(&g_link.byte, (uint16_t)n);
  return (int)n;
}

static int gnss_read_byte(int fd, char *ch, int timeout_ms)
{
  int n = gnss_read_some(fd, ch, 1, timeout_ms);

  if (n > 0) {
    return 1;
  }

  return n;
}

static void gnss_eph_fs_begin(void)
{
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  myvendor_mtp_lfs_hold("gnss");
#endif
}

static void gnss_eph_fs_end(void)
{
#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  myvendor_mtp_lfs_release("gnss");
#endif
}

static void gnss_drain(int fd)
{
  char ch;
  int n = 0;

  while (n++ < 2048 && gnss_read_byte(fd, &ch, 2) > 0) {
  }
}

static int gnss_write_all(int fd, const uint8_t *buf, int len)
{
  int off = 0;
  uint32_t t0 = gnss_now_ms();

  while (off < len) {
    gnss_heartbeat();
    if (g_diag_kick || g_gnss_stop) {
      return -EINTR;
    }

    /* 板级直连：一次调用把**剩余全发**（HAL 阻塞发整帧，返回 0 才是全发成功）。
     * 旧的 EAGAIN/EINTR 重试在这里不再需要 —— 没有 NuttX 发送环、也没有它那把锁。 */
    {
      int wr = myvendor_gnss_uart_write(buf + off, (size_t)(len - off));

      if (wr != 0) {
        gnss_link_add(&g_link.wr_fail);
        syslog(LOG_WARNING, "gnss: uart write fail off=%d/%d wr=%d\n",
               off, len, wr);
        return wr;
      }
    }

    off = len;
    t0 = gnss_now_ms();
  }

  return 0;
}

static int gnss_ubx_send(int fd, uint8_t cls, uint8_t id,
                         const uint8_t *payload, uint16_t plen)
{
  uint8_t cka;
  uint8_t ckb;

  if (plen > GNSS_EPH_INJECT_MAX) {
    return -E2BIG;
  }

  s_ubx_tx[0] = 0xb5;
  s_ubx_tx[1] = 0x62;
  s_ubx_tx[2] = cls;
  s_ubx_tx[3] = id;
  s_ubx_tx[4] = (uint8_t)(plen & 0xff);
  s_ubx_tx[5] = (uint8_t)((plen >> 8) & 0xff);
  if (plen > 0 && payload != NULL) {
    memcpy(s_ubx_tx + 6, payload, plen);
  }

  gnss_ubx_cksum(s_ubx_tx + 2, (size_t)plen + 4u, &cka, &ckb);
  s_ubx_tx[6 + plen] = cka;
  s_ubx_tx[7 + plen] = ckb;
  return gnss_write_all(fd, s_ubx_tx, (int)plen + 8);
}

static int gnss_ubx_send_pl(int fd, uint8_t cls, uint8_t id, uint16_t plen)
{
  uint8_t hdr[6];
  uint8_t ck[2];
  uint8_t cka;
  uint8_t ckb;
  uint16_t i;

  if (plen > GNSS_UBX_PAY_MAX) {
    return -E2BIG;
  }

  hdr[0] = 0xb5;
  hdr[1] = 0x62;
  hdr[2] = cls;
  hdr[3] = id;
  hdr[4] = (uint8_t)(plen & 0xff);
  hdr[5] = (uint8_t)((plen >> 8) & 0xff);
  gnss_ubx_cksum(hdr + 2, 4, &cka, &ckb);
  for (i = 0; i < plen; i++) {
    cka = (uint8_t)(cka + s_ubx_pl[i]);
    ckb = (uint8_t)(ckb + cka);
  }

  ck[0] = cka;
  ck[1] = ckb;
  if (gnss_write_all(fd, hdr, 6) < 0) {
    return -EIO;
  }

  if (plen > 0 && gnss_write_all(fd, s_ubx_pl, plen) < 0) {
    return -EIO;
  }

  return gnss_write_all(fd, ck, 2);
}

static int gnss_ubx_read_frame(int fd, uint8_t *cls, uint8_t *id,
                               uint16_t *plen, int timeout_ms)
{
  enum {
    ST_B5 = 0,
    ST_62,
    ST_CLS,
    ST_ID,
    ST_L1,
    ST_L2,
    ST_PAY,
    ST_CKA,
    ST_CKB,
    ST_NMEA
  };
  int st = ST_B5;
  uint8_t cka = 0;
  uint8_t ckb = 0;
  uint8_t got_cka = 0;
  uint16_t len = 0;
  uint16_t idx = 0;
  uint32_t start = gnss_now_ms();

  if (!gnss_ubx_pl_ensure()) {
    return 0;
  }

  while (gnss_elapsed_ms(gnss_now_ms(), start) < (uint32_t)timeout_ms) {
    char ch;
    int ret;
    uint32_t used = gnss_elapsed_ms(gnss_now_ms(), start);
    int slice;

    if (used >= (uint32_t)timeout_ms) {
      break;
    }

    slice = (int)((uint32_t)timeout_ms - used);

    if (slice < 1) {
      break;
    }

    if (slice > 200) {
      slice = 200;
    }

    ret = gnss_read_byte(fd, &ch, slice);
    if (ret < 0) {
      return ret;
    }

    if (ret == 0) {
      continue;
    }

    switch (st) {
      case ST_B5:
        if ((uint8_t)ch == 0xb5) {
          st = ST_62;
        } else if (ch == '$') {
          st = ST_NMEA;
        }
        break;

      case ST_62:
        st = ((uint8_t)ch == 0x62) ? ST_CLS : ST_B5;
        break;

      case ST_CLS:
        *cls = (uint8_t)ch;
        cka = *cls;
        ckb = cka;
        st = ST_ID;
        break;

      case ST_ID:
        *id = (uint8_t)ch;
        cka = (uint8_t)(cka + *id);
        ckb = (uint8_t)(ckb + cka);
        st = ST_L1;
        break;

      case ST_L1:
        len = (uint8_t)ch;
        cka = (uint8_t)(cka + (uint8_t)ch);
        ckb = (uint8_t)(ckb + cka);
        st = ST_L2;
        break;

      case ST_L2:
        len |= ((uint16_t)(uint8_t)ch) << 8;
        cka = (uint8_t)(cka + (uint8_t)ch);
        ckb = (uint8_t)(ckb + cka);
        if (len > GNSS_UBX_PAY_MAX) {
          st = ST_B5;
          break;
        }

        idx = 0;
        st = (len == 0) ? ST_CKA : ST_PAY;
        break;

      case ST_PAY:
        s_ubx_pl[idx] = (uint8_t)ch;
        cka = (uint8_t)(cka + (uint8_t)ch);
        ckb = (uint8_t)(ckb + cka);
        idx++;
        if (idx >= len) {
          st = ST_CKA;
        }
        break;

      case ST_CKA:
        got_cka = (uint8_t)ch;
        st = ST_CKB;
        break;

      case ST_CKB:
        if (got_cka == cka && (uint8_t)ch == ckb) {
          *plen = len;
          return 1;
        }

        st = ST_B5;
        break;

      case ST_NMEA:
        if (ch == '\n') {
          st = ST_B5;
        }
        break;

      default:
        st = ST_B5;
        break;
    }
  }

  return 0;
}

/* UBX-CFG-VALSET RAM：等 ACK/NAK。NMEA 会被 read_frame 丢掉。 */
static int gnss_ubx_wait_ack(int fd, uint8_t want_cls, uint8_t want_id,
                              int timeout_ms)
{
  uint32_t start = gnss_now_ms();

  while (gnss_elapsed_ms(gnss_now_ms(), start) < (uint32_t)timeout_ms) {
    uint8_t cls;
    uint8_t id;
    uint16_t plen;
    int ret;

    ret = gnss_ubx_read_frame(fd, &cls, &id, &plen, 80);
    if (ret <= 0) {
      continue;
    }

    if (cls == 0x05 && plen >= 2 &&
        s_ubx_pl[0] == want_cls && s_ubx_pl[1] == want_id) {
      return (id == 0x01) ? 1 : -1;
    }
  }

  return 0;
}

static int gnss_cfg_valset_u1(int fd, uint32_t key, uint8_t val)
{
  uint8_t pl[9];

  pl[0] = 0x00;
  pl[1] = 0x01;
  pl[2] = 0x00;
  pl[3] = 0x00;
  pl[4] = (uint8_t)key;
  pl[5] = (uint8_t)(key >> 8);
  pl[6] = (uint8_t)(key >> 16);
  pl[7] = (uint8_t)(key >> 24);
  pl[8] = val;
  gnss_drain(fd);
  if (gnss_ubx_send(fd, 0x06, 0x8a, pl, sizeof(pl)) < 0) {
    return -1;
  }

  return gnss_ubx_wait_ack(fd, 0x06, 0x8a, 250);
}

static const char *gnss_dyn_name(uint8_t model)
{
  if (model == GNSS_DYN_STATIONARY)
    {
      return "stationary";
    }

  return model == GNSS_DYN_AUTO ? "automotive" : "bike";
}

/** @brief 当前想要的动态模型。
 *
 *  @details
 *  **停车切 `stationary` 默认关闭（2026-09-17 骑行结论），`ctl gnss still
 *  on|off` 可随时切**：零速约束会拖住自己的解除条件（要"可信速度 >3 km/h
 *  持续 3 s"才退出，而这模式本身正在压低速度），起步那几秒被压住 → 现场表现
 *  为"拐弯记成直行 + 速度归零"。
 *
 *  关着的时候 `gnss_dyn_feed()` 直接清零返回，`g_dyn_still` 恒假（它也喂别的
 *  消费者，见 gnss_eph_send_pos 的精度外推），模型交回 automotive/bike。
 */
static uint8_t gnss_dyn_want(void)
{
  uint8_t want = g_dyn_want;

  if (g_dyn_still_on && g_dyn_still && !g_dyn_no_still)
    {
      return GNSS_DYN_STATIONARY;
    }

  return (want == GNSS_DYN_AUTO) ? GNSS_DYN_AUTO : GNSS_DYN_BIKE;
}

/**
 * @brief 取"可信的速度"（0.01 km/h）；返回 false = 没有任何可信速度信息。
 *
 * @details
 * 优先 NMEA 解算速度；没有解算时用 PVT 的 `gSpeed`，但要求它自己说准
 * （sAcc 相对门，与 bicycle_runtime.c 里那条同源：`sAcc > max(0.6 m/s, 30%×v)`
 * 就不算数）。**这条是必需的**：没定位时模组的 gSpeed 会跳到 100+ km/h 而 sAcc
 * 同样巨大（现场 `gs=79692 sacc=257055`），拿它判"在动"会让停车判定反复进出。
 */
static bool gnss_speed_trusted(const myvendor_sys_gnss_t *fix,
                               uint32_t *out_centi)
{
  uint32_t ckmh;
  uint32_t lim;

  if (fix == NULL)
    {
      return false;
    }

  if (fix->valid)
    {
      *out_centi = fix->speed_centi_kmh;
      return true;
    }

  if (!fix->pvt_valid || !fix->pvt_gnss_ok)
    {
      return false;
    }

  /* sAcc（mm/s）→ centi-km/h 与 gSpeed 同单位：×3.6/1000×100 = ×36/100。 */
  ckmh = (uint32_t)fix->speed_acc_mm_s * 36u / 100u;
  lim = 216u + (uint32_t)fix->speed_pvt_centi_kmh * 30u / 100u;   /* 0.6 m/s + 30% */
  if (ckmh > lim)
    {
      return false;
    }

  *out_centi = fix->speed_pvt_centi_kmh;
  return true;
}

/** @brief 停车/运动状态机（1 Hz，由 gnss_publish 喂）。
 *
 * @details
 * 进入 stationary 要**两条同时成立**：速度低于阈值持续 GNSS_DYN_STILL_HOLD_MS，
 * 且当前解算质量差（fixType < 3 / gnssFixOK=0 / hAcc 大 / 没有 PVT）。
 * 退出只要"速度起来了"持续 GNSS_DYN_MOVE_HOLD_MS —— 这条必须灵敏：零速约束下
 * 模组起步的那几秒速度会被压住，拖久了就是"骑起来还显示 0"。
 *
 * **没有可信速度 = 判为静止**：室内没定位时正是最需要 stationary 的场景
 * （fix->valid 为假、PVT 也没有可信速度），这里绝不能因为"没解算"就不切换。
 */
static void gnss_dyn_feed(const myvendor_sys_gnss_t *fix, uint32_t now)
{
  bool still;
  bool quality_bad;
  uint32_t kph = 0;

  if (!g_dyn_still_on)
    {
      /* 开关关着：不判静止、不打日志，状态与计时清零 —— g_dyn_still 还喂
       * gnss_eph_send_pos 的精度外推，留着真会改变行为。 */
      g_dyn_still = false;
      g_dyn_still_since = 0;
      g_dyn_move_since = 0;
      return;
    }

  still = !gnss_speed_trusted(fix, &kph) || (kph < GNSS_DYN_STILL_KPH);

  quality_bad = !(fix != NULL && fix->pvt_valid && fix->pvt_fix_type >= 3 &&
                  fix->pvt_gnss_ok && fix->pos_acc_mm <= GNSS_DYN_POS_OK_MM);

  if (still)
    {
      g_dyn_move_since = 0;
      if (g_dyn_still_since == 0)
        {
          g_dyn_still_since = now ? now : 1u;
        }

      if (!g_dyn_still && quality_bad &&
          gnss_elapsed_ms(now, g_dyn_still_since) >= GNSS_DYN_STILL_HOLD_MS)
        {
          g_dyn_still = true;
          syslog(LOG_INFO, "gnss: motion still (stationary model)\n");
        }
    }
  else
    {
      g_dyn_still_since = 0;
      if (g_dyn_move_since == 0)
        {
          g_dyn_move_since = now ? now : 1u;
        }

      if (g_dyn_still &&
          gnss_elapsed_ms(now, g_dyn_move_since) >= GNSS_DYN_MOVE_HOLD_MS)
        {
          g_dyn_still = false;
          syslog(LOG_INFO, "gnss: motion resume\n");
        }
    }
}

static void gnss_dyn_apply(int fd, bool force)
{
  uint8_t want;
  uint32_t now;
  int rc;

  if (fd < 0 || g_eph_busy || g_dbd_run || !g_link_got_nmea) {
    return;
  }

  now = gnss_now_ms();
  if (gnss_eph_grace_locked(now)) {
    return;
  }

  want = gnss_dyn_want();
  if (want != g_dyn_try_want) {
    g_dyn_try_want = want;
    g_dyn_fails = 0;
  }

  if (!force && g_dyn_have == want) {
    return;
  }

  if (!force && g_dyn_fails >= GNSS_DYN_FAIL_MAX) {
    return;
  }

  if (!force && g_dyn_try_ms != 0
      && gnss_elapsed_ms(now, g_dyn_try_ms) < GNSS_DYN_RETRY_MS) {
    return;
  }

  g_dyn_try_ms = now;
  rc = gnss_cfg_valset_u1(fd, GNSS_CFG_DYNMODEL, want);
  if (rc > 0) {
    g_dyn_have = want;
    g_dyn_fails = 0;
    syslog(LOG_INFO, "gnss: dynmodel %s\n", gnss_dyn_name(want));
  } else {
    g_dyn_fails++;
    /* rc 区分两种完全不同的原因，别混在一起报：
     *   -1 = 模组明确回 NAK（CFG-ACK id=0x00）→ 这个 dynModel 值它不支持；
     *    0 = 250 ms 内没等到 ACK           → 是时序/丢帧问题，不是不支持。
     * `dynmodel bike failed` 之前一直是这个笼统的报法，查不出是哪种。 */
    syslog(LOG_WARNING, "gnss: dynmodel %s failed (%u/%u) rc=%d%s\n",
           gnss_dyn_name(want), (unsigned)g_dyn_fails,
           (unsigned)GNSS_DYN_FAIL_MAX, rc,
           rc == -1 ? " (NAK: 值不支持)" :
           rc == 0  ? " (超时: 没等到 ACK)" : "");

    /* 明确 NAK = 这颗模组不支持这个值，**再试多少次都一样**。
     *
     * 现场就是 `dynmodel bike failed (1/3)(2/3)(3/3) rc=-1 (NAK: 值不支持)`：
     * 每次开机白试 3 轮（3 次 CFG-VALSET + 等 ACK）。直接把计数拉满，
     * 让它走"已放弃"那条路 —— 与超时（rc==0，值得重试）区分开。 */
    if (rc == -1)
      {
        g_dyn_fails = GNSS_DYN_FAIL_MAX;

        if (want == GNSS_DYN_STATIONARY)
          {
            /* stationary 不支持就**关掉自动切换**：否则每次停车都会再把
             * g_dyn_fails 清零重试一轮（want 一变就清零），变成按停车周期
             * 反复下发。记住即可，本会话不再试。 */
            g_dyn_no_still = true;
            g_dyn_still = false;
          }
      }
  }
}

static void gnss_configure_nav(int fd)
{
  uint8_t want;
  uint8_t alt;
  int dyn;
  int elev;
  int cno;
  int spd;
  int aid;
  int pvt;
  int bds;

  /* CFG-NAVSPG-DYNMODEL：默认自行车 10。车载 4 留给轨迹均速已经
   * 出来、且自行车包络不够用（汽车 / 长下坡 SOG 被 sanity 成 0）时。 */
  want = gnss_dyn_want();
  alt = (want == GNSS_DYN_BIKE) ? GNSS_DYN_AUTO : GNSS_DYN_BIKE;
  dyn = gnss_cfg_valset_u1(fd, GNSS_CFG_DYNMODEL, want);
  if (dyn < 1) {
    dyn = gnss_cfg_valset_u1(fd, GNSS_CFG_DYNMODEL, alt);
    g_dyn_have = (dyn > 0) ? alt : 0;
    syslog(LOG_INFO, "gnss: dynmodel %s\n",
           dyn > 0 ? gnss_dyn_name(alt) : "default");
  } else {
    g_dyn_have = want;
    syslog(LOG_INFO, "gnss: dynmodel %s\n", gnss_dyn_name(want));
  }
  g_dyn_try_ms = gnss_now_ms();

  /* CFG-NAVSPG-INFIL_MINELEV=10°，少用贴地反射星。 */
  elev = gnss_cfg_valset_u1(fd, 0x201100a4u, 10);
  /* CFG-NAVSPG-INFIL_MINCNO=10 dBHz。 */
  cno = gnss_cfg_valset_u1(fd, 0x201100a3u, 10);
  /* CFG-MOT-GNSSSPEED_THRS=1 m/s：低于约 3.6 km/h 当静止。 */
  spd = gnss_cfg_valset_u1(fd, 0x20250038u, 1);
  /* CFG-NAVSPG-ACKAIDING：倒库结束要 UBX-MGA-ACK。 */
  aid = gnss_cfg_valset_u1(fd, GNSS_CFG_ACKAIDING, 1);
  /* CFG-MSGOUT-UBX_NAV-PVT：探针，见 GNSS_CFG_MSGOUT_PVT_UART1 注释。
   * 写失败不致命也不重试 —— 判据在读线程的 `pvt=` 计数，这里只说明"配置
   * 这一侧"发生了什么（rc：-1 = NAK 值/键不支持，0 = 250 ms 没等到 ACK）。 */
  pvt = gnss_cfg_valset_u1(fd, GNSS_CFG_MSGOUT_PVT_UART1, 1);
  if (pvt < 1)
    {
      int pv2 = gnss_cfg_valset_u1(fd, GNSS_CFG_MSGOUT_PVT_UART2, 1);

      syslog(pv2 > 0 ? LOG_INFO : LOG_WARNING,
             "gnss: pvt out uart1=%d uart2=%d\n", pvt, pv2);
      pvt = pv2;
    }

  /* CFG-SIGNAL-BDS：确认北斗默认开着（数据手册 §1.3：MAX-M10S 默认就是
   * GPS+Galileo+**BeiDou B1I**+QZSS+SBAS）。写它等于"被谁改坏了就恢复"。
   *
   * **不再写 GLONASS**：数据手册脚注 13 明确 —— B1I 不能与 B1C 或 GLONASS L1OF
   * 同时开（三者互斥），而这颗固件上 GLO 键本来就 NAK。真把 GLO 打开反而会把
   * B1I 顶掉，是负收益。 */
  bds = gnss_cfg_valset_u1(fd, GNSS_CFG_SIGNAL_BDS, 1);

  syslog(LOG_INFO,
         "gnss: cfg ack dyn=%d elev=%d cno=%d spd=%d aid=%d pvt=%d sig bds=%d\n",
         dyn, elev, cno, spd, aid, pvt, bds);
  /* 读回星座等关键键，打一行 —— 想知道"模块实际开的是什么"。 */
  gnss_cfg_dump_signal(fd);
}

/**
 * @brief 手机位置一刷新就补发一次位置辅助（MGA-INI-POS_LLH）。
 *
 * @details
 * 位置辅助是室内能定上的一条关键外部信息（见 docs/gnss_speed_filter.md 讨论），
 * 但原先只在**注入那一刻**发一次 —— 手机常常是过一会儿才连上，那份位置就白丢了。
 * 这里在"自己的解算还不行"时，按"手机位置有更新 + 距上次 ≥60 s"补发一次；
 * 发位置本身对模组无害（随收随用），所以不需要等注入窗口。
 *
 * 只发位置、不发时间：时间辅助要按年龄算 tAcc（gnss_eph_send_time 里那套），
 * 而模组自己的 RTC 通常更准，交给注入那条路就够。
 */
static void gnss_assist_tick(int fd)
{
  (void)fd;

  /* **空实现，调用点保留。**
   *
   * 这里原本是"手机位置一更新就补发一次 `MGA-INI-POS_LLH`" —— 即**持续辅助**。
   * 按用户 2026-09-18 的决定停用：App 不做持续辅助定位，手机位置只在**用得到的
   * 时候随星历下发带一次**，那一次由 `gnss_eph_send_pos()` 在注入时用（见那里的
   * 注释：优先用随星历带来的手机位置，没有就退回板子自己那份）。
   *
   * 也就是说：**位置辅助并没有消失，只是换了入口** —— 手机的位置由 App 随星历
   * 写进 `.ubx` 文件（文件里的 `MGA-INI-POS_LLH`，本路径原样转发），固件自己那份
   * 由 `gnss_eph_send_pos()` 在注入时按板子当前定位生成。持续补发这条被明确关掉。
   *
   * 留着函数壳与调用点，是为了让"持续补发这条被明确关掉了"在代码里可见。 */
}

/**
 * @brief 读回一个 U1 类型的 CFG 键（UBX-CFG-VALGET，RAM 层）。
 *
 * @details
 * 用来确认"模块实际开着什么"—— 我们只下发过少数几个键，星座之类的都是出厂默认，
 * 光靠猜不行。每次只读一个键：CFG-SIGNAL-*_ENA 都是 U1，单键回包的布局最简单
 * （version, layer, key[4], value[1]）。
 *
 * @return 1 读到（*out 有效）；0 超时；负 errno。
 */
static int gnss_cfg_valget_u1(int fd, uint32_t key, uint8_t *out)
{
  /* 请求载荷试**两种布局**，并记住哪个通了：
   *   A（带 2 字节保留字）= version, layer, 0, 0, key[4] —— 与 VALSET 同形
   *     （我们的 VALSET 就是靠这 2 字节才被 ACK 的，所以先试它）；
   *   B（不带）           = version, layer, key[4] —— 老文档里的写法。
   * 现场第一版只发了 B，250 ms 超时 → `gnss: sig gps=-1`，白白以为这颗读不回。
   * 保留字那 2 字节是这套 v0 载荷的固定头，漏了就等于把 key 的偏移量发错。 */
  static uint8_t layout;   /* 0=未知；1=A；2=B */
  uint8_t pl[8];
  unsigned tries = (layout == 0u) ? 2u : 1u;
  unsigned t;

  if (!gnss_ubx_pl_ensure())
    {
      return -ENOMEM;
    }

  for (t = 0; t < tries; t++)
    {
      bool with = (layout == 0u) ? (t == 0u) : (layout == 1u);
      size_t off = with ? 4u : 2u;
      uint32_t start;

      pl[0] = 0x00;                   /* version */
      pl[1] = 0x00;                   /* layer = RAM */
      pl[2] = 0x00;
      pl[3] = 0x00;
      pl[off + 0u] = (uint8_t)key;
      pl[off + 1u] = (uint8_t)(key >> 8);
      pl[off + 2u] = (uint8_t)(key >> 16);
      pl[off + 3u] = (uint8_t)(key >> 24);

      gnss_drain(fd);
      if (gnss_ubx_send(fd, 0x06, 0x8b, pl, off + 4u) < 0)
        {
          return -EIO;
        }

      start = gnss_now_ms();
      while (gnss_elapsed_ms(gnss_now_ms(), start) < 250u)
        {
          uint8_t cls;
          uint8_t id;
          uint16_t len;
          uint32_t got;
          int rc = gnss_ubx_read_frame(fd, &cls, &id, &len, 80);

          if (rc <= 0 || cls != 0x06 || id != 0x8b || len < 7u)
            {
              continue;
            }

          /* 回包同样是 version, layer,[保留?],key[4],value —— 先按当前布局取，
           * 取不到再按另一种取，避免"布局猜对了但回包偏移量判错"。 */
          got = (uint32_t)s_ubx_pl[2] | ((uint32_t)s_ubx_pl[3] << 8) |
                ((uint32_t)s_ubx_pl[4] << 16) | ((uint32_t)s_ubx_pl[5] << 24);
          if (got == key)
            {
              *out = s_ubx_pl[6];
            }
          else if (len >= 9u)
            {
              got = (uint32_t)s_ubx_pl[4] | ((uint32_t)s_ubx_pl[5] << 8) |
                    ((uint32_t)s_ubx_pl[6] << 16) |
                    ((uint32_t)s_ubx_pl[7] << 24);
              if (got != key)
                {
                  continue;
                }

              *out = s_ubx_pl[8];
            }
          else
            {
              continue;
            }

          layout = (uint8_t)(with ? 1u : 2u);
          return 1;
        }
    }

  return 0;
}

/** @brief 读回星座开关等关键键，打一行 —— 想知道"模块实际开的是什么"。 */
static void gnss_mon_ver(int fd)
{
  uint8_t cls = 0;
  uint8_t id = 0;
  uint16_t len = 0;
  char sw[32];
  char hw[16];
  unsigned i;
  int rc;

  if (fd < 0 || !gnss_ubx_pl_ensure())
    {
      return;
    }

  gnss_drain(fd);
  if (gnss_ubx_send(fd, 0x0a, 0x04, NULL, 0) < 0)
    {
      return;
    }

  rc = gnss_ubx_read_frame(fd, &cls, &id, &len, 300);
  if (rc <= 0 || cls != 0x0a || id != 0x04 || len < 40u)
    {
      syslog(LOG_INFO, "gnss: mon ver none rc=%d\n", rc);
      return;
    }

  /* UBX-MON-VER：前 30 字节软件版本、接着 10 字节硬件版本，都是 NUL 补齐的
   * ASCII。逐字节清洗（非可打印换成 '.'、NUL 换成空格）再打，免得把控制字符
   * 塞进日志。用它和模组变体规格对表 —— 星座支不支持最终要看这个。 */
  for (i = 0; i < 30u; i++)
    {
      uint8_t c = s_ubx_pl[i];

      sw[i] = (c >= 0x20u && c < 0x7fu) ? (char)c : (c == 0u ? ' ' : '.');
    }

  sw[30] = '\0';
  for (i = 30u; i < 40u; i++)
    {
      uint8_t c = s_ubx_pl[i];

      hw[i - 30u] = (c >= 0x20u && c < 0x7fu) ? (char)c : (c == 0u ? ' ' : '.');
    }

  hw[10] = '\0';
  for (i = 30u; i > 0u && sw[i - 1u] == ' '; i--)
    {
      sw[i - 1u] = '\0';
    }

  for (i = 10u; i > 0u && hw[i - 1u] == ' '; i--)
    {
      hw[i - 1u] = '\0';
    }

  syslog(LOG_INFO, "gnss: mon ver sw=[%s] hw=[%s]\n", sw, hw);
  snprintf(g_mod_fw, sizeof(g_mod_fw), "%.23s", sw);

  /* 扩展串（每 30 字节一条）：u-blox 这里会给出 PROTVER / MOD / **本机支持的
   * 星座列表**（形如 `GPS;GLO;GAL;BDS`）—— 这是"这颗到底带不带北斗"最直接的
   * 书面证据，比一个个试键强。最多打 8 条，够用了。 */
  {
    unsigned n = 0;

    for (i = 40u; i + 30u <= (unsigned)len && n < 8u; i += 30u, n++)
      {
        char ext[32];
        unsigned j;

        for (j = 0; j < 30u; j++)
          {
            uint8_t c = s_ubx_pl[i + j];

            ext[j] = (c >= 0x20u && c < 0x7fu) ? (char)c : (c == 0u ? ' ' : '.');
          }

        ext[30] = '\0';
        for (j = 30u; j > 0u && ext[j - 1u] == ' '; j--)
          {
            ext[j - 1u] = '\0';
          }

        syslog(LOG_INFO, "gnss: mon ext[%u] %s\n", n, ext);

        /* 那条星座列表（`GPS;GLO;GAL;BDS`）解析成位图 + 存原文：UI 的卫星页
         * 据此把"模组不支持"和"当前没数据"分开显示，不再让人猜。 */
        if (strchr(ext, ';') != NULL &&
            (strstr(ext, "GPS") != NULL || strstr(ext, "BDS") != NULL ||
             strstr(ext, "GAL") != NULL))
          {
            uint8_t mask = 0;

            if (strstr(ext, "GPS") != NULL)
              {
                mask |= (uint8_t)(1u << MYVENDOR_SYS_GNSS_CONST_GPS);
              }

            if (strstr(ext, "GLO") != NULL)
              {
                mask |= (uint8_t)(1u << MYVENDOR_SYS_GNSS_CONST_GLO);
              }

            if (strstr(ext, "GAL") != NULL)
              {
                mask |= (uint8_t)(1u << MYVENDOR_SYS_GNSS_CONST_GAL);
              }

            if (strstr(ext, "BDS") != NULL)
              {
                mask |= (uint8_t)(1u << MYVENDOR_SYS_GNSS_CONST_BDS);
              }

            if (strstr(ext, "QZSS") != NULL)
              {
                mask |= (uint8_t)(1u << MYVENDOR_SYS_GNSS_CONST_QZSS);
              }

            g_mod_const = mask;
            snprintf(g_mod_gnss, sizeof(g_mod_gnss), "%s", ext);
          }
      }
  }
}

static void gnss_cfg_dump_signal(int fd)
{
  static const struct
  {
    const char *name;
    uint32_t    key;
  } keys[] =
  {
    { "gps", 0x1031001fu },
    { "qzss", 0x10310020u },
    { "gal", 0x10310021u },
    { "sbas", 0x10310022u },
    { "bds", 0x10310024u },
    { "glo", 0x10310025u },
    { "fixmode", 0x20110011u },
  };
  char buf[128];
  size_t k = 0;
  unsigned i;

  /* 每次**模组上电**读一次就够：本固件根本不回 UBX-CFG-VALGET
   * （现场 `gnss: sig gps=-1`，即第一个键就超时），而那一轮要花掉 ~250 ms 的
   * 超时。配置只在模组掉电后才会回默认，所以用 g_cfg_dumped 挡住重复读。 */
  if (g_cfg_dumped)
    {
      return;
    }

  g_cfg_dumped = true;
  /* 模组身份先读：星座支不支持最终要看固件/变体，这一行是和规格对表的依据。 */
  gnss_mon_ver(fd);
  buf[0] = '\0';
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    {
      uint8_t v = 0;
      int rc = gnss_cfg_valget_u1(fd, keys[i].key, &v);
      int w;

      if (k + 24u >= sizeof(buf))
        {
          break;
        }

      w = snprintf(buf + k, sizeof(buf) - k, "%s%s=%d",
                   (k > 0u) ? " " : "", keys[i].name, (rc > 0) ? (int)v : -1);
      if (w <= 0 || (size_t)w >= sizeof(buf) - k)
        {
          break;
        }

      k += (size_t)w;

      /* 第一个键就超时 = 这颗固件根本不回 VALGET，后面别一个个等满 250 ms
       * （那会给每次会话开头加接近两秒）。已读到的照打，其余不试。 */
      if (rc == 0)
        {
          break;
        }
    }

  /* -1 = 没读到（这条固件不认这个键，或超时），不是"关着"。 */
  syslog(LOG_INFO, "gnss: sig %s\n", buf);
}

/**
 * @brief 星座实验（`ctl gnss bdsonly|b1c|ver`）：只在读线程里写串口。
 *
 * @details
 * 目的只有一个：把"北斗为什么一句都不出"这件事分成两半 —— **前端频带**
 * （B1I 在 1561.098 MHz，离 GPS L1 中心 14 MHz，窄带天线/SAW 会把它压掉）
 * 还是**模组根本没解调北斗**。
 *
 * @param mode 0=无；1=只留北斗；2=恢复；3=试开 B1C；4=打 MON-VER。
 */
static void gnss_probe_tick(int fd)
{
  static const struct
  {
    const char *name;
    uint32_t    key;
  } on[] =
  {
    { "gps", 0x1031001fu },
    { "qzss", 0x10310020u },
    { "gal", 0x10310021u },
  };
  /* B1C（1575.42 MHz，与 GPS L1 同频）的候选键：本固件读不回配置，只能逐个试
   * 并看 ACK/NAK。**故意不猜值**：试中的那个在日志里明确写出来，之后照它来。 */
  static const uint32_t b1c[] = { 0x10310026u, 0x10310027u, 0x10310028u };
  int mode = g_probe_req;
  unsigned i;

  if (mode == 0 || fd < 0 || g_eph_busy || g_dbd_run)
    {
      return;
    }

  g_probe_req = 0;
  switch (mode)
    {
      case 1:
        {
          int rc = gnss_cfg_valset_u1(fd, GNSS_CFG_SIGNAL_BDS, 1);

          for (i = 0; i < sizeof(on) / sizeof(on[0]); i++)
            {
              int r = gnss_cfg_valset_u1(fd, on[i].key, 0);

              syslog(LOG_WARNING, "gnss: probe bdsonly %s=0 rc=%d\n",
                     on[i].name, r);
            }

          /* 至少留一个星座开着，否则模组会拒绝这套组合。 */
          syslog(LOG_WARNING,
                 "gnss: probe bdsonly ON (bds=%d) — 看 link extra 的 gsv "
                 "gp/gl/ga/bd/gq\n", rc);
        }
        break;

      case 2:
        syslog(LOG_WARNING, "gnss: probe bdsonly OFF (restore)\n");
        for (i = 0; i < sizeof(on) / sizeof(on[0]); i++)
          {
            int r = gnss_cfg_valset_u1(fd, on[i].key, 1);

            syslog(LOG_WARNING, "gnss: probe restore %s=1 rc=%d\n",
                   on[i].name, r);
          }

        break;

      case 3:
        /* ACK 的记下来；NAK/超时的照原值留下（不改任何东西）。 */
        for (i = 0; i < sizeof(b1c) / sizeof(b1c[0]); i++)
          {
            int r = gnss_cfg_valset_u1(fd, b1c[i], 1);

            syslog(LOG_WARNING, "gnss: probe b1c key=0x%08lx rc=%d%s\n",
                   (unsigned long)b1c[i], r,
                   r > 0 ? " (ACK — 看 gsv 的 bd 项)" : "");
          }

        break;

      case 4:
        gnss_mon_ver(fd);
        break;

      default:
        break;
    }
}

void myvendor_gnss_probe(int mode)
{
  g_probe_req = mode;
}

static void gnss_eph_send_time(int fd)
{
  uint8_t pl[24];
  uint8_t cls;
  uint8_t id;
  uint16_t rlen;
  struct tm tm_buf;
  struct tm *tmp;
  time_t now;

  now = time(NULL);
  tmp = gmtime_r(&now, &tm_buf);
  if (tmp == NULL || tmp->tm_year + 1900 < 2020) {
    return;
  }

  memset(pl, 0, sizeof(pl));
  pl[0] = 0x10;
  pl[3] = 18;
  pl[4] = (uint8_t)((tmp->tm_year + 1900) & 0xff);
  pl[5] = (uint8_t)(((tmp->tm_year + 1900) >> 8) & 0xff);
  pl[6] = (uint8_t)(tmp->tm_mon + 1);
  pl[7] = (uint8_t)tmp->tm_mday;
  pl[8] = (uint8_t)tmp->tm_hour;
  pl[9] = (uint8_t)tmp->tm_min;
  pl[10] = (uint8_t)tmp->tm_sec;
  pl[16] = 5;
  gnss_drain(fd);
  (void)gnss_ubx_send(fd, 0x13, 0x40, pl, sizeof(pl));
  (void)gnss_ubx_read_frame(fd, &cls, &id, &rlen, GNSS_EPH_ACK_MS);
}

/**
 * @brief 小端写 4 字节（本文件里 UBX 载荷都是小端）。
 */
static void gnss_put_le32(uint8_t *p, int32_t v)
{
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

/**
 * @brief 注入前把「粗位置」也告诉模组（MGA-INI-POS_LLH）。
 *
 * 为什么需要：模组刚上电（或被搬动过）时不知道自己在哪，位置辅助能让它走乐观
 * 启动策略（u-blox：pacc ≤100 km 才会），明显缩短首次定位。时间那一帧一直在发，
 * 位置这一帧此前是缺的。
 *
 * 位置来源按「谁知道我现在在哪」排：
 *   1. **手机刚报上来的定位**（0xFF14，RAM 里那份）—— 它知道的是"现在"，最准；
 *   2. 本机本次会话内刚采到的定位（自己十分钟前定过位，人没走远）；
 *   3. 都没有就不发这一帧。
 *
 * 不拿 `persist.ride.last_*`（上次骑行终点）：那个没有时间戳，无法界定设备现在
 * 离它多远 —— u-blox 明确警告「位置精度报得比实际准会明显劣化甚至起不来」，
 * 宁可少发一帧也不谎报。
 */
static void gnss_eph_send_pos(int fd)
{
  uint8_t pl[20];
  uint8_t cls;
  uint8_t id;
  uint16_t rlen;
  int32_t lat_e7;
  int32_t lon_e7;
  int32_t alt_mm;
  int32_t acc_cm;
  const char *src;
  struct companion_gnss_fix phone;

  (void)phone;

  /* **位置辅助只用板子自己的定位；手机的位置不经这条路上模组。**
   *
   * 手机的位置随星历一起下发 —— 但那是 **App 在生成 .ubx 时把
   * `MGA-INI-POS_LLH` 直接写进文件**（见 App 的 `eph_service.dart:_assistHead()`），
   * 而本注入路径"只过滤 DBD 帧、其余原样转发"，所以那一帧本来就会进模组，
   * 不需要固件再读 0xFF14 补一次。
   *
   * 于是这里只剩"自己那份、且还新鲜"一条路（按年龄外推 + hAcc 打底）；两者都没有
   * 就不发 —— 宁可没有辅助，也不拿一个来自别处、可能已经过期的坐标去约束模组。
   * 用户 2026-09-18 的决定：手机不做持续辅助定位。 */
  if (g_fix.valid && g_fix.stamp_ms != 0 &&
      gnss_elapsed_ms(gnss_now_ms(), g_fix.stamp_ms) < GNSS_EPH_POS_SELF_MS)
    {
      uint32_t age_ms = gnss_elapsed_ms(gnss_now_ms(), g_fix.stamp_ms);

      lat_e7 = g_fix.lat_e7;
      lon_e7 = g_fix.lon_e7;
      alt_mm = g_fix.alt_mm;
      /* 精度**按真实值发**，不用笼统的 20 km：那等于"在地球上"，对模组几乎没有约束力。
       *   - 有 PVT 就用 hAcc 打底（这就是它的用途）；
       *   - 加上"这段时间可能已经骑出去多远"的外推：停车 2 m/s，运动 15 m/s。
       * 停车时能发出几百米级的约束（室内起步最有用的一条），骑行中也不谎报精度。 */
      acc_cm = (g_fix.pos_acc_mm != 0) ? (int32_t)(g_fix.pos_acc_mm / 10u)
                                       : (int32_t)(GNSS_EPH_POS_SELF_ACC_CM / 10);
      acc_cm += (int32_t)(age_ms / 1000u) * (g_dyn_still ? 200 : 1500);
      if (acc_cm < 10000)
        {
          acc_cm = 10000; /* 下限 100 m */
        }

      src = "self";
    }
  else
    {
      return;
    }

  memset(pl, 0, sizeof(pl));
  pl[0] = 0x01;
  gnss_put_le32(pl + 4, lat_e7);
  gnss_put_le32(pl + 8, lon_e7);
  gnss_put_le32(pl + 12, alt_mm / 10); /* 毫米 -> 厘米 */
  gnss_put_le32(pl + 16, acc_cm);
  gnss_drain(fd);
  (void)gnss_ubx_send(fd, 0x13, 0x40, pl, sizeof(pl));
  (void)gnss_ubx_read_frame(fd, &cls, &id, &rlen, GNSS_EPH_ACK_MS);
  syslog(LOG_INFO, "gnss: eph pos assist %s %.5f,%.5f acc=%ldcm\n",
         src, (double)lat_e7 / 1e7, (double)lon_e7 / 1e7, (long)acc_cm);
}

static int gnss_is_leap(int year)
{
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static time_t gnss_utc_to_unix(int y, int mo, int d, int h, int mi, int s)
{
  static const int md[12] =
    {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
  long days = 0;
  int i;

  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
    return 0;
  }

  for (i = 1970; i < y; i++) {
    days += 365 + gnss_is_leap(i);
  }

  for (i = 0; i < mo - 1; i++) {
    days += md[i];
    if (i == 1 && gnss_is_leap(y)) {
      days++;
    }
  }

  days += d - 1;
  return (time_t)(days * 86400L + h * 3600L + mi * 60L + s);
}

/* mga_<unix>.ubx or mga_YYYYMMDDTHHMMSSZ.ubx */
static bool gnss_eph_parse_utc(const char *name, uint32_t *utc)
{
  unsigned long v;
  int y;
  int mo;
  int d;
  int h;
  int mi;
  int s;
  char extra;
  time_t t;

  if (name == NULL || utc == NULL) {
    return false;
  }

  extra = 0;
  if (sscanf(name, "mga_%lu.ubx%c", &v, &extra) == 1 &&
      v >= (unsigned long)GNSS_TIME_MIN_UNIX && v <= 0xfffffffful) {
    *utc = (uint32_t)v;
    return true;
  }

  extra = 0;
  if (sscanf(name, "mga_%4d%2d%2dT%2d%2d%2dZ.ubx%c",
             &y, &mo, &d, &h, &mi, &s, &extra) == 6) {
    t = gnss_utc_to_unix(y, mo, d, h, mi, s);
    if (t >= (time_t)GNSS_TIME_MIN_UNIX) {
      *utc = (uint32_t)t;
      return true;
    }
  }

  return false;
}

static void gnss_path_set(char *slot, const char *src)
{
  pthread_mutex_lock(&g_lock);
  if (src == NULL || src[0] == '\0') {
    slot[0] = '\0';
  } else {
    size_t n = strlen(src);

    if (n >= GNSS_EPH_PATH_MAX) {
      n = GNSS_EPH_PATH_MAX - 1u;
    }

    memcpy(slot, src, n);
    slot[n] = '\0';
  }

  pthread_mutex_unlock(&g_lock);
}

static bool gnss_path_get(const char *slot, char *buf, size_t n)
{
  bool ok;

  if (buf == NULL || n == 0) {
    return false;
  }

  pthread_mutex_lock(&g_lock);
  ok = (slot[0] != '\0');
  if (ok) {
    size_t len = strlen(slot);

    if (len + 1u > n) {
      ok = false;
      buf[0] = '\0';
    } else {
      memcpy(buf, slot, len + 1u);
    }
  } else {
    buf[0] = '\0';
  }

  pthread_mutex_unlock(&g_lock);
  return ok;
}

static uint32_t gnss_name_utc(void)
{
  uint32_t utc;
  time_t now;

  pthread_mutex_lock(&g_lock);
  utc = g_fix.utc_sec;
  pthread_mutex_unlock(&g_lock);
  if (utc >= (uint32_t)GNSS_TIME_MIN_UNIX) {
    return utc;
  }

  now = time(NULL);
  if (now >= (time_t)GNSS_TIME_MIN_UNIX) {
    return (uint32_t)now;
  }

  return (uint32_t)GNSS_TIME_MIN_UNIX;
}

static void gnss_eph_mga_path(char *buf, size_t n, uint32_t utc)
{
  snprintf(buf, n, "%s/mga_%lu.ubx",
           SF32LB52_GNSS_EPH_DIR, (unsigned long)utc);
}

static void gnss_dump_dest_path(char *buf, size_t n)
{
  uint32_t utc = gnss_name_utc();
  struct stat st;
  unsigned i;

  for (i = 0; i < 100u; i++) {
    gnss_eph_mga_path(buf, n, utc + i);
    if (stat(buf, &st) != 0) {
      return;
    }
  }
}

/**
 * @brief 读第一帧 class/id；不是 UBX 则失败。
 */
static int gnss_eph_peek(const char *path, uint8_t *cls, uint8_t *id)
{
  uint8_t hdr[6];
  struct stat st;
  int fd;
  ssize_t n;

  if (path == NULL || stat(path, &st) != 0 || st.st_size <= 64) {
    return -1;
  }

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }

  n = read(fd, hdr, 6);
  close(fd);
  if (n != 6 || hdr[0] != 0xb5 || hdr[1] != 0x62) {
    return -1;
  }

  if (cls != NULL) {
    *cls = hdr[2];
  }

  if (id != NULL) {
    *id = hdr[3];
  }

  return 0;
}

/**
 * @brief 可注入的 UBX（AssistNow 或模组 dump）。
 */
static bool gnss_eph_path_ubx(const char *path)
{
  return gnss_eph_peek(path, NULL, NULL) == 0;
}

/**
 * @brief 可注入的 mga_*.ubx（下发 AssistNow 与模组 dump 同一套命名）。
 */
static bool gnss_eph_path_usable(const char *path)
{
  return gnss_eph_path_ubx(path);
}

/**
 * @brief 下发星历和 dump 共用 mga_<utc>.ubx，一共最多 GNSS_EPH_KEEP 份。
 *
 * 按文件名 UTC 排序，不区分 AssistNow / MGA-DBD。
 */
/** 单轮 eph 家务（prune / 选源）的时间预算。
 *
 * 这两件事都是**为注入服务**的，而注入才是目的 —— SD 静音/LFS 忙时它们会被拖到
 * 秒级：2026-09-18 现场一次 prune 花了 4.7 s（`readdir` 在等被静音卡住的 LFS 锁），
 * 把星历注入推到 15.9 s 才开始（`rt 8 eph-enter +10377`）。超预算就收手，交给
 * 调用方的重试退避。
 * 注：预算只拦在**调用之间** —— 单次 readdir/stat 卡多久拦不住，但那只影响一次，
 * 之后我们就退出了。 */
#define GNSS_EPH_FS_BUDGET_MS 1500u

static void gnss_eph_prune(void)
{
  char oldest[GNSS_EPH_PATH_MAX];
  char path[GNSS_EPH_PATH_MAX];
  DIR *dir;
  struct dirent *de;
  uint32_t utc;
  uint32_t oldest_utc;
  unsigned count;
  unsigned first = UINT32_MAX;
  int n;
  uint32_t t0 = gnss_now_ms();

  for (;;) {
    count = 0;

    if ((++g_gnss_canary_tick & 0xFFu) == 0u) {
      gnss_stack_canary_check();
    }
    oldest[0] = '\0';
    oldest_utc = 0xffffffffu;
    dir = opendir(SF32LB52_GNSS_EPH_DIR);
    if (dir == NULL) {
      return;
    }

    while ((de = readdir(dir)) != NULL) {
      /* 家务要有预算：prune 在注入**之前**跑，而 SD 静音会把它拖成秒级。
       * 2026-09-18 现场：静音正好砸进 prune，整轮 prune 4.7 s，星历注入被推到
       * 15.9 s 才开始（`rt 8 eph-enter +10377`）。超预算就收手，下一轮再来。 */
      if (gnss_elapsed_ms(gnss_now_ms(), t0) >= GNSS_EPH_FS_BUDGET_MS) {
        closedir(dir);
        syslog(LOG_WARNING, "gnss: eph prune deferred (%u ms budget)\n",
               (unsigned)GNSS_EPH_FS_BUDGET_MS);
        return;
      }

      if (!gnss_eph_parse_utc(de->d_name, &utc)) {
        continue;
      }

      n = snprintf(path, sizeof(path), "%s/%s",
                   SF32LB52_GNSS_EPH_DIR, de->d_name);
      if (n < 0 || n >= (int)sizeof(path)) {
        continue;
      }

      count++;
      if (utc < oldest_utc ||
          (utc == oldest_utc &&
           (oldest[0] == '\0' || strcmp(path, oldest) < 0))) {
        oldest_utc = utc;
        memcpy(oldest, path, (size_t)n + 1u);
      }
    }

    closedir(dir);
    if (first == UINT32_MAX) {
      first = count;
      if (count > GNSS_EPH_KEEP) {
        syslog(LOG_INFO, "gnss: eph prune %u -> %u\n",
               count, (unsigned)GNSS_EPH_KEEP);
      }
    }

    if (count <= GNSS_EPH_KEEP || oldest[0] == '\0') {
      return;
    }

    syslog(LOG_INFO, "gnss: prune eph %s\n", oldest);
    (void)unlink(oldest);
  }
}

void myvendor_gnss_eph_maintain(void)
{
  gnss_eph_fs_begin();
  gnss_eph_prune();
  gnss_eph_fs_end();
}

static void gnss_eph_inject(int uartfd);
static void gnss_eph_busy_set(bool busy);

static void gnss_module_ensure(void)
{
  if (g_mod_pwr) {
    return;
  }

  syslog(LOG_INFO, "gnss: module power-on (settle in gnss thread)\n");
  if (sf32lb52_l96_init() == 0) {
    g_mod_pwr = true;
  }
}

static void gnss_eph_run(int fd)
{
  bool auto_eph = myvendor_devctl_eph_auto_get();
  bool was_injected = g_eph_injected;

  /* 诊断：记下进入时的标志位，方便和结束时的 g_eph_injected 对比。 */
  syslog(LOG_WARNING,
         "gnss: eph run enter auto=%d injected_before=%d pending=%d "
         "ui_ready=%d cost_prev=%u\n",
         auto_eph ? 1 : 0, was_injected ? 1 : 0, g_eph_pending ? 1 : 0,
         g_ui_ready ? 1 : 0, (unsigned)g_eph_last_cost_ms);

  if (!auto_eph) {
    g_eph_pending = false;
    g_eph_injected = true;
    syslog(LOG_WARNING, "gnss: eph run skip (auto off) -> injected=1\n");
    return;
  }

  /* 没人点名要重灌时，**按模块自己报的时间裁决**（用户规则，见 g_mod_time_ms）：
   * 时间正常 ⇒ 它的备份里有星历，灌它是多余的。（"时间转坏就重新允许注入"由
   * gnss_eph_boot_try() 周期判定，那里才叫得到。） */
  if (!g_eph_reload && gnss_mod_time_ok())
    {
      g_eph_pending = false;
      g_eph_injected = true;
      g_eph_time_skip = true;
      syslog(LOG_INFO,
             "gnss: eph skip (module time ok, backup keeps its own eph)\n");
      return;
    }

  g_eph_skip_same_src = false;
  g_eph_pending = false;
  gnss_eph_busy_set(true);
  g_eph_last_busy = false;
  myvendor_gnss_eph_maintain();
  gnss_eph_inject(fd);

  /* 只有**真的注进去帧**才算本轮星历已完成。
   *
   * 原来这里无条件 `g_eph_injected = true`，于是任何一次 0 帧（最典型的是
   * "BLE 正在写 LittleFS、注入让路 2 秒"）都被记成成功，本轮开机不再重试 ——
   * 一次几秒的合法让路放大成整轮无星历、冷启动长时间无定位。
   * 现在改成：0 帧就不落"已注入"，按退避重试（重试用既有 g_eph_pending 路径）。
   * 真正失败（文件空/被并发改写）也走同一条路，只是会一直退避到 30 s 一次。
   *
   * 例外：整份文件都是**超龄星历**（stale>0）时算本轮完成 —— 重试不会让文件变新，
   * 只会每 30 s 白扫一遍、顺带白发几帧辅助；手机重新下发后
   * myvendor_gnss_eph_reload() 会清掉这个闩锁，新文件自然会被注入。 */
  if (g_eph_last_frames > 0 || g_eph_last_stale > 0 || g_eph_skip_same_src)
    {
      g_eph_injected = true;
      g_eph_retry_ms = 0;
      g_eph_retry_gap_ms = 0;
      g_eph_retry_n = 0;

      /* 认账这份源的指纹：同一份文件下次不再重灌。 */
      g_eph_sig_valid = true;
    }
  else
    {
      g_eph_retry_n++;
      g_eph_retry_gap_ms = (g_eph_retry_gap_ms == 0) ? GNSS_EPH_RETRY_MIN_MS
                                                     : g_eph_retry_gap_ms * 2u;
      if (g_eph_retry_gap_ms > GNSS_EPH_RETRY_MAX_MS)
        {
          g_eph_retry_gap_ms = GNSS_EPH_RETRY_MAX_MS;
        }

      g_eph_retry_ms = gnss_now_ms();
      g_eph_pending = true;
      syslog(LOG_WARNING,
             "gnss: eph retry #%u scheduled in %u ms (frames=0 busy=%d skip=%d "
             "stale=%d badck=%d)\n",
             (unsigned)g_eph_retry_n, (unsigned)g_eph_retry_gap_ms,
             g_eph_last_busy ? 1 : 0, g_eph_last_skip, g_eph_last_stale,
             g_eph_last_badck);
    }

  syslog(LOG_WARNING,
         "gnss: eph run exit injected=%d(was=%d) frames=%d skip=%d "
         "stale=%d badck=%d busy=%d\n",
         g_eph_injected ? 1 : 0, was_injected ? 1 : 0,
         g_eph_last_frames, g_eph_last_skip, g_eph_last_stale, g_eph_last_badck,
         g_eph_last_busy ? 1 : 0);
}

static void gnss_eph_boot_try(int fd)
{
  uint32_t now;

  /* **周期性**重开：因"模块时间正常"跳过之后（g_eph_time_skip），时间一旦转坏
   * 就重新允许自动注入。放在这里是因为这里是被周期判定的地方 ——
   * gnss_eph_run() 在跳过之后往往不会再被叫到（闩锁已置、pending 已清）。
   * `!g_idle_parked`：park 时模块是断电的，绝不能在那时候安排注入。
   * 上电窗口：模块刚唤醒的头 10 s 里它还没报出时间，不能据此判定"时间丢了"
   * （否则每次唤醒都白灌一遍，2026-09-18 现场）。 */
  if (g_eph_time_skip && g_eph_injected && !g_idle_parked && !g_dbd_run &&
      g_mod_up_ms != 0 &&
      gnss_elapsed_ms(gnss_now_ms(), g_mod_up_ms) >= GNSS_EPH_MOD_TIME_WAIT_MS &&
      !gnss_mod_time_ok())
    {
      g_eph_injected = false;
      g_eph_time_skip = false;
      g_eph_pending = true;
      syslog(LOG_WARNING,
             "gnss: eph re-arm (module time lost — backup state unclear)\n");
    }

  if (fd < 0 || !g_eph_pending || g_eph_injected || g_eph_busy || g_dbd_run) {
    /* 诊断：这几条早退是本函数唯一的行为，但原来完全不可见——
     * injected=1 且 pending 未清时说明"待注入"被静默丢弃了。 */
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
    {
      static uint32_t last_skip_log;

      now = gnss_now_ms();
      if (last_skip_log == 0 ||
          gnss_elapsed_ms(now, last_skip_log) >= 10000u) {
        last_skip_log = now;
        GNSS_TRACE("eph boot_try skip fd=%d pending=%d injected=%d busy=%d "
                 "dbd=%d",
                 fd, g_eph_pending ? 1 : 0, g_eph_injected ? 1 : 0,
                 g_eph_busy ? 1 : 0, g_dbd_run ? 1 : 0);
      }
    }
#endif
    return;
  }

  now = gnss_now_ms();
  if (!g_ui_ready && g_boot_ms != 0
      && gnss_elapsed_ms(now, g_boot_ms) < GNSS_EPH_BOOT_WAIT_MS) {
    return;
  }

  /* 0 帧失败后的退避闸门。本函数每轮主循环（约 200 ms）都会被调一次，
   * 而每次重试都要短暂持有 LittleFS 扫目录（myvendor_gnss_eph_maintain /
   * gnss_eph_src），不设闸门会变成 200 ms 一次扫盘、并且顺带让地图的
   * quiesce 检查每 200 ms 跳一次 tile 读。 */
  if (g_eph_retry_ms != 0 &&
      gnss_elapsed_ms(now, g_eph_retry_ms) < g_eph_retry_gap_ms) {
    return;
  }

  syslog(LOG_INFO, "gnss: eph inject after ui%s\n",
         g_ui_ready ? "" : " timeout");
  gnss_eph_run(fd);
}

static void gnss_eph_remember(uint32_t utc)
{
  if (utc < (uint32_t)GNSS_TIME_MIN_UNIX) {
    return;
  }

  pthread_mutex_lock(&g_lock);
  if (utc >= g_eph_last_utc) {
    g_eph_last_utc = utc;
  }

  g_eph_times_ms = 0;
  pthread_mutex_unlock(&g_lock);
}

static void gnss_eph_busy_set(bool busy)
{
  pthread_mutex_lock(&g_lock);
  g_eph_busy = busy;
  pthread_mutex_unlock(&g_lock);
}

static bool gnss_eph_reload_take(void)
{
  bool need;

  pthread_mutex_lock(&g_lock);
  need = g_eph_reload;
  g_eph_reload = false;
  pthread_mutex_unlock(&g_lock);
  return need;
}

static bool gnss_eph_try_reload(int uartfd)
{
  if (uartfd < 0 || g_dbd_run || !gnss_eph_reload_take()) {
    return false;
  }

  gnss_eph_inject(uartfd);
  return true;
}

/**
 * @brief 选出一个可用的星历文件，把路径写进 `g_eph_src_path` 并返回它。
 *
 * @details 扫描 `SF32LB52_GNSS_EPH_DIR`，按时间戳挑最新的一份。
 *
 *          **这个函数是卡死现场的头号嫌疑**：5 次复现都停在这里
 *          （`eph-src +11544 ms`），而**同形态**的 `eph-prune`（同样是
 *          opendir/readdir 扫描）只要 66 ms，差 175 倍。所以给每一次
 *          opendir / readdir / stat 都单独计时：
 *            - 只报超过 `GNSS_EPH_SLOW_MS`(20 ms) 的那些（正常的 LFS 小读是亚毫秒级）；
 *            - 末尾打一行汇总（次数 + 最大耗时 + 总耗时）。
 *
 *          判据：某次调用从亚毫秒跳到几百毫秒、随后直接失败 → 那就是触发器，
 *          而且能同时拿到"是哪个文件"；如果全部都是几毫秒而失败发生在循环之后，
 *          说明卡死在别处（同一时段还有 BLE companion 起来和 hop 排队），要转线查。
 *
 * @note 结论：**LittleFS 的目录扫描本身就是慢的**，慢在 `readdir` 上，
 *       而且候选文件多时会一个个 `stat` 过去。所以星历目录里的文件数要控制住。
 *
 * @return 星历文件路径（指向静态缓冲，调用者不要释放）；没找到则返回 NULL。
 */
static const char *gnss_eph_src(void)
{
  DIR *dir;
  struct dirent *de;
  uint32_t best_utc = 0;
  uint32_t utc;
  char path[GNSS_EPH_PATH_MAX];
  int n;

  /* 现场 5 次复现都停在这个函数里：`eph-src +11544 ms`，而**同形态**的
   * eph-prune（同样是 opendir/readdir 扫描）只要 66 ms，差 175 倍。
   * 所以这里给每一次 opendir / readdir / stat 单独计时：
   *   - 只报超过阈值的那些（正常的 LFS 小读是亚毫秒级）；
   *   - 末尾再打一行汇总（次数 + 最大耗时 + 总耗时）。
   * 判据：某次调用从亚毫秒跳到几百毫秒、随后直接失败 → 那就是触发器，
   * 而且能同时拿到是"哪个文件"；如果全部都是几毫秒而失败发生在循环之后，
   * 说明卡死在别处（同一时段还有 BLE companion 起来和 hop 排队），要转线查。
   */
  uint32_t t_all = gnss_now_ms();
  uint32_t t_src0 = t_all;
  bool gave_up = false;
  uint32_t t_opendir = 0;
  uint32_t rd_max = 0;
  uint32_t st_max = 0;
  unsigned rd_n = 0;
  unsigned st_n = 0;
  unsigned slow_n = 0;

#define GNSS_EPH_SLOW_MS 20u

  g_eph_src_path[0] = '\0';

  {
    uint32_t t0 = gnss_now_ms();

    dir = opendir(SF32LB52_GNSS_EPH_DIR);
    t_opendir = gnss_now_ms() - t0;
  }

  if (t_opendir >= GNSS_EPH_SLOW_MS) {
    syslog(LOG_WARNING, "gnss: eph-src slow opendir %u ms\n",
           (unsigned)t_opendir);
    slow_n++;
  }

  if (dir != NULL) {
    for (;;) {
      uint32_t t0 = gnss_now_ms();
      uint32_t dt;

      /* 预算：选源也是为注入服务的，超时就当"这一轮没有源"（调用方按 frames==0
       * 退避重试，见 GNSS_EPH_FS_BUDGET_MS 的注释）。 */
      if (gnss_elapsed_ms(t0, t_src0) >= GNSS_EPH_FS_BUDGET_MS) {
        syslog(LOG_WARNING,
               "gnss: eph-src deferred (%u ms budget, %u entries)\n",
               (unsigned)GNSS_EPH_FS_BUDGET_MS, (unsigned)rd_n);
        gave_up = true;
        break;
      }

      de = readdir(dir);
      dt = gnss_now_ms() - t0;
      if (de == NULL) {
        break;
      }

      rd_n++;
      if (dt > rd_max) {
        rd_max = dt;
      }

      if (dt >= GNSS_EPH_SLOW_MS) {
        slow_n++;
        syslog(LOG_WARNING, "gnss: eph-src slow readdir %u ms at #%u (%s)\n",
               (unsigned)dt, rd_n, de->d_name);
      }

      if (!gnss_eph_parse_utc(de->d_name, &utc) || utc < best_utc) {
        continue;
      }

      n = snprintf(path, sizeof(path), "%s/%s",
                   SF32LB52_GNSS_EPH_DIR, de->d_name);
      if (n < 0 || n >= (int)sizeof(path)) {
        continue;
      }

      {
        bool ok;

        t0 = gnss_now_ms();
        ok = gnss_eph_path_ubx(path);
        dt = gnss_now_ms() - t0;
        st_n++;
        if (dt > st_max) {
          st_max = dt;
        }

        if (dt >= GNSS_EPH_SLOW_MS) {
          slow_n++;
          syslog(LOG_WARNING, "gnss: eph-src slow stat %u ms (%s)\n",
                 (unsigned)dt, path);
        }

        if (!ok) {
          continue;
        }
      }

      best_utc = utc;
      memcpy(g_eph_src_path, path, (size_t)n + 1u);
    }

    closedir(dir);
  }

  /* 汇总行总是打：它是"这次扫描正不正常"的唯一判据。全部是几毫秒 = 正常；
   * 出现几百毫秒/秒级 = 卡在那一刻开始出问题。 */
  syslog(LOG_WARNING,
         "gnss: eph-src %u ms opendir=%u readdir=%u/max=%u stat=%u/max=%u slow=%u hit=%s\n",
         (unsigned)(gnss_now_ms() - t_all), (unsigned)t_opendir,
         rd_n, (unsigned)rd_max, st_n, (unsigned)st_max, slow_n,
         g_eph_src_path[0] != '\0' ? g_eph_src_path : "-");

#undef GNSS_EPH_SLOW_MS

  /* 超预算 ⇒ "这一轮没有源"，按 frames==0 退避重试（调用方已有该路径）。
   * **不能**返回已经挑到的那个：它可能不是最新的那份（新推的文件往往排在后面），
   * 灌旧星历比不灌更糟。 */
  if (gave_up) {
    return NULL;
  }

  if (g_eph_src_path[0] != '\0') {
    return g_eph_src_path;
  }

  if (gnss_eph_path_ubx(SF32LB52_GNSS_EPH_FILE)) {
    return SF32LB52_GNSS_EPH_FILE;
  }

  if (gnss_eph_path_ubx(SF32LB52_GNSS_EPH_DUMP)) {
    return SF32LB52_GNSS_EPH_DUMP;
  }

  return NULL;
}

/** @brief 这一帧是不是会过期的星历载荷（MGA-xxx-EPH）。
 *
 *  MGA 的 msgId 在不同命名空间里复用（0x05 既是 QZSS-EPH 也是 GPS-UTC），
 *  光看 msgId 认不准，所以再用 type 字节（EPH 恒为 0x01）和 payload 长度一起认：
 *  GPS/QZSS 68、GAL 76、BDS 88 —— 与 App 生成端一一对应。
 *  认不出来的一律当作"不会过期"，宁可多灌也不误杀；时间/位置辅助帧（id 0x40）
 *  和导航库帧（0x80）本来就不走这个判据。
 */
/**
 * @brief 从星历帧里取 `toe`（秒，周内）。
 *
 * 三套格式的偏移和单位都不同（这是最容易抄错的一栏）：
 *   GPS/QZSS-EPH：offset 40，U2，**×16 s**；GAL-EPH：offset 50，U2，**×60 s**；
 *   BDS-EPH：offset 24，U4，**×8 s**。已逐字段对照 pyubx2 的字段表核实。
 *
 * @return 周内秒；不是星历帧或长度不够时返回 -1。
 */
static int32_t gnss_ubx_eph_toe_s(uint8_t cls, uint8_t id, uint16_t plen,
                                  const uint8_t *pl)
{
  uint32_t raw;

  if (cls != 0x13u || pl == NULL) {
    return -1;
  }

  if (id == 0x00u || id == 0x05u) {          /* MGA-GPS/QZSS-EPH */
    if (plen < 42u) {
      return -1;
    }

    raw = (uint32_t)pl[40] | ((uint32_t)pl[41] << 8);
    return (int32_t)(raw * 16u);
  }

  if (id == 0x02u) {                         /* MGA-GAL-EPH */
    if (plen < 52u) {
      return -1;
    }

    raw = (uint32_t)pl[50] | ((uint32_t)pl[51] << 8);
    return (int32_t)(raw * 60u);
  }

  if (id == 0x03u) {                         /* MGA-BDS-EPH */
    if (plen < 28u) {
      return -1;
    }

    raw = (uint32_t)pl[24] | ((uint32_t)pl[25] << 8) |
          ((uint32_t)pl[26] << 16) | ((uint32_t)pl[27] << 24);
    return (int32_t)(raw * 8u);
  }

  return -1;
}

static bool gnss_ubx_is_eph_frame(uint8_t cls, uint8_t id, uint16_t plen,
                                  const uint8_t *pl)
{
  if (cls != 0x13 || pl == NULL || plen < 1u || pl[0] != 0x01) {
    return false;
  }

  switch (id) {
    case 0x00: /* MGA-GPS-EPH */
    case 0x05: /* MGA-QZSS-EPH */
      return plen == 68u;
    case 0x02: /* MGA-GAL-EPH */
      return plen == 76u;
    case 0x03: /* MGA-BDS-EPH */
      return plen == 88u;
    default:
      return false;
  }
}

/** @brief 按文件年龄把辅助帧里的精度字段改成**如实值**。
 *
 *  文件是 App 在下发那一刻生成的：里面的时间是"当时"的手机时间、坐标是"当时"的位置，
 *  而固件可能在几小时以后才把它注入模组。模组无从知道这段延迟 —— 它只能信帧里写的
 *  tAcc / posAcc。所以这里把年龄写回不确定度里：
 *    - `MGA-INI-TIME_UTC`：tAcc 在 offset 16（U2，秒），取 max(原值, 年龄)；
 *    - `MGA-INI-POS_LLH`：posAcc 在 offset 16（U4，厘米），取 max(原值, 年龄 × 50 m/s)。
 *  年龄未知（assist_age_s = GNSS_EPH_ASSIST_AGE_MAX）就把两个字段顶到上限。
 *
 *  只改 payload，校验和由 gnss_ubx_send() 重算（它按 payload 重新组帧），
 *  所以这里必须在**文件自身校验通过之后**调用。
 */
static void gnss_eph_assist_fixup(uint16_t plen, uint8_t *pl, uint32_t age_s)
{
  uint32_t v;
  uint64_t drift;

  if (pl == NULL || plen < 18u) {
    return;
  }

  if (pl[0] == 0x10u) {
    v = (uint32_t)pl[16] | ((uint32_t)pl[17] << 8);
    if (v < age_s) {
      v = age_s;
    }
    if (v < 1u) {
      v = 1u;
    }
    if (v > GNSS_EPH_ASSIST_AGE_MAX) {
      v = GNSS_EPH_ASSIST_AGE_MAX;
    }
    pl[16] = (uint8_t)(v & 0xff);
    pl[17] = (uint8_t)((v >> 8) & 0xff);
    syslog(LOG_INFO, "gnss: assist time age=%us -> tAcc=%us\n",
           (unsigned)age_s, (unsigned)v);
  } else if (plen == 20u && pl[0] == 0x01u) {
    v = (uint32_t)pl[16] | ((uint32_t)pl[17] << 8) |
        ((uint32_t)pl[18] << 16) | ((uint32_t)pl[19] << 24);
    drift = (uint64_t)age_s * GNSS_EPH_ASSIST_DRIFT_CMS;
    if (drift > GNSS_EPH_ASSIST_PACC_MAX) {
      drift = GNSS_EPH_ASSIST_PACC_MAX;
    }
    if ((uint64_t)v < drift) {
      v = (uint32_t)drift;
    }
    pl[16] = (uint8_t)(v & 0xff);
    pl[17] = (uint8_t)((v >> 8) & 0xff);
    pl[18] = (uint8_t)((v >> 16) & 0xff);
    pl[19] = (uint8_t)((v >> 24) & 0xff);
    syslog(LOG_INFO, "gnss: assist pos age=%us -> pacc=%ucm\n",
           (unsigned)age_s, (unsigned)v);
  }
}

/* ---- 注入回执（UBX-MGA-ACK）统计 ------------------------------------------
 *
 * 为什么要有：`eph done frames=100 badck=0 brk=eof` 只证明**我们把 100 帧完整写出
 * 去了**，不证明**模组收下了**。模组对每一帧 MGA 都会回 `UBX-MGA-ACK`（0x13/0x60），
 * 里面的 `infoCode` 才是"收没收"的判据：
 *
 *   0 accepted / 1 no-time（要先发 MGA-INI-TIME）/ 2 type-not-supported /
 *   3 size-mismatch / 4 database-store-failed / 5 not-ready / 6 unknown-type
 *
 * 注入循环原来每帧前 `gnss_drain()`，把这些回执连同 NMEA 一起读掉丢了 —— 所以
 * "模组到底收下了哪些帧"这件事从上线起就没被观测过。
 *
 * 起因：2026-09-27 用倒库做字节搜索，发现灌进去的 GAL/BDS 参数能在模组导航库里
 * 找到（30/30、32/32），**GPS/QZSS 一条都找不到（0/31、0/5）**，而且在"注入后 5 秒
 * 就倒库、没有覆盖时间"的条件下稳定复现；同时两个独立来源（pyubx2 字段表、
 * ubx-mga 参考实现）都证实 App 的四个星座打包偏移无误，LNAV 子帧流也不存在。
 * 剩下的唯一判据就是这些 infoCode。
 * ------------------------------------------------------------------------- */
#define GNSS_ACK_INFO_ACCEPTED 0u
#define GNSS_ACK_KIND_MAX      8u

/** `pl[0]`：1 = UBX-MGA-ACK-DATA0（受理），0 = UBX-MGA-NAK-DATA0（不受理）。
 *  两者**字段布局相同**（type/version/infoCode/msgId/payloadStart），所以只有这一个
 *  字节能区分"模组答复是 ACK 还是 NAK"——不记它就没法解释 infoCode 的来源。 */
#define GNSS_ACK_TYPE_ACK      1u

static uint16_t g_eph_ack_ok[GNSS_ACK_KIND_MAX];
static uint16_t g_eph_ack_bad[GNSS_ACK_KIND_MAX];
static uint8_t  g_eph_ack_last[GNSS_ACK_KIND_MAX];
static uint8_t  g_eph_ack_last_type[GNSS_ACK_KIND_MAX];
static uint16_t g_eph_ack_other;

static void gnss_eph_ack_note(uint8_t ack_type, uint8_t msg_id, uint8_t code)
{
  if (msg_id >= GNSS_ACK_KIND_MAX) {
    g_eph_ack_other++;
    return;
  }

  if (ack_type == GNSS_ACK_TYPE_ACK && code == GNSS_ACK_INFO_ACCEPTED) {
    g_eph_ack_ok[msg_id]++;
  } else {
    g_eph_ack_bad[msg_id]++;
    g_eph_ack_last[msg_id] = code;
    g_eph_ack_last_type[msg_id] = ack_type;
  }
}

/**
 * @brief 读掉待收字节，顺手统计 MGA-ACK。
 *
 * 取代注入循环里原来的 `gnss_drain()`：一样起"清空待收"的作用，但不再把最重要的
 * 回执一起丢掉。读到没有完整帧为止（每轮最多等 ms 毫秒）。
 */
static void gnss_eph_ack_sweep(int fd, int rounds, int ms)
{
  int i;

  for (i = 0; i < rounds; i++) {
    uint8_t cls = 0;
    uint8_t id = 0;
    uint16_t plen = 0;

    if (gnss_ubx_read_frame(fd, &cls, &id, &plen, ms) <= 0) {
      break;
    }

    /* MGA-ACK/NAK-DATA0 载荷：type(0) version(1) infoCode(2) msgId(3) payloadStart(4..7) */
    if (cls == 0x13u && id == 0x60u && plen >= 8u) {
      gnss_eph_ack_note(s_ubx_pl[0], s_ubx_pl[3], s_ubx_pl[2]);
    }
  }
}

static void gnss_eph_ack_reset(void)
{
  memset(g_eph_ack_ok, 0, sizeof(g_eph_ack_ok));
  memset(g_eph_ack_bad, 0, sizeof(g_eph_ack_bad));
  memset(g_eph_ack_last, 0, sizeof(g_eph_ack_last));
  memset(g_eph_ack_last_type, 0, sizeof(g_eph_ack_last_type));
  g_eph_ack_other = 0;
}

static void gnss_eph_ack_report(void)
{
  syslog(LOG_WARNING,
         "gnss: mga ack gps=%u/%u qzss=%u/%u gal=%u/%u bds=%u/%u other=%u "
         "(ok/bad)\n",
         (unsigned)g_eph_ack_ok[0x00], (unsigned)g_eph_ack_bad[0x00],
         (unsigned)g_eph_ack_ok[0x05], (unsigned)g_eph_ack_bad[0x05],
         (unsigned)g_eph_ack_ok[0x02], (unsigned)g_eph_ack_bad[0x02],
         (unsigned)g_eph_ack_ok[0x03], (unsigned)g_eph_ack_bad[0x03],
         (unsigned)g_eph_ack_other);

  if (g_eph_ack_bad[0x00] != 0u || g_eph_ack_bad[0x05] != 0u ||
      g_eph_ack_bad[0x02] != 0u || g_eph_ack_bad[0x03] != 0u) {
    /* 每个星座打**最后一个被拒的** (type/infoCode)：
     *   type：1 = ACK-DATA0，0 = NAK-DATA0
     *   infoCode：1 no-time / 2 type-unsupported / 3 size-mismatch /
     *             4 database-store-failed / 5 not-ready / 6 unknown-type
     * 另外注意：ACK 覆盖不全（实测 154 帧 DBD 只回 40 个、98 帧星历回 92 个），
     * 且 INI（时间/位置）帧**完全不回执** ⇒ 别拿这里的计数当帧数。 */
    syslog(LOG_WARNING,
           "gnss: mga ack rejected gps=%u/%u qzss=%u/%u gal=%u/%u bds=%u/%u "
           "(type/info)\n",
           (unsigned)g_eph_ack_last_type[0x00], (unsigned)g_eph_ack_last[0x00],
           (unsigned)g_eph_ack_last_type[0x05], (unsigned)g_eph_ack_last[0x05],
           (unsigned)g_eph_ack_last_type[0x02], (unsigned)g_eph_ack_last[0x02],
           (unsigned)g_eph_ack_last_type[0x03], (unsigned)g_eph_ack_last[0x03]);
  }
}

static void gnss_eph_inject(int uartfd)
{
  /* 真有灌注在跑（不管谁触发的）就说明"需要星历"这件事已经满足了。 */
  g_eph_wake_defer_ms = 0;
  const char *path;
  uint8_t hdr[8];
  uint8_t cls;
  uint8_t id;
  uint16_t plen;
  uint8_t cka;
  uint8_t ckb;
  uint8_t a;
  uint8_t b;
  uint16_t i;
  uint16_t maxpl;
  int fd;
  int frames = 0;
  ssize_t n;
  bool dbd = false;
  bool eph_stale = false;
  bool drop_ini_time = false;
  bool toe_checked = false;   /* 帧内 toe ↔ 文件名 的一致性只查第一帧星历 */
  uint32_t tow_doc = 0;       /* 文件名里的 UTC 换算成 GPS 周内秒 */
  uint32_t assist_age_s = GNSS_EPH_ASSIST_AGE_MAX;

  /* 诊断：注入取证。这些变量只被 syslog 读取，不影响注入行为。 */
  struct stat st0;
  struct stat st1;
  int skip = 0;                 /* 被过滤规则跳过的帧 */
  int stale = 0;                /* 其中因"文件超龄"丢掉的帧 */
  int time_drop = 0;            /* 其中丢掉的时间辅助帧（固件自己那帧更准） */
  int badck = 0;                /* 校验失败的帧 */
  int bytes = 0;                /* 实际读掉的文件字节数 */
  bool have_st0 = false;
  bool have_st1 = false;
  const char *brk = "eof";      /* 循环退出原因 */
  uint32_t t_inj0 = gnss_now_ms();

  gnss_eph_busy_set(true);
  gnss_heartbeat();

  /* 诊断：0 帧是本函数最危险的结局（调用方会当成"已注入"），
   * 所以从进函数就开始计时并记录来源文件属性。 */
  gnss_rt_mark("eph-enter");
  syslog(LOG_WARNING, "gnss: eph inject enter\n");

  /* 每次注入重新计。早退（mtp busy / 无文件）时保持 0，调用方据此不会误判。 */
  g_eph_last_stale = 0;
  /* 回执统计同样每次注入重开（见 gnss_eph_ack_report 的说明）。 */
  gnss_eph_ack_reset();

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  if (myvendor_mtp_lfs_quiesce()) {
    /* 打出**具体成因**：quiesce 是四项或起来的，只说 "mtp busy" 分不清是真在
     * 传输、还是 hold 计数泄漏、还是"仅插着 USB 充电"就把 LFS 全冻住了。
     * 现场就出现过用户只是插着充电、星历注入却被跳过的情况。 */
    /* 必须容得下 quiesce_why 的完整输出：基础四项约 30 字符，hold 命中时还要
     * 追加 `hold_by[a+1234ms b+5678ms …]`（4 条最多 ~80 字符）。给 64 字节时
     * 会把它截断成 `hold_by[gnss+29458ms gnss+19699ms g` —— 最要紧的"谁挂的"
     * 恰好被切掉，等于白加。 */
    char why[160];

    myvendor_mtp_lfs_quiesce_why(why, sizeof(why));
    syslog(LOG_INFO, "gnss: eph inject skip (mtp busy: %s)\n", why);
    /* 这是一次**瞬态**让路：BLE 写 LittleFS 时 companion_fs 会
     * xfer_lfs_acquire，几秒后就释放。调用方据此安排退避重试，
     * 而不是把它当成"本轮星历已完成"。 */
    g_eph_last_busy = true;
    goto done;
  }
#endif

  if (!gnss_ubx_pl_ensure()) {
    syslog(LOG_ERR, "gnss: eph inject skip (no BoardPSRAM payload buf)\n");
    goto done;
  }

  /* 只短扫目录/开文件，UART 灌帧期间不要 lfs_hold，否则 splash 地图一直等。 */
  /* rt 面包屑：`eph inject enter` 到首次 SD 失败之间恒定有 4.2 秒，而那几件事
   * 里只有 gnss_ubx_pl_ensure() 的 board_malloc_psram 不是"读"。这几个 mark
   * 打出每段各花多久，是定死那 4.2 秒的唯一直接办法 —— GNSS_PHASE 是读线程
   * 和 eph 共用的全局量，读线程会把它覆盖掉，所以 phase= 看不出 eph 卡在哪。 */
  gnss_rt_mark("eph-psram-done");
  GNSS_PHASE("eph-fs-begin");
  gnss_eph_fs_begin();
  gnss_rt_mark("eph-fs-begin");
  GNSS_PHASE("eph-prune");
  gnss_eph_prune();
  gnss_rt_mark("eph-prune");
  GNSS_PHASE("eph-src");
  path = gnss_eph_src();
  gnss_rt_mark("eph-src");
  if (path == NULL) {
    gnss_eph_fs_end();
    syslog(LOG_INFO, "gnss: no mga_*.ubx to inject\n");
    gnss_path_set(g_inject_last_path, NULL);
    goto done;
  }

  gnss_path_set(g_inject_last_path, path);

  /* 诊断：记录开读之前的文件属性，结束时再 stat 一次做对比。
   * size/mtime 在注入期间变化 = 有人在并发改写这个文件。 */
  have_st0 = (stat(path, &st0) == 0);
  syslog(LOG_WARNING,
         "gnss: eph src %s size=%ld mtime=%ld stat_ok=%d\n",
         path, have_st0 ? (long)st0.st_size : -1L,
         have_st0 ? (long)st0.st_mtime : -1L, have_st0 ? 1 : 0);

  /* 同一份源 + 本次开机已经注入成功过 ⇒ 不重灌。
   *
   * 触发源多半是手机的 `TIME_SYNC -> eph reload`（见 myvendor_gnss_eph_reload）：
   * 文件根本没变，模块备份里星历还在，再灌 106 帧只是多花 7 s SD 和一次 UART
   * 突发，还打断它刚开始的首次定位。判定放在这里是因为路径/大小/mtime 本来
   * 就要读，不额外扫盘。 */
  if (g_eph_injected && g_eph_sig_valid && have_st0)
    {
      const char *bn = strrchr(path, '/');

      bn = (bn != NULL) ? (bn + 1) : path;
      if (strcmp(bn, g_eph_sig_name) == 0 &&
          (uint32_t)st0.st_size == g_eph_sig_size &&
          (uint32_t)st0.st_mtime == g_eph_sig_mtime)
        {
          g_eph_skip_same_src = true;
          syslog(LOG_WARNING,
                 "gnss: eph skip same src %s size=%ld mtime=%ld (already injected)\n",
                 bn, (long)st0.st_size, (long)st0.st_mtime);
          gnss_path_set(g_inject_last_path, path);
          gnss_eph_fs_end();
          goto done;
        }
    }

  /* 记住这份源（成功注入后由 gnss_eph_run 认账，见 g_eph_sig_valid）。 */
  if (have_st0)
    {
      const char *bn = strrchr(path, '/');
      unsigned k = 0;

      bn = (bn != NULL) ? (bn + 1) : path;
      while (k + 1u < (unsigned)sizeof(g_eph_sig_name) && bn[k] != '\0')
        {
          g_eph_sig_name[k] = bn[k];
          k++;
        }

      g_eph_sig_name[k] = '\0';
      g_eph_sig_size = (uint32_t)st0.st_size;
      g_eph_sig_mtime = (uint32_t)st0.st_mtime;
    }

  /* 注入前的年龄门限：EPH 只有 2–4 h 有效，灌老星历不是"聊胜于无"，而是可能让模组
   * 按错误的卫星位置起算。同文件里的 INI 时间和位置辅助**不设门限**，照发 —— 它们
   * 不随星历过期，而且正好是加快首次定位要用的东西。DBD dump 是诊断用的原样回灌，
   * 同样不设门限。
   *
   * 年龄取文件名里的 UTC（与 gnss_eph_src() 的选法一致），旧名字
   * （mga.ubx/dump.ubx）退回 mtime。任一项拿不到 —— 文件名没时间、RTC 还没对时
   * （time() 未初始化会返回 -1）—— 就放行，保持老行为，绝不在信息不足时误杀。 */
  {
    const char *base = strrchr(path, '/');
    time_t now_t = time(NULL);
    uint32_t utc = 0;
    uint32_t now_s = 0;

    base = (base != NULL) ? (base + 1) : path;
    if (!gnss_eph_parse_utc(base, &utc) &&
        have_st0 && st0.st_mtime >= (time_t)GNSS_TIME_MIN_UNIX) {
      utc = (uint32_t)st0.st_mtime;
    }

    if (now_t >= (time_t)GNSS_TIME_MIN_UNIX &&
        now_t <= (time_t)0xffffffffu) {
      now_s = (uint32_t)now_t;
    }

    if (utc >= (uint32_t)GNSS_TIME_MIN_UNIX) {
      /* 文件名 → GPS 周内秒（下面"帧内 toe 互校"要用）。**从 GPS 历元起算**。 */
      tow_doc = (uint32_t)(((uint64_t)utc + GNSS_LEAP_SEC - GNSS_GPS_EPOCH_SEC)
                           % 604800u);
    }

    if (utc >= (uint32_t)GNSS_TIME_MIN_UNIX && now_s != 0) {
      assist_age_s = (now_s > utc) ? (now_s - utc) : 0u;
      if (assist_age_s > GNSS_EPH_VALID_SEC) {
        eph_stale = true;
      }
      /* RTC 有效且文件里的时间已经过时：不再发那帧（固件自己那帧更准）。 */
      if (assist_age_s > GNSS_EPH_ASSIST_TIME_FRESH_SEC) {
        drop_ini_time = true;
      }
    }

    syslog(LOG_WARNING,
           "gnss: eph age utc=%lu now=%lu limit=%u stale=%d assist_age=%lu "
           "time_drop=%d\n",
           (unsigned long)utc, (unsigned long)now_s,
           (unsigned)GNSS_EPH_VALID_SEC, eph_stale ? 1 : 0,
           (unsigned long)assist_age_s, drop_ini_time ? 1 : 0);
  }

  GNSS_PHASE("eph-open");
  fd = open(path, O_RDONLY);
  gnss_eph_fs_end();
  gnss_rt_mark("eph-open");
  if (fd < 0) {
    syslog(LOG_WARNING, "gnss: eph open %s failed %d\n", path, errno);
    brk = "open-fail";
    goto done;
  }

  GNSS_PHASE("eph-read");
  n = read(fd, hdr, 6);
  bytes += (int)n;
  if (n != 6 || hdr[0] != 0xb5 || hdr[1] != 0x62) {
    close(fd);
    syslog(LOG_WARNING, "gnss: eph %s not UBX\n", path);
    /* 诊断：把预读结果原样打出来。n==0 就是文件当时为空（或已被截断），
     * 这是"注入 0 帧"最可能的成因。 */
    syslog(LOG_WARNING,
           "gnss: eph pre-read n=%d first=%02x%02x (n!=6 -> 空/被截断)\n",
           (int)n, hdr[0], hdr[1]);
    brk = "pre-read-not-ubx";
    goto done;
  }

  dbd = (hdr[2] == 0x13 && hdr[3] == 0x80);
  syslog(LOG_WARNING,
         "gnss: eph first frame cls=0x%02x id=0x%02x mode=%s\n",
         hdr[2], hdr[3], dbd ? "DBD" : "MGA");
  if (lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    brk = "lseek-fail";
    goto done;
  }

  gnss_eph_send_time(uartfd);
  gnss_eph_send_pos(uartfd);

  for (;;) {
    n = read(fd, hdr, 6);
    bytes += (int)n;
    if (n == 0) {
      brk = "eof";
      break;
    }

    if (n != 6 || hdr[0] != 0xb5 || hdr[1] != 0x62) {
      syslog(LOG_WARNING, "gnss: eph %s not UBX at %d\n", path, frames);
      brk = "not-ubx-mid";
      break;
    }

    cls = hdr[2];
    id = hdr[3];
    plen = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
    maxpl = dbd ? GNSS_UBX_PAY_MAX : GNSS_EPH_INJECT_MAX;
    if (plen > maxpl) {
      syslog(LOG_WARNING, "gnss: eph payload %u too big\n", (unsigned)plen);
      brk = "payload-too-big";
      break;
    }

    if (plen > 0) {
      n = read(fd, s_ubx_pl, plen);
      bytes += (int)n;
      if (n != (ssize_t)plen) {
        /* 诊断：短读。文件在读完 payload 之前就结束了 = 半截帧。 */
        syslog(LOG_WARNING,
               "gnss: eph short payload want=%u got=%d frame=%d\n",
               (unsigned)plen, (int)n, frames);
        brk = "short-payload";
        break;
      }
    }

    n = read(fd, hdr + 6, 2);
    bytes += (int)n;
    if (n != 2) {
      /* 诊断：连校验位都读不到，同样是文件被截断的特征。 */
      syslog(LOG_WARNING,
             "gnss: eph short cksum want=2 got=%d frame=%d\n",
             (int)n, frames);
      brk = "short-cksum";
      break;
    }

    gnss_ubx_cksum(hdr + 2, 4, &cka, &ckb);
    a = cka;
    b = ckb;
    for (i = 0; i < plen; i++) {
      a = (uint8_t)(a + s_ubx_pl[i]);
      b = (uint8_t)(b + a);
    }

    if (hdr[6] != a || hdr[7] != b) {
      badck++;
      syslog(LOG_WARNING, "gnss: eph bad checksum frame %d\n", frames);
      brk = "bad-checksum";
      break;
    }

    /* 文件里的辅助帧（MGA-INI-TIME_UTC / POS_LLH）：按年龄把 tAcc/posAcc 改成如实值。
     * 时间帧若已过时且 RTC 有效，就不发这一帧（固件自己刚发的那帧更准）；
     * DBD dump 是模组自己的数据，不碰。 */
    if (!dbd && cls == 0x13 && id == 0x40 && plen > 0u) {
      if (drop_ini_time && s_ubx_pl[0] == 0x10u) {
        stale++;
        time_drop++;
        syslog(LOG_INFO, "gnss: assist time frame dropped (age=%us)\n",
               (unsigned)assist_age_s);
        continue;
      }

      /* 年龄算不出来（没有时基，`now == 0`）或已经老到顶：**位置辅助帧不发**。
       *
       * pacc 只能按龄漂移推，算不出龄时就是哨兵值 —— 2026-09-18 现场那帧是
       * `assist pos age=65535s -> pacc=100000000cm`（1000 km）。这种"辅助"比不发
       * 更糟：模组要么忽略，要么按一个错的位置起算。
       * 时间帧照发：它带的 UTC 本身就是有效值，能帮模组对时（同一现场，
       * 有 RTC 时同一条路径就是 `age=1s -> pacc=5000cm`）。 */
      if (plen == 20u && s_ubx_pl[0] == 0x01u &&
          assist_age_s >= GNSS_EPH_ASSIST_AGE_MAX) {
        skip++;
        syslog(LOG_INFO, "gnss: assist pos frame dropped (age unknown)\n");
        continue;
      }

      gnss_eph_assist_fixup(plen, s_ubx_pl, assist_age_s);
    }

    if (dbd) {
      if (cls != 0x13 || id != 0x80) {
        skip++;
        continue;
      }
    } else if (cls == 0x13 && id == 0x80) {
      /* 旧 dump 若混进 AssistNow 文件，丢掉导航库帧，只灌广播星历。 */
      skip++;
      continue;
    } else if (eph_stale && gnss_ubx_is_eph_frame(cls, id, plen, s_ubx_pl)) {
      /* 超龄星历：不灌，但继续扫完文件 —— 同文件里还有时间和位置辅助要发。 */
      stale++;
      skip++;
      continue;
    }

    /* 一次性闸门：拿**帧内 toe** 和**文件名里的 UTC** 互校（只查第一帧星历）。
     *
     * 为什么不能只看文件名：`eph age` 用的就是文件名/mtime，所以"旧内容配新名字"
     * —— App 用了缓存的旧星历、手机时钟错、下到的 BRDC 不是当天 —— 能整份通过年龄
     * 门限。帧内 toe 才是数据自己的时间。判据取 2 h：toe 本来就在 2 h 网格上，
     * 生成时刻离最近网格点最多 1 h，留一倍余量。 */
    if (!toe_checked && tow_doc != 0u) {
      int32_t toe_s = gnss_ubx_eph_toe_s(cls, id, plen, s_ubx_pl);

      if (toe_s >= 0) {
        int32_t d = (int32_t)((int64_t)tow_doc - (int64_t)toe_s);

        toe_checked = true;
        if (d > 302400) {
          d -= 604800;
        } else if (d < -302400) {
          d += 604800;
        }

        if (d < 0) {
          d = -d;
        }

        if (d > 7200) {
          syslog(LOG_WARNING,
                 "gnss: eph toe mismatch %s: name_tow=%lu toe=%ld diff=%ld s "
                 "(内容与文件名的时间不符 —— 旧星历配了新名字？)\n",
                 path, (unsigned long)tow_doc, (long)toe_s, (long)d);
        }
      }
    }

    /* 清空待收 + 收走上一帧的回执（原来是 gnss_drain，把回执一起丢了）。 */
    gnss_eph_ack_sweep(uartfd, 64, 2);
    if (dbd) {
      if (gnss_ubx_send_pl(uartfd, cls, id, plen) < 0) {
        brk = "send-dbd-fail";
        break;
      }

      (void)gnss_ubx_read_frame(uartfd, &cls, &id, &plen, 40);
    } else if (gnss_ubx_send(uartfd, cls, id, s_ubx_pl, plen) < 0) {
      /* 诊断：UART 写失败导致提前退出。gnss_write_all 内部另有详细日志。 */
      syslog(LOG_WARNING,
             "gnss: eph uart send fail frame=%d cls=0x%02x id=0x%02x\n",
             frames, cls, id);
      brk = "send-mga-fail";
      break;
    }

    frames++;
    /* 星历下发的"正在发生"标记（状态栏卫星图标黄闪用）：**逐帧**打点，见 g_eph_frame_ms。 */
    g_eph_frame_ms = (uint32_t)TICK2MSEC(clock_systime_ticks());
    if (g_eph_frame_ms == 0u)
      {
        g_eph_frame_ms = 1u;
      }
#ifdef CONFIG_MYVENDOR_GNSS_LOG
    if ((frames % 50) == 0) {
      GNSS_LOG("eph inject %d %s frames %s",
               frames, dbd ? "DBD" : "MGA", path);
    }
#endif
    if ((frames & 7) == 0) {
      gnss_heartbeat();
      g_nmea_life_ms = gnss_now_ms();
    }

    if (g_diag_kick || g_gnss_stop) {
      syslog(LOG_WARNING, "gnss: eph inject abort (diag)\n");
      brk = "abort-diag";
      break;
    }

    gnss_sleep_us(GNSS_EPH_GAP_US);
  }

  close(fd);
  /* 收尾再扫一次：最后一帧的回执往往在循环退出之后才到。 */
  gnss_sleep_ms(20);
  gnss_eph_ack_sweep(uartfd, 32, 20);
  gnss_drain(uartfd);
  gnss_eph_ack_report();
  syslog(LOG_INFO, "gnss: injected %d %s frames from %s\n",
         frames, dbd ? "DBD" : "MGA", path);

  /* 诊断：注入取证汇总。brk 说明循环为什么停，bytes 是实际读掉多少字节，
   * size 前后对比说明文件有没有在注入期间被改写。 */
  have_st1 = (stat(path, &st1) == 0);
  g_eph_last_frames = frames;
  g_eph_last_skip = skip;
  g_eph_last_badck = badck;
  g_eph_last_bytes = bytes;
  g_eph_last_stale = stale;
  g_eph_last_cost_ms = gnss_elapsed_ms(gnss_now_ms(), t_inj0);

  /* 指纹"认账"放在这里而不是 gnss_eph_run()：手机的 `TIME_SYNC -> eph reload`
   * 走的是 `gnss_eph_try_reload()` → 直接调本函数，**绕过 run**。两条路径都要
   * 认账，否则手机推过新文件之后还会被当成"没注入过"再灌一遍。 */
  if (frames > 0 && have_st0)
    {
      g_eph_sig_valid = true;
    }
  syslog(LOG_WARNING,
         "gnss: eph done frames=%d skip=%d stale=%d tdrop=%d badck=%d "
         "bytes=%d brk=%s mode=%s size=%ld->%ld mtime=%ld->%ld changed=%d "
         "cost=%u\n",
         frames, skip, stale, time_drop, badck, bytes, brk,
         dbd ? "DBD" : "MGA",
         have_st0 ? (long)st0.st_size : -1L,
         have_st1 ? (long)st1.st_size : -1L,
         have_st0 ? (long)st0.st_mtime : -1L,
         have_st1 ? (long)st1.st_mtime : -1L,
         (have_st0 && have_st1 &&
          (st0.st_size != st1.st_size || st0.st_mtime != st1.st_mtime)) ? 1 : 0,
         (unsigned)g_eph_last_cost_ms);

  if (frames == 0) {
    /* 诊断：这是本日志最重要的一行。调用方 gnss_eph_run() 之后会无条件
     * 置 g_eph_injected=true，本轮不再重试——0 帧就是"星历报废"的直接原因。 */
    syslog(LOG_ERR,
           "gnss: eph ZERO FRAMES brk=%s bytes=%d stale=%d mode=%s changed=%d "
           "(stale>0 -> caller marks done, else retry)\n",
           brk, bytes, stale, dbd ? "DBD" : "MGA",
           (have_st0 && have_st1 &&
            (st0.st_size != st1.st_size ||
             st0.st_mtime != st1.st_mtime)) ? 1 : 0);
  }

  gnss_rt_mark("eph-done");

  if (!dbd && frames > 0) {
    const char *base;
    uint32_t utc = 0;
    struct stat st;

    base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    if (gnss_eph_parse_utc(base, &utc)) {
      gnss_eph_remember(utc);
    } else if (stat(path, &st) == 0 &&
               st.st_mtime >= (time_t)GNSS_TIME_MIN_UNIX) {
      gnss_eph_remember((uint32_t)st.st_mtime);
    }
  }

done:
  /* 诊断：早退（mtp busy / 无文件 / 打开失败）时也把帧数归零，
   * 否则 gnss_eph_run() 的收尾日志会报上一次的旧值，产生误判。 */
  if (frames == 0) {
    g_eph_last_frames = 0;
  }

  if (frames > 0) {
    gnss_eph_grace_begin();
    syslog(LOG_INFO, "gnss: eph grace %u ms (wait nmea)\n",
           (unsigned)GNSS_EPH_GRACE_MS);
  }

  gnss_eph_busy_set(false);
}

static bool gnss_dbd_buf_ensure(void)
{
  if (s_dbd_buf != NULL) {
    return true;
  }

  s_dbd_buf = board_malloc_psram(GNSS_EPH_DUMP_MAX);
  return s_dbd_buf != NULL;
}

static int gnss_dbd_append(int used, uint8_t cls, uint8_t id, uint16_t plen)
{
  int n = (int)plen + 8;
  uint8_t cka;
  uint8_t ckb;
  uint16_t i;

  if (s_dbd_buf == NULL || used < 0 || used + n > (int)GNSS_EPH_DUMP_MAX) {
    return -ENOSPC;
  }

  s_dbd_buf[used] = 0xb5;
  s_dbd_buf[used + 1] = 0x62;
  s_dbd_buf[used + 2] = cls;
  s_dbd_buf[used + 3] = id;
  s_dbd_buf[used + 4] = (uint8_t)(plen & 0xff);
  s_dbd_buf[used + 5] = (uint8_t)((plen >> 8) & 0xff);
  if (plen > 0) {
    memcpy(s_dbd_buf + used + 6, s_ubx_pl, plen);
  }

  gnss_ubx_cksum(s_dbd_buf + used + 2, 4, &cka, &ckb);
  for (i = 0; i < plen; i++) {
    cka = (uint8_t)(cka + s_ubx_pl[i]);
    ckb = (uint8_t)(ckb + cka);
  }

  s_dbd_buf[used + 6 + plen] = cka;
  s_dbd_buf[used + 7 + plen] = ckb;
  return used + n;
}

static void gnss_dbd_mark_done(void)
{
  pthread_mutex_lock(&g_lock);
  g_dump_done = true;
  pthread_mutex_unlock(&g_lock);
  g_dbd_halt = false;
}

static void gnss_dbd_abort(void)
{
  if (!g_dbd_run) {
    return;
  }

  g_dbd_run = false;
  syslog(LOG_WARNING, "gnss: dbd dump abort\n");
  gnss_dbd_mark_done();
}

static void gnss_dbd_finish(void)
{
  char dest[GNSS_EPH_PATH_MAX];
  int wfd;
  int ret;
  bool ok;

  if (!g_dbd_run) {
    return;
  }

  g_dbd_run = false;
  g_dbd_saved = true;
  ok = (g_dbd_n >= (int)GNSS_DBD_MIN_FRAMES && g_dbd_used > 64);
  if (!ok) {
    syslog(LOG_WARNING, "gnss: dbd dump incomplete ack=%d dbd=%d bytes=%d\n",
           g_dbd_ack ? 1 : 0, g_dbd_n, g_dbd_used);
    gnss_dbd_mark_done();
    return;
  }

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  if (myvendor_mtp_lfs_quiesce()) {
    /* 必须容得下 quiesce_why 的完整输出：基础四项约 30 字符，hold 命中时还要
     * 追加 `hold_by[a+1234ms b+5678ms …]`（4 条最多 ~80 字符）。给 64 字节时
     * 会把它截断成 `hold_by[gnss+29458ms gnss+19699ms g` —— 最要紧的"谁挂的"
     * 恰好被切掉，等于白加。 */
    char why[160];

    myvendor_mtp_lfs_quiesce_why(why, sizeof(why));
    syslog(LOG_INFO, "gnss: dbd dump skip write (mtp busy: %s)\n", why);
    gnss_dbd_mark_done();
    return;
  }
#endif

  if (g_diag_kick || g_gnss_stop) {
    syslog(LOG_WARNING, "gnss: dbd dump skip write (diag)\n");
    gnss_dbd_mark_done();
    return;
  }

  gnss_heartbeat();
  gnss_eph_fs_begin();
  gnss_dump_dest_path(dest, sizeof(dest));
  wfd = open(SF32LB52_GNSS_EPH_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd < 0) {
    gnss_eph_fs_end();
    syslog(LOG_WARNING, "gnss: dbd dump create failed %d\n", errno);
    gnss_dbd_mark_done();
    return;
  }

  {
    size_t off = 0;
    size_t total = (size_t)g_dbd_used;

    ret = 0;
    while (off < total) {
      size_t chunk = total - off;
      ssize_t nw;

      if (chunk > 4096u) {
        chunk = 4096u;
      }

      gnss_heartbeat();
      if (g_diag_kick || g_gnss_stop) {
        ret = -1;
        break;
      }

      nw = write(wfd, s_dbd_buf + off, chunk);
      if (nw != (ssize_t)chunk) {
        ret = -1;
        break;
      }

      off += chunk;
    }
  }
  close(wfd);
  if (ret == 0) {
    (void)unlink(dest);
    if (rename(SF32LB52_GNSS_EPH_TMP, dest) != 0) {
      (void)unlink(SF32LB52_GNSS_EPH_TMP);
      ret = -1;
    } else {
      (void)unlink(SF32LB52_GNSS_EPH_DUMP);
      gnss_eph_prune();
    }
  } else {
    (void)unlink(SF32LB52_GNSS_EPH_TMP);
  }

  gnss_eph_fs_end();
  if (ret != 0) {
    syslog(LOG_WARNING, "gnss: dbd dump write failed\n");
    gnss_dbd_mark_done();
    return;
  }

  gnss_path_set(g_dump_last_path, dest);
  syslog(LOG_INFO, "gnss: dumped %d DBD messages, %d bytes ack=%d -> %s\n",
         g_dbd_n, g_dbd_used, g_dbd_ack ? 1 : 0, dest);
  gnss_dbd_mark_done();
}

static void gnss_dbd_on_frame(uint8_t cls, uint8_t id, uint16_t plen)
{
  int n;

  if (!g_dbd_run) {
    return;
  }

  if (cls == 0x13 && id == 0x80) {
    n = gnss_dbd_append(g_dbd_used, cls, id, plen);
    if (n < 0) {
      syslog(LOG_WARNING, "gnss: dbd dump overflow at %d bytes\n", g_dbd_used);
      gnss_dbd_finish();
      return;
    }

    g_dbd_used = n;
    g_dbd_n++;
    g_dbd_tlast = gnss_now_ms();
  } else if (cls == 0x13 && id == 0x60) {
    g_dbd_ack = true;
    g_dbd_tlast = gnss_now_ms();
    gnss_dbd_finish();
  }
}

static void gnss_dbd_try_start(int fd, bool had_valid, uint32_t first_fix_ms)
{
  bool want = false;
  bool halt = false;

  if (g_dbd_run || fd < 0) {
    return;
  }

  /* Factory keeps USB MTP on /mnt/lfs; do not poll DBD or write eph. */
  if (myvendor_is_factory()) {
    static bool logged;
    bool need_done = false;

    pthread_mutex_lock(&g_lock);
    if (g_dump_req) {
      g_dump_req = false;
      need_done = true;
    }

    pthread_mutex_unlock(&g_lock);
    g_dbd_saved = true;
    if (!logged) {
      logged = true;
      syslog(LOG_INFO, "gnss: dbd dump disabled in factory\n");
    }

    if (need_done) {
      gnss_dbd_mark_done();
    }

    return;
  }

  pthread_mutex_lock(&g_lock);
  if (g_dump_req) {
    want = true;
    halt = g_dump_halt;
    g_dump_req = false;
  } else if (g_eph_busy || g_eph_reload) {
    pthread_mutex_unlock(&g_lock);
    return;
  }

  pthread_mutex_unlock(&g_lock);

  if (!want && myvendor_devctl_eph_auto_get() &&
      had_valid && !g_dbd_saved && first_fix_ms != 0 &&
      gnss_elapsed_ms(gnss_now_ms(), first_fix_ms) >= GNSS_DBD_AFTER_FIX_MS) {
    /* 自动倒库：刚被挡下过就晚点再问（见 GNSS_DBD_RETRY_MS）。
     * **只限速自动这条**：显式请求在上面已经把 g_dump_req 取走了，这里再
     * return 会把请求吞掉、再没人重试。 */
    if (g_dbd_quiet_ms != 0 &&
        gnss_elapsed_ms(gnss_now_ms(), g_dbd_quiet_ms) < GNSS_DBD_RETRY_MS) {
      return;
    }

    want = true;
  }

  if (!want) {
    return;
  }

  if (!had_valid || !gnss_ubx_pl_ensure() || !gnss_dbd_buf_ensure()) {
    g_dbd_quiet_ms = gnss_now_ms();
    syslog(LOG_INFO, "gnss: dbd dump skip (uart/fix/buf)\n");
    gnss_dbd_mark_done();
    return;
  }

#ifdef CONFIG_MYVENDOR_MTP_SIMPLE
  if (myvendor_mtp_lfs_quiesce()) {
    /* 必须容得下 quiesce_why 的完整输出：基础四项约 30 字符，hold 命中时还要
     * 追加 `hold_by[a+1234ms b+5678ms …]`（4 条最多 ~80 字符）。给 64 字节时
     * 会把它截断成 `hold_by[gnss+29458ms gnss+19699ms g` —— 最要紧的"谁挂的"
     * 恰好被切掉，等于白加。 */
    char why[160];

    myvendor_mtp_lfs_quiesce_why(why, sizeof(why));
    g_dbd_quiet_ms = gnss_now_ms();
    syslog(LOG_INFO, "gnss: dbd dump skip (mtp busy: %s)\n", why);
    gnss_dbd_mark_done();
    return;
  }
#endif

  g_dbd_used = 0;
  g_dbd_n = 0;
  g_dbd_ack = false;
  g_dbd_halt = halt;
  g_dbd_t0 = gnss_now_ms();
  g_dbd_tlast = g_dbd_t0;
  g_dbd_run = true;
  gnss_path_set(g_dump_last_path, NULL);
  if (gnss_ubx_send(fd, 0x13, 0x80, NULL, 0) < 0) {
    syslog(LOG_WARNING, "gnss: dbd dump poll send failed\n");
    g_dbd_run = false;
    gnss_dbd_mark_done();
    return;
  }

  syslog(LOG_INFO, "gnss: dbd dump start (nmea on)\n");
}

static void gnss_dbd_tick(void)
{
  uint32_t now;
  uint32_t idle;
  uint32_t total;

  if (!g_dbd_run) {
    return;
  }

  now = gnss_now_ms();
  idle = gnss_elapsed_ms(now, g_dbd_tlast);
  total = gnss_elapsed_ms(now, g_dbd_t0);
  if (g_dbd_n < (int)GNSS_DBD_MIN_FRAMES && idle >= GNSS_DBD_NODATA_MS) {
    syslog(LOG_INFO, "gnss: dbd dump skip (no data %u ms)\n",
           (unsigned)idle);
    g_dbd_run = false;
    gnss_dbd_mark_done();
    return;
  }

  if (g_dbd_ack ||
      (g_dbd_n >= (int)GNSS_DBD_MIN_FRAMES && idle >= GNSS_DBD_IDLE_MS) ||
      total >= GNSS_DBD_CAPTURE_MS) {
    gnss_dbd_finish();
  }
}

static void gnss_publish(myvendor_sys_gnss_t *fix)
{
  if (fix == NULL) {
    return;
  }

  fix->alive = true;
  /* PVT 与 NMEA 是同一历元的两个来源：发布前把最近一帧 PVT 贴上（没有就清无效），
   * 消费方才能拿到 gSpeed / sAcc，见 docs/gnss_speed_filter.md 步骤 2。 */
  gnss_pvt_attach(fix, gnss_now_ms());
  /* 停车/运动判定（1 Hz）：决定要不要把模组切到 stationary，见 gnss_dyn_feed。 */
  gnss_dyn_feed(fix, gnss_now_ms());
  /* 模组身份（MON-VER 读到的星座列表/固件串）：随快照带给 UI 的卫星页。 */
  fix->mod_const_mask = g_mod_const;
  snprintf(fix->mod_gnss, sizeof(fix->mod_gnss), "%s", g_mod_gnss);
  snprintf(fix->mod_fw, sizeof(fix->mod_fw), "%s", g_mod_fw);
  pthread_mutex_lock(&g_lock);
  g_fix = *fix;
  g_fix.rx_hz = gnss_rx_hz_now(gnss_now_ms());
  g_fix_ms = gnss_now_ms();
  /* 采样时刻写进快照本身：消费方据此算年龄。myvendor_gnss_get() 在恢复/
   * 保持窗口会原样回吐这份旧 g_fix，时间戳就是唯一能看出它旧了的线索。 */
  g_fix.stamp_ms = g_fix_ms;
  g_eph_grace_ms = 0;
  pthread_mutex_unlock(&g_lock);
  g_nmea_life_ms = g_fix_ms;

  /* 唤醒保持窗：唯一写者就是这里。有申请 → 从这一刻起算（不能被本次清掉），
   * 没申请 → 清。见 g_wake_hold_req 的注释。 */
  if (g_wake_hold_req) {
    g_wake_hold_req = false;
    g_wake_hold_ms = (g_fix_ms != 0) ? g_fix_ms : 1u;
  } else {
    g_wake_hold_ms = 0;
  }
  g_recovering = false;
  g_uart_fails = 0;
  gnss_link_add(&g_link.pub);
  if (!g_link_got_pub) {
    g_link_got_pub = true;
    syslog(LOG_INFO, "gnss: first publish valid=%d sats=%u q=%u\n",
           fix->valid ? 1 : 0, (unsigned)fix->satellites,
           (unsigned)fix->fix_quality);
  }
#ifdef CONFIG_MYVENDOR_GNSS_TRACE
  {
    static uint32_t last_pub_log;
    uint32_t t = g_fix_ms;

    if (last_pub_log == 0 || gnss_elapsed_ms(t, last_pub_log) >= 1000u) {
      last_pub_log = t;
      GNSS_TRACE("pub valid=%d sats=%u hear=%u view=%u q=%u spd=%u hz=%u "
               "pvt=%d gs=%u sacc=%u pacc=%u",
               fix->valid ? 1 : 0,
               (unsigned)fix->satellites,
               (unsigned)fix->sats_heard,
               (unsigned)fix->sats_in_view,
               (unsigned)fix->fix_quality,
               (unsigned)fix->speed_centi_kmh,
               (unsigned)fix->rx_hz,
               fix->pvt_valid ? 1 : 0,
               (unsigned)fix->speed_pvt_centi_kmh,
               (unsigned)fix->speed_acc_mm_s,
               (unsigned)(fix->pos_acc_mm / 1000u));
    }
  }
#endif
}

static void gnss_begin_epoch(myvendor_sys_gnss_t *fix)
{
  if (g_gsv_view_acc > 0) {
    fix->sats_in_view = g_gsv_view_acc;
  }

  if (g_gsv_heard_acc > 0) {
    fix->sats_heard = g_gsv_heard_acc;
  }

  g_gsv_view_acc = 0;
  g_gsv_heard_acc = 0;
}

static void gnss_apply_rmc(myvendor_sys_gnss_t *fix,
                           const struct minmea_sentence_rmc *rmc)
{
  float lat;
  float lon;
  float kn;
  float course;

  gnss_begin_epoch(fix);
  gnss_remember_date(&rmc->date);
  gnss_try_set_utc(fix, &rmc->date, &rmc->time);

  if (!rmc->valid) {
    /* 时间可先锁定；定位仍以本周期 GGA 为准，不要把跟踪星数清掉。 */
    fix->valid = false;
    fix->fix_quality = 0;
    fix->speed_centi_kmh = 0;
    g_rmc_pos_ms = 0;
    return;
  }

  lat = minmea_tocoord(&rmc->latitude);
  lon = minmea_tocoord(&rmc->longitude);
  if (!isfinite(lat) || !isfinite(lon)) {
    fix->valid = false;
    fix->fix_quality = 0;
    g_rmc_pos_ms = 0;
    return;
  }

  fix->valid = true;
  fix->lat_e7 = gnss_to_e7(lat);
  fix->lon_e7 = gnss_to_e7(lon);
  kn = minmea_tofloat(&rmc->speed);
  if (isfinite(kn) && kn > 0.0f) {
    unsigned centi = (unsigned)(kn * 185.2f + 0.5f);

    if (centi > 65535u) {
      centi = 65535u;
    }

    fix->speed_centi_kmh = (uint16_t)centi;
  } else {
    fix->speed_centi_kmh = 0;
  }

  g_rmc_pos_ms = gnss_now_ms();

  course = minmea_tofloat(&rmc->course);
  /* 低速 COG 乱转，停着时保持上一航向。 */
  if (isfinite(course) && kn >= 1.0f) {
    int deg = (int)(course + 0.5f) % 360;

    if (deg < 0) {
      deg += 360;
    }

    fix->course_deg = (uint16_t)deg;
  }

  if (fix->fix_quality < 1) {
    fix->fix_quality = 1;
  }
}

static void gnss_apply_gga(myvendor_sys_gnss_t *fix,
                           const struct minmea_sentence_gga *gga)
{
  float lat;
  float lon;
  float alt;
  int q;
  uint8_t hx;

  q = gga->fix_quality;
  hx = gnss_hdop_x10(&gga->hdop);
  if (hx != 0) {
    fix->hdop_x10 = hx;
  }

  if (gga->satellites_tracked > 0) {
    fix->satellites = (uint8_t)gga->satellites_tracked;
  }

  gnss_try_set_utc(fix, NULL, &gga->time);

  if (q <= 0) {
    if (g_rmc_pos_ms == 0 ||
        gnss_elapsed_ms(gnss_now_ms(), g_rmc_pos_ms) > 1500u) {
      fix->valid = false;
      fix->fix_quality = 0;
    }

    return;
  }

  lat = minmea_tocoord(&gga->latitude);
  lon = minmea_tocoord(&gga->longitude);
  if (isfinite(lat) && isfinite(lon)) {
    /* 本周期已有 RMC 坐标则不要覆盖，否则箭头在 GGA/RMC 之间来回跳。 */
    if (g_rmc_pos_ms == 0 ||
        gnss_elapsed_ms(gnss_now_ms(), g_rmc_pos_ms) > 1500u) {
      fix->lat_e7 = gnss_to_e7(lat);
      fix->lon_e7 = gnss_to_e7(lon);
    }

    fix->valid = true;
  }

  alt = minmea_tofloat(&gga->altitude);
  if (isfinite(alt)) {
    fix->alt_mm = (int32_t)(alt * 1000.0f + (alt >= 0.0f ? 0.5f : -0.5f));
  }

  /* GGA quality 是 SPS/DGPS，不是 2D/3D。无 GSA 时用海拔推断。 */
  if (fix->fix_quality < 1) {
    fix->fix_quality = isfinite(alt) ? 2 : 1;
  }
}

/** @brief 发话者 → 星座索引（`$GPGSV`/`$BDGSV`… 的第 3 个字符）；不认识返回 -1。
 *
 *  `$GN*`（多星座合并）故意不映射：本模组按星座分开出句子，合并的那些没有
 *  单星座归属，硬塞会给 UI 报错数。 */
static int gnss_talker_idx(const char *line)
{
  if (line == NULL || line[0] != '$' || line[2] == '\0')
    {
      return -1;
    }

  switch (line[2])
    {
      case 'P': return MYVENDOR_SYS_GNSS_CONST_GPS;
      case 'L': return MYVENDOR_SYS_GNSS_CONST_GLO;
      case 'A': return MYVENDOR_SYS_GNSS_CONST_GAL;
      /* 北斗**两个**发话者都要认：NMEA 4.10 用 `GB`（$GBGSV），老写法是 `BD`
       * （$BDGSV）。手机那份 Cellular-Z 抓的就是 `$GBGSV` —— 只认 'D' 会让
       * 普查与 UI 的北斗一栏永远是 0，哪怕模组在发。 */
      case 'D': return MYVENDOR_SYS_GNSS_CONST_BDS;
      case 'B': return MYVENDOR_SYS_GNSS_CONST_BDS;
      case 'Q': return MYVENDOR_SYS_GNSS_CONST_QZSS;
      default:  return -1;
    }
}

static void gnss_apply_gsa(myvendor_sys_gnss_t *fix,
                           const struct minmea_sentence_gsa *gsa,
                           const char *line)
{
  uint8_t q;
  uint8_t hx;
  int idx;
  int i;
  unsigned used = 0;

  q = gnss_quality_from_gsa(gsa->fix_type);
  if (q > fix->fix_quality) {
    fix->fix_quality = q;
  }

  hx = gnss_hdop_x10(&gsa->hdop);
  if (hx != 0) {
    fix->hdop_x10 = hx;
  }

  for (i = 0; i < 12; i++) {
    if (gsa->sats[i] > 0) {
      used++;
    }
  }

  if (used > fix->satellites) {
    fix->satellites = (uint8_t)used;
  }

  /* 按星座分别记"参与解算的星数"：UI 的卫星页要按星座分组显示，
   * 光有一个总数看不出是哪几路在贡献。 */
  idx = gnss_talker_idx(line);
  if (idx >= 0 && used <= 255u) {
    fix->sats_locked_c[idx] = (uint8_t)used;
  }
}

static void gnss_apply_gsv(myvendor_sys_gnss_t *fix,
                           const struct minmea_sentence_gsv *gsv,
                           const char *line)
{
  int i;
  int idx;

  /* 按**发话者**分：$GPGSV/$GLGSV/$GAGSV/$BDGSV/$GQGSV 一种星座一句 ——
   * 既是 UI 卫星页的数据源，也是确认"哪几个星座真在跑"的唯一证据
   * （本固件不回 UBX-CFG-VALGET，星座开关读不回来）。 */
  idx = gnss_talker_idx(line);
  if (idx >= 0)
    {
      if (g_gsv_talk[idx] < 255u)
        {
          g_gsv_talk[idx]++;
        }

      if (gsv->msg_nr == 1)
        {
          /* 本星座这一轮的第一句：**归零并记在视总数**。
           * `msg_nr==1` 那句带 total_sats，后面几句不含，不能重复累加；
           * 而"跟踪/锁定数"要按历元重数，所以在这里清零再逐颗累加。 */
          fix->sats_in_view_c[idx] = (uint8_t)gsv->total_sats;
          fix->sats_heard_c[idx] = 0;
        }
    }

  for (i = 0; i < 4; i++) {
    if (gsv->sats[i].nr > 0 && gsv->sats[i].snr > 0 && g_gsv_heard_acc < 255) {
      g_gsv_heard_acc++;

      if (idx >= 0 && fix->sats_heard_c[idx] < 255u)
        {
          fix->sats_heard_c[idx]++;
        }
    }
  }

  if (gsv->msg_nr == 1 && gsv->total_sats > 0) {
    unsigned v = (unsigned)g_gsv_view_acc + (unsigned)gsv->total_sats;

    g_gsv_view_acc = (uint8_t)(v > 255u ? 255u : v);
  }

  for (i = 0; i < 4; i++) {
    if (gsv->sats[i].nr > 0 && gsv->sats[i].snr > 0 && g_gsv_heard_acc < 255) {
      g_gsv_heard_acc++;
    }
  }

  if (g_gsv_view_acc > fix->sats_in_view) {
    fix->sats_in_view = g_gsv_view_acc;
  }

  if (g_gsv_heard_acc > 0) {
    fix->sats_heard = g_gsv_heard_acc;
  }
}

static void gnss_handle_line(myvendor_sys_gnss_t *acc, const char *line)
{
  enum minmea_sentence_id id = minmea_sentence_id(line, false);

  if (id == MINMEA_SENTENCE_RMC) {
    struct minmea_sentence_rmc rmc;

    if (minmea_parse_rmc(&rmc, line)) {
      gnss_apply_rmc(acc, &rmc);
      gnss_publish(acc);
    }

    return;
  }

  if (id == MINMEA_SENTENCE_GGA) {
    struct minmea_sentence_gga gga;

    if (minmea_parse_gga(&gga, line)) {
      gnss_apply_gga(acc, &gga);
      gnss_publish(acc);
    }

    return;
  }

  if (id == MINMEA_SENTENCE_GSA) {
    struct minmea_sentence_gsa gsa;

    if (minmea_parse_gsa(&gsa, line)) {
      gnss_apply_gsa(acc, &gsa, line);
      gnss_publish(acc);
    }

    return;
  }

  if (id == MINMEA_SENTENCE_GSV) {
    struct minmea_sentence_gsv gsv;

    if (minmea_parse_gsv(&gsv, line)) {
      gnss_apply_gsv(acc, &gsv, line);
      gnss_publish(acc);
    }

    return;
  }

  if (id == MINMEA_SENTENCE_ZDA) {
    struct minmea_sentence_zda zda;

    if (minmea_parse_zda(&zda, line)) {
      gnss_remember_date(&zda.date);
      gnss_try_set_utc(acc, &zda.date, &zda.time);
      gnss_publish(acc);
    }
  }
}

/** @brief UBX-NAV-PVT（0x01 0x07）载荷定长（protocol 27 / M10 都是 92 B）。 */
#define GNSS_PVT_LEN 92u

static int32_t gnss_pvt_i32(const uint8_t *p)
{
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t gnss_pvt_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief mm/s → centi-km/h，与 NMEA RMC 那条路同一个单位（`pub … spd=`）。 */
static long gnss_pvt_centi_kmh(int32_t mm_s)
{
  int32_t c = mm_s * 36;

  return (c >= 0) ? (long)((c + 50) / 100) : (long)((c - 50) / 100);
}

/**
 * @brief 把一帧 PVT 的关键字段存起来，等 `gnss_publish()` 贴进快照。
 *
 * @details
 * 与下面只负责打印的 `gnss_pvt_report()` 分开：**这条与日志开关无关**，
 * 没开 `CONFIG_MYVENDOR_GNSS_LOG` 时也要工作（快照要靠它）。
 * 载荷偏移（protocol 27）：fixType 20、flags 21、numSV 23、hAcc 40、
 * velN 48、velE 52、velD 56、gSpeed 60、sAcc 68、pDOP 76。
 */
static void gnss_pvt_store(const uint8_t *p, uint32_t now)
{
  int32_t gs = gnss_pvt_i32(p + 60);
  uint32_t sacc = gnss_pvt_u32(p + 68);
  long gsc;

  if (gs < 0) {
    gs = -gs;
  }

  if (sacc > 0xffffu) {
    sacc = 0xffffu;
  }

  gsc = gnss_pvt_centi_kmh(gs);
  if (gsc > 65535) {
    gsc = 65535;
  }

  g_pvt_last.ms = now;
  g_pvt_last.gspeed_centi_kmh = (uint16_t)gsc;
  g_pvt_last.sacc_mm_s = (uint16_t)sacc;
  /* hAcc 也带上：它是"位置（因此位移窗）可不可信"的唯一依据，
   * 见 bicycle_runtime.c 的两个源都不可信时的保持逻辑。 */
  g_pvt_last.hacc_mm = gnss_pvt_u32(p + 40);
  g_pvt_last.fix_type = p[20];
  g_pvt_last.flags = p[21];
}

/**
 * @brief 把最近一帧 PVT 贴到即将发布的 fix 上（没有就清成无效）。
 *
 * @details
 * 必须在 `gnss_publish()` 里、连同 `stamp_ms` 一起做：PVT 与 NMEA 是同一历元的
 * 两个来源，各 1 Hz，贴在一起消费方才能判断"这份速度带不带精度信息"。
 * 每帧 PVT 单独记录到达时刻，是因为串口静默/灌星历窗口里 NMEA 可能来自旧快照，
 * 不能让一份两秒前的 PVT 看起来还新鲜。
 */
static void gnss_pvt_attach(myvendor_sys_gnss_t *fix, uint32_t now)
{
  bool fresh = (g_pvt_last.ms != 0 &&
                gnss_elapsed_ms(now, g_pvt_last.ms) <= GNSS_PVT_FRESH_MS);

  fix->pvt_valid = fresh;
  fix->pvt_gnss_ok = fresh && ((g_pvt_last.flags & 0x01u) != 0);
  fix->pvt_fix_type = fresh ? g_pvt_last.fix_type : 0;
  fix->speed_pvt_centi_kmh = fresh ? g_pvt_last.gspeed_centi_kmh : 0;
  fix->speed_acc_mm_s = fresh ? g_pvt_last.sacc_mm_s : 0;
  fix->pos_acc_mm = fresh ? g_pvt_last.hacc_mm : 0;
}

#ifdef CONFIG_MYVENDOR_GNSS_LOG

/**
 * @brief 一帧 UBX-NAV-PVT 的关键字段打一行（探针：只读，**不进快照/UI**）。
 *
 * @details
 * 目的就一个：拿到 `gSpeed`/`sAcc` 的**现场数字**，跟同一个历元的 NMEA SOG
 * （`pub … spd=`）对着看，定 `sAcc` 门的阈值。
 *
 * 单位：`gs`/`vn`/`ve` 是 centi-km/h（与 `pub … spd=` 同单位，便于直接比）；
 * `sacc` mm/s、`hacc` mm、`pdop` 0.01 —— 加速度那几项保持 u-blox 原始单位，
 * 不换算，免得对不上文档。载荷偏移（protocol 27）：fixType 20、flags 21、
 * numSV 23、hAcc 40、velN 48、velE 52、velD 56、gSpeed 60、sAcc 68、pDOP 76。
 */
static void gnss_pvt_report(const uint8_t *p)
{
  if (GNSS_TRACE_ON)
    syslog(LOG_DEBUG,
           "gnss: pvt fix=%u gnss=%u sv=%u gs=%ld vn=%ld ve=%ld "
         "sacc=%u hacc=%u pdop=%u\n",
         (unsigned)p[20], (unsigned)(p[21] & 0x01u), (unsigned)p[23],
         gnss_pvt_centi_kmh(gnss_pvt_i32(p + 60)),
         gnss_pvt_centi_kmh(gnss_pvt_i32(p + 48)),
         gnss_pvt_centi_kmh(gnss_pvt_i32(p + 52)),
         (unsigned)gnss_pvt_u32(p + 68), (unsigned)gnss_pvt_u32(p + 40),
         (unsigned)((uint16_t)p[76] | ((uint16_t)p[77] << 8)));
}

/** @brief 首帧十六进制原文，给"解出来的值对不对"留个对照。 */
static void gnss_pvt_dump(const uint8_t *p, uint16_t n)
{
  char buf[80];
  uint16_t i;
  int k = 0;

  for (i = 0; i < n; i++) {
    k += snprintf(buf + k, sizeof(buf) - (size_t)k, "%02x", p[i]);
    if (k >= 62 || (uint16_t)(i + 1u) == n) {
      syslog(LOG_INFO, "gnss: pvt raw %s\n", buf);
      k = 0;
    }
  }
}
#endif /* CONFIG_MYVENDOR_GNSS_LOG */

static void *gnss_thread(void *arg)
{
  myvendor_sys_gnss_t acc;
  char line[GNSS_LINE_MAX];
  char rx[GNSS_RX_BATCH];
  uint8_t pvt_pay[GNSS_PVT_LEN];   /* 只给 PVT 解码/入库用（见 rx_st==4） */
  size_t cnt = 0;
  bool discard = false;
  /* restart 路径会留下仍 open 的 USART2（避免 uart_reset_sem）；继承它。 */
  int fd = g_uart_fd;
  int rxn = 0;
  int rxi = 0;
  uint8_t rx_st = 0;
  uint8_t ubx_hdr[4];
  uint8_t ubx_hdr_i = 0;
  uint16_t ubx_len = 0;
  uint16_t ubx_i = 0;
  uint8_t ubx_ck = 0;
  uint8_t ubx_got_cka = 0;
  uint8_t ubx_cka = 0;      /* 探针用：整帧校验和（含 cls/id/len + payload） */
  uint8_t ubx_ckb = 0;
  uint32_t last_nmea_ms;
  bool had_valid = false;
  uint32_t first_fix_ms = 0;
  bool pkt_open = false;

  /* pollfd 现在是**按线程**取的（`gnss_uart_pfd()`），不再需要用参数传槽号；
   * 线程身份本身就把 pollfd 隔离开了，也就没有"两个读者共用一份"这件事。 */
  (void)arg;

  memset(&acc, 0, sizeof(acc));
  g_boot_ms = gnss_now_ms();
  last_nmea_ms = g_boot_ms;
  g_nmea_life_ms = last_nmea_ms;
  /* 取证：入口跑过的第一条证据（见 g_rd_enter_ms 的注释）。放在 pid 之前，
   * 这样"入口没跑"和"pid 没写进去"就是两件可区分的事。 */
  g_rd_enter_ms = g_boot_ms;
  g_rd_loop_seq = 0;
#ifndef CONFIG_DISABLE_PTHREAD
  g_rd_self = (unsigned long)pthread_self();
#endif
  /* **必须是 gettid()，不是 getpid()。**
   *
   * `getpid()` 在 NuttX 返回的是 **task group 的 pid**（`task_getpid.c` →
   * `task_get_info()->ta_pid`），而本线程的 group 恰好是 0：读线程是从 bringup
   * 上下文建出来的，`ps` 里那行 `9 0 50 RR … gnss` 的 GROUP 列就是 0。
   * 所以它一直读到 0 —— 源码里确实没有任何地方写 0，这个值从第一行起就是 0；
   * 之前"谁把它覆盖了"的疑问到此为止。
   *
   * 代价是**恢复路径上一切按 pid 走的判据全部失灵**，其中最贵的一条是
   * `gnss_diag_why()` 的 TCB 现场：nxsched_get_tcb(0) 只能拿到 IDLE 的 TCB，
   * 那里为此特意 guard 了 `> 0`，于是**最需要那行 st/lock/wait/wd 的时候它是缺的**
   * （2026-09-17 那次读线程卡死在 poll 里，日志只有 `diag tcb unknown (pid=0/0)`，
   * 而 wd 是否 armed 正是区分"真在 nxsem_wait_slow"还是"卡在驱动里"的唯一字段）。
   *
   * gettid() 给的是线程自己的 pid（ps / schedmon 里那个 9），正是这里要的东西。 */
  g_gnss_pid = gettid();
  /* 另存一份：万一入口这次没写进去，两个变量分开记才分得清"没写"和"被覆盖"。 */
  g_gnss_pid_boot = g_gnss_pid;
  GNSS_PHASE("thread-enter");

  /* 新读线程不继承上一个线程留下的"正在注入星历"。
   *
   * 注入状态是**模块级静态量**（`g_eph_busy`/`g_eph_pending`/grace），旧线程若在
   * 注入中途退出，它没机会清；新线程起来后 `gnss_eph_run()` 会被这几位置挡住
   * （门口条件里有 `g_eph_busy`），于是"待注入"永远注入不了、UI 也一直显示
   * "星历同步"。这里在入口清干净，语义是"新线程眼里没有任何注入在飞"。
   *
   * 注：**卡死**的旧线程收不回来时根本 spawn 不了新线程（`g_gnss_stack_busy`），
   * 那种场景靠消费侧 `myvendor_sys_gnss_eph_busy()` 的健康裁剪保证屏幕不说谎。 */
  pthread_mutex_lock(&g_lock);
  g_eph_busy     = false;
  g_eph_pending  = false;
  g_eph_grace_ms = 0;
  pthread_mutex_unlock(&g_lock);

  /* 禁止本线程被 pthread_cancel 就地撕掉。
   *
   * 它会持有 LittleFS 占用（gnss_eph_fs_begin）和 UART 驱动内部锁，从
   * cancellation point 上被撕掉会留下"持锁的尸体"（文件头对此有明确禁令）。
   * 退出只走 `g_gnss_stop` 的优雅路径 —— 而"一定能回到循环顶"以前靠的是读
   * 等待的有界超时，那个超时由 tick 驱动，计时基准一被打断就永远走不到。
   * 现在改由 `gnss_reader_wake()` 发的信号保证（不经过 tick），见该函数注释。
   *
   * 这一行是必需的而不是保险：`poll()` 本身就是 cancellation point
   * （nuttx/fs/vfs/fs_poll.c 的 `enter_cancellation_point()`），reap 那次
   * cancel 就真的能撕到它。 */
#ifndef CONFIG_DISABLE_PTHREAD
  (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
#endif

  /* 装唤醒信号，让 reap / diag kick 能把本线程从阻塞的 poll() 里叫回来。
   *
   * `sa_flags = 0` 是重点：**故意不设 SA_RESTART**。设了的话
   * nxsem_tickwait()/nxsem_wait() 会自己重启等待，信号就白发一场。
   * 要的就是它返回 -EINTR，把控制权交回主循环。
   *
   * 信号掩码是从父线程继承的，不能假定它是开的，所以显式解阻塞。 */
  {
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gnss_wake_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(GNSS_WAKE_SIG, &sa, NULL);

    sigemptyset(&set);
    sigaddset(&set, GNSS_WAKE_SIG);
    (void)pthread_sigmask(SIG_UNBLOCK, &set, NULL);
  }

  /* 读等待的计时基准换成 DWT 周期计数器（不依赖 tick）。幂等，可重复调。 */
  gnss_dwt_init_once();

  g_gnss_stop = false;
  g_idle_parked = false;
  gnss_recover_begin();
  gnss_heartbeat();
#ifndef CONFIG_DISABLE_PTHREAD
  pthread_setname_np(pthread_self(), "gnss");
#endif

  /* 把"我还在动"交给 diag 的时基盯着（见 myvendor_diag.h）：
   * poll() 的超时看门狗一旦被 wd 链剪掉，200 ms 超时永不到期，只有信号能把
   * 那一次 poll 打断 —— 有了这一条，卡住最多持续一个 stall 窗口（1.5 s），
   * 由 diag 踢一脚即可自愈，不必等 8 s/12 s 那条线程重启（那要断电重启模组）。 */
  (void)myvendor_diag_watch("gnss-reader", pthread_self(), GNSS_WAKE_SIG,
                            MYVENDOR_DIAG_WATCH_MS_DEFAULT,
                            gnss_reader_progress_ms);

  /* 诊断：新会话起点，并清掉上一会话的活跃度统计。 */
  g_rd_session_ms = gnss_now_ms();
  g_rd_last_ok_ms = 0;
  g_rd_last_loop_ms = 0;
  g_rd_gap_max_ms = 0;
  g_rd_loop_max_ms = 0;
  g_rd_stall_1s = 0;
  g_rd_poll_close = 0;
  g_pvt_first_done = false;
  syslog(LOG_WARNING, "gnss: reader enter\n");

  for (;;) {
    char ch;
    int rc;
    uint8_t b;
    uint32_t now;
    uint32_t silent;

    /* 诊断：主循环相邻两轮的间隔。慢循环会让接收环溢出。 */
    {
      uint32_t lp_now = gnss_now_ms();

      if (g_rd_last_loop_ms != 0) {
        uint32_t lp = gnss_elapsed_ms(lp_now, g_rd_last_loop_ms);

        if (lp > g_rd_loop_max_ms) {
          g_rd_loop_max_ms = lp;
        }
      }

      g_rd_last_loop_ms = lp_now;
    }

    /* 取证：循环在推进。它停止增长而 phase 仍停在某个值上，就说明卡在那一处。
     * 这一行**保留在每轮**：本循环一轮只解析**一个字节**（下面 `rx[rxi++]`），
     * 所以它就是"字节迭代次数"，是判断"卡在哪一步"最细的粒度，而且只有一个
     * 存储动作、代价可忽略。 */
    g_rd_loop_seq++;

    /* 以下两件（心跳 + 时间记账）是**纯诊断**，而且是"每字节做一次没意义"的
     * 开销：本循环约 1000 轮/秒（1 KB/s ÷ 1 字节/轮），每轮一次 DWT 读 +
     * 算术是白花的。改成**只在"本轮要去读"时做**，也就是每个读周期一次
     * （约 45 次/秒），心跳按这个频率喂远高于 diag 的 8 秒判据，够用。
     *
     * 刻意**只门控这两件纯诊断**：`g_gnss_stop` 检查、`fd < 0` 打开路径、
     * 以及解析状态机一律不动 —— 循环结构与语义保持原样，这是最稳妥的改法。 */
    if (rxi >= rxn)
      {
        gnss_heartbeat();
        GNSS_PHASE("loop");
        {
          uint32_t t_now = gnss_now_ms();

          if (g_rd_prev_loop_ms != 0)
            {
              uint32_t dt = gnss_elapsed_ms(t_now, g_rd_prev_loop_ms);

              /* 异常 dt 只计数不累加。
               *
               * Σdt 在数学上恰好等于窗口长度（每轮都在顶部刷新 prev），所以
               * `g_rd_work_ms` **不可能**超过窗口 —— 一旦超过，说明这个累计器
               * 的假设被打破了。最可能的情形是**同时有两个读循环在跑**：
               * 两者共享 `g_rd_prev_loop_ms`，于是各自读到对方更早的时间戳，
               * 算出巨大的 dt。现场（改这个门控之前）确实出现过
               * `busy=16133 > win=5087`、`busy_pm=3171`。
               * 加这道闸以后，`dthuge` 增长就等于"假设被打破"的直接证据，
               * 而 busy/busy_pm 至少不会再被污染到不可读。 */
              if (dt > GNSS_WORK_DT_MAX_MS)
                {
                  g_rd_dt_huge++;
                }
              else if (dt > g_poll_last_ms)
                {
                  g_rd_work_ms += dt - g_poll_last_ms;
                }
            }

          g_rd_prev_loop_ms = t_now ? t_now : 1u;
        }
      }

    if (g_gnss_stop) {
      GNSS_PHASE("stopping");
      gnss_fd_release(&fd);
      break;
    }

    /** ── 电源 / 串口时序：**PX4 式 open / close**（2026-09-23 改）────────────
     *
     *  原做法是"先 open，之后一直 poll；低功耗只切模组 VCC"：休眠时把 fd
     *  **抱着**、让读线程继续 `poll()` 一个**已经断电的 UART**，指望唤醒后那个
     *  poll 自己回来。现场（`202609231720` / `202609231852`）就是它不回来：
     *  `phase=read-poll`、`in_poll=106835 ms`、`shutdown_n=0`（整个上电周期
     *  **一次 close 都没发生过**），线程被"计时基准坏掉后不再到期的 poll"钉死
     *  ⇒ 心跳停 ⇒ 判僵尸 ⇒ 只能重启救。
     *
     *  改成 PX4 的 GPS 时序：**一个线程拥有 fd；要停就先退出读循环、再 close；
     *  要开就重新 open**。close 只发生在**循环边界**（这一刻没有 poll 在途），
     *  所以不会撞上 `uart_close()` 最后一次 close 干的那三件事
     *  （`uart_detach` 摘中断 / `uart_shutdown` 关 UART / `uart_reset_sem`
     *  reset `recv.lock`）—— 那正是 `gnss_fd_release()` 注释里
     *  "运行中 close 会把 recv.lock 搞坏"所禁的情形，**循环边界的 close
     *  不在此列**（PX4 也是这么做的）。
     *
     *  顺序：**先停读 → 再 close → 最后断电**；唤醒反过来
     *  **先上电 → 稳定 → 再 open**（由下面 `fd < 0` 那一段落，保持单一入口）。
     */
    if (g_idle_sleep) {
      if (fd >= 0) {
        gnss_module_park();       /* 断 VCC + 释放 pin（不动 fd） */
        gnss_fd_release(&fd);     /* ← 循环边界 close：此刻没有 poll 在途 */
        g_idle_parked = true;
        /* **不清 g_eph_injected**：模块有备份电源，park 只关 VCC、不丢星历，
         * 唤醒后没必要重灌（重灌 = 读 9.8 KB LFS + 106 帧 UBX，而且正好打断
         * 它刚开始的定位）。真要重灌由"换了源文件"或"模块时间不正常"触发。 */
        g_eph_wake_defer_ms = gnss_now_ms();
        /* park 是自家窗口：唤醒后那条 `reader gap N s` 不该当告警
         * （不灌星历的那次没人设这个标记，于是它按丢字节报了）。 */
        g_rd_selfblock_ms = gnss_now_ms();
        syslog(LOG_INFO, "gnss: idle sleep, " GNSS_MOD_PN
               " VCC off PA43, fd closed (PX4-style open/close)\n");
      }

      /* 已断电、fd 已关：**只等唤醒，不碰串口、不 poll**。
       * 这里必须真睡一下 —— 主循环是"一轮一个字节"的紧循环，不睡会空转烧满 CPU。 */
      gnss_sleep_ms(GNSS_IDLE_NAP_MS);

      /** **休眠期照样喂心跳。**
       *
       *  diag 的 kick 判据是"进度源（= 本心跳）多久没动"（`myvendor_diag.c`
       *  的 `kick = !moved && !held && age >= stall_ms`）。休眠时读线程**按设计
       *  什么都不做**，心跳自然不动 ⇒ 每 2 s 被 kick 一次（现场 `202609231911`
       *  三轮休眠里 `kick gnss-reader` 5 次）。而 kick 会把 `g_diag_kick` 置上，
       *  唤醒后立刻被消费成一次**多余的 VCC recover**（`PX4-style open` 计数多出来
       *  的那几次），等于每次唤醒都白穿插一个 3.5 s 的断电重来，正好打断模块
       *  刚恢复的定位。
       *
       *  走 `myvendor_diag_watch_hold()` 那条路不行：它的上限是
       *  `DIAG_WATCH_HOLD_MAX_MS`（10 s），长睡必然过期。而心跳的语义本来就是
       *  "这个线程在正常运转"——休眠是它**该做**的事，所以这里喂心跳不是掩盖，
       *  是把"活着且在睡"如实报给 diag。 */
      gnss_heartbeat();
      continue;
    }

    if (g_idle_parked) {
      unsigned on_ms = (unsigned)CONFIG_BOARD_L96_GNSS_PWR_DELAY_MS;

      g_idle_parked = false;
      syslog(LOG_INFO, "gnss: idle wake, " GNSS_MOD_PN
             " VCC on PA43 (settle %u ms, then open)\n", on_ms);
      GNSS_LOG("idle wake hold=%d eph_inj=%d fails=%u fd=%d",
               g_wake_hold_ms != 0 ? 1 : 0,
               g_eph_injected ? 1 : 0,
               (unsigned)g_uart_fails, fd);
      sf32lb52_l96_uart_pins();
      (void)sf32lb52_l96_power(true);
      gnss_sleep_ms(on_ms);
      g_mod_pwr = true;
      /* 不在这里 open：落到下面的 `fd < 0` 路径开，保持单一入口。 */
    }

    if (fd < 0) {
      bool cycle = false;

      /* 整段"上电 / 等模块 / 开串口"都是已知在等：开机第一次就要 2 s 以上
       * （实测 2.19 s→3.73 s），期间读线程不更新心跳。对 diag 时基明说"我在忙"，
       * 否则会被踢一脚 —— 而 `gnss_sleep_ms()` 会按 EINTR 提前返回，模组的上电
       * 稳定时间就不够了。两个出口各自 release（见下面两处）。 */
      myvendor_diag_watch_hold(pthread_self(), true);

      cycle = (g_uart_fails >= GNSS_PWR_FAILS);
      gnss_module_ensure();
      if (cycle) {
        gnss_module_power_cycle();
        g_uart_fails = 0;
      } else {
        sf32lb52_l96_uart_pins();
      }

      fd = gnss_open();
      g_uart_fd = fd;
      if (fd < 0) {
        g_uart_fails++;
        syslog(LOG_WARNING, "gnss: open %s failed errno=%d fail=%u\n",
               CONFIG_BOARD_L96_GNSS_DEVPATH, errno,
               (unsigned)g_uart_fails);
        gnss_sleep_ms(GNSS_RETRY_MS);
        myvendor_diag_watch_hold(pthread_self(), false);  /* 出口一：这一轮失败 */
        continue;
      }

      myvendor_diag_watch_hold(pthread_self(), false);    /* 出口二：串口已开 */
      gnss_rt_mark("uart-open");
      gnss_reconfig_after_vcc(fd, cycle);

      syslog(LOG_INFO, "gnss: open %s @ %d%s (" GNSS_MOD_PN
             ", PX4-style open/close)\n",
             CONFIG_BOARD_L96_GNSS_DEVPATH, CONFIG_BOARD_L96_GNSS_BAUD,
             cycle ? " (pwr)" : "");
      goto rx_fresh;
    }

    if (g_diag_kick) {
      g_diag_kick = false;
      if (!g_idle_parked) {
        gnss_recover_begin();
        syslog(LOG_WARNING,
               "gnss: diag kick, " GNSS_MOD_PN " VCC cycle (UART open)\n");
        if ((g_uart_fails + 1u) >= GNSS_PWR_FAILS) {
          g_eph_hold = true;
        }

        gnss_vcc_recover(fd, true, "diag-kick");
        goto rx_fresh;
      }
    }

    goto rx_run;

rx_fresh:
    cnt = 0;
    discard = false;
    rx_st = 0;
    rxn = 0;
    rxi = 0;
    pkt_open = false;
    ubx_hdr_i = 0;
    ubx_len = 0;
    ubx_i = 0;
    ubx_ck = 0;
    g_house_ms = 0;          /* 新会话（开机/VCC 恢复/唤醒）立刻跑一次 housekeeping */
    last_nmea_ms = gnss_now_ms();
    g_nmea_life_ms = last_nmea_ms;

rx_run:
    /* 结构闸：这一段从"每收一个字节跑一遍"改成按 GNSS_HOUSEKEEP_MS 的节奏跑。
     * 见该常量的注释 —— 它同时消掉"漏限频就刷屏"这一类问题。 */
    if (!g_idle_parked &&
        (g_house_ms == 0 ||
         gnss_elapsed_ms(gnss_now_ms(), g_house_ms) >= GNSS_HOUSEKEEP_MS)) {
      g_house_ms = gnss_now_ms();
      /* 这一段里 gnss_eph_boot_try 会进 LittleFS（open/read/目录扫描），
       * gnss_dyn_apply 会写串口 —— 都是可能长时间阻塞的地方，所以逐个打点。 */
      GNSS_PHASE("eph-boot");
      gnss_eph_boot_try(fd);
      /* 手机位置一刷新就补发一次位置辅助（室内起步最有用的一条），见该函数。 */
      gnss_assist_tick(fd);
      /* 星座实验（ctl gnss bdsonly|b1c|ver），见 gnss_probe_tick。 */
      gnss_probe_tick(fd);
      GNSS_PHASE("dbd");
      gnss_dbd_try_start(fd, had_valid, first_fix_ms);
      gnss_dbd_tick();
      GNSS_PHASE("dyn");
      gnss_dyn_apply(fd, false);

      now = gnss_now_ms();
      last_nmea_ms = g_nmea_life_ms;
      silent = gnss_elapsed_ms(now, last_nmea_ms);
      GNSS_PHASE("report");
      gnss_link_report(fd, rx_st, silent);
      GNSS_PHASE("silence-check");
      {
        uint32_t quiet_lim = GNSS_SILENCE_MS;

        if (gnss_hold_unlocked(now) || g_eph_busy || g_dbd_run) {
          quiet_lim = GNSS_EPH_GRACE_MS;
        } else {
          pthread_mutex_lock(&g_lock);
          if (gnss_eph_grace_locked(now)) {
            quiet_lim = GNSS_EPH_GRACE_MS;
          }

          pthread_mutex_unlock(&g_lock);
        }

        if (silent > quiet_lim) {
          /* 诊断：把判定输入原样打出来，区分"模组真静默"与"解析器没产出
           * publish"。窗口计数是 g_link 的当前值（本窗口内的量）。 */
          syslog(LOG_WARNING,
                 "gnss: silence trig silent=%u lim=%u hold=%d eph=%d dbd=%d "
                 "grace=%d kick=%d fail=%u sess=%u gap_max=%u stall1s=%u "
                 "win_byte=%u win_nmea=%u win_bad=%u win_ubx=%u win_pvt=%u "
                 "win_junk=%u win_rd=%u win_poll_to=%u win_pub=%u\n",
                 (unsigned)silent, (unsigned)quiet_lim,
                 gnss_hold_unlocked(now) ? 1 : 0, g_eph_busy ? 1 : 0,
                 g_dbd_run ? 1 : 0,
                 (g_eph_grace_ms != 0 &&
                  gnss_elapsed_ms(now, g_eph_grace_ms) < GNSS_EPH_GRACE_MS) ? 1 : 0,
                 g_diag_kick ? 1 : 0, (unsigned)g_uart_fails,
                 g_rd_session_ms != 0
                   ? (unsigned)gnss_elapsed_ms(now, g_rd_session_ms) : 0u,
                 (unsigned)g_rd_gap_max_ms, (unsigned)g_rd_stall_1s,
                 (unsigned)g_link.byte, (unsigned)g_link.nmea,
                 (unsigned)g_link.nmea_bad, (unsigned)g_link.ubx,
                 (unsigned)g_link.pvt, (unsigned)g_link.junk,
                 (unsigned)g_link.rd,
                 (unsigned)g_link.poll_to, (unsigned)g_link.pub);

          if (g_dbd_run) {
            gnss_dbd_abort();
          }

          if ((g_uart_fails + 1u) >= GNSS_PWR_FAILS) {
            g_eph_hold = true;
          }

          syslog(LOG_WARNING,
                 "gnss: no publish for %u ms, " GNSS_MOD_PN
                 " VCC cycle (fail=%u, UART open)\n",
                 (unsigned)silent, (unsigned)(g_uart_fails + 1u));
          GNSS_LOG("vcc silence quiet_lim=%u eph=%d dbd=%d recov=%d",
                   (unsigned)quiet_lim, g_eph_busy ? 1 : 0,
                   g_dbd_run ? 1 : 0, g_recovering ? 1 : 0);
          gnss_vcc_recover(fd, false, "silence");
          goto rx_fresh;
        }
      }
    }

    if (rxi >= rxn) {
      GNSS_PHASE("uart-read");
      /* 主读循环：-1 = 一直等（有数据才返回；周期记账由 diag 的 SIGUSR1 提供）。 */
      rc = gnss_read_some(fd, rx, GNSS_RX_BATCH, -1);
      if (rc < 0) {
        if (g_idle_parked) {
          GNSS_LOG("idle uart read %d errno=%d (USART2 hold)", rc, errno);
          continue;
        }

        syslog(LOG_WARNING,
               "gnss: uart read %d, " GNSS_MOD_PN " VCC cycle (fail=%u)\n",
               rc, (unsigned)(g_uart_fails + 1u));
        GNSS_LOG("vcc read rc=%d errno=%d poll_run=%u",
                 rc, errno, (unsigned)g_poll_to_run);
        gnss_vcc_recover(fd, g_diag_kick, "read-err");
        goto rx_fresh;
      }

      if (rc == 0) {
        g_rd_poll_close++;

        /* ① 空读退避：`poll()` 报"可读"但 `read()` 拿不到数据时，原来的路径是
         * **不睡直接回到循环顶**，于是又立刻 poll 一次 —— 实测这就是那个
         * ~1050 次/秒 的空转（`loop≈5300/5 s` 而真正读到数据的只有 ~9 次/秒），
         * 也就是读线程 4~17% CPU 的全部来源。原因在驱动侧：poll 会因 bounce
         * 缓存报可读，而 uart_read 此时拿不到东西；两侧对"可读"的判断不一致。
         *
         * 这里做**指数退避**：连续空读越多，让出越久，一旦真读到数据立刻归零。
         * 退避步长受 `usleep` 的时基粒度限制（100 Hz → 1 tick = 10 ms），
         * 所以档位是 0/10/20/40/80 ms，最多给 NMEA 突发加约 80 ms 延迟 ——
         * 相对 1 Hz 的报文周期可以接受，而 CPU 从 ~15% 掉到 ~1%。
         * 驱动侧的真正修法（让 poll 与 read 一致）是另一件事，这里先止血。 */
        /* 空转期间必须给看门狗一个"驱动还活着"的信号。
         *
         * 退避用的是 usleep()，而 poll 超时与 usleep **都走 tick** —— 计时基准一被打断
         * （本平台已多次记录），两者会一起退化成"立刻返回"，这里就成了上面说的那种
         * ~1.4 kHz 紧自旋（现场 `poll_n=26267 in_poll=19198 ms`）。
         * 读线程 pri=50：高于 IDLE(0) ⇒ 饿死 idle 心跳；低于 UI(100) ⇒ UI 照常 pet；
         * 而这条路径原先一个 pet 都不发 ⇒ work 心跳一起停。于是 `wdt_should_feed()`
         * 的 `cpu_ok` 恒假、喂狗被否决、15 s 后硬件复位 —— n576/n577/n578 那三份
         * `kind=wdt` 就是这个签名（idle 10.6~12.1 s、work 10.6~11.8 s、ui 10~40 ms、cfsr=0）。
         *
         * 所以这里补上 work_beat，**不是放宽喂狗条件**：work_beat 的语义正是"某驱动在
         * 忙等但系统还活着"（同文件 dbd 导出循环就是这么写的）。整机真卡死时没人 pet，
         * 照样复位；读线程自己另有 diag 的 `stall=1500 ms` 看门狗负责重启。 */
        myvendor_watchdog_work_beat();

        if (g_rd_empty_run < UINT32_MAX) {
          g_rd_empty_run++;
        }
        if (g_rd_empty_run >= GNSS_EMPTY_BACKOFF_AFTER) {
          if (g_rd_backoff_n < GNSS_EMPTY_BACKOFF_MAX) {
            g_rd_backoff_n++;
          }
        }
        /* 退避只加在**空转**那一类上：poll 正常超时（≈200 ms）本身就已经起到了
         * 节奏作用，再补一刀会把记账节奏变成 280 ms。判据就用"上一次 poll 是不是
         * 很快就返回了" —— 快 = 没起到 pacing 作用 = 这才是要退避的情况。 */
        if (g_rd_backoff_n != 0 && g_poll_last_ms < GNSS_EMPTY_BACKOFF_FAST_MS &&
            !g_idle_parked) {
          usleep(GNSS_EMPTY_BACKOFF_STEP_MS * g_rd_backoff_n);
        }

        if (!g_idle_parked) {
          pkt_open = false;
          gnss_dbd_tick();
          if (gnss_eph_try_reload(fd)) {
            rx_st = 0;
            cnt = 0;
            discard = false;
            last_nmea_ms = gnss_now_ms();
            g_nmea_life_ms = last_nmea_ms;
          }
        }

        continue;
      }

      /* 诊断：两次成功 read 之间的间隔。超过 GNSS_RD_GAP_WARN_MS 才报 ——
       * 阈值必须大于模组输出周期（见该常量的注释），否则 1 Hz 模组每个周期
       * 都刷一次。真要判断有没有丢字节看 `bad`/`junk`/`wr_fail`，不是看这里。 */
      {
        uint32_t rd_now = gnss_now_ms();

        if (g_rd_last_ok_ms != 0) {
          uint32_t gap = gnss_elapsed_ms(rd_now, g_rd_last_ok_ms);

          if (gap > g_rd_gap_max_ms) {
            g_rd_gap_max_ms = gap;
          }

          if (gap >= GNSS_RD_GAP_WARN_MS) {
            bool self = false;

            g_rd_stall_1s++;

            /* 星历注入 / 休眠 park / DBD 回放这些窗口里，是**我们自己**把读线程
             * 按住的：那段间隔是预期行为（注入那笔由 `eph done … cost=` 报），
             * 不是"读线程卡住"的前兆。真丢没丢字节看 bad/junk/wr_fail，不看这里
             * —— 拿它当告警反而会把真正的意外间隔（那才是卡死前兆）淹掉。
             * 计数与 g_rd_gap_max_ms 都照旧，只是在自家窗口里不吭声。
             *
             * 判据两条：① 窗口标记（窗口收尾置、这里消费一次，不依赖判定时那些
             * 瞬时标志还在不在，见 g_rd_selfblock_ms 的说明）；② 判定时刻的 hold
             * 仍然亮着（park/回放那类窗口期间就还亮着）。 */
            if (g_rd_selfblock_ms != 0) {
              g_rd_selfblock_ms = 0;
              self = true;
            } else if (gnss_hold_locked(rd_now)) {
              self = true;
            }

            if (!self) {
              /* 报**实际生效**的那个环与它的丢字节计数（2026-09-24 更正）。
               *
               * 原先这条打的是 `rx buf %u B` = `CONFIG_UART_BUFSZ`(1536) + 硬编码
               * `~800 B/s` —— 那是**旧 NuttX 串口环**的常量；数据面切到
               * `myvendor_gnss_uart` 的 8 KB 直连环之后，1536 与它一点关系都没有，
               * 反而会让人以为"GNSS 缓存不够"（现场就这么被误导过一次）。
               * 判"丢没丢字节"只看 `drop`：它 >0 会另有 ERROR 直报。
               *
               * 再加两个**自上次报以来的增量**（`rx+/feed+`）—— 下一次出现时
               * 它能直接指认是哪一侧静默：
               *   · `feed+0`（ISR 一次都没喂）⇒ **模组/中断侧**没数据（RF 或模组忙）；
               *   · `feed+N` 而 `rx+0`  ⇒ 数据进来了、读者没用上 ⇒ 主机侧交接问题。
               * 两种原因的修法完全不同，先分叉再动手。 */
              static uint32_t s_gap_rx_seen;
              static uint32_t s_gap_feed_seen;
              struct myvendor_gnss_uart_stats st;

              myvendor_gnss_uart_stats(&st);
              syslog(LOG_WARNING,
                   "gnss: reader gap %u ms (direct ring %u B, drop=%lu wait=%lu, since-last rx+%lu feed+%lu)\n",
                   (unsigned)gap, (unsigned)MYVENDOR_GNSS_UART_RING,
                   (unsigned long)st.rx_drop, (unsigned long)st.wait_n,
                   (unsigned long)(st.rx_bytes - s_gap_rx_seen),
                   (unsigned long)(st.feed_n - s_gap_feed_seen));
              s_gap_rx_seen = st.rx_bytes;
              s_gap_feed_seen = st.feed_n;
            } else if (!g_rd_selfblock_logged) {
              /* 只报一次（开机后第一条）：确认"是自家窗口"这件事，同时把第一次
               * 被吞掉的间隔数字留下来，免得后面没人知道它有多长。 */
              g_rd_selfblock_logged = true;
              syslog(LOG_INFO,
                     "gnss: reader gap %u ms suppressed (self eph/park window)\n",
                     (unsigned)gap);
            }
          }
        }

        g_rd_last_ok_ms = rd_now;
      }

      rxn = rc;
      rxi = 0;
      if (g_idle_parked) {
        /* 模组 VCC 已关：丢掉浮空 RX，UART 仍按 NSH 方式一直收。 */
        rxi = rxn;
        continue;
      }

      if (!pkt_open) {
        pkt_open = true;
        pthread_mutex_lock(&g_lock);
        gnss_rx_note(gnss_now_ms());
        pthread_mutex_unlock(&g_lock);
      }
    }

    ch = rx[rxi++];

    b = (uint8_t)ch;

    /* 0 idle  1 nmea  2 ubx-wait-62  3 ubx-hdr  4 ubx-pay  5 ubx-ck */
    if (rx_st == 0) {
      if (ch == '$') {
        rx_st = 1;
        cnt = 0;
        discard = false;
        line[cnt++] = ch;
      } else if (b == 0xb5) {
        rx_st = 2;
      } else {
        gnss_link_add(&g_link.junk);
      }

      continue;
    }

    if (rx_st == 1) {
      if (ch == '\r') {
        continue;
      }

      if (ch == '\n') {
        /* 只有 gnss_publish 才算活着。任意 $ 行（坏校验 / TXT）若在
         * 这里清 fail，垃圾句会掐死断电恢复，UI 就一直「无数据」。 */
        if (discard) {
          gnss_link_add(&g_link.nmea_bad);
        } else if (cnt > 0) {
          line[cnt] = '\0';
          gnss_link_add(&g_link.nmea);
          if (!g_link_got_nmea) {
            g_link_got_nmea = true;
            syslog(LOG_INFO, "gnss: first nmea %.8s\n", line);
          }

          /* 诊断：恢复后第一条 NMEA 是"重新收数"的里程碑。 */
          if (g_rt_wait_nmea) {
            g_rt_wait_nmea = false;
            g_rt_nmea_ms = gnss_now_ms();
            gnss_rt_mark("first-nmea");
          }

          gnss_handle_line(&acc, line);
          if (acc.valid) {
            if (!had_valid) {
              first_fix_ms = gnss_now_ms();
              if (!g_link_got_fix) {
                g_link_got_fix = true;
                syslog(LOG_INFO, "gnss: first fix sats=%u q=%u\n",
                       (unsigned)acc.satellites,
                       (unsigned)acc.fix_quality);
              }

              /* 诊断：恢复后首个有效定位，本轮 episode 到此结束。 */
              if (g_rt_wait_fix) {
                g_rt_wait_fix = false;
                g_rt_fix_ms = first_fix_ms;
                gnss_rt_mark("first-fix");
                gnss_rt_end("fix");
              }
            }

            had_valid = true;
          }

          if (gnss_eph_try_reload(fd)) {
            last_nmea_ms = gnss_now_ms();
            g_nmea_life_ms = last_nmea_ms;
          }
        }

        cnt = 0;
        discard = false;
        rx_st = 0;
        continue;
      }

      if (!discard) {
        if (cnt + 1 >= sizeof(line)) {
          discard = true;
        } else {
          line[cnt++] = ch;
        }
      }

      continue;
    }

    if (rx_st == 2) {
      if (b == 0x62) {
        rx_st = 3;
        ubx_hdr_i = 0;
        ubx_cka = 0;
        ubx_ckb = 0;
      } else if (ch == '$') {
        rx_st = 1;
        cnt = 0;
        discard = false;
        line[cnt++] = ch;
      } else if (b == 0xb5) {
        rx_st = 2;
      } else {
        rx_st = 0;
      }

      continue;
    }

    if (rx_st == 3) {
      ubx_hdr[ubx_hdr_i++] = b;
      ubx_cka = (uint8_t)(ubx_cka + b);
      ubx_ckb = (uint8_t)(ubx_ckb + ubx_cka);
      if (ubx_hdr_i < 4) {
        continue;
      }

      ubx_len = (uint16_t)ubx_hdr[2] | ((uint16_t)ubx_hdr[3] << 8);
      ubx_i = 0;
      if (ubx_len > GNSS_UBX_PAY_MAX) {
        gnss_link_add(&g_link.junk);
        rx_st = 0;
      } else {
        rx_st = (ubx_len == 0) ? 5 : 4;
        ubx_ck = 0;
      }

      continue;
    }

    if (rx_st == 4) {
      ubx_cka = (uint8_t)(ubx_cka + b);
      ubx_ckb = (uint8_t)(ubx_ckb + ubx_cka);
      /* PVT 的载荷单独收一份：它要进快照（gnss_pvt_store），与日志开关无关。
       * 倒库路径用 s_ubx_pl，两者互不干扰。 */
      if (ubx_hdr[0] == 0x01 && ubx_hdr[1] == 0x07 && ubx_i < GNSS_PVT_LEN) {
        pvt_pay[ubx_i] = b;
      }
      if (g_dbd_run && ubx_i < GNSS_UBX_PAY_MAX) {
        s_ubx_pl[ubx_i] = b;
      }

      ubx_i++;
      if (ubx_i >= ubx_len) {
        rx_st = 5;
        ubx_ck = 0;
      }

      continue;
    }

    if (rx_st == 5) {
      if (ubx_ck == 0) {
        ubx_got_cka = b;
        ubx_ck = 1;
      } else {
        bool ck_ok = (ubx_cka == ubx_got_cka && ubx_ckb == b);

        if (g_dbd_run) {
          uint8_t cka;
          uint8_t ckb;
          uint16_t i;

          /* 校验和在这里独立重算，**故意不与上面探针的增量累加合并**：
           * 倒库路径是验证过的（见 gnss_dbd_*），探针不该改动它。 */
          gnss_ubx_cksum(ubx_hdr, 4, &cka, &ckb);
          for (i = 0; i < ubx_len; i++) {
            cka = (uint8_t)(cka + s_ubx_pl[i]);
            ckb = (uint8_t)(ckb + cka);
          }

          if (cka == ubx_got_cka && ckb == b) {
            gnss_dbd_on_frame(ubx_hdr[0], ubx_hdr[1], ubx_len);
          }
        }

        /* 探针（docs/gnss_speed_filter.md 步骤 2）：UBX-NAV-PVT 到没到。
         *
         * **只认整帧且校验通过** —— 这一条要能扛住"其实没发"这个结论，所以
         * 宁可漏计也不能把噪声里凑出来的 `b5 62 01 07` 当成证据。
         * 帧头的 cls/id 在 rx_st 3 已经收全，长度在这里顺手报出来：
         * M10 的 PVT 是 92 字节，别的长度说明固件/协议版本不一样。 */
        if (ck_ok && ubx_hdr[0] == 0x01 && ubx_hdr[1] == 0x07) {
          gnss_link_add(&g_link.pvt);
          if (ubx_len == GNSS_PVT_LEN) {
            /* 进快照的那份：与日志开关无关（见 gnss_pvt_store）。 */
            gnss_pvt_store(pvt_pay, gnss_now_ms());
          }

          if (!g_pvt_first_done) {
            g_pvt_first_done = true;
            syslog(LOG_INFO, "gnss: first pvt len=%u\n", (unsigned)ubx_len);
#ifdef CONFIG_MYVENDOR_GNSS_LOG
            if (ubx_len == GNSS_PVT_LEN) {
              gnss_pvt_dump(pvt_pay, ubx_len);
            }
#endif
          }

#ifdef CONFIG_MYVENDOR_GNSS_LOG
          /* 探针的值直接打出来（1 Hz，与 `pub` 行同频，便于对着比）。 */
          if (ubx_len == GNSS_PVT_LEN) {
            gnss_pvt_report(pvt_pay);
          }
#endif
        }

        gnss_link_add(&g_link.ubx);
        rx_st = 0;
      }
    }
  }

  /* 先把 diag 的看门狗摘掉再清心跳：反过来的话，diag 可能在"心跳已清 0、
   * 条目还在"的窗口里对着一个正在退出的 tid 踢一脚（rc=ESRCH 只是噪音，
   * 但没必要留着）。 */
  myvendor_diag_unwatch(pthread_self());

  g_gnss_pid = (pid_t)-1;
  g_heartbeat_ms = 0;
  g_gnss_stack_busy = false;
  /* 诊断：退出原因与本次会话的完整画像。stop=1 表示是被 reap 请求退出；
   * stop=0 说明是外部 cancel。gap_max/loop_max 用来判断接收环风险。
   * phase= 是退出瞬间的阶段（正常退应该是 "loop"/"stopping"；其它值说明是
   * 从某段里被 cancel 出来的）。pid/pid_boot 用于排查 pid 丢失。 */
  syslog(LOG_WARNING,
         "gnss: reader exit stop=%d phase=%s pid=%d/%d sess=%u fd=%d rd=%u "
         "byte=%u nmea=%u bad=%u ubx=%u pvt=%u junk=%u pub=%u poll_to=%u poll_err=%u "
         "wr_fail=%u gap_max=%u loop_max=%u stall1s=%u poll_close=%u\n",
         g_gnss_stop ? 1 : 0, g_gnss_phase,
         (int)g_gnss_pid, (int)g_gnss_pid_boot,
         g_rd_session_ms != 0
           ? (unsigned)gnss_elapsed_ms(gnss_now_ms(), g_rd_session_ms) : 0u,
         fd, (unsigned)g_link.rd, (unsigned)g_link.byte,
         (unsigned)g_link.nmea, (unsigned)g_link.nmea_bad,
         (unsigned)g_link.ubx, (unsigned)g_link.pvt, (unsigned)g_link.junk,
         (unsigned)g_link.pub, (unsigned)g_link.poll_to,
         (unsigned)g_link.poll_err, (unsigned)g_link.wr_fail,
         (unsigned)g_rd_gap_max_ms, (unsigned)g_rd_loop_max_ms,
         (unsigned)g_rd_stall_1s, (unsigned)g_rd_poll_close);
  gnss_rt_end("reader-exit");
  return NULL;
}

/**
 * @brief 记录"收不回来"的连续时长，超过 GNSS_REAP_GIVEUP_MS 打一次明确故障。
 *
 * 恢复路径以前只有"下个周期再试"，**没有任何时间维度**：线程永不退出时日志里
 * 只有一串 EBUSY，看不出"GNSS 已经没了"这件事 —— 判死只能靠人盯 hb_age 猜。
 * 这里给它一个硬截止，让故障显性化。
 *
 * 注意这是**纯诊断**：不改变任何恢复行为，重试会一直继续（见 §2.3）。
 */
static void gnss_reap_note_stuck(void)
{
  uint32_t now = gnss_now_ms();

  if (g_reap_stuck_ms == 0) {
    g_reap_stuck_ms = now;
    return;
  }

  if (!g_reap_giveup_logged &&
      gnss_elapsed_ms(now, g_reap_stuck_ms) >= GNSS_REAP_GIVEUP_MS) {
    g_reap_giveup_logged = true;
    syslog(LOG_ERR,
           "gnss: RECOVERY FAILED after %u ms — reader 无法回收 "
           "(busy=%d has=%d pid=%d/%d phase=%s)；GNSS 失效，仍在重试\n",
           (unsigned)gnss_elapsed_ms(now, g_reap_stuck_ms),
           g_gnss_stack_busy ? 1 : 0, g_gnss_has_thread ? 1 : 0,
           (int)g_gnss_pid, (int)g_gnss_pid_boot, g_gnss_phase);
  }
}

static int gnss_wait_pid_gone(pid_t pid, unsigned wait_ms)
{
  unsigned steps;
  unsigned i;

  if (pid <= 0) {
    return 0;
  }

  steps = (wait_ms + 99u) / 100u;
  for (i = 0; i < steps; i++) {
    if (!gnss_pid_alive(pid)) {
      return 0;
    }

    gnss_sleep_ms(100);
  }

  return gnss_pid_alive(pid) ? -ETIMEDOUT : 0;
}

/**
 * @brief 有界等待"读线程已不存在"，判据用 **pthread 句柄**而不是 pid。
 *
 * @details
 * 为什么不能用 pid：`gnss_pid_alive(pid)` 在 `pid <= 0` 时恒返回假，而现场已经
 * 三次抓到 `g_gnss_pid == 0`。**那个 0 已经查明**：入口用的是 `getpid()`，而
 * NuttX 的 getpid() 返回的是 **task group 的 pid**（`task_get_info()->ta_pid`），
 * 读线程的 group 恰好是 0（从 bringup 上下文建出来，ps 里 GROUP 列就是 0）——
 * 源码里确实没有任何地方写 0，它从入口第一行就是 0。现在入口改用 `gettid()`
 * （线程自己的 pid，即 ps 里的 9），但**这条等待仍然按句柄判**：pid 只用于打点，
 * 句柄判据不依赖它，两种失效模式都盖得住。当时 pid=0 的后果是双向的：
 *   - `gnss_wait_pid_gone(pid)` 开头 `if (pid <= 0) return 0;` —— 于是本该
 *     等 8 秒的有界等待**只花了 6 ms**（日志里 `gnss: reap wait2 6 ms alive=0 busy=1`），
 *     就算信号唤醒有效也来不及；
 *   - 同时 `busy=1` 的闩锁又阻止 join，于是每轮 reap 都 abort。
 *
 * 判据换成 `pthread_cancel()` 的返回值：它是内核按句柄查的，
 * `ESRCH` = 该句柄已不对应任何线程。读线程显式关掉了 cancellation，
 * 所以这里只把它当**存活性探针**用，不会真的把线程撕掉（原注释已说明）。
 *
 * @param wait_ms 最长等待毫秒数。
 * @return 0 已确认线程不存在；-ETIMEDOUT 仍在。
 */
static int gnss_wait_handle_gone(uint32_t wait_ms)
{
  uint32_t steps = (wait_ms + 99u) / 100u;
  uint32_t i;

  if (!g_gnss_has_thread)
    {
      return 0;
    }

  for (i = 0; i < steps; i++)
    {
      if (pthread_cancel(g_gnss_thread) == ESRCH)
        {
          return 0;
        }

      gnss_sleep_ms(100);
    }

  return -ETIMEDOUT;
}

/**
 * @brief 等读线程退出并 join，再允许复用 g_gnss_stack。
 * @return 0 栈空闲；负 errno 仍占用，禁止 spawn。
 */
/**
 * @brief 读线程是不是"走丢的僵尸"：状态 RUNNING、却不在等任何东西。
 *
 * @details
 * 判据来自现场实测（第 4 次卡死）：
 *   `gnss: diag tcb pid=9 st=3 lock=0 wait=0 wd=0`
 *   `gnss: diag uart tx_hold=-1 rx_hold=-1 open_cnt=1 setup_n=1 shutdown_n=0`
 *
 * `st=3` = `TSTATE_TASK_RUNNING`：唤醒路径已经跑完（waitobj 清空、进了就绪链、
 * 状态标 RUNNING），**但 CPU 再也没切到它**；`wait/wd/lock` 三个全空说明它不是
 * "卡在等什么"，`tx_hold/rx_hold=-1` + `shutdown_n=0` 说明它也不在串口驱动里、
 * 没人 close 过端口。所以按"它卡在某个等待里"设计的手段全部无效（SIGUSR1、
 * poll 超时、pthread_cancel、reap 的 wait1/wait2）。
 *
 * 这里据此放行回收：它不持锁、不在驱动里 ⇒ 关掉重开串口、起新读线程都安全；
 * 万一它哪天被调度回来，循环顶的 `g_gnss_stop` 会让它自己退出。
 */
static bool gnss_reader_stranded(pid_t pid)
{
  FAR struct tcb_s *tcb;
  bool stranded = false;

  if (pid <= 0)
    {
      return false;
    }

  /* 与 `gnss_reader_lost()` 同口径：整段在 `sched_lock()` 里做 —— 既让遍历到
   * 的就绪链是稳定快照，也保证"判完到调用方动手"之间没人把它挂回去。 */
  sched_lock();
  tcb = nxsched_get_tcb(pid);   /* 必须与 put 成对 */
  if (tcb == NULL)
    {
      sched_unlock();
      return false;
    }

  /** ⚠ 2026-09-23 `st=2` 这第三种形态：**状态标 READYTORUN、却不在就绪链上**。
   *
   *  原来这里（以及 `gnss_reader_lost()`）只认 `st == TSTATE_TASK_RUNNING`，
   *  于是这种 reader 两个判据都不认 ⇒ reap 走 `reader not exited` 直接
   *  `-EBUSY`（**不 release zombie**）⇒ spawn 永远被 `stack_busy=1` 拒
   *  ⇒ `gnss_reader_lost()` 那条回收**根本到不了** ⇒ GNSS 死到重启。
   *
   *  现场（固件 `202609231852`，uptime 370→540 s）：静止休眠把模块电关掉、
   *  唤醒后再没回来，`diag tcb pid=9 … st=2 lock=0 wait=0 wd=0 on_rdy=0`、
   *  `hb_age` 一路涨、`kick gnss-reader` 的 kicks 计到 30+，
   *  全程没有 `reclaim zombie stack slot` / `no free reader stack` 任何一条
   *  —— 也就是**两条恢复路都被这一个状态卡住**。
   *
   *  为什么 `st=2 && on_rdy=0` 一定不可达：NuttX 里
   *  `TSTATE_TASK_READYTORUN` 的字面定义就是"TCB is in the ready to run
   *  list"（`TSTATE_TASK_PENDING` 才是"ready 但不在任何表里"）。
   *  标着 2 又不在链上 = 自相矛盾，没有任何唤醒路径能再碰到它。
   *
   *  **牙齿那一半必须一起上**：光放宽状态会误伤真在跑的线程，所以下面照
   *  `gnss_reader_lost()` 补"不在 `this_task()` 起头的就绪链上" 这条。
   *
   *  自证伪：若放宽后出现"两具 reader 交替说话"（HCI/NMEA 行里两套 pid
   *  交错，或 `g_rd_loop_seq` 两边都在涨）⇒ 说明被隔离的那具其实还会被
   *  调度 ⇒ 撤回本次放宽。 */
  if ((tcb->task_state == TSTATE_TASK_RUNNING ||
       tcb->task_state == TSTATE_TASK_READYTORUN) &&
      tcb->waitobj == NULL &&
      tcb->lockcount == 0 && !WDOG_ISACTIVE(&tcb->waitdog))
    {
      FAR struct tcb_s *t = this_task();
      int hops;

      stranded = true;   /* 先假设丢了；在链上就翻案 */
      for (hops = 0; t != NULL && hops < 64; hops++, t = t->flink)
        {
          if (t == tcb)
            {
              stranded = false;
              break;
            }
        }
    }

  if (stranded)
    {
      syslog(LOG_ERR,
             "gnss: reader stranded (st=%u wait=0 wd=0 lock=0 on_rdy=0) — treat as zombie\n",
             (unsigned)tcb->task_state);
    }

  nxsched_put_tcb(tcb);
  sched_unlock();
  return stranded;
}

/* 「被调度器丢掉」的判据 —— 比 `gnss_reader_stranded()` 更硬的一条。
 *
 * 2026-09-21 实测的终态：读线程 pid=106 把自己的一次 `poll()` 留在半路
 * （`g_poll_enter_ms` 一直非 0、`g_rd_loop_seq`/`g_poll_n` 冻住、in_poll 一路涨
 * 到 3.07 h），TCB 现场是 `st=3 wait=0 wd=0 lock=0` **且 `on_rdy=0`** ——
 * 也就是源码注释里点名的"现场第 4 次"签名：唤醒路径已经跑完（waitobj 清、
 * 状态标回 RUNNING），但**没有谁把它挂回就绪链**。这种线程没有任何唤醒路径
 * 能够再碰到它（它的 waitlist 成员资格已解掉，没有任何 sem 持有它的引用），
 * 所以它永远不会再被调度、它的栈也永远不会再被踩。
 *
 * 判据口径（必须与"还活着的等待者"分开，否则就是在抢一个真的在跑的线程的栈）：
 *   - `st == RUNNING`：不是 WAIT_SEM/WAIT_SIG（那类线程会被正常唤醒）；
 *   - `waitobj == NULL && !WDOG_ISACTIVE(waitdog) && lockcount == 0`：
 *     不挂在任何 sem/watchdog/临界区上；
 *   - 不在 `this_task()` 起头的就绪链上：没有后续调度机会。
 * 前三条单独出现**还不够**（正常 park 在驱动里的线程也是这个签名，见
 * gnss_reader_stranded 的注释），所以第四条才是这条判据的牙齿。
 *
 * 整段在 `sched_lock()` 里做：既让遍历到的链是稳定快照，也顺带保证
 * 判完到调用方动手之间没有别人把这个线程挂回去。 */
static bool gnss_reader_lost(pid_t pid)
{
  FAR struct tcb_s *tcb;
  bool lost = false;

  if (pid <= 0)
    {
      return false;
    }

  sched_lock();
  tcb = nxsched_get_tcb(pid);   /* 必须与 put 成对 */
  /* 状态放宽到 `READYTORUN`：2026-09-23 实测到 `st=2`（字面定义就是
   * "TCB is in the ready to run list"）却 `on_rdy=0` 的形态 —— 标着在链上、
   * 实际不在，没有任何唤醒路径能再碰到它。放宽是安全的：**牙齿在下面那条
   * "不在就绪链上"**，真在跑/真在表里的线程照样翻案。详见
   * `gnss_reader_stranded()` 里同日的注释。 */
  if (tcb != NULL && tcb->pid == pid &&
      (tcb->task_state == TSTATE_TASK_RUNNING ||
       tcb->task_state == TSTATE_TASK_READYTORUN) &&
      tcb->waitobj == NULL &&
      tcb->lockcount == 0 && !WDOG_ISACTIVE(&tcb->waitdog))
    {
      FAR struct tcb_s *t = this_task();
      int hops;

      lost = true;   /* 先假设丢了；在链上就翻案 */
      for (hops = 0; t != NULL && hops < 64; hops++, t = t->flink)
        {
          if (t == tcb)
            {
              lost = false;
              break;
            }
        }
    }

  if (tcb != NULL)
    {
      nxsched_put_tcb(tcb);
    }

  sched_unlock();
  return lost;
}

static int gnss_thread_reap(unsigned wait_ms)
{
  pid_t pid = g_gnss_pid;
  uint32_t t0 = gnss_now_ms();
  int jr;

  /* 诊断：进入 reap 时的现场。alive=0 说明读线程已自己退出（不会再打
   * "stop reader"），这决定了后续 close/reopen 是否必要。
   * pid/pid_boot 两个都打：现场出现过 pid=0 而 pid_boot 才是真的。 */
  syslog(LOG_WARNING,
         "gnss: reap enter pid=%d pid_boot=%d alive=%d has_thread=%d stack_busy=%d wait_ms=%u stop_before=%d hb_age=%u phase=%s\n",
         (int)pid, (int)g_gnss_pid_boot, gnss_pid_alive(pid) ? 1 : 0,
         g_gnss_has_thread ? 1 : 0, g_gnss_stack_busy ? 1 : 0,
         wait_ms, g_gnss_stop ? 1 : 0,
         g_heartbeat_ms != 0
           ? (unsigned)gnss_elapsed_ms(t0, g_heartbeat_ms) : 0u,
         g_gnss_phase);

  /* 取证：判定"卡在哪"的四个量，跟上面那行一起看。
   *
   *   self    读线程自己记下的 pthread 句柄（0 = 入口没执行过）
   *   entered 入口标记是否被写过（0 = TCB 建了但没跑起来）
   *   seq     循环推进次数（不再增长 = 循环体停了）
   *   poll_n  poll 调用总次数
   *   in_poll 非 0 = **此刻有一次 poll 已经进去了还没出来**，后面跟它已持续多久
   *           （毫秒）。这一项是"卡在 poll 里"的直接证据；它恒为 0 则说明线程
   *           从没进过 poll，据此可以排除整条 poll 路径。
   *   last   最近一次 poll 的实际耗时（正常应 ≈ 超时值 200 ms） */
  syslog(LOG_WARNING,
         "gnss: reap rd self=%lx entered=%u seq=%u poll_n=%u in_poll=%u last=%u ms\n",
         g_rd_self, g_rd_enter_ms != 0u ? 1u : 0u, (unsigned)g_rd_loop_seq,
         (unsigned)g_poll_n,
         g_poll_enter_ms != 0u
           ? (unsigned)gnss_elapsed_ms(gnss_now_ms(), g_poll_enter_ms) : 0u,
         (unsigned)g_poll_last_ms);

  g_gnss_stop = true;
  g_diag_kick = true;

  /* 置位不够，还得**把线程从阻塞的 poll() 里捅一下**。
   *
   * 读线程关掉了 cancellation，所以它唯一的退出途径是自己走到循环顶看
   * g_gnss_stop。以前那个"走到循环顶"靠的是读等待的有界超时，而超时由
   * tick 驱动 —— tick 一被打断就永远走不到，于是 reap 只能放弃、
   * g_gnss_stack_busy 永远是 1，这就是"异常无法恢复"。信号不经过 tick，
   * 所以这一步才是恢复路径真正有牙齿的地方。 */
  gnss_reader_wake();

  if (gnss_pid_alive(pid) || g_gnss_has_thread) {
    /* 有界等待走**句柄**判据（pid 会读成 0，见 gnss_wait_handle_gone 的注释）：
     * 否则这一整段在 pid=0 时会被整体跳过，8 秒的等待退化成 0 秒。 */
    syslog(LOG_WARNING, "gnss: stop reader pid=%d\n", (int)pid);
    (void)gnss_wait_handle_gone(wait_ms);
    syslog(LOG_WARNING, "gnss: reap wait1 %u ms alive=%d\n",
           (unsigned)gnss_elapsed_ms(gnss_now_ms(), t0),
           gnss_pid_alive(pid) ? 1 : 0);
  }

  /* pthread 不能 task_delete：nxtask_delete 会 DEBUGPANIC（task_delete.c:103）。 */

  /* cancel 只看"有没有线程句柄"，**不再依赖 pid**。
   *
   * 现场故障：g_gnss_pid 读成 0（成因已查明 = getpid() 返回 group 的 pid，
   * 见 gnss_thread 入口注释；那时它让 gnss_pid_alive(0) 恒假），于是旧代码
   * 跳过 wait1 和这里的 cancel，直接走到 pthread_join；join 没有超时，而线程
   * 又卡在系统调用里，一下把 diag 线程钉了 468 秒，GPS 恢复被整体推迟。
   * pthread_cancel 作用在 pthread_t 上，不依赖 pid，所以放在这个条件里最稳。 */
  if (g_gnss_has_thread) {
    /* 返回值在这里当**权威存活性判据**用，不只是"发个请求"：
     *   - ESRCH → 线程已不是可调度对象（sched/pthread/pthread_cancel.c：
     *             "The pid does not correspond to any known thread. The thread
     *              has probably already exited."）。它**完全不看 g_gnss_pid**，
     *             所以不会被 gnss_pid_alive(0) 那种假阴性骗到。
     *   - OK    → 线程还在。注意读线程把 cancellation 关掉了，所以这里的 OK
     *             只表示"请求已记下/已投递"，**不代表它会退出**。以前这行日志
     *             把 OK 当成功报，看起来像已经处理过，实际什么都没发生 ——
     *             这是排查时最大的误导，所以现在把 ret 和语义一起打出来。 */
    int cr = pthread_cancel(g_gnss_thread);

    syslog(LOG_ERR,
           "gnss: pthread_cancel pid=%d alive=%d busy=%d ret=%d%s\n",
           (int)pid, gnss_pid_alive(pid) ? 1 : 0,
           g_gnss_stack_busy ? 1 : 0, cr,
           cr == ESRCH ? " (线程已不存在)" :
           cr == 0     ? " (仅记账：cancellation 已关闭，非强制杀)" : "");

    if (cr == ESRCH) {
      /* 线程已经不在 —— 此时 g_gnss_stack_busy 是**过期启发式**：只有读线程
       * 正常跑完退出尾部才会清它，被 cancel / 强杀 / 异常终止时不会。
       * 旧代码先看 busy 再看存活性，于是"线程早就死了"被误判成"还活着"：
       * reap 永远 -EBUSY、spawn 永远被拒 —— 这是"异常无法恢复"的闩锁之根。
       * 以 ESRCH 为准接管：对象已不可调度，join 必定立即返回，顺手清掉闩锁。 */
      jr = pthread_join(g_gnss_thread, NULL);
      g_gnss_has_thread = false;
      g_gnss_pid = (pid_t)-1;
      g_gnss_stack_busy = false;
      g_reap_stuck_ms = 0;
      g_reap_giveup_logged = false;
      syslog(LOG_WARNING,
             "gnss: reader gone (ESRCH) join=%d, stale latch cleared in %u ms\n",
             jr, (unsigned)gnss_elapsed_ms(gnss_now_ms(), t0));
      return 0;
    }

    /* 同上：这一轮等待也改用句柄判据。原来 pid=0 时它是 0 秒，
     * 现场就表现为 `reap wait2 6 ms` —— 本该给 1 秒的窗口。 */
    (void)gnss_wait_handle_gone(1000u);
    syslog(LOG_ERR, "gnss: reap wait2 %u ms alive=%d busy=%d\n",
           (unsigned)gnss_elapsed_ms(gnss_now_ms(), t0),
           gnss_pid_alive(pid) ? 1 : 0, g_gnss_stack_busy ? 1 : 0);
  }

  /* 有界判定：g_gnss_stack_busy 是"退出尾部没跑完"的启发式 —— 读线程的退出
   * 尾段会清它。busy=1 意味着线程可能还在用那块栈，此时 pthread_join 可能永久
   * 阻塞；宁可放弃 join（保留栈占用、返回 EBUSY 让 diag 下个周期重试），
   * 也不能把调用者钉死。
   *
   * 注意这条路径**不是终态**：卡住只是"这一轮收不回来"，diag 会按自己的节奏
   * 再调 restart → reap。配合 §2.2 的有界等待，读线程最多 200 ms 就会回到循环
   * 顶看到 g_gnss_stop 并退出，所以下一次必然能回收 —— "无法恢复"的真正原因
   * 是读线程当时被 tick 驱动的 poll 钉死、永远走不到循环顶，而不是这里不重试。 */
  if (g_gnss_has_thread && g_gnss_stack_busy) {
    if (gnss_reader_stranded(pid)) {
      gnss_reader_release_zombie(pid);
      return 0;
    }

    gnss_reap_note_stuck();
    syslog(LOG_ERR,
           "gnss: reader not exited (pid=%d/%d busy=1 phase=%s), skip join to stay bounded\n",
           (int)pid, (int)g_gnss_pid_boot, g_gnss_phase);
    return -EBUSY;
  }

  /* 到这里已经过了 ESRCH 那一关（线程确实存在）。pid 判据只当冗余保险：
   * pid 可能被读到 0 而假阴性，所以它**只能用来更保守**，不能用来放行 join ——
   * 历史故障正是 pid 假阴性导致跳过这道闸、直奔 pthread_join，把 diag 钉了 468 s。 */
  if (gnss_pid_alive(pid) || gnss_pid_alive(g_gnss_pid)) {
    if (gnss_reader_stranded(pid)) {
      gnss_reader_release_zombie(pid);
      return 0;
    }

    gnss_reap_note_stuck();
    syslog(LOG_ERR,
           "gnss: reader still in syscall pid=%d, skip join (UART open)\n",
           (int)(g_gnss_pid > 0 ? g_gnss_pid : pid));
    /* 诊断：这条路径直接决定恢复要再花多久（放弃 join 后仍会继续处理）。 */
    syslog(LOG_ERR, "gnss: reap EBUSY after %u ms\n",
           (unsigned)gnss_elapsed_ms(gnss_now_ms(), t0));
    return -EBUSY;
  }

  if (g_gnss_has_thread) {
    jr = pthread_join(g_gnss_thread, NULL);
    if (jr != 0) {
      syslog(LOG_ERR, "gnss: join failed %d pid=%d\n", jr, (int)pid);
    }
  }

  g_gnss_has_thread = false;
  g_gnss_pid = (pid_t)-1;
  g_gnss_stack_busy = false;
  g_reap_stuck_ms = 0;
  g_reap_giveup_logged = false;
  syslog(LOG_WARNING, "gnss: reap ok in %u ms\n",
         (unsigned)gnss_elapsed_ms(gnss_now_ms(), t0));
  return 0;
}

/** @brief 判到僵尸读线程之后**真正放行**：清闩锁 + 隔离它占的那块栈。
 *
 *  以前这里只 `return 0`（注释写着"已按僵尸放行，见下"，但下面是空的），
 *  `g_gnss_has_thread`/`g_gnss_stack_busy`/`g_gnss_pid` 一个都没清 ——
 *  spawn 于是永远被拒（现场：`spawn refused busy=1 has=1 pid=9`，每 30 s 一次，
 *  GNSS 死到重启）。这里按"线程收不回来"处理：不 join（会永久阻塞），
 *  把它的栈槽标成僵尸、永不复用，闩锁全部清掉，让下一次 spawn 用另一块栈。 */
static void gnss_reader_release_zombie(pid_t pid)
{
  uint8_t mask = (uint8_t)(1u << g_gnss_stack_slot);

  g_gnss_stack_zombie |= mask;
  g_gnss_zombie_pid[g_gnss_stack_slot] = pid;   /* 回收时要用它判"走没走" */
  g_gnss_has_thread = false;
  g_gnss_stack_busy = false;
  g_gnss_pid = (pid_t)-1;
  g_reap_stuck_ms = 0;
  g_reap_giveup_logged = false;
  syslog(LOG_ERR,
         "gnss: zombie released pid=%d stack slot %u quarantined "
         "(zombie=0x%02x) — next spawn uses the other slot\n",
         (int)pid, (unsigned)g_gnss_stack_slot, (unsigned)g_gnss_stack_zombie);
}

static int gnss_thread_spawn(void)
{
  pthread_attr_t attr;
  struct sched_param sp;
  int ret;

  if (g_gnss_spawning || g_gnss_has_thread || g_gnss_stack_busy ||
      gnss_pid_alive(g_gnss_pid)) {
    syslog(LOG_ERR, "gnss: spawn refused busy=%d has=%d pid=%d\n",
           g_gnss_stack_busy ? 1 : 0,
           g_gnss_has_thread ? 1 : 0, (int)g_gnss_pid);
    return -EBUSY;
  }

  g_gnss_spawning = true;
  g_gnss_stack_busy = true;
  g_gnss_stop = false;
  pthread_attr_init(&attr);
  {
    unsigned s;

    for (s = 0; s < GNSS_STACK_SLOTS; s++) {
      if ((g_gnss_stack_zombie & (1u << s)) == 0u) {
        break;
      }

      /* 两种可回收情形 —— 只认第一种是不够的，2026-09-21 实机就是被第二种卡住的：
       *
       *   ① pid 已死（`gnss_pid_alive()` 明确说不在了）；
       *   ② pid 还在，但**被调度器丢了**（`gnss_reader_lost()`：st=RUNNING、
       *      不挂任何 sem/wdog/锁、且不在就绪链上）。这种线程永远不可能再被
       *      调度，它的栈永远不会再被踩；只按 ① 判就会把它当"还活着"而拒绝回收
       *      —— 实测 pid=106 就这么把槽位占死 3.07 h（`zombie=0x03`、
       *      每 30 s 重试一次 `ret=-12`），GPS 到关机都不回来。
       *
       * pid 可能被复用的顾虑仍在，但 ② 不是靠 pid 判活，是靠"这个线程还能不能
       * 被调度"判的 —— 复用出来的新线程只要在跑就在就绪链上，② 自然不成立。 */
      if (g_gnss_zombie_pid[s] != 0 &&
          (!gnss_pid_alive(g_gnss_zombie_pid[s]) ||
           gnss_reader_lost(g_gnss_zombie_pid[s]))) {
        bool alive = gnss_pid_alive(g_gnss_zombie_pid[s]);

        syslog(LOG_WARNING,
               "gnss: reclaim zombie stack slot %u (pid=%d %s)\n",
               s, (int)g_gnss_zombie_pid[s],
               alive ? "lost by scheduler (unreachable)" : "confirmed gone");
        g_gnss_zombie_pid[s] = 0;
        g_gnss_stack_zombie &= (uint8_t)~(1u << s);
        break;
      }
    }

    if (s >= GNSS_STACK_SLOTS) {
      /* 单槽之后这条就是终态：唯一的槽被一个"还活着又判不出丢了"的线程占着，
       * 每次重试都会走到这里。**判据字符串保持原样**（巡检 grep
       * `no free reader stack`），后面补上占槽的 pid 便于现场定位。 */
      syslog(LOG_ERR,
             "gnss: no free reader stack (zombie=0x%02x slot0_pid=%d)"
             " -- GNSS down until reboot\n",
             (unsigned)g_gnss_stack_zombie, (int)g_gnss_zombie_pid[0]);
      g_gnss_spawning = false;
      g_gnss_stack_busy = false;
      return -ENOMEM;
    }

    g_gnss_stack_slot = (uint8_t)s;
  }

  /* 栈留 SRAM：实测水位 3,260/8,112（40%，2026-09-25 `test cpu -t 2`），
   * 没有理由搬 PSRAM —— 要搬的是**大缓冲**（UBX payload / eph dump / DBD
   * 已经是 PSRAM 了）。这里只保留指针数组结构，方便以后按需切换。 */
  if (g_gnss_stack[g_gnss_stack_slot] == NULL) {
    g_gnss_stack[g_gnss_stack_slot] = g_gnss_stack_sram[g_gnss_stack_slot];
  }

  /* 栈金丝雀：整块先刷成 0xA5，栈底最后 32 B 留作"深水位警戒线"。
   * 谁踩到这段地址范围，读数就不是 0xA5 —— 由 gnss 读线程每轮自查并报出地址。 */
  {
    uint8_t *stk = g_gnss_stack[g_gnss_stack_slot];

    memset(stk, 0xA5, GNSS_STACK_SIZE);

#if defined(CONFIG_ARM_MPU)
    /* 栈底 32 B 设成**只读**（MPU 没有 "P:None"，只读对写而言等效）：任何线程
     * 写到这段（栈长到最深处、或有人越界写进来）立刻 MemManage fault，
     * 现场里的 MMFAR/BFAR + PC 就是**写者和写它的那条指令** —— 这就是抓现行。
     * 头文件在 arch 层，服务里沿用本项目"调用点 extern"的做法。 */
    {
      extern unsigned int mpu_configure_region(uintptr_t base, size_t size,
                                               uint32_t ap, uint32_t rlar);
      const uint32_t MPU_AP_RONO = (2u << 1);   /* P:RO U:None */

      (void)mpu_configure_region((uintptr_t)stk, 32u, MPU_AP_RONO, 0u);
      syslog(LOG_WARNING, "gnss: stack guard armed (RO 32B @%p)\n",
             (const void *)stk);
    }
#endif
    syslog(LOG_WARNING, "gnss: reader stack slot %u srcck canary armed\n",
           (unsigned)g_gnss_stack_slot);
  }

  ret = pthread_attr_setstack(&attr, g_gnss_stack[g_gnss_stack_slot],
                              GNSS_STACK_SIZE);
  sp.sched_priority = GNSS_THREAD_PRIO;
#ifdef PTHREAD_EXPLICIT_SCHED
  (void)pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#endif
  (void)pthread_attr_setschedparam(&attr, &sp);
  if (ret == 0) {
    ret = pthread_create(&g_gnss_thread, &attr, gnss_thread, NULL);
  }

  pthread_attr_destroy(&attr);
  if (ret != 0) {
    g_gnss_stack_busy = false;
    g_gnss_spawning = false;
    syslog(LOG_ERR, "gnss: thread create failed %d\n", ret);
    return -ret;
  }

  g_gnss_has_thread = true;
  g_gnss_spawning = false;
  syslog(LOG_INFO, "gnss: NMEA reader on %s prio=%d (" GNSS_MOD_PN ")\n",
         CONFIG_BOARD_L96_GNSS_DEVPATH, GNSS_THREAD_PRIO);
  gnss_rt_mark("reader-spawn");
  return 0;
}

static int gnss_thread_restart(void)
{
  uint32_t t0 = gnss_now_ms();
  int fd;
  int ret;

  if (g_gnss_spawning) {
    return -EBUSY;
  }

  /* 诊断：谁要求重启 + 当时心跳有多旧。 */
  syslog(LOG_WARNING, "gnss: restart begin why=%s hb_age=%u sess=%u\n",
         g_restart_why,
         g_heartbeat_ms != 0
           ? (unsigned)gnss_elapsed_ms(t0, g_heartbeat_ms) : 0u,
         g_rd_session_ms != 0
           ? (unsigned)gnss_elapsed_ms(t0, g_rd_session_ms) : 0u);
  gnss_rt_begin("reader-restart");

  gnss_recover_begin();
  ret = gnss_thread_reap(GNSS_REAP_WAIT_MS);
  if (ret != 0) {
    syslog(LOG_ERR, "gnss: restart aborted, keep USART2 fd=%d\n", g_uart_fd);
    return ret;
  }

  /* 读线程已退出后**故意不 close USART2**。
   *
   * 以前这里 close → 最后一次 close 走 uart_reset_sem() → 强行
   * nxmutex_reset(recv/xmit.lock)。若此时还有等待者（半路 poll、或
   * 另一条路径误 open），那些 nxmutex_lock 会永远醒不来；读线程 pri=50
   * 又高于 IDLE，空转/卡锁会饿死 idle+work 心跳，IWDT 15 s 后复位
   * （WDT 现场 n576/n583/n585：idle/work 过期、ui 仍新鲜、companion
   * 卡在别的 mutex 上等）。NSH 对 console 也是进程在才关。
   *
   * 新读线程在 fd<0 时会自己 open；这里把仍有效的 g_uart_fd 留给它
   * 继承（见 gnss_thread 入口对 g_uart_fd 的接管）。 */
  fd = g_uart_fd;
  if (fd < 0) {
    syslog(LOG_WARNING, "gnss: restart: USART2 already closed\n");
  } else {
    syslog(LOG_INFO, "gnss: restart: keep USART2 fd=%d for next reader\n",
           fd);
  }

  g_uart_fails = GNSS_PWR_FAILS;
  g_eph_injected = false;
  ret = gnss_thread_spawn();
  syslog(LOG_WARNING, "gnss: restart done ret=%d in %u ms (eph_cleared)\n",
         ret, (unsigned)gnss_elapsed_ms(gnss_now_ms(), t0));
  return ret;
}

int myvendor_gnss_start(void)
{
  int ret;

  if (g_started) {
    return 0;
  }

  if (!gnss_ubx_pl_ensure()) {
    syslog(LOG_ERR, "gnss: BoardPSRAM UBX payload alloc failed\n");
    return -ENOMEM;
  }

  /* 读等待的计时基准（DWT）尽早自检：跑不起来就整体退回 usleep 老路径。 */
  gnss_dwt_init_once();

  g_restart_why = "boot";
  ret = gnss_thread_spawn();
  if (ret != 0) {
    return ret;
  }

  g_started = true;
  return 0;
}

bool myvendor_gnss_get(myvendor_sys_gnss_t *out)
{
  bool ok = false;
  bool hold;
  uint32_t now = gnss_now_ms();

  pthread_mutex_lock(&g_lock);
  hold = gnss_hold_locked(now);
  if (g_fix.alive && g_fix_ms != 0 &&
      (hold || gnss_elapsed_ms(now, g_fix_ms) < GNSS_STALE_MS)) {
    if (out != NULL) {
      *out = g_fix;
      out->alive = true;
      out->rx_hz = hold ? g_fix.rx_hz : gnss_rx_hz_now(now);
    }

    ok = true;
  } else if (hold) {
    if (out != NULL) {
      if (g_fix_ms != 0) {
        *out = g_fix;
      } else {
        memset(out, 0, sizeof(*out));
      }

      out->alive = true;
    }

    ok = true;
  }

  pthread_mutex_unlock(&g_lock);
  if (!ok && out != NULL) {
    memset(out, 0, sizeof(*out));
  }

  /* 读线程健康：**在这里现填**，不是读线程 publish 的 —— 读线程一旦卡死，
   * publish 根本不会再跑，那样恰好把要看的信息冻在卡死之前（设计见
   * include/myvendor_sys.h 的字段注释）。
   *
   * 2026-09-18 之前这几个字段**一直没有生产者**：`myvendor_sys.c` 的「星历
   * 同步」门控判 `rd_state != HL_OK`，而 `HL_OK == 0`、`rd_state` 恒为 0 ⇒
   * 判据恒不成立 ⇒ 那次为"粘滞的星历同步显示"加的修复等于空操作。这里补上
   * 生产者，门控才真的生效。 */
  if (out != NULL)
    {
      uint32_t hb = g_heartbeat_ms;
      bool stalled = (hb == 0) || gnss_elapsed_ms(now, hb) >= GNSS_THREAD_STALL_MS;
      int open_cnt = 0;
      int rx_hold = -1;
      int tx_hold = -1;
      unsigned setup_n = 0;
      unsigned shutdown_n = 0;

      out->rd_hb_age_ms = (hb == 0) ? 0u : gnss_elapsed_ms(now, hb);
      out->rd_in_poll_ms = (g_poll_enter_ms != 0)
                             ? gnss_elapsed_ms(now, g_poll_enter_ms) : 0u;
      out->rd_phase = g_gnss_phase;
      out->rd_restarts = (g_diag_restarts > 255u) ? 255u : (uint8_t)g_diag_restarts;

      /* 卡死 + 正在回收 = 回收也没成功（HL_FAILED，端口保持 open）；
       * 只卡死 = HL_STALL；没卡死而在回收 = HL_RECOVER。 */
      if (stalled)
        {
          out->rd_state = g_recovering ? MYVENDOR_SYS_GNSS_HL_FAILED
                                       : MYVENDOR_SYS_GNSS_HL_STALL;
        }
      else
        {
          out->rd_state = g_recovering ? MYVENDOR_SYS_GNSS_HL_RECOVER
                                       : MYVENDOR_SYS_GNSS_HL_OK;
        }

      /* 驱动器侧证据：只读几个字段，不取锁，任何线程都能问（见 sifli_uart.c）。 */
      if (sifli_uart_diag(GNSS_UART_NUM, &open_cnt, &rx_hold, &tx_hold,
                          &setup_n, &shutdown_n, NULL) == 0)
        {
          out->uart_open_cnt = (uint16_t)open_cnt;
          out->uart_close_cnt = (uint16_t)shutdown_n;
          out->uart_rx_hold = (int16_t)rx_hold;
          out->uart_tx_hold = (int16_t)tx_hold;
        }
    }

  if (ok) {
    if (!g_get_ok) {
      syslog(LOG_INFO, "gnss: snapshot live (UI) hz=%u valid=%d sats=%u\n",
             (unsigned)(out != NULL ? out->rx_hz : 0),
             (out != NULL && out->valid) ? 1 : 0,
             (unsigned)(out != NULL ? out->satellites : 0));
    }

    g_get_ok = true;
  } else if (g_get_ok) {
    uint32_t nmea_age = (g_nmea_life_ms == 0) ?
                        0 : gnss_elapsed_ms(now, g_nmea_life_ms);

    syslog(LOG_WARNING,
           "gnss: snapshot stale (UI 无数据) nmea_age=%u fail=%u recov=%d\n",
           (unsigned)nmea_age, (unsigned)g_uart_fails,
           g_recovering ? 1 : 0);
    g_get_ok = false;
  }

  return ok;
}

static uint32_t gnss_eph_scan_best_utc(void)
{
  DIR *dir;
  struct dirent *de;
  char path[GNSS_EPH_PATH_MAX];
  uint32_t best = 0;
  uint32_t utc;
  int n;

  dir = opendir(SF32LB52_GNSS_EPH_DIR);
  if (dir != NULL) {
    while ((de = readdir(dir)) != NULL) {
      if (!gnss_eph_parse_utc(de->d_name, &utc) || utc < best) {
        continue;
      }

      n = snprintf(path, sizeof(path), "%s/%s",
                   SF32LB52_GNSS_EPH_DIR, de->d_name);
      if (n < 0 || n >= (int)sizeof(path)) {
        continue;
      }

      if (!gnss_eph_path_usable(path)) {
        continue;
      }

      best = utc;
    }

    closedir(dir);
  }

  if (best == 0 && gnss_eph_path_usable(SF32LB52_GNSS_EPH_FILE)) {
    struct stat st;

    if (stat(SF32LB52_GNSS_EPH_FILE, &st) == 0 &&
        st.st_mtime >= (time_t)GNSS_TIME_MIN_UNIX) {
      best = (uint32_t)st.st_mtime;
    }
  }

  return best;
}

bool myvendor_gnss_eph_times(uint32_t *last_utc, uint32_t *next_utc)
{
  uint32_t last;
  uint32_t now;

  now = gnss_now_ms();
  pthread_mutex_lock(&g_lock);
  if (g_eph_times_ms != 0 &&
      gnss_elapsed_ms(now, g_eph_times_ms) < GNSS_EPH_TIMES_CACHE_MS) {
    last = g_eph_times_cached;
    pthread_mutex_unlock(&g_lock);
  } else {
    pthread_mutex_unlock(&g_lock);
    last = gnss_eph_scan_best_utc();
    pthread_mutex_lock(&g_lock);
    if (g_eph_last_utc > last) {
      last = g_eph_last_utc;
    } else {
      g_eph_last_utc = last;
    }

    g_eph_times_cached = last;
    g_eph_times_ms = gnss_now_ms();
    pthread_mutex_unlock(&g_lock);
  }

  if (last_utc != NULL) {
    *last_utc = last;
  }

  if (next_utc != NULL) {
    *next_utc = (last != 0) ? last + GNSS_EPH_VALID_SEC : 0;
  }

  return last >= (uint32_t)GNSS_TIME_MIN_UNIX;
}

void myvendor_gnss_eph_reload(void)
{
  pthread_mutex_lock(&g_lock);
  g_eph_reload = true;
  g_eph_busy = true;
  g_eph_times_ms = 0;

  /* **不再无条件清 `g_eph_injected`。** 这个入口有两个来源：
   *
   *   1. 手机真的推了一份新星历（源文件换了）—— 必须注入；
   *   2. 手机只是同步了时钟（`TIME_SYNC -> eph reload`）—— 文件没变、模块
   *      备份里星历还在，重灌纯属多余（2026-09-18 现场：开机因此注了两遍，
   *      多花 7.8 s 的 SD/LFS）。
   *
   * 两者在**请求时刻**分不出来（要 stat 才知道，而这里跑在协议栈线程上，
   * 不能做盘 I/O），所以判定留给 GNSS 线程：`gnss_eph_inject()` 用
   * 路径/大小/mtime 比对指纹，同源就跳过并按"已完成"处理；
   * 换了源则照常注入。
   *
   * 以前无条件清闩锁的理由（新星历被门口 `g_eph_injected` 挡掉）现在由指纹
   * 判定覆盖，语义反而更准。 */

  pthread_mutex_unlock(&g_lock);
}

bool myvendor_gnss_eph_busy(void)
{
  bool busy;
  uint32_t now = gnss_now_ms();

  pthread_mutex_lock(&g_lock);
  busy = g_eph_busy || gnss_eph_grace_locked(now);
  pthread_mutex_unlock(&g_lock);
  return busy;
}

void myvendor_gnss_idle_sleep(bool sleep)
{
  if (sleep) {
    GNSS_LOG("idle_sleep request on parked=%d fd=%d",
             g_idle_parked ? 1 : 0, g_uart_fd);
    g_idle_sleep = true;
    return;
  }

  if (g_idle_sleep) {
    GNSS_LOG("idle_sleep request off parked=%d", g_idle_parked ? 1 : 0);
    /* 只投申请：时间戳由读线程在 publish 路径落地（单写者，见 g_wake_hold_req）。 */
    g_wake_hold_req = true;
  }

  g_idle_sleep = false;
}

bool myvendor_gnss_idle_get(void)
{
  return g_idle_sleep;
}

myvendor_gnss_off_t myvendor_gnss_prepare_poweroff(void)
{
  myvendor_sys_gnss_t fix;
  uint32_t start;

  if (!g_started) {
    return MYVENDOR_GNSS_OFF_SKIP;
  }

  if (g_idle_sleep) {
    syslog(LOG_INFO, "gnss: skip dbd dump at poweroff (idle sleep)\n");
    return MYVENDOR_GNSS_OFF_SKIP_IDLE;
  }

  if (myvendor_is_factory()) {
    syslog(LOG_INFO, "gnss: skip dbd dump at poweroff (factory)\n");
    return MYVENDOR_GNSS_OFF_SKIP_FACTORY;
  }

  if (!myvendor_devctl_eph_auto_get()) {
    syslog(LOG_INFO, "gnss: skip dbd dump at poweroff (eph auto off)\n");
    return MYVENDOR_GNSS_OFF_SKIP_AUTO;
  }

  if (!myvendor_gnss_get(&fix) || !fix.valid) {
    syslog(LOG_INFO, "gnss: skip dbd dump at poweroff (no fix)\n");
    return MYVENDOR_GNSS_OFF_SKIP_NOFIX;
  }

  pthread_mutex_lock(&g_lock);
  g_dump_halt = true;
  g_dump_req = true;
  g_dump_done = false;
  pthread_mutex_unlock(&g_lock);

  /* 倒库在 GNSS 线程；这里只等到完成。无 DBD 时线程 5 秒会 mark_done。 */
  start = gnss_now_ms();
  while (gnss_elapsed_ms(gnss_now_ms(), start) < 12000u) {
    bool done;
    uint32_t elapsed;

    pthread_mutex_lock(&g_lock);
    done = g_dump_done;
    pthread_mutex_unlock(&g_lock);
    if (done) {
      return MYVENDOR_GNSS_OFF_DUMPED;
    }

    elapsed = gnss_elapsed_ms(gnss_now_ms(), start);
    if (g_dbd_n < (int)GNSS_DBD_MIN_FRAMES && elapsed >= GNSS_DBD_NODATA_MS) {
      syslog(LOG_INFO, "gnss: skip dbd dump at poweroff (no data 5s)\n");
      return MYVENDOR_GNSS_OFF_SKIP_NODATA;
    }

    myvendor_watchdog_ui_beat();
    myvendor_watchdog_work_beat();
    myvendor_watchdog_hw_pet();
    gnss_sleep_ms(20);
  }

  syslog(LOG_WARNING, "gnss: dbd dump timeout at poweroff\n");
  return MYVENDOR_GNSS_OFF_TIMEOUT;
}

int myvendor_gnss_dump_request(void)
{
  myvendor_sys_gnss_t fix;

  if (!g_started) {
    return -ENODEV;
  }

  if (myvendor_is_factory()) {
    return -EACCES;
  }

  if (g_idle_sleep) {
    return -ENODATA;
  }

  if (g_dbd_run) {
    return -EBUSY;
  }

  if (!myvendor_gnss_get(&fix) || !fix.valid) {
    return -ENODATA;
  }

  pthread_mutex_lock(&g_lock);
  if (g_dump_req || g_eph_busy) {
    pthread_mutex_unlock(&g_lock);
    return -EBUSY;
  }

  g_dump_halt = false;
  g_dump_req = true;
  g_dump_done = false;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

bool myvendor_gnss_dump_busy(void)
{
  bool busy;

  /* 两个字段都在锁内读：原来 `g_dbd_run` 是解锁之后裸读的（审计发现），
   * 和读线程写它并发 —— 判"倒库忙不忙"正是要靠它。 */
  pthread_mutex_lock(&g_lock);
  busy = g_dump_req || g_dbd_run;
  pthread_mutex_unlock(&g_lock);
  return busy;
}

bool myvendor_gnss_dump_last_path(char *buf, size_t n)
{
  return gnss_path_get(g_dump_last_path, buf, n);
}

bool myvendor_gnss_inject_last_path(char *buf, size_t n)
{
  return gnss_path_get(g_inject_last_path, buf, n);
}

void myvendor_gnss_dynmodel_set(uint8_t model)
{
  if (model != GNSS_DYN_BIKE && model != GNSS_DYN_AUTO) {
    return;
  }

  g_dyn_want = model;
}

void myvendor_gnss_dyn_still_enable(bool on)
{
  g_dyn_still_on = on;
  syslog(LOG_INFO, "gnss: still switch %s\n", on ? "on" : "off");
}

void myvendor_gnss_ui_ready(void)
{
  g_ui_ready = true;
}

int myvendor_gnss_inject_request(void)
{
  if (!g_started) {
    return -ENODEV;
  }

  if (g_dbd_run) {
    return -EBUSY;
  }

  pthread_mutex_lock(&g_lock);
  if (g_dump_req || g_eph_busy || g_eph_reload) {
    pthread_mutex_unlock(&g_lock);
    return -EBUSY;
  }

  g_eph_reload = true;
  g_eph_busy = true;
  g_inject_last_path[0] = '\0';
  pthread_mutex_unlock(&g_lock);
  return 0;
}

bool myvendor_gnss_diag_ok(void)
{
  uint32_t last;
  uint32_t now;

  if (!g_started) {
    return true;
  }

  if (g_idle_sleep || g_idle_parked) {
    return true;
  }

  if (!gnss_thread_ok(GNSS_THREAD_STALL_MS)) {
    gnss_diag_why("thread", 0);
    return false;
  }

  now = gnss_now_ms();
  if (gnss_hold_unlocked(now)) {
    return true;
  }

  pthread_mutex_lock(&g_lock);
  if (g_eph_busy || g_dbd_run || gnss_eph_grace_locked(now)) {
    pthread_mutex_unlock(&g_lock);
    return true;
  }

  pthread_mutex_unlock(&g_lock);

  if (g_recovering &&
      gnss_elapsed_ms(now, g_recover_ms) < GNSS_RECOVER_MS) {
    return true;
  }

  last = g_nmea_life_ms;
  if (last == 0) {
    gnss_diag_why("no-nmea", 0);
    return false;
  }

  {
    uint32_t age = gnss_elapsed_ms(now, last);

    if (age >= 8000u) {
      gnss_diag_why("nmea-age", age);
      return false;
    }
  }

  return true;
}

int myvendor_gnss_diag_restart(void)
{
  if (!g_started) {
    return -ENODEV;
  }

  if (g_idle_sleep) {
    return 0;
  }

  g_diag_restarts++;
  gnss_dbd_abort();
  gnss_snapshot_dead();

  if (!gnss_thread_ok(GNSS_THREAD_STALL_MS)) {
    syslog(LOG_ERR, "gnss: diag thread respawn\n");
    g_restart_why = "diag-thread-stall";
    return gnss_thread_restart();
  }

  syslog(LOG_WARNING, "gnss: diag restart (" GNSS_MOD_PN " VCC cycle, UART open)\n");
  /* 诊断：diag 侧的判据快照，说明它凭什么认为需要恢复。 */
  syslog(LOG_WARNING,
         "gnss: diag restart why nmea_age=%u fail=%u recov=%d eph=%d dbd=%d "
         "episodes=%u\n",
         g_nmea_life_ms != 0
           ? (unsigned)gnss_elapsed_ms(gnss_now_ms(), g_nmea_life_ms) : 0u,
         (unsigned)g_uart_fails, g_recovering ? 1 : 0, g_eph_busy ? 1 : 0,
         g_dbd_run ? 1 : 0, (unsigned)g_rt_episodes);
  gnss_recover_begin();
  g_diag_kick = true;

  /* 同上：置位之后必须捅一下，否则读线程阻塞在 poll() 里看不到这个 kick。 */
  gnss_reader_wake();
  return 0;
}

int myvendor_gnss_crash_format(char *buf, size_t n)
{
  if (buf == NULL || n < 8)
    {
      return 0;
    }

  return snprintf(buf, n,
                  "gnss last=%s valid=%d idle=%d parked=%d eph=%d dbd=%d recov=%d fd=%d nmea_life=%u fix_ms=%u phase=%s pid=%d/%d busy=%d",
                  g_link_last[0] != '\0' ? g_link_last : "-",
                  g_fix.valid ? 1 : 0,
                  g_idle_sleep ? 1 : 0, g_idle_parked ? 1 : 0,
                  g_eph_busy ? 1 : 0, g_dbd_run ? 1 : 0,
                  g_recovering ? 1 : 0, g_uart_fd,
                  (unsigned)g_nmea_life_ms, (unsigned)g_fix_ms,
                  g_gnss_phase, (int)g_gnss_pid, (int)g_gnss_pid_boot,
                  g_gnss_stack_busy ? 1 : 0);
}

#else /* !CONFIG_BOARD_L96_GNSS */

int myvendor_gnss_start(void)
{
  return -ENOTSUP;
}

bool myvendor_gnss_get(myvendor_sys_gnss_t *out)
{
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
  }

  return false;
}

bool myvendor_gnss_eph_times(uint32_t *last_utc, uint32_t *next_utc)
{
  if (last_utc != NULL) {
    *last_utc = 0;
  }

  if (next_utc != NULL) {
    *next_utc = 0;
  }

  return false;
}

void myvendor_gnss_eph_reload(void)
{
}

void myvendor_gnss_eph_maintain(void)
{
}

bool myvendor_gnss_eph_busy(void)
{
  return false;
}

myvendor_gnss_off_t myvendor_gnss_prepare_poweroff(void)
{
  return MYVENDOR_GNSS_OFF_SKIP_FACTORY;
}

void myvendor_gnss_idle_sleep(bool sleep)
{
  (void)sleep;
}

bool myvendor_gnss_idle_get(void)
{
  return false;
}

int myvendor_gnss_dump_request(void)
{
  return -ENOTSUP;
}

bool myvendor_gnss_dump_busy(void)
{
  return false;
}

bool myvendor_gnss_dump_last_path(char *buf, size_t n)
{
  if (buf != NULL && n > 0) {
    buf[0] = '\0';
  }

  return false;
}

bool myvendor_gnss_inject_last_path(char *buf, size_t n)
{
  if (buf != NULL && n > 0) {
    buf[0] = '\0';
  }

  return false;
}

int myvendor_gnss_inject_request(void)
{
  return -ENOTSUP;
}

bool myvendor_gnss_diag_ok(void)
{
  return true;
}

int myvendor_gnss_diag_restart(void)
{
  return -ENOTSUP;
}

void myvendor_gnss_dynmodel_set(uint8_t model)
{
  (void)model;
}

void myvendor_gnss_dyn_still_enable(bool on)
{
  (void)on;
}

void myvendor_gnss_ui_ready(void)
{
}

int myvendor_gnss_crash_format(char *buf, size_t n)
{
  if (buf != NULL && n > 0)
    {
      buf[0] = '\0';
    }

  return 0;
}

#endif
