/**
 * @file gpx_writer.c
 * @brief GPX 1.1 流式写入实现。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_writer.h"

#include <nuttx/config.h>

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#include "gpx_port.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define GPX_HEAD \
  "<gpx version=\"1.1\" creator=\"%s\"\n" \
  " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n" \
  " xmlns=\"http://www.topografix.com/GPX/1/1\"\n" \
  " xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 " \
  "http://www.topografix.com/GPX/1/1/gpx.xsd\"\n>\n"

/**
 * @brief 写入 CDATA 包裹的 XML 元素（空值跳过）。
 *
 * @param file   文件句柄
 * @param tag    元素标签名
 * @param value  文本内容
 * @return 0 成功，负 errno 失败
 */
static int gpx_write_cdata_name(gpx_port_file_t *file, const char *tag,
                                const char *value)
{
  if (value == NULL || value[0] == '\0')
    {
      return 0;
    }

  /* CDATA 不能含 ]]>，否则整份 GPX 截断。 */
  if (strstr(value, "]]>") != NULL)
    {
      return 0;
    }

  return gpx_port_file_write_fmt(file, "<%s><![CDATA[%s]]></%s>\n",
                                 tag, value, tag);
}

/**
 * @brief 计算单点格式化最大字节数（标准字段 + 已注册扩展）。
 *
 * @return 单点 XML 最大字节数
 */
size_t gpx_writer_trkpt_fmt_max(void)
{
  return GPX_TRKPT_BASE_FMT_MAX + gpx_ext_fmt_max_per_trkpt();
}

/**
 * @brief 创建 GPX 文件并写入头部与 <trkseg> 起始标签。
 *
 * @param writer  写入器句柄
 * @param path    输出文件路径（自动创建父目录）
 * @param meta    元数据；可为 NULL
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_open(gpx_writer_t *writer, const char *path,
                    const gpx_meta_t *meta)
{
  const char *creator = CONFIG_MYVENDOR_PRODUCT_NAME;
  int ret;

  if (writer == NULL || path == NULL)
    {
      return -EINVAL;
    }

  if (writer->open)
    {
      return -EBUSY;
    }

  ret = gpx_port_file_ensure_parent_dir(path);
  if (ret != 0)
    {
      return ret;
    }

  writer->file.fd = -1;
  writer->file.created = false;
  writer->file.rbuf = NULL;
  writer->file.wbuf = NULL;
  writer->file.rpos = 0;
  writer->file.rlen = 0;
  writer->file.wlen = 0;
  writer->file.rcap = 0;
  writer->file.wcap = 0;
  writer->file.path[0] = '\0';
  ret = gpx_port_file_open_write(&writer->file, path);
  if (ret != 0)
    {
      return ret;
    }

  if (meta != NULL && meta->creator != NULL)
    {
      creator = meta->creator;
    }

  if (gpx_port_file_write_fmt(&writer->file, GPX_HEAD, creator) != 0)
    {
      goto fail;
    }

  if (gpx_port_file_write_str(&writer->file, "<metadata>\n") != 0)
    {
      goto fail;
    }

  if (meta != NULL)
    {
      if (gpx_write_cdata_name(&writer->file, "name", meta->meta_name) != 0)
        {
          goto fail;
        }

      if (gpx_write_cdata_name(&writer->file, "desc", meta->meta_desc) != 0)
        {
          goto fail;
        }
    }

  if (gpx_port_file_write_str(&writer->file, "</metadata>\n<trk>\n") != 0)
    {
      goto fail;
    }

  if (meta != NULL)
    {
      if (gpx_write_cdata_name(&writer->file, "name", meta->track_name) != 0)
        {
          goto fail;
        }

      if (gpx_write_cdata_name(&writer->file, "desc", meta->track_desc) != 0)
        {
          goto fail;
        }
    }

  if (gpx_port_file_write_str(&writer->file, "<trkseg>\n") != 0)
    {
      goto fail;
    }

  writer->open = true;
  /* 空文件已占住。头留在 PSRAM。落盘只在缓存满或 close：append→fsync→close。 */
  return 0;

fail:
  gpx_port_file_close(&writer->file);
  writer->open = false;
  return -EIO;
}

/**
 * @brief 打开已有 GPX 继续写点，不写头部。
 */
