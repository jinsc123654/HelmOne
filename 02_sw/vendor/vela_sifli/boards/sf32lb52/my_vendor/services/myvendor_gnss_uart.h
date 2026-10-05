/****************************************************************************
 * vendor/my_vendor/boards/sf32lb52/my_vendor/services/myvendor_gnss_uart.h
 *
 * GNSS 串口的**直连数据面**（不经 NuttX serial 框架）。
 *
 * 背景（为什么要另开一条路）：
 *   原先 GNSS 读线程 `open("/dev/ttyS0")` + `poll()` + `read()`。poll 这条路在
 *   NuttX 里有两个我们反复踩到的性质：
 *     1) 驱动侧要为 poll 长期保留一份 pollfd 注册；读线程被判 stranded、栈被回收
 *        之后，那份注册仍指向死栈，ISR 一回调就是
 *        `nxsem_get_value(已被复用成 mutex 的内存)` → `DEBUGASSERT(!NXSEM_IS_MUTEX)`
 *        → panic（n587 / n588 / n590 同族）。
 *     2) poll 的超时由 tick/wdog 驱动，`g_wdactivelist` 被剪掉就永不到期。
 *   本模块改用 SiFli SDK 那一套数据面模型（HAL 的 DMA 环形 + IDLE 中断收数，
 *   ISR 推环形缓冲 + 信号量，消费者**阻塞等信号量**）：没有 pollfd 注册、
 *   没有 tick 超时、没有被信号量之外的任何东西握住的内存。
 *
 * 数据流：USART2 DMA/IDLE ISR
 *            → sifli_uart.c 的 sifli_uart_rx_put()
 *            → myvendor_gnss_uart_feed()      （本模块，ISR 上下文）
 *            → 环形缓冲 + sem_post
 *            → myvendor_gnss_uart_read()      （GNSS 读线程，阻塞）
 *
 * 本文件只依赖 libc + NuttX 的信号量；发送借用芯片驱动的直连接口
 * （`sifli_uart_direct_tx()`，同一个 HAL 句柄，阻塞发小帧）。
 ****************************************************************************/

#ifndef __MYVENDOR_GNSS_UART_H
#define __MYVENDOR_GNSS_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 环形缓冲容量（2 的幂；掩码取模）。 */
#define MYVENDOR_GNSS_UART_RING 8192u

/** @brief 计数（诊断用；只在 feed/read 里更新，读侧允许脏读）。 */
struct myvendor_gnss_uart_stats
{
  uint32_t rx_bytes;   /**< 已进环的字节数。 */
  uint32_t rx_drop;    /**< 环满丢弃的字节数（丢最新，让解析器按同步字重对齐）。 */
  uint32_t feed_n;     /**< ISR 喂入次数。 */
  uint32_t wait_n;     /**< 消费者进入等待的次数。 */
  uint32_t wait_eintr; /**< 等待被信号打断的次数（diag 踢 / 收工）。 */
  uint32_t tx_bytes;   /**< 已发出的字节数。 */
  uint32_t tx_err;     /**< 发送失败次数。 */
};

/**
 * @brief 置活并清计数；可重复调用（幂等）。
 * @return 0 成功，负值为错误。
 */
int myvendor_gnss_uart_start(void);

/** @brief 置停：feed 直接丢弃，读者被唤醒后看到 0 字节。 */
void myvendor_gnss_uart_stop(void);

/** @brief 当前可读字节数（不等待）。 */
size_t myvendor_gnss_uart_avail(void);

/**
 * @brief 阻塞读；被信号打断时返回 0（不是错误）。
 *
 * **只在主读循环里用**（愿意一直等到有数据；周期性的记账由 diag 的 SIGUSR1
 * 踢回来）。短等待一律用下面的 @ref myvendor_gnss_uart_read_timeout ——
 * 它保证"无论如何都会在 timeout_ms 后返回"，不受计时基准影响。
 *
 * @param buf 目标缓冲。
 * @param n   最多读多少字节。
 * @return 实际读到的字节数；0 = 被打断或未置活。
 */
size_t myvendor_gnss_uart_read(void *buf, size_t n);

/**
 * @brief 有界读：最多等 timeout_ms，然后无论有没有数据都返回。
 *
 * 语义对应"旧的 poll(timeout_ms) + 非阻塞读"：有数据就取走，没有就返回 0。
 * 内部用**按次数自旋**（每次 1 ms）而不是 tick 驱动的超时，所以计时基准
 * （SysTick / wdog 链）被破坏时也一定会返回。
 *
 * @return 实际读到的字节数；0 = 超时无数据（计一次 wait）。
 */
size_t myvendor_gnss_uart_read_timeout(void *buf, size_t n, uint32_t timeout_ms);

/**
 * @brief 发送（阻塞，帧很小）；供 UBX / NMEA 命令用。
 * @return 0 成功，负值为错误。
 */
int myvendor_gnss_uart_write(const void *buf, size_t n);

/** @brief 丢弃待读数据（重新同步 / VCC 重上电后用）。 */
void myvendor_gnss_uart_flush(void);

/** @brief 取计数快照。 */
void myvendor_gnss_uart_stats(struct myvendor_gnss_uart_stats *out);

/**
 * @brief 打一行本模块的计数器（`DRIVER=direct cnt …`；`ctl cnt gnss` 的报表用）。
 *
 * 只打印，不动状态；由 diag 线程调用（中断侧只累加计数）。
 */
void myvendor_gnss_uart_cnt_report(void);

/**
 * @brief 异常直报：计数一旦异常增长就**立刻** `LOG_ERR` 打一行（每拍由 diag 调）。
 *
 * **不受 `ctl cnt` 开关影响** —— 周期报表可以关，异常不行；用 ERROR 级也是
 * 为了在 `ctl log err`（屏蔽位只留错误）下仍然可见。
 */
void myvendor_gnss_uart_check_anomaly(void);

/**
 * @brief ISR 入口：把刚收到的字节推进环形并唤醒读者。
 *
 * 由 `chips/sf32lb52/sifli_uart.c` 在 USART2 的收数路径上调用（弱符号，
 * 没有本模块的构建里自动变成空操作）。
 */
void myvendor_gnss_uart_feed(const uint8_t *buf, size_t n);

#endif /* __MYVENDOR_GNSS_UART_H */
