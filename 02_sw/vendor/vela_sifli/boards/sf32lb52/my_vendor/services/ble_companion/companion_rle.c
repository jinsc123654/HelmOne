/**
 * @file companion_rle.c
 * @brief BLE Companion FS 的 RLE/LZ 编解码实现。
 *
 * 供 companion_fs READ/WRITE 压缩载荷；COMPANION_RLE_HOST 可编主机 roundtrip 测试。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "companion_rle.h"

#include <errno.h>
#include <string.h>

/** @brief 向 256 字节历史窗口追加一字节。 */
static void hist_push(uint8_t *hist, uint8_t *pos, uint16_t *n, uint8_t b)
{
  hist[*pos] = b;
  *pos = (uint8_t)(*pos + 1);
  if (*n < COMPANION_RLE_WIN)
    {
      (*n)++;
    }
}

/** @brief 读取历史窗口 off 字节前的字节。 */
static uint8_t hist_at(const uint8_t *hist, uint8_t pos, uint16_t n,
                       uint16_t off)
{
  (void)n;
  return hist[(uint8_t)(pos - (uint8_t)off)];
}

/****************************************************************************
 * 解码器
 ****************************************************************************/

/** @brief 初始化解码器。 */
void companion_rle_dec_init(struct companion_rle_dec *d)
{
  memset(d, 0, sizeof(*d));
}

/** @brief 解码器是否空闲。 */
bool companion_rle_dec_idle(const struct companion_rle_dec *d)
{
  return d->st == COMPANION_RLE_ST_CTRL && d->need == 0;
}

/** @brief 解码输出一字节并更新历史。 */
static int dec_emit(struct companion_rle_dec *d, uint8_t b,
                    companion_rle_out_fn out, void *ctx)
{
  int ret;

  ret = out(ctx, &b, 1);
  if (ret != 0)
    {
      return ret;
    }

  hist_push(d->hist, &d->hist_pos, &d->hist_n, b);
  return 0;
}

/** @brief 喂入压缩流并回调输出明文。 */
int companion_rle_dec_feed(struct companion_rle_dec *d,
                           const uint8_t *src, uint16_t n,
                           companion_rle_out_fn out, void *ctx)
{
  uint16_t i = 0;
  int      ret;

  while (i < n || d->st == COMPANION_RLE_ST_RUN ||
         d->st == COMPANION_RLE_ST_CPY)
    {
      switch (d->st)
        {
        case COMPANION_RLE_ST_CTRL:
          if (i >= n)
            {
              return 0;
            }

          {
            uint8_t c = src[i++];
            if (c < 0x80)
              {
                d->need = (uint8_t)(c + 1);
                d->st   = COMPANION_RLE_ST_LIT;
              }
            else if (c < 0xC0)
              {
                d->need = (uint8_t)((c & 0x3f) + 2);
                d->st   = COMPANION_RLE_ST_RUN_BYTE;
              }
            else
              {
                d->need = (uint8_t)((c & 0x3f) + 3);
                d->st   = COMPANION_RLE_ST_CPY_OFF;
              }
          }
          break;

        case COMPANION_RLE_ST_LIT:
          if (i >= n)
            {
              return 0;
            }

          ret = dec_emit(d, src[i++], out, ctx);
          if (ret != 0)
            {
              return ret;
            }

          if (--d->need == 0)
            {
              d->st = COMPANION_RLE_ST_CTRL;
            }
          break;

        case COMPANION_RLE_ST_RUN_BYTE:
          if (i >= n)
            {
              return 0;
            }

          d->arg = src[i++];
          d->st  = COMPANION_RLE_ST_RUN;
          break;

        case COMPANION_RLE_ST_RUN:
          while (d->need > 0)
            {
              ret = dec_emit(d, d->arg, out, ctx);
              if (ret != 0)
                {
                  return ret;
                }

              d->need--;
            }

          d->st = COMPANION_RLE_ST_CTRL;
          break;

        case COMPANION_RLE_ST_CPY_OFF:
          if (i >= n)
            {
              return 0;
            }

          d->arg = src[i++];
          if (d->arg == 0)
            {
              return EINVAL;
            }

          d->st = COMPANION_RLE_ST_CPY;
          break;

        case COMPANION_RLE_ST_CPY:
          if (d->arg > d->hist_n)
            {
              return EINVAL;
            }

          while (d->need > 0)
            {
              uint8_t b = hist_at(d->hist, d->hist_pos, d->hist_n, d->arg);
              ret = dec_emit(d, b, out, ctx);
              if (ret != 0)
                {
                  return ret;
                }

              d->need--;
            }

          d->st = COMPANION_RLE_ST_CTRL;
          break;

        default:
          return EINVAL;
        }
    }

  return 0;
}

/****************************************************************************
 * 编码器
 ****************************************************************************/

/** @brief 初始化编码器。 */
void companion_rle_enc_init(struct companion_rle_enc *e)
{
  memset(e, 0, sizeof(*e));
}