int gpx_writer_open_append(gpx_writer_t *writer, const char *path)
{
  int ret;

  if (writer == NULL || path == NULL)
    {
      return -EINVAL;
    }

  if (writer->open)
    {
      return -EBUSY;
    }

  writer->file.fd = -1;
  writer->file.created = false;
  writer->file.rbuf = NULL;
  writer->file.wbuf = NULL;
  writer->file.rpos = 0;
  writer->file.rlen = 0;
  writer->file.wlen = 0;
  writer->file.rcap = 0;
  writer->file.wcap = 0;
  writer->file.path[0] = '\0';
  ret = gpx_port_file_open_append(&writer->file, path);
  if (ret != 0)
    {
      return ret;
    }

  writer->open = true;
  return 0;
}

/**
 * @brief 将轨迹点格式化为 GPX XML 文本（含扩展）。
 *
 * @param point  轨迹点
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 写入字节数（>0），失败返回负 errno
 */
int gpx_writer_format_trkpt(const gpx_point_t *point, char *buf, size_t size)
{
  int n;
  int off = 0;

  if (point == NULL || buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  n = snprintf(buf, size,
               "  <trkpt lat=\"%.6f\" lon=\"%.6f\">\n",
               (double)point->latitude,
               (double)point->longitude);
  if (n < 0 || (size_t)n >= size)
    {
      return -ENOMEM;
    }

  off = n;

  if (point->has_altitude)
    {
      n = snprintf(buf + off, size - (size_t)off,
                   "    <ele>%.2f</ele>\n",
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
                   "    <time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>\n",
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

  if (gpx_ext_registered_count() > 0)
    {
      n = gpx_ext_format_trkpt(point, buf + off, size - (size_t)off);
      if (n < 0)
        {
          return n;
        }

      if (n > 0)
        {
          off += n;
        }
    }

  n = snprintf(buf + off, size - (size_t)off, "  </trkpt>\n");
  if (n < 0 || (size_t)off + (size_t)n >= size)
    {
      return -ENOMEM;
    }

  return off + n;
}

/**
 * @brief 向 GPX 文件写入原始字节（批写用）。
 *
 * @param writer  写入器句柄
 * @param data    数据指针
 * @param len     字节数
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_write_bytes(gpx_writer_t *writer, const void *data, size_t len)
{
  if (writer == NULL || data == NULL || !writer->open)
    {
      return -EINVAL;
    }

  return gpx_port_file_write(&writer->file, data, len);
}

/**
 * @brief 尽量把缓存 write() 进文件系统（不 fsync）。
 */
int gpx_writer_flush(gpx_writer_t *writer)
{
  if (writer == NULL || !writer->open)
    {
      return -EINVAL;
    }

  return gpx_port_file_flush(&writer->file);
}

/**
 * @brief 把已写入的 GPX 刷到存储。
 */
int gpx_writer_sync(gpx_writer_t *writer)
{
  if (writer == NULL || !writer->open)
    {
      return -EINVAL;
    }

  return gpx_port_file_sync(&writer->file);
}

/**
 * @brief 格式化并写入单个轨迹点。
 *
 * @param writer  写入器句柄
 * @param point   轨迹点
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_write_trkpt(gpx_writer_t *writer, const gpx_point_t *point)
{
  size_t cap = gpx_writer_trkpt_fmt_max();
  char stack_buf[GPX_TRKPT_BASE_FMT_MAX];
  char *buf = stack_buf;
  char *heap = NULL;
  int n;
  int ret;

  if (writer == NULL || point == NULL || !writer->open)
    {
      return -EINVAL;
    }

  if (cap > sizeof(stack_buf))
    {
      heap = gpx_port_malloc(cap);
      if (heap == NULL)
        {
          return -ENOMEM;
        }

      buf = heap;
    }

  n = gpx_writer_format_trkpt(point, buf, cap);
  if (n < 0)
    {
      gpx_port_free(heap);
      return n;
    }

  ret = gpx_writer_write_bytes(writer, buf, (size_t)n);
  gpx_port_free(heap);
  return ret;
}

/**
 * @brief 写入 </trkseg></trk> 尾部并关闭文件。
 *
 * @param writer  写入器句柄
 * @return 0 成功，负 errno 失败
 */
int gpx_writer_close(gpx_writer_t *writer)
{
  int ret = 0;
  int close_ret;

  if (writer == NULL)
    {
      return -EINVAL;
    }

  if (!writer->open)
    {
      return 0;
    }

  if (gpx_port_file_write_str(&writer->file, "</trkseg>\n</trk>\n</gpx>\n") != 0)
    {
      ret = -EIO;
    }

  if (gpx_port_file_sync(&writer->file) != 0 && ret == 0)
    {
      ret = -EIO;
    }

  close_ret = gpx_port_file_close(&writer->file);
  if (close_ret != 0 && ret == 0)
    {
      ret = close_ret;
    }

  writer->open = false;
  return ret;
}