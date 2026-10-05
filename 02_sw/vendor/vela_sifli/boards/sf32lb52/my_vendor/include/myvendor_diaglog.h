/**
 * @file myvendor_diaglog.h
 * @brief 日志落盘：RAM 环缓冲 + 定期写入 /mnt/kv/diag/（**自己的目录**，与崩溃转储
 *        /mnt/kv/coredump 分开；命名沿用 coredump 那套，前缀 d）。
 *
 * 实现方式是注册一个 **syslog channel**，只往 RAM 环里搬字节、不做任何 I/O，
 * 因此：
 *   - 所有进 syslog 的输出自动进环，**不需要在每个调用点埋点**；
 *   - 可以在中断上下文里被调用（sc_write_force 走同一条路）。
 *
 * **只留异常级别**：环里只收 <= DIAGLOG_MAX_SEV（默认 LOG_WARNING）的行，
 * 依据是 CONFIG_SYSLOG_PRIORITY 插进消息前缀的 `[ ERROR] ` 之类记号；不带
 * 记号的续行继承上一行的判定。info/debug 的例行刷屏既不占环也不写卡。
 *
 * 落盘策略（按规格）：
 *   - **每次开机一个文件**；同一次开机跨天则**按天再开一个**；
 *   - 目录里**最多 10 个**，超出按时间删最旧（与 coredump 同一套做法）；
 *     运行期还会周期性补清一次（myvendor_diaglog_prune()），并把 `seq` 与日志
 *     之外的东西一律当垃圾清掉；
 *   - 死机时由 coredump 那条路把环里尚未落盘的内容一起写出去
 *     （见 myvendor_diaglog_snapshot()）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MYVENDOR_DIAGLOG_H
#define __MYVENDOR_DIAGLOG_H

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief 注册 syslog 通道并准备目录。开机早期调用一次即可。
 *
 * @return 0 成功；负 errno。
 */
int myvendor_diaglog_start(void);

/**
 * @brief 异常日志通道是否已装好（start 成功过）。
 *
 * @details
 * `false` 意味着**一条日志都不会落盘**（flush 直接 -EAGAIN）。上电时 start 失败
 * 曾经是静默的（错误经 `serr()`，本构建编译为空），于是整轮开机没有任何日志、
 * 却看不出原因。`test diag` 用这个函数把"通道没装"和"装了但写不进去"一次分开。
 * flush 现在也会每 10 s 自己重试一次 start（自愈）。
 */
bool myvendor_diaglog_started(void);

/**
 * @brief 把 RAM 环里尚未落盘的内容追加到当前 diag 文件（线程上下文）。
 *
 * @details 由 diag 线程定期调用。会先按需滚动文件名（开机/按天）并清理超量
 *          的旧文件。环满时丢新字节、不阻塞 —— 日志绝不允许拖慢调用者。
 *
 * @return 本次写出的字节数；<0 为错误（写失败不算致命，留到下次再试）。
 */
int myvendor_diaglog_flush(void);

/**
 * @brief 环内尚未落盘的字节数（纯内存读，无 I/O）。
 *
 * @details 给调用方做**落盘节流**用：不必为了判断"有没有积压"去开文件。
 *          只读两个索引，用于阈值判断足够了。
 *
 * @return 环内待写字节数（0 表示已落盘干净）。
 */
size_t myvendor_diaglog_pending(void);

/**
 * @brief 运行期周期性清理入口（线程上下文；内部限频，默认 10 分钟）。
 *
 * @details 为什么需要它（2026-09-25 用户反馈"diag 文件一直累加"）：清理原先只在
 *          **开机**与**按天滚动**时跑 —— 只要设备不重启、不跨天，目录就只涨不落；
 *          而且旧实现失败时静默 break，堆到把 `/mnt/kv` 撑满也无人知晓。
 *          本入口由 diag 线程每拍调用，自身限频，超量时按**最小序号/最老戳**删到
 *          `DIAGLOG_KEEP` 个，失败一律 WARNING 出声（与 `[coredump] prune stuck` 同形）。
 *          顺带兼容早期命名 `n*`（本目录曾沿用 coredump 的前缀）。
 *
 * @return 0（清理失败不影响调用方）。
 */
