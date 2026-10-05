/**
 * @file gpx_decode.c
 * @brief GPX 解码器句柄实现（同步 pull，无线程）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_decode.h"

#include "gpx_ext.h"
#include "gpx_port.h"
#include "gpx_reader.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#ifndef CONFIG_MYVENDOR_GPX_DEFAULT_DECODE_BATCH
#  define CONFIG_MYVENDOR_GPX_DEFAULT_DECODE_BATCH  32
#endif

#ifndef GPX_DECODE_SCRATCH
#  define GPX_DECODE_SCRATCH  1024
#endif

/** 相邻文件点超过该距离视为坏点（0,0 / 解析失败），不计入里程。 */
#ifndef GPX_DECODE_LEN_HOP_MAX_M
#  define GPX_DECODE_LEN_HOP_MAX_M  100000.0
#endif

/** 解码句柄内部状态。 */
struct gpx_decode
{
  gpx_file_io_t fio;
  gpx_io_t io;
  gpx_decode_cfg_t cfg;
  gpx_decode_stats_t stats;
  unsigned skip_remaining;
  bool opened;
  bool have_tail;
  bool have_last_out;
  float len_lon;
  float len_lat;
  bool have_len_pt;
  gpx_point_t tail;
  gpx_point_t last_out;
  char *scratch;
  size_t scratch_cap;
  void *ext_bind[GPX_EXT_SLOT_MAX];
};

static bool gpx_decode_ll_ok(float lon, float lat)
{
  if (!isfinite((double)lat) || !isfinite((double)lon) ||
      lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f)
    {
      return false;
    }

  /* 未定位 / memset / 解析失败常见写成 0,0，接到中国会变成 ~12600 km。 */
  if (lat == 0.0f && lon == 0.0f)
    {
      return false;
    }

  return true;
}

static double gpx_decode_dist_m(float lon1, float lat1, float lon2, float lat2)
{
  const double r = 6371000.0;
  const double dlat = ((double)lat2 - (double)lat1)
      * (3.14159265358979323846 / 180.0);
  const double dlon = ((double)lon2 - (double)lon1)
      * (3.14159265358979323846 / 180.0);
  const double lat1r = (double)lat1 * (3.14159265358979323846 / 180.0);
  const double lat2r = (double)lat2 * (3.14159265358979323846 / 180.0);
  const double a = sin(dlat * 0.5) * sin(dlat * 0.5)
      + cos(lat1r) * cos(lat2r) * sin(dlon * 0.5) * sin(dlon * 0.5);

  return 2.0 * r * asin(a < 1.0 ? sqrt(a) : 1.0);
}

static void gpx_decode_add_len(gpx_decode_t *dec, float lon, float lat)
{
  double d;

  if (dec == NULL || !gpx_decode_ll_ok(lon, lat))
    {
      return;
    }

  if (dec->have_len_pt)
    {
      d = gpx_decode_dist_m(dec->len_lon, dec->len_lat, lon, lat);
      if (d <= GPX_DECODE_LEN_HOP_MAX_M)
        {
          dec->stats.length_m += d;
        }
    }

  dec->len_lon = lon;
  dec->len_lat = lat;
  dec->have_len_pt = true;
}

void gpx_decode_cfg_sparse(gpx_decode_cfg_t *cfg, unsigned limit)
{
  if (cfg == NULL)
    {
      return;
    }

  memset(cfg, 0, sizeof(*cfg));
  cfg->stride = 0;
  cfg->limit = (limit > 0) ? limit : GPX_DECODE_POINT_MAX;
}

static unsigned gpx_decode_auto_stride(unsigned limit, uint32_t file_bytes)
{
  unsigned est;

  if (limit < 2u)
    {
      return 1;
    }

  est = file_bytes / GPX_DECODE_TRKPT_AVG_BYTES;
  if (est < 2u)
    {
      est = 2;
    }

  if (est <= limit)
    {
      return 1;
    }

  return (est - 1u + (limit - 2u)) / (limit - 1u);
}

static int gpx_decode_resolve_cfg(const gpx_decode_cfg_t *cfg,
                                  unsigned *batch_max,
                                  unsigned *stride,
                                  unsigned *limit)
{
  unsigned batch;
  unsigned st;
  unsigned lim;

  if (cfg != NULL && cfg->batch_max > 0)
    {
      batch = cfg->batch_max;
    }
  else
    {
      batch = CONFIG_MYVENDOR_GPX_DEFAULT_DECODE_BATCH;
    }

  st = (cfg != NULL) ? cfg->stride : 1u;
  lim = (cfg != NULL) ? cfg->limit : 0;

  if (batch == 0)
    {
      return -EINVAL;
    }

  *batch_max = batch;
  *stride = st;
  *limit = lim;
  return 0;
}

