/**
 * @file companion_rle.h
 * @brief BLE FS 用流式 RLE + 255 字节 LZ 回溯编解码器。
 *
 * 线格式（每个 WRITE 会话 / 每个 READ 命令独立流）：
 * - 0xxxxxxx：后跟 1..128 字面字节（n = ctrl+1）
 * - 10xxxxxx byte：重复 byte（n = (ctrl&0x3f)+2）次，2..65
 * - 11xxxxxx off：从输出[-off] 拷贝（n = (ctrl&0x3f)+3）字节，off 1..255，允许重叠
 *
 * 线上的 size/CRC 与续传 offset 均不压缩；每次 OPEN/READ 双方重置编解码状态。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MYVENDOR_COMPANION_RLE_H
#define MYVENDOR_COMPANION_RLE_H

#include <stdbool.h>
#include <stdint.h>

#define COMPANION_RLE_WIN       255  /**< 回溯窗口大小。 */
#define COMPANION_RLE_LOOK      192  /**< 编码器前瞻缓冲。 */
#define COMPANION_RLE_MAX_LIT   128  /**< 最大字面 run。 */
#define COMPANION_RLE_MAX_RUN   65   /**< 最大重复 run。 */
#define COMPANION_RLE_MAX_CPY   66   /**< 最大拷贝 run。 */

/** @brief 解码器状态。 */
enum companion_rle_dec_st
{
  COMPANION_RLE_ST_CTRL = 0,
  COMPANION_RLE_ST_LIT,
  COMPANION_RLE_ST_RUN_BYTE,
  COMPANION_RLE_ST_RUN,
  COMPANION_RLE_ST_CPY_OFF,
  COMPANION_RLE_ST_CPY
};

/** @brief 流式解码器上下文。 */
struct companion_rle_dec
{
  uint8_t  hist[256];
  uint8_t  hist_pos;
  uint16_t hist_n;
  uint8_t  st;
  uint8_t  need;
  uint8_t  arg;
};

/** @brief 流式编码器上下文。 */
struct companion_rle_enc
{
  uint8_t  hist[256];
  uint8_t  hist_pos;
  uint16_t hist_n;
  uint8_t  look[COMPANION_RLE_LOOK];
  uint16_t look_n;
};

/** @brief 解码输出回调；返回非 0 中止 feed。 */
typedef int (*companion_rle_out_fn)(void *ctx, const uint8_t *p, uint16_t n);

/** @brief 初始化解码器。 */
void companion_rle_dec_init(struct companion_rle_dec *d);

/** @brief 解码器是否处于空闲（无未完成 token）。 */
bool companion_rle_dec_idle(const struct companion_rle_dec *d);

/**
 * @brief 喂入压缩字节流并解码输出。
 * @return 0 成功；非 0 为 out 回调错误。
 */
int  companion_rle_dec_feed(struct companion_rle_dec *d,
                            const uint8_t *src, uint16_t n,
                            companion_rle_out_fn out, void *ctx);

/** @brief 初始化编码器。 */
void     companion_rle_enc_init(struct companion_rle_enc *e);

/** @brief 编码器 look 缓冲剩余可 push 字节数。 */
uint16_t companion_rle_enc_room(const struct companion_rle_enc *e);

/** @brief 编码器待编码字节数。 */
uint16_t companion_rle_enc_pending(const struct companion_rle_enc *e);

/**
 * @brief 向编码器 push 明文。
 * @return 0 成功；EINVAL 空间不足。
 */
int      companion_rle_enc_push(struct companion_rle_enc *e,
                                const uint8_t *src, uint16_t n);

/**
 * @brief 从编码器 pull 压缩字节。
 * @param finish true 表示输入已结束，flush 剩余。
 * @return 写入 out 的字节数。
 */
uint16_t companion_rle_enc_pull(struct companion_rle_enc *e,
                                uint8_t *out, uint16_t out_max, bool finish);

#endif /* MYVENDOR_COMPANION_RLE_H */
