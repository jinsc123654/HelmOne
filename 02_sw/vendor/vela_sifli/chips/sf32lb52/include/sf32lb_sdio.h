/**
 * @file sf32lb_sdio.h
 * @brief SF32LB52 SD/eMMC（SDIO1）MTD 窗口与卷挂载。
 *
 * 三卷，互不共用 superblock：
 * - `/dev/sdkv`     → `/mnt/kv`（KV_REGION，LittleFS，persist / OTA）
 * - `/dev/sd0`      → `/mnt/lfs`（FS_REGION，LittleFS，MTP / BLE / GPX）
 * - `/dev/mtdblock0`→ `/mnt/fat`（FAT_REGION；产品 MS_RDONLY，工厂 RW；根下 map/ fonts/）
 *
 * FAT_REGION `max_size=0` 表示卡余量。用户 LFS 固定 512 MiB，不再 grow。
 * `/mnt/lfs` 不 autoformat；`/mnt/kv` 空白切片仍 autoformat。
 * 分区见 `docs/sd_partition.md`。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SF32LB_SDIO_H__
#define __SF32LB_SDIO_H__

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdbool.h>
#include <stdint.h>

#define SF32LB_SD_HWTEST_SEQ    (1u << 0)  /**< 顺序读。 */
#define SF32LB_SD_HWTEST_RND    (1u << 1)  /**< 随机 4 KiB。 */
#define SF32LB_SD_HWTEST_WRITE  (1u << 2)  /**< 写路径。 */
#define SF32LB_SD_HWTEST_ALL    (SF32LB_SD_HWTEST_SEQ | \
                                 SF32LB_SD_HWTEST_RND | \
                                 SF32LB_SD_HWTEST_WRITE)

/**
 * @brief 打印 SD 控制器 / 卡状态（调试）。
 * @param tag 日志前缀，可为 NULL。
 */
void sf32lb_sd_diag_dump(const char *tag);

/**
 * @brief 一层 SDIO/MTD 压测：经 MTD_BREAD/BWRITE 走 CMD17/18/24/25，不挂 LittleFS。
 *
 * @param flags    `SF32LB_SD_HWTEST_*` 位或。
 * @param seq_kib  顺序读长度（KiB）；≤0 则默认 256 KiB。
 * @param rnd_ops  随机 4 KiB 次数；≤0 则默认 64。
 * @return 通过为 0，失败为负 errno。
 *
 * 顺序读还会报告 8 扇区 CMD18 burst。
 */
int sf32lb_sd_hwtest(unsigned flags, int seq_kib, int rnd_ops);

/**
 * @brief 识别 SD 卡并注册 `/dev/sdN` MTD 窗口（FS_REGION）。
 *
 * @param minor       设备号（现为 0 → `/dev/sd0`）。
 * @param byte_offset 卡上字节偏移（`FS_REGION_OFFSET`）。
 * @param byte_size   窗口字节数；0 或 `0xffffffff` 表示 CSD 余量（仅 FAT 使用）。
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_automount(int minor, uint32_t byte_offset, uint32_t byte_size);

/**
 * @brief 将 `/dev/sdN` 挂到 `/mnt/lfs`（LittleFS，**不** autoformat）。
 *
 * 挂载前调用 `sf32lb_sd_grow_lfs()`：种子 `block_count` 小于 MTD 窗口则改写
 * superblock；挂载失败最多再试 2 次。空白/损坏卡仍不 autoformat。不支持缩小。
 *
 * @param minor 与 `sf32lb_sd_automount()` 相同的设备号。
 * @return 成功为 0（含已挂载的 EBUSY），失败为负 errno。
 */
int sf32lb_sd_mount_littlefs(int minor);

/**
 * @brief 注册 `/dev/sdkv` 并挂到 `/mnt/kv`（独立 KV LittleFS）。
 *
 * 窗口来自 ptab `KV_REGION`（256 MiB @ 256 MiB）。空白切片 autoformat；
 * 若已有更小的种子则挂载前 grow superblock。不与 `/mnt/lfs` 共用 superblock。
 *
 * @param byte_offset 卡上字节偏移（`KV_REGION_OFFSET`）。
 * @param byte_size   窗口字节数（`KV_REGION_SIZE`，须 ≥ 4 个 erase 块）。
 * @return 成功为 0（含已注册），失败为负 errno。
 */
int sf32lb_sd_mount_kv(uint32_t byte_offset, uint32_t byte_size);

