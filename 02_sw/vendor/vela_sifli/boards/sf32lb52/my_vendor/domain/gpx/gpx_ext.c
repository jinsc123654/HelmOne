/**
 * @file gpx_ext.c
 * @brief GPX 1.1 扩展注册表实现。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gpx_ext.h"
#include "gpx_port.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct gpx_ext_slot
{
  bool used;
  gpx_ext_desc_t desc;
} gpx_ext_slot_t;

static gpx_ext_slot_t g_slots[GPX_EXT_SLOT_MAX];

/**
 * @brief 校验扩展描述符是否合法。
 *
 * @param desc  待校验描述符
 * @return true 合法，false 非法
 */
static bool gpx_ext_desc_valid(const gpx_ext_desc_t *desc)
{
  if (desc == NULL || desc->name == NULL || desc->format == NULL)
    {
      return false;
    }

  if (desc->fmt_max == 0)
    {
      return false;
    }

  if (desc->parse != NULL && desc->data_size == 0)
    {
      return false;
    }

  if (desc->print != NULL && desc->data_size == 0)
    {
      return false;
    }

  return true;
}

/**
 * @brief 注册扩展（format 必填，parse / print 可选）。
 *
 * @param desc  扩展描述符
 * @return slot 编号 0 … GPX_EXT_SLOT_MAX-1，失败返回负 errno
 */
int gpx_ext_register(const gpx_ext_desc_t *desc)
{
  int slot;

  if (!gpx_ext_desc_valid(desc))
    {
      return -EINVAL;
    }

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      if (!g_slots[slot].used)
        {
          g_slots[slot].used = true;
          g_slots[slot].desc = *desc;
          return slot;
        }
    }

  return -ENOMEM;
}

/**
 * @brief 注销指定 slot 的扩展。
 *
 * @param slot  先前 gpx_ext_register() 返回的 slot
 * @return 0 成功，负 errno 失败
 */
int gpx_ext_unregister(int slot)
{
  if (slot < 0 || slot >= GPX_EXT_SLOT_MAX)
    {
      return -EINVAL;
    }

  if (!g_slots[slot].used)
    {
      return -ENOENT;
    }

  memset(&g_slots[slot], 0, sizeof(g_slots[slot]));
  return 0;
}

/** 注销所有已注册扩展。 */
void gpx_ext_unregister_all(void)
{
  memset(g_slots, 0, sizeof(g_slots));
}

/**
 * @brief 查询当前已注册扩展数量。
 *
 * @return 已占用 slot 个数
 */
unsigned gpx_ext_registered_count(void)
{
  unsigned count = 0;
  int slot;

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      if (g_slots[slot].used)
        {
          count++;
        }
    }

  return count;
}

/**
 * @brief 查询指定 slot 的 data_size。
 *
 * @param slot  slot 编号
 * @return data_size；未使用 slot 返回 0
 */
size_t gpx_ext_data_size(int slot)
{
  if (slot < 0 || slot >= GPX_EXT_SLOT_MAX || !g_slots[slot].used)
    {
      return 0;
    }

  return g_slots[slot].desc.data_size;
}

/**
 * @brief 计算所有已注册扩展单点 fmt_max 之和，含 <extensions> 包裹开销。
 *
 * @return 单点 XML 扩展部分理论最大字节数；无注册扩展时 0
 */
size_t gpx_ext_fmt_max_per_trkpt(void)
{
  size_t total = 0;
  int slot;

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      if (g_slots[slot].used)
        {
          total += g_slots[slot].desc.fmt_max;
        }
    }

  if (total == 0)
    {
      return 0;
    }

  return total + GPX_EXT_WRAP_OVERHEAD;
}

/**
 * @brief 判断轨迹点是否携带可格式化的扩展 payload。
 *
 * @param point  轨迹点
 * @return true 至少一个活跃 slot 有 ext_data，false 否则
 */
static bool gpx_ext_point_has_payload(const gpx_point_t *point)
{
  int slot;

  if (point == NULL || point->ext_mask == 0)
    {
      return false;
    }

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      if ((point->ext_mask & (uint8_t)(1u << slot)) != 0 &&
          g_slots[slot].used &&
          gpx_ext_data_bound(point->ext_data[slot]))
        {
          return true;
        }
    }

  return false;
}

/**
 * @brief 格式化轨迹点上所有活跃扩展（含 <extensions> 包裹）。
 *
 * @param point  轨迹点（ext_mask / ext_data 有效）
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 0 无内容，>0 写入字节数，负 errno 失败
 */
int gpx_ext_format_trkpt(const gpx_point_t *point, char *buf, size_t size)
{
  int off = 0;
  int n;
  int slot;
  bool opened = false;

  if (point == NULL || buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  if (!gpx_ext_point_has_payload(point))
    {
      return 0;
    }

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      const gpx_ext_desc_t *desc;
      int written;

      if ((point->ext_mask & (uint8_t)(1u << slot)) == 0 ||
          !g_slots[slot].used ||
          !gpx_ext_data_bound(point->ext_data[slot]))
        {
          continue;
        }

      desc = &g_slots[slot].desc;
      if (!opened)
        {
          n = snprintf(buf + off, size - (size_t)off,
                       "    <extensions>\n");
          if (n < 0 || (size_t)off + (size_t)n >= size)
            {
              return -ENOMEM;
            }

          off += n;
          opened = true;
        }

      written = desc->format(point->ext_data[slot],
                             buf + off, size - (size_t)off);
      if (written < 0)
        {
          return written;
        }

      if (written == 0)
        {
          continue;
        }

      if ((size_t)off + (size_t)written >= size)
        {
          return -ENOMEM;
        }

      off += written;
    }

  if (!opened)
    {
      return 0;
    }

  n = snprintf(buf + off, size - (size_t)off, "    </extensions>\n");
  if (n < 0 || (size_t)off + (size_t)n >= size)
    {
      return -ENOMEM;
    }

  return off + n;
}

