/**
 * @file gpx_reader.c
 * @brief GPX 1.1 轨迹点增量解码实现。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_reader.h"

#include "gpx_port.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpx_ext.h"

/* GPX_EXT_PARSE_BLOCK_MAX 现在定义在 gpx_reader.h（gpx_decode 也要用它给
 * scratch 定尺寸），见那里的说明。 */

#ifndef GPX_IO_FIND_MAX_BYTES
#  define GPX_IO_FIND_MAX_BYTES  (256 * 1024)
#endif

#ifndef GPX_IO_SKIP_WS_MAX_BYTES
#  define GPX_IO_SKIP_WS_MAX_BYTES  (64 * 1024)
#endif

#ifndef GPX_IO_UNTIL_MAX_BYTES
#  define GPX_IO_UNTIL_MAX_BYTES  (16 * 1024)
#endif

#ifndef GPX_IO_TAG_MAX_BYTES
#  define GPX_IO_TAG_MAX_BYTES  1024
#endif

#ifndef GPX_IO_TRKPT_OPEN_MAX
#  define GPX_IO_TRKPT_OPEN_MAX  1024
#endif

/**
 * @brief 跳过行首空白字符。
 *
 * @param line  输入行
 * @return 第一个非空白字符指针；line 为 NULL 时返回 NULL
 */
static const char *gpx_line_skip_ws(const char *line)
{
  if (line == NULL)
    {
      return NULL;
    }

  while (*line == ' ' || *line == '\t')
    {
      line++;
    }

  return line;
}

/**
 * @brief 从 gpx_io 读取一个字节。
 *
 * @param io  字节流
 * @return 0–255 字节值，失败返回 -1
 */
static int gpx_io_read_byte(gpx_io_t *io)
{
  if (io == NULL || io->read == NULL)
    {
      return -1;
    }

  return io->read(io->ctx);
}

/**
 * @brief 在流中顺序匹配 needle 字符串。
 *
 * @param io      字节流
 * @param needle  待匹配字符串
 * @return 0 匹配成功，-1 失败或 EOF
 */
static int gpx_io_find(gpx_io_t *io, const char *needle)
{
  size_t i = 0;
  size_t scanned = 0;

  if (needle == NULL)
    {
      return -1;
    }

  while (needle[i] != '\0')
    {
      int ch = gpx_io_read_byte(io);

      if (ch < 0)
        {
          return -1;
        }

      if (++scanned > GPX_IO_FIND_MAX_BYTES)
        {
          return -1;
        }

      if ((char)ch == needle[i])
        {
          i++;
        }
      else if (i > 0)
        {
          i = ((char)ch == needle[0]) ? 1 : 0;
        }
    }

  return 0;
}

/**
 * @brief 从 <trkpt lat="…" lon="…"> 行解析经纬度。
 *
 * @param line   XML 行
 * @param point  输出轨迹点
 * @return 0 成功，-1 失败
 */
static int gpx_parse_latlon(const char *line, gpx_point_t *point)
{
  const char *lat;
  const char *lon;
  char *end;

  lat = strstr(line, "lat=\"");
  lon = strstr(line, "lon=\"");
  if (lat == NULL || lon == NULL)
    {
      return -1;
    }

  lat += 5;
  lon += 5;
  point->latitude = strtof(lat, &end);
  if (end == lat || *end != '"')
    {
      return -1;
    }

  point->longitude = strtof(lon, &end);
  if (end == lon || *end != '"')
    {
      return -1;
    }

  if (!isfinite((double)point->latitude) ||
      !isfinite((double)point->longitude) ||
      point->latitude < -90.0f || point->latitude > 90.0f ||
      point->longitude < -180.0f || point->longitude > 180.0f)
    {
      return -1;
    }

  return 0;
}

/**
 * @brief 从单行 <time>…</time> 解析 ISO8601 UTC 时间。
 *
 * @param line   XML 行
 * @param point  输出轨迹点
 * @return 0 成功，-1 失败
 */