/**
 * @brief 注册 FAT MTD + FTL 块设备，挂到 `/mnt/fat`（FAT32）。
 *
 * 窗口来自 ptab `FAT_REGION`（1 GiB 起，余量）。产品 `MS_RDONLY`，
 * 工厂读写。不 autoformat（空白卡请用 pack-sd-img 或 nsh `mkfatfs`）。
 *
 * @param byte_offset 卡上字节偏移（`FAT_REGION_OFFSET`）。
 * @param byte_size   窗口字节数；0 或 `0xffffffff` 表示 CSD 余量。
 * @return 成功为 0（含已挂载），失败为负 errno。
 */
int sf32lb_sd_mount_fat(uint32_t byte_offset, uint32_t byte_size);

/**
 * @brief HCPU 升频后按当前 HCLK 重算 SD 时钟分频（保持 ~24 MHz）。
 */
void sf32lb_sd_reclock(void);

/**
 * @brief 切 HCLK 之前把 SD 时钟夹住：取总线锁并停掉 SDCLK。
 *
 * @details 与 #sf32lb_sd_reclock 配对。中间那次 HCLK 切换期间卡上没有时钟，
 *          所以既不受 HCLK 切换抖动影响，也不受分频更新滞后影响（实测
 *          g_sd_lock 被文件系统占住时能拖后 4.86 s）。reclock 会在重设
 *          分频时自动放钟并解锁，切频失败路径也必须调 reclock 兜底。
 */
void sf32lb_sd_clk_hold(void);

/**
 * @brief 只读打印 SD 时钟拓扑（CSR 的 MPI 源选择、DLL2、ENR2 门控、CLKCR）。
 *
 * @details 用于确认 SD 挂在哪条时钟分支上、以及分频反推的 SDCLK 是否等于
 *          预期值。只读，不修改任何寄存器。
 */
void sf32lb_sd_clk_probe(void);

/**
 * @brief 裸读连续 n 个 512 B 块并逐块报时（绕过判死闸门，不进 LFS）。
 *
 * @param byte_off 卡内字节偏移（LFS 区 = 0x20000000 起，故 LFS 块 194 = 0x200C2000）。
 * @param nblocks  块数，0 为 -EINVAL。
 * @return 0 全部读到；负 errno 表示首个失败的块。
 */
int sf32lb_sd_raw_read(uint64_t byte_off, uint32_t nblocks);

/**
 * @brief 往 n 个 512 B 块写 0xFF（与 LittleFS 的"擦除"同一种操作）并回读校验。
 *
 * @details 只读测试测不到写路径，而 FAT 是只读挂载、所有写/擦都只发生在两个
 *          LFS 上 —— 本函数就是对那条路径的直接复现。
 *
 * @param byte_off 卡内字节偏移。**只允许落在卡的最后 1 MiB**（唯一不属于任何
 *                 文件系统的区域），其余偏移一律 -EINVAL，闸门在驱动内部。
 * @param nblocks  块数，0 为 -EINVAL。
 * @return 0 全部通过；负 errno（首个失败的块）。
 */
int sf32lb_sd_raw_write(uint64_t byte_off, uint32_t nblocks);

/**
 * @brief 两个 LFS 挂载点是否都还能用（用 FAT 当"卡还活着"的独立证据）。
 *
 * @details 判据分两层，缺一不可：
 *
 *          **第一层"卡还能不能读"用 FAT 探** —— FAT 和两个 LFS 在同一张卡上，
 *          所以"FAT 还能读"就是"卡没坏"的**独立证据**。这比看 g_sd_dead 强：
 *          g_sd_dead 是 SD 层自己的意见，而且只在多次重识别失败后才置起，
 *          中间有一段窗口；FAT 探测是直接测量。
 *
 *          **第二层"LFS 能不能用"用 statfs 探** —— `df` 也是这么判的，所以
 *          "两个 LFS 从 df 里消失"正是这个意思：挂载点还在，超级块读不出来。
 *
 *          三种组合对应三种处置，混在一起会做错事：
 *            - FAT 读不到            → 卡的问题，交给 SD 层（判死 + 硬复位轮询），
 *                                      此时重挂 LFS 是白费；
 *            - FAT 能读、LFS 读不到  → 卡是好的，坏的是 LFS 实例 → 该重挂；
 *            - 都读得到              → 健康。
 *
 * @note 未初始化（g_sd_initialized 为假）时恒返回 true，即"不归我管"。
 * @return true 无需处理（健康，或问题不在 FS 层）；false 卡可读但 LFS 不可用。
 */
bool sf32lb_sd_fs_ok(void);

