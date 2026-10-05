/****************************************************************************
 * vendor/my_vendor/chips/sf32lb52/include/myvendor_idle_stat.h
 *
 * True CPU idle share measured in up_idle() (WFI + DWT), not NuttX cpuload.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef MYVENDOR_IDLE_STAT_H
#define MYVENDOR_IDLE_STAT_H

#include <stddef.h>
#include <stdint.h>

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C" {
#endif

struct myvendor_idle_stat_snap_s
{
  uint32_t true_idle_x10;   /* 0..1000 = 0.0%..100.0% (DWT in WFI) */
  uint32_t ps_idle_x10;     /* NuttX cpuload PID0 inverted, same scale */
  uint32_t win_ms;
  uint64_t idle_cyc;
  uint64_t total_cyc;
};

void myvendor_idle_stat_init(void);
void myvendor_idle_stat_window_begin(void);
void myvendor_idle_stat_on_wfi(uint32_t cycles);
void myvendor_idle_stat_snapshot(FAR struct myvendor_idle_stat_snap_s * snap);

/**
 * @brief 在 addr 上挂一个 DWT 数据观察点（给写内存的人点名）。
 *
 * @param addr       被观察的地址（4 字节对齐）。
 * @param size_bytes 1 / 2 / 4，匹配窗口大小。
 * @return 0 成功；-ENOSPC 四个比较器占满；-EINVAL 参数不合法。
 *
 * 命中后硬件把 `DWT_FUNCTIONn.MATCHED` 置位（事后 `ctl wt` 可查），并且
 * （本板无调试器时）经 exception_common 走既有 coredump，`pc` 即那条访问
 * 指令的地址。DWT 接在核的 load/store 通路上，**DMA 写不会命中**。
 */
int myvendor_dwt_watch_arm(uint32_t addr, unsigned size_bytes);

/** @brief 撤掉全部观察点（含 MATCHED 位）。 */
void myvendor_dwt_watch_clear(void);

/**
 * @brief 把观察点现场写进调用方缓冲（每个比较器的地址/大小/MATCHED）。
 * @return 写出的字节数（正常 >0）。
 */
int myvendor_dwt_watch_status(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* MYVENDOR_IDLE_STAT_H */
