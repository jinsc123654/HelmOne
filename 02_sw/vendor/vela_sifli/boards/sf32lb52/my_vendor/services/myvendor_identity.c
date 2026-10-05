/**
 * @file myvendor_identity.c
 * @brief 产品身份：BLE/USB 广播名、序列号、启动槽与版本信息。
 *
 * 从 eFuse MAC 生成 `Helm One-XXXX` 与 12 位 serial；同步写入 USB MTP 字符串。
 * 启动槽从 `/mnt/kv/db/persist.boot.*` 读取。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_identity.h"
#include "companion_proto.h"

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "sf32lb52_bd_addr.h"
#include "myvendor_build_stamp.h"

#ifndef MYVENDOR_BUILD_DATE_STR
#  define MYVENDOR_BUILD_DATE_STR (__DATE__ " " __TIME__)
#endif

#ifndef CONFIG_MYVENDOR_PRODUCT_NAME
#  define CONFIG_MYVENDOR_PRODUCT_NAME "Helm One"
#endif

#define IDENTITY_NAME_MAX  (sizeof(CONFIG_MYVENDOR_PRODUCT_NAME) + 6)

static char g_name[IDENTITY_NAME_MAX];
static char g_serial[13];
static bool g_ready;

/** @brief 加载启动槽与软件版本到全局缓存。 */
static void bootinfo_load(void);