static bool gpx_decode_same_ll(const gpx_point_t *a, const gpx_point_t *b)
{
  return a->latitude == b->latitude && a->longitude == b->longitude;
}

static void gpx_decode_emit(gpx_decode_t *dec, gpx_point_t *dst,
                            const gpx_point_t *src)
{
  *dst = *src;
  dec->last_out = *src;
  dec->have_last_out = true;
  dec->stats.points_delivered++;
}

static void gpx_decode_prep_point(gpx_decode_t *dec, gpx_point_t *pt)
{
  int slot;

  memset(pt, 0, sizeof(*pt));
  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      if (dec->ext_bind[slot] != NULL)
        {
          pt->ext_data[slot] = dec->ext_bind[slot];
          pt->ext_mask |= (uint8_t)(1u << slot);
        }
    }
}

int gpx_decode_bind_ext(gpx_decode_t *dec, int slot, void *buf)
{
  if (dec == NULL || buf == NULL || slot < 0 || slot >= GPX_EXT_SLOT_MAX)
    {
      return -EINVAL;
    }

  dec->ext_bind[slot] = buf;
  return 0;
}

int gpx_decode_open(const char *path, const gpx_decode_cfg_t *cfg,
                    gpx_decode_t **out)
{
  gpx_decode_t *dec;
  unsigned batch_max;
  unsigned stride;
  unsigned limit;
  int ret;

  if (path == NULL || out == NULL)
    {
      return -EINVAL;
    }

  *out = NULL;

  ret = gpx_decode_resolve_cfg(cfg, &batch_max, &stride, &limit);
  if (ret != 0)
    {
      return ret;
    }

  dec = gpx_port_calloc(1, sizeof(*dec));
  if (dec == NULL)
    {
      return -ENOMEM;
    }

  dec->cfg.batch_max = batch_max;
  dec->cfg.stride = stride;
  dec->cfg.limit = limit;
  dec->skip_remaining = 0;

  ret = gpx_file_io_open(&dec->fio, path);
  if (ret != 0)
    {
      gpx_port_free(dec);
      return ret;
    }

  {
    uint32_t sz = 0;
    int szret = gpx_port_file_size(&dec->fio, &sz);

    if (szret == -EFBIG)
      {
        gpx_file_io_close(&dec->fio);
        gpx_port_free(dec);
        return -EFBIG;
      }

    if (dec->cfg.stride == 0)
      {
        if (dec->cfg.limit == 0)
          {
            dec->cfg.stride = 1;
          }
        else if (szret != 0)
          {
            dec->cfg.stride = 1;
          }
        else
          {
            dec->cfg.stride = gpx_decode_auto_stride(dec->cfg.limit, sz);
          }
      }
  }

  gpx_file_io_bind(&dec->fio, &dec->io);
  {
    size_t cap = GPX_DECODE_SCRATCH;
    size_t need = gpx_ext_fmt_max_per_trkpt() + 64u;

    /* scratch 也要盖住读取器的 <extensions> 块上限：只按我们自己的 fmt_max 算，
     * 读第三方文件（扩展块 600～800 字节）就会退回到每点一次 malloc（能跑，但
     * 白白多一次分配），块本身也可能被读窄。 */
    if (need < GPX_EXT_PARSE_BLOCK_MAX)
      {
        need = GPX_EXT_PARSE_BLOCK_MAX;
      }

    if (need > cap)
      {
        cap = need;
      }

    dec->scratch = gpx_port_malloc(cap);
    if (dec->scratch != NULL)
      {
        dec->scratch_cap = cap;
        dec->io.scratch = dec->scratch;
        dec->io.scratch_cap = cap;
      }
  }

  dec->opened = true;
  *out = dec;
  return 0;
}

static int gpx_decode_append_tail(gpx_decode_t *dec, gpx_point_t *points,
                                  unsigned max, unsigned *delivered)
{
  if (!dec->have_tail || *delivered >= max)
    {
      return 0;
    }

  if (dec->cfg.limit > 0 &&
      dec->stats.points_delivered >= dec->cfg.limit)
    {
      return 0;
    }

  if (dec->have_last_out &&
      gpx_decode_same_ll(&dec->tail, &dec->last_out))
    {
      return 0;
    }

  if (!gpx_decode_ll_ok(dec->tail.longitude, dec->tail.latitude))
    {
      return 0;
    }

  gpx_decode_emit(dec, &points[*delivered], &dec->tail);
  (*delivered)++;
  dec->have_tail = false;
  return 0;
}

static int gpx_decode_drain_tail(gpx_decode_t *dec)
{
  gpx_read_result_t res;

  for (;;)
    {
      res = gpx_reader_skip(&dec->io, &dec->tail);
      if (res == GPX_READ_EOF)
        {
          dec->stats.eof = true;
          return 0;
        }

      if (res != GPX_READ_OK)
        {
          return -EIO;
        }

      dec->have_tail = true;
      dec->stats.file_points++;
      dec->stats.points_skipped++;
      gpx_decode_add_len(dec, dec->tail.longitude, dec->tail.latitude);
    }
}