/**
 * @brief 将 <extensions> 内部 XML 解析到 point->ext_data[slot]。
 *
 * @param xml    扩展 XML 片段
 * @param len    xml 长度（字节）
 * @param point  输出轨迹点
 * @return 0 至少一个扩展匹配，1 无匹配，负 errno 错误
 */
int gpx_ext_parse_trkpt(const char *xml, size_t len, gpx_point_t *point)
{
  int slot;
  int ret;
  bool any = false;

  if (xml == NULL || point == NULL)
    {
      return -EINVAL;
    }

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      const gpx_ext_desc_t *desc;

      if (!g_slots[slot].used)
        {
          continue;
        }

      desc = &g_slots[slot].desc;
      if (desc->parse == NULL || !gpx_ext_data_bound(point->ext_data[slot]))
        {
          continue;
        }

      ret = desc->parse(xml, len, point->ext_data[slot]);
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          point->ext_mask |= (uint8_t)(1u << slot);
          any = true;
        }
    }

  return any ? 0 : 1;
}

/**
 * @brief 获取扩展 print 回调的单点文本容量上限。
 *
 * @param desc  扩展描述符
 * @return print_max；为 0 时返回 GPX_EXT_PRINT_DEFAULT_MAX
 */
static size_t gpx_ext_print_cap(const gpx_ext_desc_t *desc)
{
  if (desc->print_max > 0)
    {
      return desc->print_max;
    }

  return GPX_EXT_PRINT_DEFAULT_MAX;
}

/**
 * @brief 打印轨迹点上所有活跃扩展的可读字段（空格前缀字段）。
 *
 * @param point  轨迹点
 * @param buf    输出缓冲区
 * @param size   缓冲区容量（字节）
 * @return 0 无内容，>0 写入字节数，负 errno 失败
 */
int gpx_ext_print_trkpt(const gpx_point_t *point, char *buf, size_t size)
{
  int off = 0;
  int slot;

  if (point == NULL || buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  if (point->ext_mask == 0)
    {
      return 0;
    }

  buf[0] = '\0';

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      const gpx_ext_desc_t *desc;
      int n;

      if ((point->ext_mask & (uint8_t)(1u << slot)) == 0 ||
          !g_slots[slot].used ||
          !gpx_ext_data_bound(point->ext_data[slot]))
        {
          continue;
        }

      desc = &g_slots[slot].desc;
      if (desc->print == NULL)
        {
          continue;
        }

      {
        size_t room = size - (size_t)off;
        size_t cap = gpx_ext_print_cap(desc);

        if (cap < room)
          {
            room = cap;
          }

        if (room <= 1)
          {
            continue;
          }

        n = desc->print(point->ext_data[slot], buf + off, room);
      }
      if (n < 0)
        {
          return n;
        }

      if (n == 0)
        {
          continue;
        }

      if ((size_t)off + (size_t)n >= size)
        {
          return -ENOMEM;
        }

      off += n;
    }

  return off;
}

/**
 * @brief 深拷贝 src 的扩展 payload 到 dst（每 slot malloc）。
 *
 * @param dst  目标轨迹点
 * @param src  源轨迹点
 * @return 0 成功，负 errno 失败（失败时释放 dst 已分配扩展）
 */
int gpx_ext_point_clone_ext(gpx_point_t *dst, const gpx_point_t *src)
{
  int slot;

  if (dst == NULL || src == NULL)
    {
      return -EINVAL;
    }

  gpx_ext_point_free_ext(dst);

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      const gpx_ext_desc_t *desc;
      void *copy;
      size_t data_size;

      if ((src->ext_mask & (uint8_t)(1u << slot)) == 0 ||
          !gpx_ext_data_bound(src->ext_data[slot]) ||
          !g_slots[slot].used)
        {
          continue;
        }

      desc = &g_slots[slot].desc;
      data_size = desc->data_size;
      if (data_size == 0)
        {
          continue;
        }

      copy = gpx_port_malloc(data_size);
      if (copy == NULL)
        {
          gpx_ext_point_free_ext(dst);
          return -ENOMEM;
        }

      memcpy(copy, src->ext_data[slot], data_size);
      dst->ext_data[slot] = copy;
      dst->ext_mask |= (uint8_t)(1u << slot);
    }

  return 0;
}

/**
 * @brief 释放 point 拥有的所有 ext_data[slot] 并清零 ext_mask。
 *
 * @param point  轨迹点
 */
void gpx_ext_point_free_ext(gpx_point_t *point)
{
  int slot;

  if (point == NULL)
    {
      return;
    }

  for (slot = 0; slot < GPX_EXT_SLOT_MAX; slot++)
    {
      if ((point->ext_mask & (uint8_t)(1u << slot)) == 0 ||
          !gpx_ext_data_bound(point->ext_data[slot]))
        {
          continue;
        }

      gpx_port_free(point->ext_data[slot]);
      point->ext_data[slot] = NULL;
    }

  point->ext_mask = 0;
}