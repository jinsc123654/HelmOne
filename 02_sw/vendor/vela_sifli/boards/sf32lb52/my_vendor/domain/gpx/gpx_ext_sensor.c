/**
 * @file gpx_ext_sensor.c
 * @brief 传感器 GPX 扩展实现：心率 / 踏频 / 速度（gpxtpx v2）+ 功率（gpxpx）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_ext_sensor.h"

#include "gpx_ext.h"
#include "gpx_port.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPX_SENSOR_NS_GPXTPX \
  "http://www.garmin.com/xmlschemas/TrackPointExtension/v2"
#define GPX_SENSOR_NS_POWER \
  "http://www.garmin.com/xmlschemas/PowerExtension/v1"

/* 两个外层标签各自带命名空间声明，只写一次；下面的字段都是纯元素。
 * 单独做成常量而不是把 XML 直接拼进 format：format 串跨行拼接会被安全扫描
 * 判成隐患（本文件曾经就是这个写法）。
 *
 * 用 v2 而不是 v1：gpxtpx:speed 只在 v2 里定义。
 */
static const char k_gpxtpx_open[] =
  "      <gpxtpx:TrackPointExtension xmlns:gpxtpx=\""
  GPX_SENSOR_NS_GPXTPX "\">\n";

/**
 * @brief 将传感器扩展格式化为 GPX 1.1 <extensions> 内部 XML 片段。
 *
 * @param data  gpx_sensor_ext_t 指针
 * @param buf   输出缓冲区
 * @param size  缓冲区容量（字节）
 * @return 写入字节数（>0），无内容时 0，失败返回负 errno
 */