int gpx_decode_read(gpx_decode_t *dec, gpx_point_t *points, unsigned max,
                    unsigned *n_out)
{
  gpx_read_result_t res;
  unsigned delivered = 0;

  if (dec == NULL || points == NULL || n_out == NULL || max == 0)
    {
      return -EINVAL;
    }

  if (!dec->opened)
    {
      return -EINVAL;
    }

  if (max > dec->cfg.batch_max)
    {
      max = dec->cfg.batch_max;
    }

  if (dec->stats.eof)
    {
      gpx_decode_append_tail(dec, points, max, &delivered);
      *n_out = delivered;
      return 1;
    }

  if (dec->stats.limit_reached)
    {
      int drain = gpx_decode_drain_tail(dec);

      if (drain != 0)
        {
          *n_out = 0;
          return drain;
        }

      gpx_decode_append_tail(dec, points, max, &delivered);
      *n_out = delivered;
      return 1;
    }

  while (delivered < max)
    {
      if (dec->cfg.limit > 0 &&
          dec->stats.points_delivered >= dec->cfg.limit)
        {
          dec->stats.limit_reached = true;
          break;
        }

      /* 抽稀时留 1 个给文件末点；stride=1 则全部完整解析直到 limit。 */
      if (dec->cfg.limit > 0 && dec->cfg.stride > 1 &&
          dec->stats.points_delivered + 1u >= dec->cfg.limit)
        {
          dec->stats.limit_reached = true;
          break;
        }

      if (dec->skip_remaining > 0)
        {
          res = gpx_reader_skip(&dec->io, &dec->tail);
          if (res == GPX_READ_EOF)
            {
              dec->stats.eof = true;
              gpx_decode_append_tail(dec, points, max, &delivered);
              *n_out = delivered;
              return 1;
            }

          if (res != GPX_READ_OK)
            {
              *n_out = delivered;
              return delivered > 0 ? 0 : -EIO;
            }

          dec->have_tail = true;
          dec->stats.file_points++;
          dec->skip_remaining--;
          dec->stats.points_skipped++;
          gpx_decode_add_len(dec, dec->tail.longitude, dec->tail.latitude);
          continue;
        }

      gpx_decode_prep_point(dec, &points[delivered]);
      res = gpx_reader_next(&dec->io, &points[delivered]);
      if (res == GPX_READ_EOF)
        {
          dec->stats.eof = true;
          gpx_decode_append_tail(dec, points, max, &delivered);
          *n_out = delivered;
          return 1;
        }

      if (res != GPX_READ_OK)
        {
          *n_out = delivered;
          return delivered > 0 ? 0 : -EIO;
        }

      dec->stats.file_points++;
      if (!gpx_decode_ll_ok(points[delivered].longitude,
                            points[delivered].latitude))
        {
          dec->stats.points_skipped++;
          dec->have_tail = false;
          continue;
        }
      gpx_decode_add_len(dec, points[delivered].longitude,
                         points[delivered].latitude);
      gpx_decode_emit(dec, &points[delivered], &points[delivered]);
      delivered++;
      dec->have_tail = false;

      if (dec->cfg.stride > 1)
        {
          dec->skip_remaining = dec->cfg.stride - 1;
        }

      if (dec->cfg.limit > 0 && dec->cfg.stride > 1 &&
          dec->stats.points_delivered + 1u >= dec->cfg.limit)
        {
          dec->stats.limit_reached = true;
          break;
        }

      if (dec->cfg.limit > 0 &&
          dec->stats.points_delivered >= dec->cfg.limit)
        {
          dec->stats.limit_reached = true;
          break;
        }
    }

  if (dec->stats.limit_reached && !dec->stats.eof)
    {
      int drain = gpx_decode_drain_tail(dec);

      if (drain != 0)
        {
          *n_out = delivered;
          return delivered > 0 ? 0 : drain;
        }

      gpx_decode_append_tail(dec, points, max, &delivered);
    }

  *n_out = delivered;

  if (dec->stats.eof || dec->stats.limit_reached)
    {
      return 1;
    }

  return 0;
}

void gpx_decode_close(gpx_decode_t **dec)
{
  if (dec == NULL || *dec == NULL)
    {
      return;
    }

  if ((*dec)->opened)
    {
      gpx_file_io_close(&(*dec)->fio);
      (*dec)->opened = false;
    }

  gpx_port_free((*dec)->scratch);
  (*dec)->scratch = NULL;
  gpx_port_free(*dec);
  *dec = NULL;
}

int gpx_decode_get_stats(gpx_decode_t *dec, gpx_decode_stats_t *stats)
{
  if (dec == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  *stats = dec->stats;
  return 0;
}
