/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/services/myvendor_gnss_uart.c
 *
 * GNSS 串口直连数据面（见 .h 里的背景）。设计约束，逐条对着写：
 *
 *   1) **ISR 里不做任何可能阻塞的事**：只搬字节 + `sem_post`（NuttX 允许在
 *      中断里 post）。不 syslog（会拖慢 ISR，也会和 diag 抢锁）。
 *   2) **环满丢最新**（不是丢最旧）：字节流的解析器本来就靠同步字重对齐，
 *      保住已进环的那段连续流比保住最新一个字节更值。
 *   3) **单生产者/单消费者**：索引只由各自一方前进，读者只读 `[rd, wr)`；
 *      写者只在 wr 之上写。索引用 32 位单调计数、按掩码取模，天然处理回卷。
 *   4) **读者只等信号量**：没有 pollfd 注册、没有 tick 超时；`sem_wait` 被
 *      信号打断（diag 的 SIGUSR1 踢）时返回 0 让调用者去做记账/心跳。
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>

#include <errno.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "myvendor_gnss_uart.h"

/* 芯片驱动的直连接口（同一个 USART2 HAL 句柄，阻塞发小帧）。 */
extern int sifli_uart_direct_tx(const uint8_t *buf, size_t n);

#define GNSS_UART_MASK (MYVENDOR_GNSS_UART_RING - 1u)

static uint8_t g_ring[MYVENDOR_GNSS_UART_RING] __attribute__((aligned(16)));

/* 单调递增的读写游标：只由各自一方修改（见文件头第 3 条）。 */
static volatile uint32_t g_wr;
static volatile uint32_t g_rd;

static sem_t g_rx_sem;
static bool  g_sem_ready;
static volatile bool g_active;

static struct myvendor_gnss_uart_stats g_stats;
static uint32_t g_rep_bytes;   /* 报告节奏：每 N 字节一条（按**字节**，不是读次数） */

/** @brief 报"当前走的是哪个驱动"：一条带 DRIVER= 标签 + 全部计数的行。
 *
 *  为什么要有它：新旧两条取数路径（`DRIVER=poll` 走 NuttX serial + poll；
 *  `DRIVER=direct` 走本模块的环形+信号量）在日志里必须一眼可分 —— 否则
 *  "换了驱动没有"只能靠读代码。判据是**成对**的：
 *    · `DRIVER=direct` 出现且 rx/feed 在涨；
 *    · 同时 `DRIVER=poll` 那一行不再增长（旧路径的 poll 计数停在原地）。 */
static void gnss_uart_report(const char *why)
{
  /* 级别：**INFO**。这条是"计数在怎么走"的信息（默认屏蔽位 0x7f 下可见），
   * 不是告警 —— WARN/ERROR 留给真异常（丢字节/发送失败会以 ERROR 直报，
   * 且不受 `ctl cnt` 开关影响）。 */
  syslog(LOG_INFO,
         "gnss_uart: DRIVER=direct %s rx=%u feed=%u drop=%u wait=%u eintr=%u tx=%u txerr=%u\n",
         why,
         (unsigned)g_stats.rx_bytes,
         (unsigned)g_stats.feed_n,
         (unsigned)g_stats.rx_drop,
         (unsigned)g_stats.wait_n,
         (unsigned)g_stats.wait_eintr,
         (unsigned)g_stats.tx_bytes,
         (unsigned)g_stats.tx_err);
}

static inline void gnss_uart_barrier(void)
{
  __asm__ __volatile__("dmb sy" ::: "memory");
}

int myvendor_gnss_uart_start(void)
{
  if (!g_sem_ready)
    {
      if (sem_init(&g_rx_sem, 0, 0) != 0)
        {
          syslog(LOG_ERR, "gnss_uart: sem_init fail errno=%d\n", errno);
          return -errno;
        }

      g_sem_ready = true;
    }

  g_rd = 0;
  g_wr = 0;
  g_rep_bytes = 0;
  memset(&g_stats, 0, sizeof(g_stats));
  g_active = true;

  /* 置活即报一次：新驱动的"我上线了"证据（含驱动标签）。 */
  gnss_uart_report("start(ring=8192, no poll, no tick timeout)");
  return 0;
}