static int gpx_parse_time(const char *line, gpx_point_t *point)
{
  const char *tag;
  const char *start;
  const char *end;
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;

  tag = gpx_line_skip_ws(line);
  if (tag == NULL || strncmp(tag, "<time", 5) != 0)
    {
      return -1;
    }

  start = strchr(tag, '>');
  if (start == NULL)
    {
      return -1;
    }

  start++;
  end = strstr(start, "</time");
  if (end == NULL || end <= start)
    {
      return -1;
    }

  /* Mi Fitness 等导出为 …T19:25:15.000Z，小数秒可选。 */
  if (sscanf(start, "%d-%d-%dT%d:%d:%d",
             &year, &month, &day, &hour, &minute, &second) != 6)
    {
      return -1;
    }

  if (year < 1970 || year > 2100 ||
      month < 1 || month > 12 ||
      day < 1 || day > 31 ||
      hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 ||
      second < 0 || second > 60)
    {
      return -1;
    }

  point->time.year = (int16_t)year;
  point->time.month = (uint8_t)month;
  point->time.day = (uint8_t)day;
  point->time.hour = (uint8_t)hour;
  point->time.minute = (uint8_t)minute;
  point->time.second = (uint8_t)second;
  point->has_time = true;
  return 0;
}

/**
 * @brief 计算扩展 XML 解析块缓冲区容量。
 *
 * @return max(gpx_ext_fmt_max_per_trkpt(), GPX_EXT_PARSE_BLOCK_MAX)
 */
static size_t gpx_reader_ext_block_cap(void)
{
  size_t cap = gpx_ext_fmt_max_per_trkpt();

  if (cap < GPX_EXT_PARSE_BLOCK_MAX)
    {
      cap = GPX_EXT_PARSE_BLOCK_MAX;
    }

  return cap;
}

/**
 * @brief 跳过 XML 空白字符。
 *
 * @param io  字节流
 * @return 0 成功，-1 I/O 失败
 */
static int gpx_io_skip_ws(gpx_io_t *io)
{
  size_t scanned = 0;

  for (;;)
    {
      int ch = gpx_io_read_byte(io);

      if (ch < 0)
        {
          return -1;
        }

      if (++scanned > GPX_IO_SKIP_WS_MAX_BYTES)
        {
          return -1;
        }

      if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
        {
          if (ch != '<')
            {
              continue;
            }

          return 0;
        }
    }
}

/**
 * @brief 读取 <trkpt …> 开标签并解析 lat/lon（兼容单行压缩 GPX）。
 *
 * @param io     字节流（已定位在 "<trkpt" 之后）
 * @param point  输出轨迹点
 * @return 0 成功，-1 失败
 */
static int gpx_reader_read_trkpt_open(gpx_io_t *io, gpx_point_t *point)
{
  char line[256];
  size_t n = 0;
  size_t scanned = 0;
  static const char prefix[] = "<trkpt";

  memcpy(line, prefix, sizeof(prefix) - 1);
  n = sizeof(prefix) - 1;

  for (;;)
    {
      int ch = gpx_io_read_byte(io);

      if (ch < 0)
        {
          return -1;
        }

      if (++scanned > GPX_IO_TRKPT_OPEN_MAX)
        {
          return -1;
        }

      if (n + 1 >= sizeof(line))
        {
          if ((char)ch == '>')
            {
              break;
            }

          continue;
        }

      line[n++] = (char)ch;
      if ((char)ch == '>')
        {
          line[n] = '\0';
          break;
        }
    }

  return gpx_parse_latlon(line, point);
}

/**
 * @brief 读取标签名（不含 '<'；遇 '>' / 空白结束并消费至 '>'）。
 *
 * @param io    字节流（'<’ 已消费）
 * @param name  输出缓冲区
 * @param cap   容量
 * @return 标签名长度，-1 失败
 */