static int gpx_sensor_format(const void *data, char *buf, size_t size)
{
  const gpx_sensor_ext_t *sensor = data;
  int off = 0;
  int n;

  if (sensor == NULL || buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  if (sensor->has_hr || sensor->has_cad || sensor->has_speed)
    {
      n = snprintf(buf + off, size - (size_t)off, "%s", k_gpxtpx_open);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;

      if (sensor->has_hr)
        {
          n = snprintf(buf + off, size - (size_t)off,
                       "        <gpxtpx:hr>%u</gpxtpx:hr>\n",
                       (unsigned)sensor->hr);
          if (n < 0 || (size_t)off + (size_t)n >= size)
            {
              return -ENOMEM;
            }

          off += n;
        }

      if (sensor->has_cad)
        {
          n = snprintf(buf + off, size - (size_t)off,
                       "        <gpxtpx:cad>%u</gpxtpx:cad>\n",
                       (unsigned)sensor->cad);
          if (n < 0 || (size_t)off + (size_t)n >= size)
            {
              return -ENOMEM;
            }

          off += n;
        }

      if (sensor->has_speed)
        {
          /* gpxtpx:speed 的单位是 m/s（Garmin TrackPointExtension v1）。 */
          n = snprintf(buf + off, size - (size_t)off,
                       "        <gpxtpx:speed>%.2f</gpxtpx:speed>\n",
                       (double)sensor->speed_mps);
          if (n < 0 || (size_t)off + (size_t)n >= size)
            {
              return -ENOMEM;
            }

          off += n;
        }

      n = snprintf(buf + off, size - (size_t)off,
                   "      </gpxtpx:TrackPointExtension>\n");
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  if (sensor->has_power)
    {
      /* Garmin PowerExtension v1 只定义 PowerInWatts 一个元素（xsd:unsignedShort，
       * 单位 W），直接挂在 <extensions> 下、没有外层包裹。 */
      n = snprintf(buf + off, size - (size_t)off,
                   "      <gpxpx:PowerInWatts xmlns:gpxpx=\"%s\">%u</gpxpx:PowerInWatts>\n",
                   GPX_SENSOR_NS_POWER, (unsigned)sensor->power_w);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  return off;
}

/**
 * @brief 从扩展 XML 中解析 <gpxtpx:hr> 字段。
 *
 * @param xml     扩展 XML 片段
 * @param sensor  输出结构（写入 hr 与 has_hr）
 * @return 0 解析成功，1 未找到，负 errno 解析错误
 */
static int gpx_sensor_parse_hr(const char *xml, gpx_sensor_ext_t *sensor)
{
  const char *tag = "<gpxtpx:hr>";
  const char *start;
  char *end;
  unsigned long val;

  start = strstr(xml, tag);
  if (start == NULL)
    {
      return 1;
    }

  start += strlen(tag);
  val = strtoul(start, &end, 10);
  if (end == start)
    {
      return -EINVAL;
    }

  sensor->hr = (uint16_t)val;
  sensor->has_hr = true;
  return 0;
}

/**
 * @brief 从扩展 XML 中解析 <gpxtpx:cad> 字段。
 *
 * @param xml     扩展 XML 片段
 * @param sensor  输出结构（写入 cad 与 has_cad）
 * @return 0 解析成功，1 未找到，负 errno 解析错误
 */
static int gpx_sensor_parse_cad(const char *xml, gpx_sensor_ext_t *sensor)
{
  const char *tag = "<gpxtpx:cad>";
  const char *start;
  char *end;
  unsigned long val;

  start = strstr(xml, tag);
  if (start == NULL)
    {
      return 1;
    }

  start += strlen(tag);
  val = strtoul(start, &end, 10);
  if (end == start)
    {
      return -EINVAL;
    }

  sensor->cad = (uint16_t)val;
  sensor->has_cad = true;
  return 0;
}

/**
 * @brief 从扩展 XML 中解析 <gpxtpx:speed> 字段（m/s）。
 *
 * @param xml     扩展 XML 片段
 * @param sensor  输出结构（写入 speed_mps 与 has_speed）
 * @return 0 解析成功，1 未找到，负 errno 解析错误
 */
static int gpx_sensor_parse_speed(const char *xml, gpx_sensor_ext_t *sensor)
{
  const char *tag = "<gpxtpx:speed>";
  const char *start;
  char *end;
  double val;

  start = strstr(xml, tag);
  if (start == NULL)
    {
      return 1;
    }

  start += strlen(tag);
  val = strtod(start, &end);
  if (end == start)
    {
      return -EINVAL;
    }

  sensor->speed_mps = (float)val;
  sensor->has_speed = true;
  return 0;
}

/**
 * @brief 从扩展 XML 中解析 <gpxpx:PowerInWatts> 字段（W）。
 *
 * @param xml     扩展 XML 片段
 * @param sensor  输出结构（写入 power_w 与 has_power）
 * @return 0 解析成功，1 未找到，负 errno 解析错误
 */
static int gpx_sensor_parse_power(const char *xml, gpx_sensor_ext_t *sensor)
{
  /* 开标签带命名空间声明（xmlns:gpxpx=…），所以不能直接 strstr 找 "<…>"：
   * 先定位标签名，再跳到它的 '>' 之后取数值。 */
  const char *tag = "<gpxpx:PowerInWatts";
  const char *start;
  char *end;
  unsigned long val;

  start = strstr(xml, tag);
  if (start == NULL)
    {
      return 1;
    }

  start = strchr(start, '>');
  if (start == NULL)
    {
      return -EINVAL;
    }

  start++;
  val = strtoul(start, &end, 10);
  if (end == start)
    {
      return -EINVAL;
    }

  sensor->power_w = (uint16_t)val;
  sensor->has_power = true;
  return 0;
}

/**
 * @brief 解析 <extensions> 内部 XML 到 gpx_sensor_ext_t。
 *
 * @param xml   扩展 XML（不必 NUL 结尾，使用 len）
 * @param len   xml 长度（字节）
 * @param data  输出 gpx_sensor_ext_t 指针
 * @return 0 匹配并解析成功，1 不匹配（跳过），负 errno 错误
 */
static int gpx_sensor_parse(const char *xml, size_t len, void *data)
{
  gpx_sensor_ext_t scratch;
  gpx_sensor_ext_t *sensor = data;
  char *copy = NULL;
  const char *src;
  int ret_hr;
  int ret_cad;
  int ret_speed;
  int ret_power;
  bool matched = false;

  if (xml == NULL || !gpx_ext_data_bound(sensor))
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return 1;
    }

  /* 调用方已 NUL 结尾（流式 scratch）则免拷。 */
  if (xml[len] == '\0')
    {
      src = xml;
    }
  else
    {
      copy = gpx_port_malloc(len + 1);
      if (copy == NULL)
        {
          return -ENOMEM;
        }

      memcpy(copy, xml, len);
      copy[len] = '\0';
      src = copy;
    }

  memset(&scratch, 0, sizeof(scratch));
  ret_hr = gpx_sensor_parse_hr(src, &scratch);
  if (ret_hr < 0)
    {
      gpx_port_free(copy);
      return ret_hr;
    }

  ret_cad = gpx_sensor_parse_cad(src, &scratch);
  if (ret_cad < 0)
    {
      gpx_port_free(copy);
      return ret_cad;
    }

  ret_speed = gpx_sensor_parse_speed(src, &scratch);
  if (ret_speed < 0)
    {
      gpx_port_free(copy);
      return ret_speed;
    }

  ret_power = gpx_sensor_parse_power(src, &scratch);
  if (ret_power < 0)
    {
      gpx_port_free(copy);
      return ret_power;
    }

  gpx_port_free(copy);

  if (ret_hr == 0 || ret_cad == 0 || ret_speed == 0 || ret_power == 0)
    {
      matched = true;
    }

  if (!matched)
    {
      return 1;
    }

  *sensor = scratch;
  return 0;
}

/**
 * @brief 将传感器扩展格式化为可读文本（decode / NSH 打印用）。
 *
 * @param data  gpx_sensor_ext_t 指针
 * @param buf   输出缓冲区
 * @param size  缓冲区容量（字节）
 * @return 写入字节数（>0），无内容时 0，失败返回负 errno
 */
static int gpx_sensor_print(const void *data, char *buf, size_t size)
{
  const gpx_sensor_ext_t *sensor = data;
  int off = 0;
  int n;

  if (sensor == NULL || buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  if (sensor->has_hr)
    {
      n = snprintf(buf + off, size - (size_t)off, " hr=%u",
                   (unsigned)sensor->hr);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  if (sensor->has_cad)
    {
      n = snprintf(buf + off, size - (size_t)off, " cad=%u",
                   (unsigned)sensor->cad);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  if (sensor->has_speed)
    {
      /* 与文件里一致用 m/s；decode 出来 ÷3.6 才是 km/h。 */
      n = snprintf(buf + off, size - (size_t)off, " speed=%.2f",
                   (double)sensor->speed_mps);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  if (sensor->has_power)
    {
      n = snprintf(buf + off, size - (size_t)off, " power=%u",
                   (unsigned)sensor->power_w);
      if (n < 0 || (size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  return off;
}

/**
 * @brief 注册传感器扩展（format + parse + print）。
 *
 * @return slot 编号 0 … GPX_EXT_SLOT_MAX-1，失败返回负 errno
 */
int gpx_ext_sensor_register(void)
{
  gpx_ext_desc_t desc;

  memset(&desc, 0, sizeof(desc));
  desc.name = "sensor";
  desc.data_size = sizeof(gpx_sensor_ext_t);
  desc.fmt_max = GPX_EXT_SENSOR_FMT_MAX;
  /* 四个读数 + 风向的文字都比显式数字长（" speed=12.34 power=9999"），
   * decode/NSH 打印缓冲按这个值分配，给小了会 -ENOMEM 少打字段。 */
  desc.print_max = 64;
  desc.format = gpx_sensor_format;
  desc.parse = gpx_sensor_parse;
  desc.print = gpx_sensor_print;

  return gpx_ext_register(&desc);
}

/**
 * @brief 注销先前注册的传感器扩展 slot。
 *
 * @param slot  gpx_ext_sensor_register() 返回的 slot
 * @return 0 成功，负 errno 失败
 */
int gpx_ext_sensor_unregister(int slot)
{
  return gpx_ext_unregister(slot);
}