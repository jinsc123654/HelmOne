/**
 * @file sf32lb_lfs_super.c
 * @brief 板级 LittleFS superblock 扩容（不改 `nuttx/fs/littlefs`）。
 *
 * 打包种子按 2 GiB 卡几何 format。KV 窗口固定 256 MiB；用户 LFS 固定
 * 512 MiB。原版 NuttX littlefs 要求 `superblock.block_count == MTD
 * neraseblocks`，因此在 `mount()` 前改写 `block_count` 并重算 CRC。
 * 磁盘更大则缩回窗口（旧余量种子），不 format。
 *
 * 磁盘格式：littlefs 2.x metadata pair（块 0/1，见 upstream DESIGN.md）。
 * CRC 表为 littlefs 软件 CRC（BSD-3-Clause）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>

#include "sf32lb_sdio.h"

#define LFS_TYPE_CCRC           0x500
#define LFS_TYPE_INLINESTRUCT   0x201

static uint32_t lfs_crc32(uint32_t crc, FAR const void *buffer, size_t size)
{
  static const uint32_t rtable[16] =
    {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
      0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
      0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
      0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
  FAR const uint8_t *data = buffer;
  size_t i;

  for (i = 0; i < size; i++)
    {
      crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
      crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }

  return crc;
}

static uint32_t rd_le32(FAR const uint8_t *p)
{
  return (uint32_t)p[0]
       | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16)
       | ((uint32_t)p[3] << 24);
}

static uint32_t rd_be32(FAR const uint8_t *p)
{
  return ((uint32_t)p[0] << 24)
       | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8)
       | (uint32_t)p[3];
}

static void wr_le32(FAR uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static bool tag_isvalid(uint32_t tag)
{
  return (tag & 0x80000000u) == 0;
}

static bool tag_isdelete(uint32_t tag)
{
  return ((int32_t)(tag << 22) >> 22) == -1;
}

static uint16_t tag_type2(uint32_t tag)
{
  return (uint16_t)((tag & 0x78000000u) >> 20);
}

static uint16_t tag_type3(uint32_t tag)
{
  return (uint16_t)((tag & 0x7ff00000u) >> 20);
}

static uint8_t tag_chunk(uint32_t tag)
{
  return (uint8_t)((tag & 0x0ff00000u) >> 20);
}

static uint16_t tag_id(uint32_t tag)
{
  return (uint16_t)((tag & 0x000ffc00u) >> 10);
}

static uint32_t tag_size(uint32_t tag)
{
  return tag & 0x000003ffu;
}

static uint32_t tag_dsize(uint32_t tag)
{
  return 4u + tag_size(tag + (uint32_t)tag_isdelete(tag));
}

static int lfs_scmp(uint32_t a, uint32_t b)
{
  return (int)(unsigned)(a - b);
}

/**
 * @brief 扫描一块 metadata，取出 superblock 的 `block_count` 与闭合该 commit 的 CCRC。
 *
 * @param[in]  block         整块内容。
 * @param[in]  block_size    erase 大小。
 * @param[out] disk_bc       磁盘上的 `block_count`。
 * @param[out] sb_off        24 字节 superblock 载荷起始。
 * @param[out] commit_start  该 commit 开始做 CRC 的字节偏移（0 含 rev）。
 * @param[out] crc_off       闭合该 commit 的 CCRC 标签偏移。
 * @return 找到有效 superblock 为 0，否则为 `-ENOENT` / `-EINVAL`。
 */
static int walk_mdir(FAR const uint8_t *block, uint32_t block_size,
                     FAR uint32_t *disk_bc,
                     FAR uint32_t *sb_off, FAR uint32_t *commit_start,
                     FAR uint32_t *crc_off)
{
  uint32_t ptag = 0xffffffffu;
  uint32_t off = 0;
  uint32_t crc;
  uint32_t cstart = 0;
  bool pending = false;
  uint32_t pend_bc = 0;
  uint32_t pend_sb = 0;
  uint32_t pend_cs = 0;
  bool found = false;

  if (block_size < 8)
    {
      return -EINVAL;
    }

  crc = lfs_crc32(0xffffffffu, block, 4);

  while (true)
    {
      uint32_t raw;
      uint32_t tag;
      uint32_t dsz;
      uint32_t pay;

      off += tag_dsize(ptag);
      if (off + 8 > block_size)
        {
          break;
        }

      raw = rd_be32(block + off);
      crc = lfs_crc32(crc, block + off, 4);
      tag = raw ^ ptag;

      if (!tag_isvalid(tag))
        {
          break;
        }

      dsz = tag_dsize(tag);
      if (off + dsz > block_size)
        {
          break;
        }

      ptag = tag;

      if (tag_type2(tag) == LFS_TYPE_CCRC)
        {
          uint32_t dcrc = rd_le32(block + off + 4);

          if (crc != dcrc)
            {
              break;
            }

          if (pending)
            {
              *disk_bc = pend_bc;
              *sb_off = pend_sb;
              *commit_start = pend_cs;
              *crc_off = off;
              found = true;
              pending = false;
            }

          ptag ^= (uint32_t)(tag_chunk(tag) & 1u) << 31;
          crc = 0xffffffffu;
          cstart = off + dsz;
          continue;
        }

      pay = dsz - 4u;
      crc = lfs_crc32(crc, block + off + 4, pay);

      if (tag_type3(tag) == LFS_TYPE_INLINESTRUCT &&
          tag_id(tag) == 0 && tag_size(tag) == 24 && pay >= 24)
        {
          FAR const uint8_t *sb = block + off + 4;
          uint32_t ver = rd_le32(sb + 0);
          uint32_t bsz = rd_le32(sb + 4);
          uint32_t bc  = rd_le32(sb + 8);
          uint32_t nmax = rd_le32(sb + 12);

          if ((ver >> 16) == 2 && bsz == block_size &&
              bc >= 2 && nmax <= 1022)
            {
              pending = true;
              pend_bc = bc;
              pend_sb = off + 4;
              pend_cs = cstart;
            }
        }
    }

  return found ? 0 : -ENOENT;
}