static int gpx_io_read_tag_name(gpx_io_t *io, char *name, size_t cap)
{
  size_t n = 0;
  size_t scanned = 0;
  int ch;

  if (name == NULL || cap == 0)
    {
      return -1;
    }

  for (;;)
    {
      ch = gpx_io_read_byte(io);
      if (ch < 0)
        {
          return -1;
        }

      if (++scanned > GPX_IO_TAG_MAX_BYTES)
        {
          return -1;
        }

      if ((char)ch == '>' || (char)ch == ' ' || (char)ch == '\t' ||
          (char)ch == '\r' || (char)ch == '\n')
        {
          break;
        }

      if (n + 1 < cap)
        {
          name[n++] = (char)ch;
        }
    }

  name[n] = '\0';

  if ((char)ch != '>')
    {
      while ((char)ch != '>')
        {
          ch = gpx_io_read_byte(io);
          if (ch < 0)
            {
              return -1;
            }

          if (++scanned > GPX_IO_TAG_MAX_BYTES)
            {
              return -1;
            }
        }
    }

  return (int)n;
}

/**
 * @brief 读取文本直到 marker（不含 marker 本身）。
 *
 * @param io       字节流
 * @param marker   结束标记，如 "</ele>"
 * @param buf      输出缓冲区
 * @param cap      容量
 * @param out_len  实际文本长度
 * @return 0 成功，-1 失败
 */
static int gpx_io_read_until_marker(gpx_io_t *io, const char *marker,
                                    char *buf, size_t cap, size_t *out_len)
{
  size_t mlen;
  size_t match = 0;
  size_t n = 0;
  size_t scanned = 0;
  size_t max_scan;

  if (marker == NULL || marker[0] == '\0')
    {
      return -1;
    }

  mlen = strlen(marker);
  max_scan = GPX_IO_UNTIL_MAX_BYTES;
  if (cap > 0 && cap * 8u > max_scan)
    {
      max_scan = cap * 8u;
    }

  for (;;)
    {
      int ch = gpx_io_read_byte(io);
      char c;

      if (ch < 0)
        {
          return -1;
        }

      if (++scanned > max_scan)
        {
          return -1;
        }

      c = (char)ch;

      if (buf != NULL && cap > 0 && n < cap)
        {
          buf[n++] = c;
        }
      else if (buf != NULL && cap > 0 && n == cap)
        {
          n = cap;
        }

      if (c == marker[match])
        {
          match++;
          if (match == mlen)
            {
              if (n >= mlen)
                {
                  n -= mlen;
                }

              if (cap > 0 && n >= cap)
                {
                  n = cap - 1;
                }

              if (buf != NULL && cap > 0)
                {
                  buf[n] = '\0';
                }

              if (out_len != NULL)
                {
                  *out_len = n;
                }

              return 0;
            }
        }
      else
        {
          match = (c == marker[0]) ? 1 : 0;
        }
    }
}

/**
 * @brief 从纯文本解析海拔（<ele> 内容）。
 *
 * @param text   数值字符串
 * @param point  输出轨迹点
 * @return 0 成功，-1 失败
 */
static int gpx_parse_ele_value(const char *text, gpx_point_t *point)
{
  char *end;

  if (text == NULL || text[0] == '\0')
    {
      return -1;
    }

  point->altitude = strtof(text, &end);
  if (end == text || !isfinite((double)point->altitude))
    {
      return -1;
    }

  point->has_altitude = true;
  return 0;
}

/**
 * @brief 从纯文本解析 ISO8601 时间（<time> 内容）。
 *
 * @param text   时间字符串
 * @param point  输出轨迹点
 * @return 0 成功，-1 失败
 */
static int gpx_parse_time_value(const char *text, gpx_point_t *point)
{
  char line[128];

  if (text == NULL)
    {
      return -1;
    }

  snprintf(line, sizeof(line), "<time>%s</time>", text);
  return gpx_parse_time(line, point);
}