void myvendor_gnss_uart_stop(void)
{
  g_active = false;

  /* 把可能正在等的读者叫醒（它会看到 avail=0 且 !active 后返回）。 */
  if (g_sem_ready)
    {
      (void)sem_post(&g_rx_sem);
    }
}

size_t myvendor_gnss_uart_avail(void)
{
  return (size_t)(g_wr - g_rd);
}

void myvendor_gnss_uart_feed(const uint8_t *buf, size_t n)
{
  uint32_t wr;
  uint32_t rd;
  size_t   i;
  bool     was_empty;

  if (!g_active || buf == NULL || n == 0)
    {
      return;
    }

  g_stats.feed_n++;

  wr = g_wr;
  rd = g_rd;
  was_empty = (wr == rd);

  for (i = 0; i < n; i++)
    {
      if ((uint32_t)(wr - rd) >= GNSS_UART_MASK)
        {
          /* 环满：丢最新，计数（第 2 条）。 */
          g_stats.rx_drop++;
          continue;
        }

      g_ring[wr & GNSS_UART_MASK] = buf[i];
      wr++;
      g_stats.rx_bytes++;
    }

  if (wr != g_wr)
    {
      gnss_uart_barrier();
      g_wr = wr;

      /* 只在"由空转非空"时 post：计数信号量涨多了会盖住"还有数据"这个事实。 */
      if (was_empty && g_sem_ready)
        {
          (void)sem_post(&g_rx_sem);
        }
    }
}

size_t myvendor_gnss_uart_read(void *buf, size_t n)
{
  uint8_t *dst = (uint8_t *)buf;
  uint32_t wr;
  uint32_t rd;
  uint32_t avail;
  uint32_t i;

  if (buf == NULL || n == 0 || !g_sem_ready)
    {
      return 0;
    }

  for (;;)
    {
      wr    = g_wr;
      rd    = g_rd;
      avail = wr - rd;

      if (avail > 0)
        {
          if (avail > n)
            {
              avail = n;
            }

          for (i = 0; i < avail; i++)
            {
              dst[i] = g_ring[(rd + i) & GNSS_UART_MASK];
            }

          gnss_uart_barrier();
          g_rd = rd + avail;

          /* 报告节奏按**字节**（不是读次数）：读者是逐字节读的，按次数节流会
           * 一秒刷十几行（2026-09-24 实测踩过）。64 KB @ ~800 B/s ≈ 80 s 一条。 */
          if (g_rep_bytes == 0u)
            {
              gnss_uart_report("first-data");
            }
          else if ((g_stats.rx_bytes / 65536u) != (g_rep_bytes / 65536u))
            {
              gnss_uart_report("tick");
            }

          g_rep_bytes = g_stats.rx_bytes;
          return (size_t)avail;
        }

      if (!g_active)
        {
          return 0;
        }

      g_stats.wait_n++;
      if (sem_wait(&g_rx_sem) < 0)
        {
          /* 被信号打断（diag 踢 / 收工）：返回 0，让调用者去做记账与状态检查。
           * 这不是错误，所以不区分 EINTR 与别的 errno —— 上层每次都会重新
           * 看 avail()，真丢数据由 rx_drop 计。 */
          g_stats.wait_eintr++;
          return 0;
        }
    }
}

size_t myvendor_gnss_uart_read_timeout(void *buf, size_t n, uint32_t timeout_ms)
{
  uint8_t *dst = (uint8_t *)buf;
  uint32_t wr;
  uint32_t rd;
  uint32_t avail;
  uint32_t i;
  uint32_t spins;

  if (buf == NULL || n == 0 || !g_sem_ready)
    {
      return 0;
    }

  /* 先看有没有现成的：绝大多数短等待在这里立刻返回，不进入自旋。 */
  wr    = g_wr;
  rd    = g_rd;
  avail = wr - rd;

  if (avail == 0 && timeout_ms > 0)
    {
      /** 有界等待：**按次数**自旋（tick 只影响每次睡多久，不影响"会不会退出"）。
       *
       *  这里刻意不用 `sem_timedwait` / poll 那类"由 tick 驱动的超时" —— 那正是
       *  n587/n588/n590 那条链的成因之一（`g_wdactivelist` 被剪则超时永不到期）。
       *  自旋上限 = timeout_ms 次，每次 1 ms ⇒ 最坏也就 timeout_ms 次后必返回，
       *  与计时基准无关。 */
      for (spins = 0; spins < timeout_ms; spins++)
        {
          usleep(1000);

          wr    = g_wr;
          rd    = g_rd;
          avail = wr - rd;
          if (avail > 0)
            {
              break;
            }
        }
    }

  if (avail == 0)
    {
      g_stats.wait_n++;
      return 0;
    }

  if (avail > n)
    {
      avail = n;
    }

  for (i = 0; i < avail; i++)
    {
      dst[i] = g_ring[(rd + i) & GNSS_UART_MASK];
    }

  gnss_uart_barrier();
  g_rd = rd + avail;
  return (size_t)avail;
}