/** @brief 编码器 look 缓冲剩余空间。 */
uint16_t companion_rle_enc_room(const struct companion_rle_enc *e)
{
  return (uint16_t)(COMPANION_RLE_LOOK - e->look_n);
}

/** @brief 待编码字节数。 */
uint16_t companion_rle_enc_pending(const struct companion_rle_enc *e)
{
  return e->look_n;
}

int companion_rle_enc_push(struct companion_rle_enc *e,
                           const uint8_t *src, uint16_t n)
{
  if (n > companion_rle_enc_room(e))
    {
      return EINVAL;
    }

  if (n > 0)
    {
      memcpy(e->look + e->look_n, src, n);
      e->look_n = (uint16_t)(e->look_n + n);
    }

  return 0;
}

/** @brief 消费 look 前缀并更新历史。 */
static void enc_consume(struct companion_rle_enc *e, uint16_t n)
{
  uint16_t i;

  for (i = 0; i < n; i++)
    {
      hist_push(e->hist, &e->hist_pos, &e->hist_n, e->look[i]);
    }

  e->look_n = (uint16_t)(e->look_n - n);
  if (e->look_n > 0)
    {
      memmove(e->look, e->look + n, e->look_n);
    }
}

/** @brief 编码窗口相对偏移读字节。 */
static uint8_t window_at(const struct companion_rle_enc *e, uint16_t start,
                         uint16_t off)
{
  if (off <= start)
    {
      return e->look[start - off];
    }

  return hist_at(e->hist, e->hist_pos, e->hist_n, (uint16_t)(off - start));
}

/** @brief 从 start 找最长重复 run。 */
static void find_run_from(const struct companion_rle_enc *e, uint16_t start,
                          uint16_t *run)
{
  uint16_t avail = (uint16_t)(e->look_n - start);
  uint16_t n     = 1;
  uint16_t max   = avail;

  if (max > COMPANION_RLE_MAX_RUN)
    {
      max = COMPANION_RLE_MAX_RUN;
    }

  while (n < max && e->look[start + n] == e->look[start])
    {
      n++;
    }

  *run = n;
}

/** @brief 从 start 找最长 LZ 匹配。 */
static void find_match_from(const struct companion_rle_enc *e, uint16_t start,
                            uint16_t *best_len, uint16_t *best_off)
{
  uint16_t avail      = (uint16_t)(e->look_n - start);
  uint16_t hist_total = (uint16_t)(e->hist_n + start);
  uint16_t max_len    = avail;
  uint16_t off;
  uint8_t  b0;
  uint8_t  b1;
  uint8_t  b2;

  *best_len = 0;
  *best_off = 0;

  if (hist_total > COMPANION_RLE_WIN)
    {
      hist_total = COMPANION_RLE_WIN;
    }

  if (avail < 3 || hist_total == 0)
    {
      return;
    }

  if (max_len > COMPANION_RLE_MAX_CPY)
    {
      max_len = COMPANION_RLE_MAX_CPY;
    }

  /* Compare look[start + m] with the byte `off` behind that position.
   * Using window_at(start+m, off) avoids a software `%` in the inner
   * loop (the old `off - (m % off)` form was too slow on the MCU, so
   * download RLE filled notify pages slower than raw BLE).
   */
  b0 = e->look[start];
  b1 = e->look[start + 1];
  b2 = e->look[start + 2];

  for (off = 1; off <= hist_total; off++)
    {
      uint16_t m;

      if (window_at(e, start, off) != b0)
        {
          continue;
        }

      if (window_at(e, (uint16_t)(start + 1), off) != b1 ||
          window_at(e, (uint16_t)(start + 2), off) != b2)
        {
          continue;
        }

      m = 3;
      while (m < max_len &&
             window_at(e, (uint16_t)(start + m), off) == e->look[start + m])
        {
          m++;
        }

      if (m > *best_len)
        {
          *best_len = m;
          *best_off = off;
          if (m == max_len)
            {
              break;
            }
        }
    }
}