/**
 * @brief 读取并解析 <extensions> … </extensions>（兼容单行 GPX）。
 *
 * @param io     字节流（开标签 '>' 已消费）
 * @param point  输出轨迹点
 * @return 0 成功，负 errno 失败
 */
static int gpx_reader_handle_extensions_body(gpx_io_t *io, gpx_point_t *point)
{
  size_t cap = gpx_reader_ext_block_cap();
  char *block = NULL;
  char discard[32];
  size_t len = 0;
  int ret;
  bool heap = false;

  if (io != NULL && io->scratch != NULL && io->scratch_cap >= cap)
    {
      block = io->scratch;
      cap = io->scratch_cap;
    }
  else
    {
      block = gpx_port_malloc(cap);
      heap = (block != NULL);
      if (block == NULL)
        {
          return gpx_io_read_until_marker(io, "</extensions>",
                                          discard, sizeof(discard), NULL);
        }
    }

  ret = gpx_io_read_until_marker(io, "</extensions>", block, cap, &len);
  if (ret != 0)
    {
      if (heap)
        {
          gpx_port_free(block);
        }

      return -1;
    }

  if (gpx_ext_registered_count() > 0)
    {
      (void)gpx_ext_parse_trkpt(block, len, point);
    }

  if (heap)
    {
      gpx_port_free(block);
    }

  return 0;
}

/**
 * @brief 从流中读取下一个轨迹点。
 *
 * @param io     字节流
 * @param point  输出轨迹点；解码扩展前须绑定 ext_data[slot]
 * @return GPX_READ_OK / GPX_READ_EOF / GPX_READ_ERR
 */
gpx_read_result_t gpx_reader_next(gpx_io_t *io, gpx_point_t *point)
{
  char tag[32];
  char text[96];
  void *ext_saved[GPX_EXT_SLOT_MAX];
  uint8_t bind_mask;
  int i;

  if (io == NULL || point == NULL)
    {
      return GPX_READ_ERR;
    }

  /* 只恢复调用方用 ext_mask 显式绑上的缓冲，丢掉栈垃圾指针。 */
  memcpy(ext_saved, point->ext_data, sizeof(ext_saved));
  bind_mask = point->ext_mask;
  memset(point, 0, sizeof(*point));
  for (i = 0; i < GPX_EXT_SLOT_MAX; i++)
    {
      if ((bind_mask & (uint8_t)(1u << i)) != 0 &&
          gpx_ext_data_bound(ext_saved[i]))
        {
          point->ext_data[i] = ext_saved[i];
        }
    }

  if (gpx_io_find(io, "<trkpt") != 0)
    {
      return GPX_READ_EOF;
    }

  if (gpx_reader_read_trkpt_open(io, point) != 0)
    {
      return GPX_READ_ERR;
    }

  for (;;)
    {
      if (gpx_io_skip_ws(io) != 0)
        {
          return GPX_READ_ERR;
        }

      if (gpx_io_read_tag_name(io, tag, sizeof(tag)) < 0)
        {
          return GPX_READ_ERR;
        }

      if (strcmp(tag, "/trkpt") == 0)
        {
          break;
        }

      if (strcmp(tag, "ele") == 0)
        {
          if (gpx_io_read_until_marker(io, "</ele>", text, sizeof(text), NULL) != 0)
            {
              return GPX_READ_ERR;
            }

          (void)gpx_parse_ele_value(text, point);
          continue;
        }

      if (strcmp(tag, "time") == 0)
        {
          if (gpx_io_read_until_marker(io, "</time>", text, sizeof(text), NULL) != 0)
            {
              return GPX_READ_ERR;
            }

          (void)gpx_parse_time_value(text, point);
          continue;
        }

      if (strcmp(tag, "extensions") == 0)
        {
          if (gpx_reader_handle_extensions_body(io, point) != 0)
            {
              return GPX_READ_ERR;
            }

          continue;
        }

      /* 未知子标签：跳过至闭合 '>' */
      {
        int ch;
        size_t scanned = 0;

        for (;;)
          {
            ch = gpx_io_read_byte(io);
            if (ch < 0)
              {
                return GPX_READ_ERR;
              }

            if (++scanned > GPX_IO_TAG_MAX_BYTES)
              {
                return GPX_READ_ERR;
              }

            if ((char)ch == '>')
              {
                break;
              }
          }
      }
    }

  return GPX_READ_OK;
}