/**
 * @brief 读一个 LittleFS 块（一个 erase）到 @p buf。
 */
static int read_lfs_block(FAR struct mtd_dev_s *mtd,
                          FAR const struct mtd_geometry_s *geo,
                          uint32_t lfs_block, FAR uint8_t *buf)
{
  uint32_t ppb = geo->erasesize / geo->blocksize;
  ssize_t n;

  n = MTD_BREAD(mtd, (off_t)lfs_block * ppb, ppb, buf);
  if (n < 0)
    {
      return (int)n;
    }

  return OK;
}

/**
 * @brief 擦除并写回一个 LittleFS 块。
 */
static int write_lfs_block(FAR struct mtd_dev_s *mtd,
                           FAR const struct mtd_geometry_s *geo,
                           uint32_t lfs_block, FAR const uint8_t *buf)
{
  uint32_t ppb = geo->erasesize / geo->blocksize;
  ssize_t n;
  int ret;

  ret = MTD_ERASE(mtd, (off_t)lfs_block, 1);
  if (ret < 0)
    {
      return ret;
    }

  n = MTD_BWRITE(mtd, (off_t)lfs_block * ppb, ppb, buf);
  if (n < 0)
    {
      return (int)n;
    }

  return OK;
}

/**
 * @brief 把磁盘 `block_count` 改成 @p want_blocks（未挂载时调用）。
 *
 * 读块 0/1，只改 **revision 更高** 的那一侧。已相等则不写；空白返回
 * `-ENOENT`（交给 autoformat）；磁盘更大则缩回窗口。
 *
 * @param mtd         `/dev/sd0` 或 `/dev/sdkv`。
 * @param want_blocks MTD `neraseblocks`。
 * @return 已相等或已改为 0；空白 `-ENOENT`。
 */
int sf32lb_lfs_grow_super(FAR struct mtd_dev_s *mtd, uint32_t want_blocks)
{
  struct mtd_geometry_s geo;
  FAR uint8_t *blk[2] = { NULL, NULL };
  uint32_t rev[2] = { 0, 0 };
  bool ok[2] = { false, false };
  uint32_t disk_bc[2] = { 0, 0 };
  uint32_t sb_off[2] = { 0, 0 };
  uint32_t cstart[2] = { 0, 0 };
  uint32_t crc_off[2] = { 0, 0 };
  int which = -1;
  int i;
  int ret;

  if (mtd == NULL || want_blocks < 2)
    {
      return -EINVAL;
    }

  ret = MTD_IOCTL(mtd, MTDIOC_GEOMETRY, (unsigned long)&geo);
  if (ret < 0)
    {
      return ret;
    }

  if (geo.erasesize == 0 || geo.blocksize == 0 ||
      (geo.erasesize % geo.blocksize) != 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < 2; i++)
    {
      blk[i] = (FAR uint8_t *)kmm_malloc(geo.erasesize);
      if (blk[i] == NULL)
        {
          ret = -ENOMEM;
          goto out;
        }

      memset(blk[i], 0xff, geo.erasesize);
      ret = read_lfs_block(mtd, &geo, (uint32_t)i, blk[i]);
      if (ret < 0)
        {
          continue;
        }

      rev[i] = rd_le32(blk[i]);
      if (walk_mdir(blk[i], geo.erasesize,
                    &disk_bc[i], &sb_off[i], &cstart[i],
                    &crc_off[i]) == 0)
        {
          ok[i] = true;
        }
    }

  for (i = 0; i < 2; i++)
    {
      if (!ok[i])
        {
          continue;
        }

      if (which < 0 || lfs_scmp(rev[i], rev[which]) > 0)
        {
          which = i;
        }
    }

  if (which < 0)
    {
      ret = -ENOENT;
      goto out;
    }

  if (disk_bc[which] == want_blocks)
    {
      syslog(LOG_INFO,
             "littlefs: superblock already %lu blocks\n",
             (unsigned long)want_blocks);
      ret = OK;
      goto out;
    }

  /* FS_REGION is now a fixed 512 MiB window. An older remainder-sized
   * seed (or a boot that grew LFS to the whole card) leaves block_count
   * at ~14 GiB. Refusing to shrink makes mount() fail and skips KV/FAT.
   * Files live in the first 512 MiB; extra blocks are FAT's now.
   */

  wr_le32(blk[which] + sb_off[which] + 8, want_blocks);

  {
    uint32_t crc;
    uint32_t crc_len;

    if (crc_off[which] < cstart[which] ||
        crc_off[which] + 8 > geo.erasesize)
      {
        ret = -EIO;
        goto out;
      }

    crc_len = (crc_off[which] + 4) - cstart[which];
    crc = lfs_crc32(0xffffffffu, blk[which] + cstart[which], crc_len);
    wr_le32(blk[which] + crc_off[which] + 4, crc);
  }

  ret = write_lfs_block(mtd, &geo, (uint32_t)which, blk[which]);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: littlefs superblock write failed: %d\n", ret);
      goto out;
    }

  syslog(LOG_INFO,
         "littlefs: %s superblock %lu -> %lu blocks (%llu bytes)\n",
         disk_bc[which] > want_blocks ? "shrunk" : "grew",
         (unsigned long)disk_bc[which],
         (unsigned long)want_blocks,
         (unsigned long long)want_blocks * geo.erasesize);
  ret = OK;

out:
  kmm_free(blk[0]);
  kmm_free(blk[1]);
  return ret;
}