/**
 * @brief 把坏掉的 LFS 挂载点重新挂上（哪个坏挂哪个）。
 *
 * @details 只在"**卡还能读**但 LFS 用不了"时才动手，门就是用 FAT 探的。
 *          硬复位把卡救回来之后，LittleFS 的 RAM 内状态不会自愈，必须重挂。
 *          卡读不到时直接返回，把舞台留给 SD 层的判死 / 硬复位。
 *
 *          两个踩过的坑：
 *          1. `/mnt/kv` **不能**直接重调 #sf32lb_sd_mount_kv —— 它开头
 *             `if (g_sd_kv_mtd != NULL) return OK;` 会立刻返回成功却根本没重挂。
 *             所以这里走 umount + 按设备名 mount，不重复注册 MTD。
 *          2. kv 的开机挂载带 `autoformat`；恢复路径上**刻意不传**，遵守
 *             "LFS 永不格式化"的约定。
 *
 * @warning 不要在持有 SD 总线锁的路径里调用：mount 内部还要走 SD I/O，
 *          `g_sd_lock` 不可重入，会自锁。只从 diag 线程调。
 *
 * @return 0 全部可用（含本次修好的）；-EAGAIN 卡读不到；负 errno 为失败。
 */
int sf32lb_sd_fs_heal(void);

/**
 * @brief `/mnt/lfs` 是不是只读挂上的（读写挂载失败后的降级结果）。
 *
 * @details 只读期间写操作会拿到 `-EACCES`。写盘的用户（轨迹记录、设置持久化、
 *          星历落盘）应当据此**跳过写入**，而不是反复撞 `-EACCES` 刷日志。
 *
 * @note 这是挂载阶梯（rw → ro → 重启）里第二级的标志位。写者接线的收益是
 *       省掉每 500 ms 一次的失败 open，**不是**防止磨损 —— 只读 LFS 的写被
 *       LittleFS 的**块层**拦下（`littlefs_write_block` / `erase_block` /
 *       `sync_block` 各自返回 `-EACCES`），根本到不了 SD 卡。
 * @return true 当前是只读挂载。
 */
bool sf32lb_sd_lfs_readonly(void);

/**
 * @brief 崩溃裸机路径：关掉 SD/DMA 中断，RCC 复位 SDMMC 后重新 identify。
 *
 * 假定控制器和卡状态都不可信。不拿 NuttX 锁、不走 DMA。
 * 之后只用 `sf32lb_sd_crash_read512` / `write512`。
 *
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_crash_reinit(void);

/**
 * @brief 崩溃路径读一个 512 字节扇区（PIO，不拿锁）。
 * @param byte_off 卡上字节偏移（须 512 对齐）。
 * @param buf      至少 512 字节。
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_crash_read512(uint64_t byte_off, FAR void *buf);
/**
 * @brief 崩溃路径写一个 512 字节扇区（PIO，不拿锁）。
 */
int sf32lb_sd_crash_write512(uint64_t byte_off, FAR const void *buf);

struct mtd_dev_s;

/**
 * @brief 把磁盘 superblock 的 `block_count` 改成与 MTD 窗口一致。
 *
 * 必须在未挂载时调用。只改 metadata pair 中 revision 更高的那一侧，
 * 并重算该 commit 的 CRC。磁盘 `block_count` 大于窗口则缩回窗口
 * （旧余量种子 / 误 grow 到整卡）。不修改 `nuttx/fs/littlefs`。
 *
 * @param mtd         MTD 设备（`/dev/sd0` 或 `/dev/sdkv`）。
 * @param want_blocks 目标 erase 块数（`neraseblocks`）。
 * @return 已相等或已改写为 0；空白为 `-ENOENT`。
 */
int sf32lb_lfs_grow_super(FAR struct mtd_dev_s *mtd, uint32_t want_blocks);

/**
 * @brief 对 `/mnt/lfs` 对应的 MTD 窗口调用 `sf32lb_lfs_grow_super()`。
 * @return 同 `sf32lb_lfs_grow_super()`；窗口未注册则为 `-ENODEV`。
 */
int sf32lb_sd_grow_lfs(void);

/** 传给 `sf32lb_sd_limit_fs_bytes()`：恢复 CSD 余量全窗口。 */
#define SF32LB_SD_LFS_WINDOW_FULL  0xffffffffu

/**
 * @brief 缩小或恢复 `/mnt/lfs` 的逻辑窗口（须先 umount）。
 *
 * @p bytes 向下对齐到 erase，并钳到卡余量。0 只打印当前上限。
 * `SF32LB_SD_LFS_WINDOW_FULL` 恢复完整 CSD 余量。
 *
 * @param bytes 目标逻辑字节数，或 `SF32LB_SD_LFS_WINDOW_FULL`。
 * @return 成功为 0，失败为负 errno。
 */
int sf32lb_sd_limit_fs_bytes(uint32_t bytes);

#endif /* __SF32LB_SDIO_H__ */