gpx_read_result_t gpx_reader_skip(gpx_io_t *io, gpx_point_t *point)
{
  gpx_point_t tmp;
  gpx_point_t *pt = (point != NULL) ? point : &tmp;

  if (io == NULL)
    {
      return GPX_READ_ERR;
    }

  memset(pt, 0, sizeof(*pt));

  if (gpx_io_find(io, "<trkpt") != 0)
    {
      return GPX_READ_EOF;
    }

  if (gpx_reader_read_trkpt_open(io, pt) != 0)
    {
      return GPX_READ_ERR;
    }

  if (gpx_io_find(io, "</trkpt>") != 0)
    {
      return GPX_READ_ERR;
    }

  return GPX_READ_OK;
}

/**
 * @brief 格式化已解码轨迹点为可读文本（标准字段 + 已注册扩展 print）。
 *
 * @param point  轨迹点
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 写入字节数（>0），失败返回负 errno
 */
int gpx_reader_format_trkpt(const gpx_point_t *point, char *buf, size_t size)
{
  int n;
  int off = 0;
  int ext_n;

  if (point == NULL || buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  n = snprintf(buf, size, "lat=%.6f lon=%.6f",
               (double)point->latitude, (double)point->longitude);
  if (n < 0 || (size_t)n >= size)
    {
      return -ENOMEM;
    }

  off = n;

  if (point->has_altitude)
    {
      n = snprintf(buf + off, size - (size_t)off, " ele=%.2f",
                   (double)point->altitude);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  if (point->has_time)
    {
      n = snprintf(buf + off, size - (size_t)off,
                   " time=%04d-%02d-%02dT%02d:%02d:%02dZ",
                   point->time.year,
                   point->time.month,
                   point->time.day,
                   point->time.hour,
                   point->time.minute,
                   point->time.second);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  ext_n = gpx_ext_print_trkpt(point, buf + off, size - (size_t)off);
  if (ext_n < 0)
    {
      return ext_n;
    }

  return off + ext_n;
}

/**
 * @brief 文件后端：检查是否可读（fd 有效即认为可读）。
 *
 * @param ctx  gpx_file_io_t 指针
 * @return 1 可读，0 不可读
 */
static int gpx_file_available(void *ctx)
{
  gpx_file_io_t *fio = ctx;

  if (fio == NULL || fio->fd < 0)
    {
      return 0;
    }

  return 1;
}

/**
 * @brief gpx_io 读回调：经 gpx_port 读一字节。
 *
 * @param ctx  gpx_file_io_t 指针
 * @return 0–255 或 -1
 */
static int gpx_file_read(void *ctx)
{
  return gpx_port_file_read_byte(ctx);
}

/** 打开 GPX 文件（只读）；委托 gpx_port_file_open_read。 */
int gpx_file_io_open(gpx_file_io_t *fio, const char *path)
{
  return gpx_port_file_open_read(fio, path);
}

/** 关闭 GPX 文件；委托 gpx_port_file_close。 */
void gpx_file_io_close(gpx_file_io_t *fio)
{
  (void)gpx_port_file_close(fio);
}

/**
 * @brief 将 gpx_file_io 绑定到 gpx_io 字节流接口。
 *
 * @param fio  文件 I/O 上下文
 * @param io   输出 gpx_io 实例
 */
void gpx_file_io_bind(gpx_file_io_t *fio, gpx_io_t *io)
{
  if (fio == NULL || io == NULL)
    {
      return;
    }

  io->ctx = fio;
  io->available = gpx_file_available;
  io->read = gpx_file_read;
  io->scratch = NULL;
  io->scratch_cap = 0;
}