/** @brief pull 压缩字节到 out。 */
uint16_t companion_rle_enc_pull(struct companion_rle_enc *e,
                                uint8_t *out, uint16_t out_max, bool finish)
{
  uint16_t used = 0;

  while (e->look_n > 0)
    {
      uint16_t run;
      uint16_t ml;
      uint16_t off;
      uint16_t lit;
      uint16_t max_lit;

      if (!finish && e->look_n < COMPANION_RLE_MAX_CPY)
        {
          break;
        }

      find_run_from(e, 0, &run);
      find_match_from(e, 0, &ml, &off);

      if (ml >= 3 && ml >= run)
        {
          if ((uint16_t)(out_max - used) < 2)
            {
              break;
            }

          out[used++] = (uint8_t)(0xc0 | (ml - 3));
          out[used++] = (uint8_t)off;
          enc_consume(e, ml);
        }
      else if (run >= 3)
        {
          if ((uint16_t)(out_max - used) < 2)
            {
              break;
            }

          out[used++] = (uint8_t)(0x80 | (run - 2));
          out[used++] = e->look[0];
          enc_consume(e, run);
        }
      else
        {
          if ((uint16_t)(out_max - used) < 2)
            {
              break;
            }

          max_lit = COMPANION_RLE_MAX_LIT;
          if (max_lit > e->look_n)
            {
              max_lit = e->look_n;
            }

          if (max_lit > (uint16_t)(out_max - used - 1))
            {
              max_lit = (uint16_t)(out_max - used - 1);
            }

          if (max_lit < 1)
            {
              break;
            }

          lit = 1;
          while (lit < max_lit)
            {
              uint16_t r2;
              uint16_t m2;
              uint16_t o2;

              if (!finish &&
                  (uint16_t)(e->look_n - lit) < COMPANION_RLE_MAX_CPY)
                {
                  break;
                }

              find_run_from(e, lit, &r2);
              find_match_from(e, lit, &m2, &o2);
              if ((m2 >= 3 && m2 >= r2) || r2 >= 3)
                {
                  break;
                }

              lit++;
            }

          out[used++] = (uint8_t)(lit - 1);
          memcpy(out + used, e->look, lit);
          used = (uint16_t)(used + lit);
          enc_consume(e, lit);
        }
    }

  return used;
}

#ifdef COMPANION_RLE_HOST

#include <stdio.h>
#include <stdlib.h>

struct grow
{
  uint8_t *p;
  size_t   n;
  size_t   cap;
};

static int grow_out(void *ctx, const uint8_t *p, uint16_t n)
{
  struct grow *g = ctx;
  if (g->n + n > g->cap)
    {
      size_t cap = g->cap ? g->cap * 2 : 4096;
      while (cap < g->n + n)
        {
          cap *= 2;
        }

      g->p = realloc(g->p, cap);
      g->cap = cap;
    }

  memcpy(g->p + g->n, p, n);
  g->n += n;
  return 0;
}

static uint8_t *load(const char *path, size_t *n)
{
  FILE *fp = fopen(path, "rb");
  uint8_t *p;
  if (!fp)
    {
      perror(path);
      return NULL;
    }

  fseek(fp, 0, SEEK_END);
  *n = (size_t)ftell(fp);
  fseek(fp, 0, SEEK_SET);
  p = malloc(*n);
  if (!p || fread(p, 1, *n, fp) != *n)
    {
      fclose(fp);
      free(p);
      return NULL;
    }

  fclose(fp);
  return p;
}

int main(int argc, char **argv)
{
  uint8_t *src;
  size_t   n;
  struct companion_rle_enc enc;
  struct companion_rle_dec dec;
  struct grow encb = {0};
  struct grow decb = {0};
  uint8_t tmp[256];
  size_t off = 0;

  if (argc < 2)
    {
      fprintf(stderr, "usage: %s file\n", argv[0]);
      return 1;
    }

  src = load(argv[1], &n);
  if (!src)
    {
      return 1;
    }

  companion_rle_enc_init(&enc);
  while (off < n || companion_rle_enc_pending(&enc) > 0)
    {
      uint16_t room = companion_rle_enc_room(&enc);
      uint16_t chunk;
      uint16_t produced;
      bool     finish;

      if (room > 0 && off < n)
        {
          chunk = room;
          if ((size_t)chunk > n - off)
            {
              chunk = (uint16_t)(n - off);
            }

          companion_rle_enc_push(&enc, src + off, chunk);
          off += chunk;
        }

      finish = (off >= n);
      produced = companion_rle_enc_pull(&enc, tmp, sizeof(tmp), finish);
      if (produced == 0 && finish && companion_rle_enc_pending(&enc) == 0)
        {
          break;
        }

      if (produced == 0 && !finish)
        {
          continue;
        }

      grow_out(&encb, tmp, produced);
    }

  companion_rle_dec_init(&dec);
  off = 0;
  while (off < encb.n)
    {
      uint16_t chunk = 256;
      if ((size_t)chunk > encb.n - off)
        {
          chunk = (uint16_t)(encb.n - off);
        }

      if (companion_rle_dec_feed(&dec, encb.p + off, chunk, grow_out,
                                 &decb) != 0)
        {
          fprintf(stderr, "decode error at %zu\n", off);
          return 1;
        }

      off += chunk;
    }

  if (!companion_rle_dec_idle(&dec) || decb.n != n ||
      memcmp(decb.p, src, n) != 0)
    {
      fprintf(stderr, "roundtrip FAIL src=%zu enc=%zu dec=%zu idle=%d\n",
              n, encb.n, decb.n, companion_rle_dec_idle(&dec));
      return 1;
    }

  printf("ok src=%zu enc=%zu ratio=%.2f\n", n, encb.n,
         n ? (double)n / (double)encb.n : 0);
  return 0;
}

#endif /* COMPANION_RLE_HOST */