int myvendor_gnss_uart_write(const void *buf, size_t n)
{
  int ret;

  if (buf == NULL || n == 0)
    {
      return -EINVAL;
    }

  ret = sifli_uart_direct_tx((const uint8_t *)buf, n);
  if (ret < 0)
    {
      g_stats.tx_err++;
      return ret;
    }

  g_stats.tx_bytes += (uint32_t)n;
  return 0;
}

void myvendor_gnss_uart_flush(void)
{
  gnss_uart_barrier();
  g_rd = g_wr;
}

void myvendor_gnss_uart_stats(struct myvendor_gnss_uart_stats *out)
{
  if (out != NULL)
    {
      *out = g_stats;
    }
}

/** @brief 周期报表（`ctl cnt gnss`）：复用启动时那条带 DRIVER= 标签的格式。 */
void myvendor_gnss_uart_cnt_report(void)
{
  gnss_uart_report("cnt");
}

/** @brief 异常直报（每拍调；**不受 `ctl cnt` 开关影响**）。
 *
 *  这里的计数一旦增长就说明有东西坏了，必须**立刻**以 `LOG_ERR` 报出来 ——
 *  用 ERROR 级还有个实际理由：`ctl log err` 把系统屏蔽位收紧到只留错误时，
 *  WARN 级的周期报表会消失，而错误仍然可见（"异常打印不会被关"）。
 *
 *  当前判据：
 *    · `rx_drop` 增长 —— 环形满丢字节（丢最新）。**数据真丢了**，对齐可能被破坏；
 *    · `tx_err`  增长 —— 发送失败（HAL 拒绝/HAL_OK 之外），UBX 命令或星历没发出去。
 *  两者都在中断侧只累加，打印在这里（线程）。
 */
void myvendor_gnss_uart_check_anomaly(void)
{
  static uint32_t s_drop_seen;
  static uint32_t s_txerr_seen;
  static uint32_t s_log_ms;
  uint32_t drop;
  uint32_t txerr;
  uint32_t now;

  drop = g_stats.rx_drop;
  txerr = g_stats.tx_err;

  if (drop == s_drop_seen && txerr == s_txerr_seen)
    {
      return;
    }

  /* 限频 1 s：持续溢出/持续发送失败时，把这一秒的增量一起报（不然 diag 每拍
   * 500 ms 就一条，2 行/秒刷屏）。计数不会丢 —— 增量按"上次报过的值"算。 */
  now = (uint32_t)TICK2MSEC(clock_systime_ticks());
  if (s_log_ms != 0u && (int32_t)(now - s_log_ms) < 1000)
    {
      return;
    }

  s_log_ms = now ? now : 1u;

  if (drop != s_drop_seen)
    {
      syslog(LOG_ERR,
             "gnss_uart: ERROR rx ring overflow, dropped %lu byte(s) (total %lu) "
             "rx=%lu feed=%lu — 解析可能失步\n",
             (unsigned long)(drop - s_drop_seen), (unsigned long)drop,
             (unsigned long)g_stats.rx_bytes, (unsigned long)g_stats.feed_n);
      s_drop_seen = drop;
    }

  if (txerr != s_txerr_seen)
    {
      syslog(LOG_ERR,
             "gnss_uart: ERROR tx failed %lu time(s) (total %lu) tx=%lu — "
             "UBX/星历可能没发出去\n",
             (unsigned long)(txerr - s_txerr_seen), (unsigned long)txerr,
             (unsigned long)g_stats.tx_bytes);
      s_txerr_seen = txerr;
    }
}