/** @brief 从 eFuse MAC 构建产品名与 serial。 */
static void identity_build(void)
{
  uint8_t addr[SF32LB52_BD_ADDR_LEN];

  if (g_ready)
    {
      return;
    }

  g_serial[0] = '\0';
  if (sf32lb52_bd_addr_from_efuse(addr) == 0)
    {
      snprintf(g_name, sizeof(g_name), "%s-%02X%02X",
               CONFIG_MYVENDOR_PRODUCT_NAME, addr[1], addr[0]);
      snprintf(g_serial, sizeof(g_serial),
               "%02X%02X%02X%02X%02X%02X",
               addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }
  else
    {
      snprintf(g_name, sizeof(g_name), "%s", CONFIG_MYVENDOR_PRODUCT_NAME);
    }

#ifdef CONFIG_USBMTP
  strncpy(g_usbmtp_productstr, g_name, USBMTP_PRODUCTSTR_MAX - 1);
  g_usbmtp_productstr[USBMTP_PRODUCTSTR_MAX - 1] = '\0';
  if (g_serial[0] != '\0')
    {
      strncpy(g_usbmtp_serialstr, g_serial, USBMTP_SERIALSTR_MAX - 1);
      g_usbmtp_serialstr[USBMTP_SERIALSTR_MAX - 1] = '\0';
    }
#endif

  syslog(LOG_INFO, "identity: %s%s%s\n", g_name,
         g_serial[0] != '\0' ? " serial=" : "", g_serial);
  g_ready = true;
  bootinfo_load();
}

/**
 * @brief 产品显示名（含 MAC 后缀）。
 * @return 静态字符串。
 */
const char *myvendor_identity_name(void)
{
  identity_build();
  return g_name;
}

/**
 * @brief 12 位十六进制序列号。
 * @return 静态字符串；无 MAC 时为空串。
 */
const char *myvendor_identity_serial(void)
{
  identity_build();
  return g_serial;
}

#ifndef CONFIG_MYVENDOR_PRODUCT_VERSION
#  define CONFIG_MYVENDOR_PRODUCT_VERSION "1.4.2"
#endif

#ifndef CONFIG_MYVENDOR_HARDWARE_VERSION
#  define CONFIG_MYVENDOR_HARDWARE_VERSION "1.0.0"
#endif

#define BOOT_KV_DB_DIR     "/mnt/kv/db/"
#define BOOT_KV_DB_PATH    "/mnt/kv/db"
#define BOOT_RUNNING_KEY   "persist.boot.running"
#define BOOT_TARGET_KEY    "persist.boot.target"
#define BOOT_VERSION_KEY   "persist.boot.version"

static uint8_t g_boot_slot;
static char    g_boot_fw_name[COMPANION_DEVINFO_NAME_LEN];
static char    g_boot_version[COMPANION_DEVINFO_BOOT_LEN];
static bool    g_bootinfo_ready;

/** @brief 去掉 KV 行尾空白与换行。 */
static void trim_line(char *buf)
{
  char *p;
  size_t n;

  n = strlen(buf);
  while (n > 0)
    {
      char c = buf[n - 1];

      if (c != '\0' && c != '\n' && c != '\r' && c != ' ' && c != '\t')
        {
          break;
        }

      buf[--n] = '\0';
    }

  p = buf;
  while (*p == ' ' || *p == '\t')
    {
      p++;
    }

  if (p != buf)
    {
      memmove(buf, p, strlen(p) + 1);
    }
}

#define KV_READ_OK      0
#define KV_READ_IO     -1
#define KV_READ_ABSENT -2

/**
 * @brief 读 KV 文件一行。
 * @return KV_READ_OK / KV_READ_IO / KV_READ_ABSENT。
 */
static int read_kv_line(const char *path, char *buf, size_t buflen)
{
  int fd;
  ssize_t n;

  if (path == NULL || buf == NULL || buflen < 2)
    {
      return KV_READ_IO;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return errno == ENOENT ? KV_READ_ABSENT : KV_READ_IO;
    }

  n = read(fd, buf, buflen - 1);
  close(fd);
  if (n <= 0)
    {
      return n == 0 ? KV_READ_ABSENT : KV_READ_IO;
    }

  buf[n] = '\0';
  trim_line(buf);
  return buf[0] != '\0' ? KV_READ_OK : KV_READ_ABSENT;
}

/**
 * @brief 读 `/mnt/kv/db/<key>`。
 */
static int read_boot_kv_line(const char *key, char *buf, size_t buflen)
{
  char path[80];

  snprintf(path, sizeof(path), BOOT_KV_DB_DIR "%s", key);
  return read_kv_line(path, buf, buflen);
}

/**
 * @brief 解析 persist.boot.* 槽位字符串。
 * @param[out] fw_name 可选输出 fw 名。
 * @return #COMPANION_SLOT_*。
 */
static uint8_t parse_boot_slot(const char *buf, char *fw_name, size_t fw_name_len)
{
  if (buf == NULL || buf[0] == '\0')
    {
      return COMPANION_SLOT_UNKNOWN;
    }

  if (fw_name != NULL && fw_name_len > 0)
    {
      fw_name[0] = '\0';
    }

  if (strncmp(buf, "fw", 2) == 0 &&
      (buf[2] == '\0' || buf[2] == ' ' || buf[2] == '\t'))
    {
      const char *name = buf + 2;

      while (*name == ' ' || *name == '\t')
        {
          name++;
        }

      if (fw_name != NULL && fw_name_len > 0 && name[0] != '\0')
        {
          strncpy(fw_name, name, fw_name_len - 1);
          fw_name[fw_name_len - 1] = '\0';
        }

      return COMPANION_SLOT_FW;
    }

  if (strcmp(buf, "main") == 0)
    {
      return COMPANION_SLOT_MAIN;
    }

  if (strcmp(buf, "factory") == 0)
    {
      return COMPANION_SLOT_FACTORY;
    }

  return COMPANION_SLOT_UNKNOWN;
}

static void bootinfo_load(void)
{
  char buf[96];
  const char *src = "running";
  int r;

  if (g_bootinfo_ready)
    {
      return;
    }

  g_boot_slot = COMPANION_SLOT_UNKNOWN;
  g_boot_fw_name[0] = '\0';
  g_boot_version[0] = '\0';

  r = read_boot_kv_line(BOOT_RUNNING_KEY, buf, sizeof(buf));
  if (r == KV_READ_OK)
    {
      g_boot_slot = parse_boot_slot(buf, g_boot_fw_name,
                                   sizeof(g_boot_fw_name));
      g_bootinfo_ready = true;
    }
  else if (r == KV_READ_ABSENT)
    {
      /* 旧 2SFBL 不写 persist.boot.running。sticky target 在 fw/main/factory
       * 时就是这次实际加载的槽（F 热键进工厂除外）。
       */

      r = read_boot_kv_line(BOOT_TARGET_KEY, buf, sizeof(buf));
      if (r == KV_READ_OK)
        {
          src = "target";
          g_boot_slot = parse_boot_slot(buf, g_boot_fw_name,
                                       sizeof(g_boot_fw_name));
          g_bootinfo_ready = true;
        }
      else if (r == KV_READ_ABSENT)
        {
          src = "none";
          g_bootinfo_ready = true;
        }
      else
        {
          src = "none";
        }
    }
  else
    {
      src = "none";
    }

  if (g_bootinfo_ready)
    {
      if (read_boot_kv_line(BOOT_VERSION_KEY, buf, sizeof(buf)) == KV_READ_OK)
        {
          strncpy(g_boot_version, buf, sizeof(g_boot_version) - 1);
          g_boot_version[sizeof(g_boot_version) - 1] = '\0';
        }
    }

  syslog(LOG_INFO, "bootinfo: via=%s slot=%u fw=%s sw=%s hw=%s boot=%s\n",
         src,
         (unsigned)g_boot_slot,
         g_boot_fw_name[0] != '\0' ? g_boot_fw_name : "-",
         CONFIG_MYVENDOR_PRODUCT_VERSION,
         CONFIG_MYVENDOR_HARDWARE_VERSION,
         g_boot_version[0] != '\0' ? g_boot_version : "-");
}

/**
 * @brief 当前启动槽。
 * @return #COMPANION_SLOT_*。
 */
uint8_t myvendor_boot_slot(void)
{
  bootinfo_load();
  return g_boot_slot;
}

/**
 * @brief 读 sticky `persist.boot.target`（下次 2SFBL 跳转槽）。
 */
uint8_t myvendor_boot_target(void)
{
  char buf[96];
  int r;

  r = read_boot_kv_line(BOOT_TARGET_KEY, buf, sizeof(buf));
  if (r != KV_READ_OK)
    {
      return COMPANION_SLOT_UNKNOWN;
    }

  return parse_boot_slot(buf, NULL, 0);
}

/**
 * @brief 写 `persist.boot.target` 为 fw 或 main（带结尾 NUL，对齐 2SFBL）。
 */
int myvendor_boot_target_set(uint8_t slot)
{
  const char *name;
  char path[80];
  char buf[8];
  size_t n;
  int fd;
  int ret;

  if (slot == COMPANION_SLOT_FW)
    {
      name = "fw";
    }
  else if (slot == COMPANION_SLOT_MAIN)
    {
      name = "main";
    }
  else
    {
      return -EINVAL;
    }

  ret = mkdir(BOOT_KV_DB_PATH, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_WARNING, "bootinfo: mkdir %s errno=%d\n",
             BOOT_KV_DB_PATH, errno);
      return -errno;
    }

  snprintf(path, sizeof(path), BOOT_KV_DB_DIR "%s", BOOT_TARGET_KEY);
  n = strlen(name);
  memcpy(buf, name, n);
  buf[n] = '\0';
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "bootinfo: target %s open errno=%d\n",
             name, errno);
      return -errno;
    }

  if (write(fd, buf, n + 1u) != (ssize_t)(n + 1u))
    {
      int err = errno;

      close(fd);
      syslog(LOG_WARNING, "bootinfo: target %s write errno=%d\n", name, err);
      return err != 0 ? -err : -EIO;
    }

  close(fd);
  syslog(LOG_INFO, "bootinfo: target=%s\n", name);
  return 0;
}

/**
 * @brief 当前镜像是否为工厂固件。
 */
bool myvendor_is_factory(void)
{
#ifdef CONFIG_MYVENDOR_FACTORY_MODE
  return true;
#else
  return false;
#endif
}

/** @brief 当前 fw 槽名称（fw 槽时有效）。 */
const char *myvendor_boot_fw_name(void)
{
  bootinfo_load();
  return g_boot_fw_name;
}

/** @brief persist.boot.version 字符串。 */
const char *myvendor_boot_version(void)
{
  bootinfo_load();
  return g_boot_version;
}

/** @brief Kconfig 软件版本。 */
const char *myvendor_sw_version(void)
{
  return CONFIG_MYVENDOR_PRODUCT_VERSION;
}

/** @brief ninja 生成的本地构建时间，精确到秒。 */
const char *myvendor_build_date(void)
{
  return MYVENDOR_BUILD_DATE_STR;
}

/** @brief Kconfig 硬件版本。 */
const char *myvendor_hw_version(void)
{
  return CONFIG_MYVENDOR_HARDWARE_VERSION;
}