int myvendor_diaglog_prune(void);

/**
 * @brief 取一份环内文本快照（纯内存拷贝，无 I/O）。
 *
 * @details 给 coredump 那条路用：它自己有经过验证的、裸 LittleFS 的崩溃写路径，
 *          所以由它把这份快照随寄存器转储一起写进文件。本函数可以在很糟的
 *          上下文里被调用，因此只做内存拷贝。
 *
 * @param[out] out 目标缓冲。
 * @param[in]  n   缓冲长度。
 * @return 实际拷贝的字节数（按时间顺序，最多 n-1，末尾补 '\0'）。
 */
size_t myvendor_diaglog_snapshot(char *out, size_t n);

/**
 * @brief 过滤器计数探针（`test diag` 用）：把"哪一段断了"变成可读的数字。
 *
 * @details 目录里没有新文件时，三种断点必须分开：
 *          - `feed_bytes` **不涨** ⇒ 通道根本没被调用（注册没生效/被挤掉）；
 *          - 涨了但 `keep_lines` 不涨、`last_sev < 0` ⇒ 级别记号没认出来
 *            （前缀格式变了；此时会一直沿用上一行判定，一行判"丢"就全都丢）；
 *          - `keep_lines` 涨了却不落盘（`ring_bytes` 不归零）⇒ flush 写不出去。
 *          返回值都是**累计量**，调用方取两次之差即可。
 *
 * @param[out] feed_bytes 进过滤器的字节数（可 NULL）。
 * @param[out] keep_lines 判定"留"的行数（可 NULL）。
 * @param[out] drop_lines 判定"丢"的行数（可 NULL）。
 * @param[out] last_sev   最近判定出的级别（LOG_*；-1 = 那行没有级别记号）（可 NULL）。
 * @param[out] ring_bytes 环内尚未落盘的字节数（可 NULL）。
 */
void myvendor_diaglog_probe(unsigned int *feed_bytes, unsigned int *keep_lines,
                            unsigned int *drop_lines, int *last_sev,
                            unsigned int *ring_bytes);

/**
 * @brief 过滤器最近"看到"的原始字节（滚动窗口，非可打印字节转义成 `\xNN`）。
 *
 * @details 给 `test diag` 用：当 `feed` 在涨、`keep` 却恒为 0、`last_sev=-1`
 *          时，说明**级别记号一个都没认出来**，此时唯一能定因的办法就是看
 *          实际字节 —— 前缀里有没有 `[  WARN]`、颜色码/时间戳的顺序如何，
 *          一眼即可判断是记号写法不同，还是前缀压根没进这条通道。
 *
 * @param[out] out 目标缓冲（内容按时间顺序，非可打印字节转义）。
 * @param[in]  n   缓冲长度。
 * @return 写出的字符数（不含结尾 '\0'）。
 */
size_t myvendor_diaglog_tail(char *out, size_t n);

/**
 * @brief 最近一次判定时**扫描器实际看到**的那串行首（快照，同样做转义）。
 *
 * @details 与 myvendor_diaglog_tail() 配合定位：
 *          - 两者都含 `[  WARN]` 却 keep=0 ⇒ 判级别/比对的代码有问题；
 *          - 原始字节里有、快照里没有 ⇒ 攒行首的过程丢了（PFX_MAX 截断）；
 *          - 两者都没有 ⇒ 记号压根没进这条通道（前缀不在本通道的 buffer 里）。
 *
 * @param[out] out 目标缓冲。
 * @param[in]  n   缓冲长度。
 * @return 写出的字符数（不含结尾 '\0'）。
 */
size_t myvendor_diaglog_lastpfx(char *out, size_t n);

#endif /* __MYVENDOR_DIAGLOG_H */
