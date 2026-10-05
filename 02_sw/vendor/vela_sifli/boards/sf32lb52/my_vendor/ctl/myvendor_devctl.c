/**
 * @file myvendor_devctl.c
 * @brief 系统控制 owner 实现：persist 文件 + 调用 PWM / companion / USB 控制器。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "myvendor_devctl.h"

#include <nuttx/config.h>

#include "drv_io.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/syslog/syslog.h>

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
#  include "companion_bridge.h"
#endif

#include "myvendor_identity.h"
#include "myvendor_mtp.h"
#include "myvendor_gnss.h"
#include "myvendor_sys.h"
#include "sf32lb_dvfs.h"
#include "myvendor_usbdev.h"
#include "myvendor_watchdog.h"

#include "bf0_hal.h"

#ifndef MYVENDOR_DEVCTL_BL_DEFAULT
#  define MYVENDOR_DEVCTL_BL_DEFAULT 16u
#endif

/* KV 里没有 persist.log.mask 时用编译期的默认档位 —— 不硬编码，跟着本次构建
 * 走：产品固件 defconfig 是 0xf（只留 ERROR），工厂固件是 Kconfig 默认的
 * 0xff（全开）。工厂固件另外还不读也不写这个 key，见 myvendor_devctl_load()。 */
#ifndef CONFIG_SYSLOG_DEFAULT_MASK
#  define MYVENDOR_DEVCTL_LOG_DEFAULT 0x0fu
#else
#  define MYVENDOR_DEVCTL_LOG_DEFAULT (int32_t)CONFIG_SYSLOG_DEFAULT_MASK
#endif

#define DEVCTL_KV_DIR       "/mnt/kv/db/" /**< 全部 persist.*，含 persist.boot.*。 */
#define DEVCTL_KV_DB_PATH   "/mnt/kv/db"
#define DEVCTL_PWR_KEY_CTL_PIN  29 /**< PA29 PWR_KEY_CTL：高保持、低切断。 */
#define DEVCTL_SENSORS_TMP "/mnt/kv/.bicycle_sensors.tmp"

static bool g_loaded;   /**< 是否已从 KV 载入。 */
static uint8_t g_bl;    /**< 策略背光百分比。 */
static bool g_bl_dirty; /**< apply 后尚未 commit。 */
static bool g_radio = true;   /**< 默认开手机广播（上行）。 */
static bool g_sensor = true;  /**< 默认开外设 Central（下行）。 */
static bool g_mtp = true;    /**< 默认开 USB 控制器。 */
static bool g_mtp_nav_hold;  /**< 导航期间临时关闭，不改 persist。 */
static bool g_notif = true;  /**< 默认接受手机通知。 */
static bool g_calls;         /**< 默认不只过滤来电。 */
static bool g_sound = true;  /**< 默认开蜂鸣器。 */
static bool g_autopause = true; /**< 默认开骑行自动暂停。 */
static int32_t g_last_ride_m;   /**< 上次保存骑行里程，米。 */
static int32_t g_last_lon_e7;   /**< 上次骑行终点经度 ×1e7。 */
static int32_t g_last_lat_e7;   /**< 上次骑行终点纬度 ×1e7。 */
static bool g_last_pos_have;    /**< KV 是否已有合法终点。 */
static bool g_eph_auto = true; /**< 默认开机注入并定位后倒库。 */
static bool g_gnss_rmc; /**< 默认自定义滤波。 */
static int32_t g_grade_off;  /**< 坡度零点，毫度。 */
static int16_t g_tz_min = MYVENDOR_DEVCTL_TZ_DEFAULT;  /**< 时区分钟。 */
static int g_bat_full_mv = MYVENDOR_DEVCTL_BAT_FULL_DEFAULT; /**< 100% 毫伏。 */
static bool g_bat_full_have; /**< persist.battery.full_mv 是否已有合法值。 */
static uint8_t g_log_mask = MYVENDOR_DEVCTL_LOG_DEFAULT; /**< syslog 屏蔽位。 */
/** 「充电时也按负载动态调频」模式（见 `sf32lb_dvfs.h` / `MYVENDOR_DEVCTL_DVFS_CHARGE_DYN_KEY`）。 */
static bool g_dvfs_charge_dyn;
static uint32_t g_diag_cnt_mask = 0xffffffffu; /**< 周期计数器报表开关，位序见 diag。 */

static pthread_mutex_t g_srec_lock = PTHREAD_MUTEX_INITIALIZER;
static myvendor_devctl_sensor_rec_t g_srec[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
static uint8_t g_srec_n;
static uint32_t g_srec_gen;
static bool g_srec_ready;

/**
 * @brief 夹紧时区分钟。
 */
static int16_t clamp_tz(int v)
{
  if (v < MYVENDOR_DEVCTL_TZ_MIN)
    {
      return (int16_t)MYVENDOR_DEVCTL_TZ_MIN;
    }

  if (v > MYVENDOR_DEVCTL_TZ_MAX)
    {
      return (int16_t)MYVENDOR_DEVCTL_TZ_MAX;
    }

  return (int16_t)v;
}

static int persist_ensure_db(void)
{
  int ret;

  ret = mkdir(DEVCTL_KV_DB_PATH, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static void persist_make_path(char *path, size_t n, const char *key)
{
  snprintf(path, n, "%s%s", DEVCTL_KV_DIR, key);
}

static int persist_open_ro(const char *key)
{
  char path[80];

  persist_make_path(path, sizeof(path), key);
  return open(path, O_RDONLY);
}

/**
 * @brief 读 persist 文件中的十进制整数。
 * @return 成功为解析值，否则 @p fallback。
 */
static int persist_get_i32(const char *key, int32_t fallback)
{
  char buf[32];
  int fd;
  ssize_t n;
  char *end;
  long v;

  if (key == NULL || key[0] == '\0')
    {
      return fallback;
    }

  fd = persist_open_ro(key);
  if (fd < 0)
    {
      return fallback;
    }

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    {
      return fallback;
    }

  buf[n] = '\0';
  v = strtol(buf, &end, 10);
  if (end == buf)
    {
      return fallback;
    }

  return (int32_t)v;
}

/**
 * @brief 读 `/mnt/kv/db/<key>`；文件存在且能解析则写入 @p out。
 * @return true 读到了整数。
 */
static bool persist_try_i32(const char *key, int32_t *out)
{
  char buf[32];
  int fd;
  ssize_t n;
  char *end;
  long v;

  if (key == NULL || key[0] == '\0' || out == NULL)
    {
      return false;
    }

  fd = persist_open_ro(key);
  if (fd < 0)
    {
      return false;
    }

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    {
      return false;
    }

  buf[n] = '\0';
  v = strtol(buf, &end, 10);
  if (end == buf)
    {
      return false;
    }

  *out = (int32_t)v;
  return true;
}

/**
 * @brief persist.battery.full_mv 是否在可当作 100% 点的范围内。
 */
static bool bat_full_mv_ok(int v)
{
  return v >= MYVENDOR_DEVCTL_BAT_FULL_OK_MIN &&
         v <= MYVENDOR_DEVCTL_BAT_FULL_OK_MAX;
}

static bool last_pos_ok(int32_t lon_e7, int32_t lat_e7)
{
  int32_t a_lon;
  int32_t a_lat;

  if (lon_e7 == 0 && lat_e7 == 0)
    {
      return false;
    }

  a_lon = lon_e7 < 0 ? -lon_e7 : lon_e7;
  a_lat = lat_e7 < 0 ? -lat_e7 : lat_e7;
  if (a_lon < 10000 && a_lat < 10000)
    {
      return false;
    }

  /* 全国图：72E–136E、17N–54N。超出当异常，开机用地图默认点。 */
  if (lon_e7 < 720000000 || lon_e7 > 1360000000)
    {
      return false;
    }

  if (lat_e7 < 170000000 || lat_e7 > 540000000)
    {
      return false;
    }

  return true;
}

/**
 * @brief 把整数写入 persist 文件（覆盖）。
 * @return 0 成功；负值为失败原因（mkdir / open / write 的 errno）。
 */
static int persist_set_i32(const char *key, int32_t v)
{
  char path[80];
  char buf[32];
  int fd;
  int n;

  if (key == NULL || key[0] == '\0')
    {
      return -EINVAL;
    }

  if (persist_ensure_db() < 0)
    {
      int err = errno;

      syslog(LOG_WARNING, "ctl: persist mkdir %s errno=%d", DEVCTL_KV_DB_PATH,
             err);
      return -err;
    }

  persist_make_path(path, sizeof(path), key);
  n = snprintf(buf, sizeof(buf), "%ld\n", (long)v);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      int err = errno;

      syslog(LOG_WARNING, "ctl: persist %s failed errno=%d", key, err);
      return -err;
    }

  if (write(fd, buf, (size_t)n) != n)
    {
      int err = errno;

      syslog(LOG_WARNING, "ctl: persist %s write errno=%d", key, err);
      close(fd);
      return -err;
    }

  close(fd);
  return 0;
}

/** @brief 把字符串写进 persist 文件，**按 KVDB 约定带上结尾 NUL**。
 *
 *  KVDB 的字符串值是 `property_set` 写出来的：`strlen+1`（**含 NUL**）。
 *  bootloader 侧同样是这个口径（`boot_target.c` 的
 *  `boot_lfs_put(path, buf, n + 1u)`，注释原文
 *  "match property_set strlen+1"），所以这里必须一致 ——
 *  原来那个 `persist.boot.target` 是 **3 字节**，正是 `fw\0`。
 *
 *  两边读的时候都会 trim（`boot_target.c` 的 `trim_val()` 会把结尾的
 *  `\0`/`\n`/空格都剪掉再 `strcmp`），所以少一个 NUL 也能读；
 *  但**写就得按约定写**，免得将来有人加严格解析时被这个差异坑到。 */
static int persist_set_str(const char *key, const char *s)
{
  char path[80];
  int fd;
  size_t n;

  if (key == NULL || key[0] == '\0' || s == NULL)
    {
      return -EINVAL;
    }

  if (persist_ensure_db() < 0)
    {
      int err = errno;

      syslog(LOG_WARNING, "ctl: persist mkdir %s errno=%d", DEVCTL_KV_DB_PATH,
             err);
      return -err;
    }

  persist_make_path(path, sizeof(path), key);
  n = strlen(s) + 1u;            /* +1 = 结尾 NUL，见上面的约定 */
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      int err = errno;

      syslog(LOG_WARNING, "ctl: persist %s failed errno=%d", key, err);
      return -err;
    }

  if (write(fd, s, n) != (ssize_t)n)
    {
      int err = errno;

      syslog(LOG_WARNING, "ctl: persist %s write errno=%d", key, err);
      close(fd);
      return -err;
    }

  close(fd);
  return 0;
}

/** @brief 读 persist 文件里的字符串，去掉行尾空白。成功为 true。 */
static bool persist_get_str(const char *key, char *out, size_t n)
{
  ssize_t got;
  int fd;
  char *p;

  if (key == NULL || key[0] == '\0' || out == NULL || n < 2u)
    {
      return false;
    }

  fd = persist_open_ro(key);
  if (fd < 0)
    {
      return false;
    }

  got = read(fd, out, n - 1u);
  close(fd);
  if (got <= 0)
    {
      return false;
    }

  out[got] = '\0';

  /* 写进去时没带换行，但文件可能是手工改的（或旧版带过）—— 统一修剪，
   * 否则 `ctl boot` 打印出来会多一截空白，比对时也对不上。 */
  for (p = out; *p != '\0'; p++)
    {
      if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
        {
          *p = '\0';
          break;
        }
    }

  return out[0] != '\0';
}

/** @brief 槽位名是否合法：`fw` / `main` / `factory`。
 *
 *  **必须用 `strcmp`（大小写敏感）与 `boot_target.c` 的 `boot_target_parse()`
 *  同口径**：bootloader 那边是 `if (strcmp(s, "fw") == 0) …`。放 `FW` 进去它
 *  认不出 ⇒ `BOOT_TARGET_NONE` ⇒ **静默退回默认链**，表现就是"我明明设了却没
 *  生效" —— 这是最不值得花时间查的一类症状，所以在入口就拒掉。
 *
 *  **故意不收 `boot` / `msh`**（bootloader 那个解析器收）：那是"停在 2SFBL 的
 *  msh、不进应用"的调试档。放进来只会让人误设成一块开不了机的板子；它属于
 *  bootloader 自己的 msh `target` 命令，不该由应用侧的 ctl 暴露。
 */
static bool boot_target_known(const char *t)
{
  return strcmp(t, "fw") == 0 ||
         strcmp(t, "main") == 0 ||
         strcmp(t, "factory") == 0;
}

bool myvendor_devctl_boot_target_get(char *out, size_t n)
{
  return persist_get_str(MYVENDOR_DEVCTL_BOOT_TARGET_KEY, out, n);
}

int myvendor_devctl_boot_target_set(const char *target)
{
  if (target == NULL || !boot_target_known(target))
    {
      return -EINVAL;
    }

  return persist_set_str(MYVENDOR_DEVCTL_BOOT_TARGET_KEY, target);
}

static bool srec_addr_empty(const uint8_t *addr)
{
  static const uint8_t z[MYVENDOR_DEVCTL_SENSOR_ADDR_LEN];

  return addr == NULL ||
         memcmp(addr, z, MYVENDOR_DEVCTL_SENSOR_ADDR_LEN) == 0;
}

static bool srec_addr_eq(const uint8_t *a, const uint8_t *b)
{
  return a != NULL && b != NULL &&
         memcmp(a, b, MYVENDOR_DEVCTL_SENSOR_ADDR_LEN) == 0;
}

static void srec_mac_fmt(const uint8_t *addr, char *buf, size_t n)
{
  snprintf(buf, n, "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static bool srec_mac_parse(const char *text, uint8_t *addr)
{
  unsigned v[6];
  int i;

  if (text == NULL || addr == NULL)
    {
      return false;
    }

  if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
             &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6)
    {
      return false;
    }

  for (i = 0; i < 6; i++)
    {
      if (v[i] > 255u)
        {
          return false;
        }

      addr[i] = (uint8_t)v[i];
    }

  return !srec_addr_empty(addr);
}

static void srec_name_copy(char *dst, size_t n, const char *src)
{
  size_t i;

  if (dst == NULL || n == 0)
    {
      return;
    }

  memset(dst, 0, n);
  if (src == NULL)
    {
      return;
    }

  for (i = 0; i + 1u < n && src[i] != '\0'; i++)
    {
      char c = src[i];

      if (c == '\t' || c == '\n' || c == '\r')
        {
          c = ' ';
        }

      dst[i] = c;
    }
}

static int srec_save_locked(void)
{
  FILE *fp;
  uint8_t i;
  char mac[18];

  fp = fopen(DEVCTL_SENSORS_TMP, "w");
  if (fp == NULL)
    {
      return -errno;
    }

  for (i = 0; i < g_srec_n; i++)
    {
      srec_mac_fmt(g_srec[i].addr, mac, sizeof(mac));
      if (fprintf(fp, "%u\t%u\t%s\t%s\t%u\n",
                  (unsigned)g_srec[i].kind,
                  (unsigned)g_srec[i].addr_type,
                  mac,
                  g_srec[i].name[0] != '\0' ? g_srec[i].name : "-",
                  g_srec[i].autorc ? 1u : 0u) < 0)
        {
          fclose(fp);
          unlink(DEVCTL_SENSORS_TMP);
          return -EIO;
        }
    }

  if (fclose(fp) != 0)
    {
      unlink(DEVCTL_SENSORS_TMP);
      return -EIO;
    }

  if (rename(DEVCTL_SENSORS_TMP, MYVENDOR_DEVCTL_SENSORS_PATH) != 0)
    {
      unlink(DEVCTL_SENSORS_TMP);
      return -errno;
    }

  g_srec_gen++;
  return 0;
}

static void srec_load_locked(void)
{
  FILE *fp;
  char line[160];
  bool seen_kind[3] = { false, false, false };

  g_srec_n = 0;
  memset(g_srec, 0, sizeof(g_srec));
  g_srec_ready = true;

  fp = fopen(MYVENDOR_DEVCTL_SENSORS_PATH, "r");
  if (fp == NULL)
    {
      /* 文件不存在（首启）是正常的；其它错误必须说出来 —— 否则「连过但开机
       * 不回连」分不清是**没保存**还是**没加载**（FS 没挂 / IO 错）。 */
      if (errno != ENOENT)
        {
          syslog(LOG_ERR, "devctl: sensors load %s failed: %d\n",
                 MYVENDOR_DEVCTL_SENSORS_PATH, errno);
        }

      return;
    }

  while (fgets(line, sizeof(line), fp) != NULL &&
         g_srec_n < MYVENDOR_DEVCTL_SENSOR_REC_MAX)
    {
      char *kind_text;
      char *type_text;
      char *mac_text;
      char *name_text;
      char *auto_text;
      char *save = NULL;
      size_t len = strlen(line);
      unsigned kind;
      unsigned addr_type;
      unsigned autorc;
      myvendor_devctl_sensor_rec_t rec;

      if (len > 0 && line[len - 1] == '\n')
        {
          line[--len] = '\0';
        }

      if (len > 0 && line[len - 1] == '\r')
        {
          line[--len] = '\0';
        }

      if (len == 0)
        {
          continue;
        }

      kind_text = strtok_r(line, "\t", &save);
      type_text = strtok_r(NULL, "\t", &save);
      mac_text = strtok_r(NULL, "\t", &save);
      name_text = strtok_r(NULL, "\t", &save);
      auto_text = strtok_r(NULL, "\t", &save);
      if (kind_text == NULL || type_text == NULL || mac_text == NULL ||
          name_text == NULL || auto_text == NULL)
        {
          continue;
        }

      kind = (unsigned)strtoul(kind_text, NULL, 10);
      addr_type = (unsigned)strtoul(type_text, NULL, 10);
      autorc = (unsigned)strtoul(auto_text, NULL, 10);
      if (kind > MYVENDOR_DEVCTL_SENSOR_KIND_CPS)
        {
          continue;
        }

      memset(&rec, 0, sizeof(rec));
      if (!srec_mac_parse(mac_text, rec.addr))
        {
          continue;
        }

      rec.kind = (uint8_t)kind;
      rec.addr_type = (uint8_t)addr_type;
      if (strcmp(name_text, "-") != 0)
        {
          srec_name_copy(rec.name, sizeof(rec.name), name_text);
        }

      rec.autorc = (autorc != 0) && !seen_kind[kind];
      if (rec.autorc)
        {
          seen_kind[kind] = true;
        }

      g_srec[g_srec_n++] = rec;
    }

  fclose(fp);
}

static void srec_ensure_locked(void)
{
  if (!g_srec_ready)
    {
      srec_load_locked();
    }
}

/**
 * @brief 记录用的短类型名。
 */
static const char *srec_kind_name(uint8_t kind)
{
  switch (kind)
    {
      case MYVENDOR_DEVCTL_SENSOR_KIND_HR:
        return "hr";
      case MYVENDOR_DEVCTL_SENSOR_KIND_CSC:
        return "csc";
      default:
        return "cps";
    }
}

void myvendor_devctl_sensor_rec_dump(void)
{
  myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
  struct stat st;
  size_t n = 0;
  size_t i;

  /** 落盘状态和内存表一起打。
   *
   *  「连过但开机不回连」只有两种可能：内存里有记录而**文件没有/是旧的**
   *  （保存失败），或者文件有而**加载不出来**（FS 没挂、解析丢行）。
   *  这两个数放一起，一眼就能分开。 */
  if (stat(MYVENDOR_DEVCTL_SENSORS_PATH, &st) == 0)
    {
      printf("sys rec  %s  %ld B\n", MYVENDOR_DEVCTL_SENSORS_PATH,
             (long)st.st_size);
    }
  else
    {
      printf("sys rec  %s  missing errno=%d\n", MYVENDOR_DEVCTL_SENSORS_PATH,
             errno);
    }

  (void)myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                        &n);

  for (i = 0; i < n; i++)
    {
      char mac[18];

      srec_mac_fmt(recs[i].addr, mac, sizeof(mac));
      printf("sys rec %u  %s  type %u  %s  autorc %u  %s\n", (unsigned)i,
             srec_kind_name(recs[i].kind), (unsigned)recs[i].addr_type, mac,
             recs[i].autorc ? 1u : 0u,
             recs[i].name[0] != '\0' ? recs[i].name : "-");
    }
}

/**
 * @brief 回连对账：记录（持久）↔ 槽位目标（本次开机）。
 *
 * @details
 * 回连**只**该由 `autorc` 记录驱动：武装在 `companion_sensor_autorc_all()`
 * → `ble_sensor_connect_addr()`，扫描回调本身不再替槽位挑设备（见
 * `docs/ble/ble_sensor.md` §3.9.1）。于是两个方向相反的问题在同一条判据上：
 *
 *   - **记录有 ★、槽位地址空** → 武装没上（BLE 没 started、或记录是在 BLE
 *     起来之后才写的），开机不会回连。
 *   - **记录没有、槽位却有地址** → 有东西绕过记录在连，也就是"删了也回来"
 *     的形态。修完之后不该出现。
 *
 * 两个地址都打全，方便和 `sys rec` / `sys found` 逐字节对（RPA 轮换的设备
 * 会对不上，见 §3.7）。
 */
void myvendor_devctl_sensor_link_dump(void)
{
  myvendor_devctl_sensor_rec_t recs[MYVENDOR_DEVCTL_SENSOR_REC_MAX];
  myvendor_sys_sensor_ui_t ui;
  size_t n = 0;
  uint8_t k;

  myvendor_sys_sensor_ui_get(&ui);
  (void)myvendor_devctl_sensor_recs_get(recs, MYVENDOR_DEVCTL_SENSOR_REC_MAX,
                                        &n);

  for (k = 0; k < MYVENDOR_DEVCTL_SENSOR_KIND_N; k++)
    {
      const myvendor_devctl_sensor_rec_t *rec = NULL;
      const myvendor_sys_sensor_slot_t   *slot = NULL;
      char ra[24];
      char sa[24];
      size_t r;
      bool slot_have;

      for (r = 0; r < n; r++)
        {
          if (recs[r].kind == k && recs[r].autorc)
            {
              rec = &recs[r];
              break;
            }
        }

      if (k < MYVENDOR_SYS_SENSOR_KIND_N)
        {
          slot = &ui.slot[k];
          slot_have = (slot->addr[0] | slot->addr[1] | slot->addr[2] |
                       slot->addr[3] | slot->addr[4] | slot->addr[5]) != 0;
        }
      else
        {
          slot_have = false;
        }

      if (rec == NULL && !slot_have)
        {
          continue;   /* 干净：既没配对目标，也没在连 */
        }

      if (rec != NULL)
        {
          srec_mac_fmt(rec->addr, ra, sizeof(ra));
        }
      else
        {
          snprintf(ra, sizeof(ra), "--");
        }

      if (slot_have)
        {
          srec_mac_fmt(slot->addr, sa, sizeof(sa));
        }
      else
        {
          snprintf(sa, sizeof(sa), "--");
        }

      printf("sys link %s  rec %s %s  slot %s %s\n", srec_kind_name(k),
             rec != NULL ? "*" : "-", ra,
             slot == NULL ? "--" :
             slot->link == MYVENDOR_SYS_SENSOR_LINK_READY ? "ready" :
             slot->link == MYVENDOR_SYS_SENSOR_LINK_CONNECTING ? "conn" : "idle",
             sa);

      if (rec != NULL && !slot_have)
        {
          printf("sys link %s  WARN 有回连记录但槽位没武装,开机不会回连\n",
                 srec_kind_name(k));
        }
      else if (rec == NULL && slot_have)
        {
          printf("sys link %s  WARN 无回连记录却在连,有东西绕过 autorc\n",
                 srec_kind_name(k));
        }
    }
}

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
static void srec_post_bind_locked(uint8_t idx, const uint8_t *drop_addr,
                                  uint8_t drop_kind)
{
  struct companion_sensor_bind bind;

  memset(&bind, 0, sizeof(bind));
  memcpy(bind.addr, g_srec[idx].addr, MYVENDOR_DEVCTL_SENSOR_ADDR_LEN);
  bind.addr_type = g_srec[idx].addr_type;
  bind.kind = g_srec[idx].kind;
  memcpy(bind.name, g_srec[idx].name, sizeof(bind.name));
  bind.name[sizeof(bind.name) - 1u] = '\0';
  if (drop_addr != NULL && !srec_addr_empty(drop_addr))
    {
      memcpy(bind.drop_addr, drop_addr, MYVENDOR_DEVCTL_SENSOR_ADDR_LEN);
      bind.drop_kind = drop_kind;
    }

  companion_bridge_sensor_cmd_set_bind(&bind);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_CONNECT_ADDR);
}

static void srec_post_clear_locked(const uint8_t *addr, uint8_t kind)
{
  struct companion_sensor_bind bind;

  memset(&bind, 0, sizeof(bind));
  if (addr != NULL)
    {
      memcpy(bind.addr, addr, MYVENDOR_DEVCTL_SENSOR_ADDR_LEN);
    }

  bind.kind = kind;
  companion_bridge_sensor_cmd_set_bind(&bind);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_CLEAR_WANT);
}
#endif

/**
 * @brief 把背光百分比夹到 0～硬件上限。
 */
static uint8_t clamp_bl(int v)
{
  uint8_t maxp = BSP_LCD_BL_GetMaxPct();

  if (v < 0)
    {
      return 0;
    }

  if (maxp > 0 && v > (int)maxp)
    {
      return maxp;
    }

  if (v > 100)
    {
      return 100;
    }

  return (uint8_t)v;
}

/**
 * @brief 从 persist 载入策略并 PrefPct；不点亮背光。
 *
 * 工厂固件不读 persist.usb.mtp / persist.bt.radio，RAM 里直接开 MTP 与手机 BLE，
 * 也不回写这两项，避免冲掉产品侧开关。工厂也不弹出手机通知（不写 persist.ui.notif）。
 */
void myvendor_devctl_load(void)
{
  int bl;
  int sensor;
  int notif;
  int calls;
  int sound;
  int autopause;
  int eph_auto;
  int gnss_rmc;
  int grade;
  int tz;

  /* 先把上次 `ctl log` 的屏蔽位装回去，再干别的：这样后面这一段 load 自己的
   * 日志、以及本轮之后才起来的 gnss/ble/diag/UI 线程，都直接按用户的档位走。
   *
   * `nx_setlogmask()` 会遍历所有已存在的任务一起改，新任务在
   * sched/tls/task_initinfo.c 里继承这个全局值 —— 所以晚初始化步骤不用各自
   * 再调一次。代价是 bringup 里 storage 之前的十几步（watchdog/vfs/rtc/lcd…）
   * 仍用 `CONFIG_SYSLOG_DEFAULT_MASK`：KV 那时还没挂，读不到。
   *
   * 工厂固件不读：KV 里那份是产品侧调的（产品平时压到 0xf），工厂要的是全开。
   * 跟下面 mtp / radio 两项同一个道理。 */

  if (myvendor_is_factory())
    {
      g_log_mask = (uint8_t)MYVENDOR_DEVCTL_LOG_DEFAULT;
    }
  else
    {
      g_log_mask = (uint8_t)((uint32_t)persist_get_i32(
                      MYVENDOR_DEVCTL_LOG_KEY,
                      MYVENDOR_DEVCTL_LOG_DEFAULT) & 0xffu);
    }

  nx_setlogmask((int)g_log_mask);

  /* 充电动态调频模式：从 KV 恢复（`ctl dvfs charge on` 会落盘）。
   * 放在这里而不是 DVFS 自己里：持久化的 owner 是 devctl（同 log mask）。
   * 此时 DVFS 可能还没 `sf32lb_dvfs_boot()`（`g_applied_mhz == 0`），
   * setter 内部只记标志、不强行切频 —— boot 时会自然带上这个档位。 */
  if (!myvendor_is_factory())
    {
      g_dvfs_charge_dyn =
        persist_get_i32(MYVENDOR_DEVCTL_DVFS_CHARGE_DYN_KEY, 0) != 0;
      (void)sf32lb_dvfs_charge_dyn_set(g_dvfs_charge_dyn);

      /* 周期计数器报表的开关（`ctl cnt`）。缺省 **全开** ⇒ fallback 取 -1 再翻成
       * 0xffffffff —— 0 是"全关"这个**合法值**，不能拿它当"没配过"。 */
      {
        extern void myvendor_diag_cnt_mask_set(uint32_t mask);
        int32_t m = persist_get_i32(MYVENDOR_DEVCTL_DIAG_CNT_KEY, -1);

        g_diag_cnt_mask = (m < 0) ? 0xffffffffu : (uint32_t)m;
        myvendor_diag_cnt_mask_set(g_diag_cnt_mask);
      }
    }

  bl = persist_get_i32(MYVENDOR_DEVCTL_BL_KEY, (int32_t)MYVENDOR_DEVCTL_BL_DEFAULT);
  sensor = persist_get_i32(MYVENDOR_DEVCTL_SENSOR_KEY, 1);
  notif = persist_get_i32(MYVENDOR_DEVCTL_NOTIF_KEY, 1);
  calls = persist_get_i32(MYVENDOR_DEVCTL_CALLS_KEY, 0);
  sound = persist_get_i32(MYVENDOR_DEVCTL_SOUND_KEY, 1);
  autopause = persist_get_i32(MYVENDOR_DEVCTL_AUTOPAUSE_KEY, 1);
  g_last_ride_m = persist_get_i32(MYVENDOR_DEVCTL_LAST_RIDE_M_KEY, 0);
  if (g_last_ride_m < 0)
    {
      g_last_ride_m = 0;
    }

  {
    int32_t lon_e7 = 0;
    int32_t lat_e7 = 0;

    if (persist_try_i32(MYVENDOR_DEVCTL_LAST_LON_E7_KEY, &lon_e7) &&
        persist_try_i32(MYVENDOR_DEVCTL_LAST_LAT_E7_KEY, &lat_e7) &&
        last_pos_ok(lon_e7, lat_e7))
      {
        g_last_lon_e7 = lon_e7;
        g_last_lat_e7 = lat_e7;
        g_last_pos_have = true;
      }
    else
      {
        g_last_lon_e7 = 0;
        g_last_lat_e7 = 0;
        g_last_pos_have = false;
      }
  }
  eph_auto = persist_get_i32(MYVENDOR_DEVCTL_EPH_AUTO_KEY, 1);
  /* 默认改为 **1：用模组的多普勒速度（RMC SOG）**，不用固件的位置差分滤波。
   *
   * 依据（2026-09-17 从导出的 TRK gpx 反推）：Δt 干净到最小正好 1.000 s，
   * 但位置差分在 1 Hz 下的噪声底就是 ±3 km/h —— 静止时能算出 3.1 km/h，
   * 而中位 21.8 km/h 的行程里最大跳到 89.7 km/h（24.9 m/1 s 的位置噪声），
   * 再被固件自己的 sanity 判成"异常"置 0（高速下坡那次就是这么归零的）。
   * 模组的 SOG 是多普勒测的，静止≈0、骑行平滑，没有这个问题。
   * 保留这个开关是为了排查时能切回去对比（App/UI 侧 `bicycle_runtime_set_gnss_solver_rmc()`）。
   *
   * 注意：**已经存过 0 的机器不吃这个新默认值**（persist 里有旧值），
   * 需要在 App/UI 里手动切一次，或者删掉这个 persist key。 */
  gnss_rmc = persist_get_i32(MYVENDOR_DEVCTL_GNSS_RMC_KEY, 1);
  grade = persist_get_i32(MYVENDOR_DEVCTL_GRADE_KEY, 0);
  tz = persist_get_i32(MYVENDOR_DEVCTL_TZ_KEY, MYVENDOR_DEVCTL_TZ_DEFAULT);

  g_bl = clamp_bl(bl);
  g_sensor = sensor != 0;
  g_notif = notif != 0;
  g_calls = calls != 0;
  g_sound = sound != 0;
  g_autopause = autopause != 0;
  g_eph_auto = eph_auto != 0;
  g_gnss_rmc = gnss_rmc != 0;
  g_grade_off = (int32_t)grade;
  g_tz_min = clamp_tz(tz);

  {
    int32_t full_mv;

    if (persist_try_i32(MYVENDOR_DEVCTL_BAT_FULL_KEY, &full_mv) &&
        bat_full_mv_ok((int)full_mv))
      {
        g_bat_full_mv = (int)full_mv;
        g_bat_full_have = true;
      }
    else
      {
        g_bat_full_mv = MYVENDOR_DEVCTL_BAT_FULL_DEFAULT;
        g_bat_full_have = false;
      }
  }

  if (myvendor_is_factory())
    {
      g_mtp = true;
      g_radio = true;
      g_notif = false;
    }
  else
    {
      g_radio = persist_get_i32(MYVENDOR_DEVCTL_RADIO_KEY, 1) != 0;
      g_mtp = persist_get_i32(MYVENDOR_DEVCTL_MTP_KEY, 1) != 0;
    }

  g_loaded = true;

  pthread_mutex_lock(&g_srec_lock);
  srec_load_locked();
  pthread_mutex_unlock(&g_srec_lock);

  BSP_LCD_BL_PrefPct(g_bl);

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_policy_set_radio(g_radio, false);
  companion_bridge_policy_set_sensor(g_sensor, false);
  companion_bridge_policy_set_notif(g_notif);
  companion_bridge_policy_set_calls_only(g_calls);
#endif

  syslog(LOG_INFO, "ctl: load bl=%u radio=%d sensor=%d mtp=%d notif=%d calls=%d sound=%d autopause=%d eph_auto=%d gnss_rmc=%d grade=%ld tz=%d bat_full=%d log=0x%02x%s%s",
         (unsigned)g_bl, (int)g_radio, (int)g_sensor, (int)g_mtp,
         (int)g_notif, (int)g_calls, (int)g_sound, (int)g_autopause, (int)g_eph_auto,
         (int)g_gnss_rmc,
         (long)g_grade_off, (int)g_tz_min, g_bat_full_mv,
         (unsigned)g_log_mask,
         g_bat_full_have ? "" : " (default)",
         myvendor_is_factory() ? " (factory force mtp+radio, notif off)" : "");
}

/**
 * @brief 尚未 load 时套用默认背光并 PrefPct。
 */
static void ensure_loaded(void)
{
  if (!g_loaded)
    {
      g_bl = clamp_bl((int)MYVENDOR_DEVCTL_BL_DEFAULT);
      BSP_LCD_BL_PrefPct(g_bl);
      g_loaded = true;
    }
}

int myvendor_devctl_bl_apply(uint8_t pct)
{
  uint8_t next;

  ensure_loaded();
  next = clamp_bl((int)pct);
  BSP_LCD_BL_PrefPct(next);
  if (next != g_bl)
    {
      g_bl_dirty = true;
    }

  g_bl = next;
  return 0;
}

void myvendor_devctl_bl_commit(void)
{
  if (!g_loaded || !g_bl_dirty)
    {
      return;
    }

  persist_set_i32(MYVENDOR_DEVCTL_BL_KEY, (int32_t)g_bl);
  g_bl_dirty = false;
}

int myvendor_devctl_bl_set(uint8_t pct)
{
  int ret;

  ret = myvendor_devctl_bl_apply(pct);
  if (ret < 0)
    {
      return ret;
    }

  ret = BSP_LCD_BL_SetPct(g_bl);
  myvendor_devctl_bl_commit();
  return ret < 0 ? ret : 0;
}

uint8_t myvendor_devctl_bl_get(void)
{
  ensure_loaded();
  return g_bl;
}

uint8_t myvendor_devctl_bl_max(void)
{
  return BSP_LCD_BL_GetMaxPct();
}

int myvendor_devctl_radio_set(bool on)
{
  ensure_loaded();
  if (myvendor_is_factory())
    {
      on = true;
    }

  g_radio = on;
  if (!myvendor_is_factory())
    {
      persist_set_i32(MYVENDOR_DEVCTL_RADIO_KEY, on ? 1 : 0);
    }

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_policy_set_radio(on, true);
  if (!companion_bridge_alive_get())
    {
      syslog(LOG_INFO, "ctl: radio policy %s (ble_companion not running)",
             on ? "on" : "off");
    }

  return 0;
#else
  (void)on;
  return -ENOTSUP;
#endif
}

bool myvendor_devctl_radio_get(void)
{
  ensure_loaded();
  if (myvendor_is_factory())
    {
      return true;
    }

  return g_radio;
}

int myvendor_devctl_sensor_set(bool on)
{
  ensure_loaded();
  g_sensor = on;
  persist_set_i32(MYVENDOR_DEVCTL_SENSOR_KEY, on ? 1 : 0);

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_policy_set_sensor(on, true);
  if (!companion_bridge_alive_get())
    {
      syslog(LOG_INFO, "ctl: sensor policy %s (ble_companion not running)",
             on ? "on" : "off");
    }

  return 0;
#else
  (void)on;
  return -ENOTSUP;
#endif
}

bool myvendor_devctl_sensor_get(void)
{
  ensure_loaded();
  return g_sensor;
}

int myvendor_devctl_sensor_scan(uint32_t ms)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_sensor_cmd_set_scan_ms(
      ms == 0 ? MYVENDOR_DEVCTL_SENSOR_SCAN_MS_DEFAULT : ms);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_SCAN);
  return 0;
#else
  (void)ms;
  return -ENOTSUP;
#endif
}

int myvendor_devctl_sensor_scan_stop(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_SCAN_STOP);
  return 0;
#else
  return -ENOTSUP;
#endif
}

/**
 * @brief 开一次配对窗口（设备 UI 的「配对新手机」/ app 的控制帧 / `ctl pair open`）。
 *
 * 只是**投命令**：窗口状态与广播重播都在 companion 线程做（在 ctl/UI 线程直接
 * 碰框架会撞 libuv 断言，2026-09-20 现场）。
 *
 * @param secs 窗口秒数；0 = 用默认（见 `COMPANION_TEST_PAIR_OPEN_SECS_DEFAULT`）。
 * @return 0 = 已投递；-ENOTSUP = 本固件没编 companion。
 */
int myvendor_devctl_pair_open(uint32_t secs)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_sensor_cmd_set_pair_secs(secs);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_OPEN);
  return 0;
#else
  (void)secs;
  return -ENOTSUP;
#endif
}

/**
 * @brief 配对窗口剩余毫秒（设备 UI 的「配对中 + 倒计时」用它）。
 *
 * @return 剩余毫秒；0 = 没在窗口内。
 *
 * @note 读的是 companion 侧那份窗口状态（同一镜像内的只读查询）。
 */
uint32_t myvendor_devctl_pair_window_left_ms(void)
{
  /* companion 侧的窗口状态（同镜像，直接取；只读一个 uint32，够安全）。 */
  extern uint32_t myvendor_pair_window_left_ms(void);

  return myvendor_pair_window_left_ms();
}

/**
 * @brief 读已绑定手机列表（最多 3 行 `ADDR<TAB>TYPE`）。
 */
unsigned myvendor_devctl_pair_phone_count(void)
{
  FILE *f = fopen("/mnt/kv/bt_phone.tsv", "r");
  char  line[64];
  unsigned n = 0;

  if (f == NULL)
    {
      return 0u;
    }

  while (n < 3u && fgets(line, sizeof(line), f) != NULL)
    {
      if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r')
        {
          continue;
        }

      n++;
    }

  fclose(f);
  return n;
}

int myvendor_devctl_pair_phone_at(unsigned idx, char *addr, size_t len)
{
  FILE *f = fopen("/mnt/kv/bt_phone.tsv", "r");
  char  line[64];
  unsigned n = 0;

  if (f == NULL)
    {
      return 0;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r')
        {
          continue;
        }

      if (n == idx)
        {
          size_t i = 0;

          fclose(f);
          if (addr != NULL && len > 0u)
            {
              while (line[i] != '\0' && line[i] != '\t' && line[i] != '\n'
                     && line[i] != '\r' && i + 1u < len)
                {
                  addr[i] = line[i];
                  i++;
                }

              addr[i] = '\0';
            }

          return 1;
        }

      n++;
    }

  fclose(f);
  return 0;
}

/**
 * @brief 读第一台已绑定手机（兼容）。
 */
int myvendor_devctl_pair_phone(char *addr, size_t len)
{
  return myvendor_devctl_pair_phone_at(0u, addr, len);
}

/**
 * @brief 解除全部手机绑定。
 */
int myvendor_devctl_pair_unbind(void)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_sensor_cmd_set_pair_unbind_slot(0xffu);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_UNBIND);
  return 0;
#else
  return -ENOTSUP;
#endif
}

int myvendor_devctl_pair_unbind_at(unsigned idx)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  if (idx > 2u)
    {
      return -EINVAL;
    }

  companion_bridge_sensor_cmd_set_pair_unbind_slot((uint8_t)idx);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_UNBIND);
  return 0;
#else
  (void)idx;
  return -ENOTSUP;
#endif
}

int myvendor_devctl_sensor_connect(uint8_t idx_1based)
{
  return myvendor_devctl_sensor_connect_kind(idx_1based,
                                             COMPANION_SENSOR_KIND_AUTO);
}

int myvendor_devctl_sensor_connect_kind(uint8_t idx_1based, uint8_t kind)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  if (idx_1based == 0)
    {
      return -EINVAL;
    }

  /* kind 越界交给服务侧当「取第一种匹配」处理（AUTO 就是这个语义），
   * 这里不拦：一套 UI 只有一个真值来源，判据留在 ble_sensor 里。 */
  companion_bridge_sensor_cmd_set_connect_kind(kind);
  companion_bridge_sensor_cmd_set_connect_idx(idx_1based);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_CONNECT);
  return 0;
#else
  (void)idx_1based;
  (void)kind;
  return -ENOTSUP;
#endif
}

/**
 * @brief 按 **MAC** 连接一台扫描到的设备（设备端 UI 点击用）。
 *
 * @details
 * 为什么要有这条：`myvendor_devctl_sensor_connect_kind()` 传的是**扫描表下标**，
 * 而那张表是活的 —— 扫描回调会并发改写它（"更弱的设备被替换"那条规则），
 * 而且**每次用户起扫都会整表清空**。于是"前台显示第 3 项 → 用户点它 →
 * 底层按 idx=3 去查表"这两个时刻之间，只要表被清过或那行被换过，
 * `g_found[idx-1]` 拿到的就是**别的设备、甚至全零地址**。
 *
 * 按 MAC 寻址没有这个问题：MAC 在"显示"那一刻就已经定下来了，之后表怎么变
 * 都指向同一台设备。UI 手上本来就有 MAC（`myvendor_sys_sensor_ui_get()` 的
 * `found[].addr/addr_type`，占用检查那段早就在用了）。
 *
 * 服务侧走的也是已有路径：填 `bind` → `COMPANION_TEST_SENSOR_CONNECT_ADDR`
 * → `ble_sensor_connect_addr()`（与回连记录同一条）。
 *
 * @param addr      6 字节 LE 地址。
 * @param addr_type 地址类型。
 * @param kind      绑到哪个类型槽位（0 HR / 1 CSC / 2 CPS）。
 * @param name      可为 NULL。
 * @return 0 已投递；-EINVAL 参数非法；-ENOTSUP 未编 companion。
 */
int myvendor_devctl_sensor_connect_addr(const uint8_t *addr, uint8_t addr_type,
                                        uint8_t kind, const char *name)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  struct companion_sensor_bind bind;

  if (addr == NULL)
    {
      return -EINVAL;
    }

  memset(&bind, 0, sizeof(bind));
  memcpy(bind.addr, addr, COMPANION_SENSOR_ADDR_LEN);
  bind.addr_type = addr_type;
  bind.kind      = kind;
  if (name != NULL)
    {
      strncpy(bind.name, name, sizeof(bind.name) - 1u);
      bind.name[sizeof(bind.name) - 1u] = '\0';
    }

  companion_bridge_sensor_cmd_set_bind(&bind);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_CONNECT_ADDR);
  return 0;
#else
  (void)addr;
  (void)addr_type;
  (void)kind;
  (void)name;
  return -ENOTSUP;
#endif
}

int myvendor_devctl_sensor_disconnect_kind(uint8_t kind)
{
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  if (kind > MYVENDOR_DEVCTL_SENSOR_KIND_CPS)
    {
      return -EINVAL;
    }

  companion_bridge_sensor_cmd_set_kind(kind);
  companion_bridge_sensor_cmd_post(COMPANION_TEST_SENSOR_DISCONNECT_KIND);
  return 0;
#else
  (void)kind;
  return -ENOTSUP;
#endif
}

int myvendor_devctl_mtp_set(bool on)
{
  int ret;

  ensure_loaded();
  if (myvendor_is_factory())
    {
      on = true;
    }

  g_mtp = on;
  if (!myvendor_is_factory())
    {
      persist_set_i32(MYVENDOR_DEVCTL_MTP_KEY, on ? 1 : 0);
    }

  ret = myvendor_usbdev_set(g_mtp_nav_hold ? false : on);
  if (ret == 0 && on && !g_mtp_nav_hold)
    {
      (void)myvendor_mtp_transfer_begin();
    }

  return ret;
}

bool myvendor_devctl_mtp_get(void)
{
  ensure_loaded();
  if (myvendor_is_factory())
    {
      return true;
    }

  return g_mtp;
}

int myvendor_devctl_mtp_nav_hold(bool hold)
{
  int ret;

  ensure_loaded();
  if (g_mtp_nav_hold == hold)
    {
      return 0;
    }

  g_mtp_nav_hold = hold;
  if (hold)
    {
      if (g_mtp)
        {
          myvendor_mtp_transfer_end();
        }
      ret = myvendor_usbdev_set(false);
      syslog(LOG_INFO, "ctl: MTP suspended for navigation ret=%d", ret);
      return ret;
    }

  ret = myvendor_usbdev_set(g_mtp);
  if (ret == 0 && g_mtp)
    {
      (void)myvendor_mtp_transfer_begin();
    }

  syslog(LOG_INFO, "ctl: MTP navigation hold released policy=%d ret=%d",
         (int)g_mtp, ret);
  return ret;
}

int myvendor_devctl_notif_set(bool on)
{
  ensure_loaded();
  if (myvendor_is_factory())
    {
      on = false;
    }

  g_notif = on;
  if (!myvendor_is_factory())
    {
      persist_set_i32(MYVENDOR_DEVCTL_NOTIF_KEY, on ? 1 : 0);
    }

#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_policy_set_notif(on);
#endif
  return 0;
}

bool myvendor_devctl_notif_get(void)
{
  ensure_loaded();
  if (myvendor_is_factory())
    {
      return false;
    }

  return g_notif;
}

int myvendor_devctl_notif_calls_only_set(bool on)
{
  ensure_loaded();
  g_calls = on;
  persist_set_i32(MYVENDOR_DEVCTL_CALLS_KEY, on ? 1 : 0);
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  companion_bridge_policy_set_calls_only(on);
#endif
  return 0;
}

bool myvendor_devctl_notif_calls_only_get(void)
{
  ensure_loaded();
  return g_calls;
}

int myvendor_devctl_sound_set(bool on)
{
  ensure_loaded();
  g_sound = on;
  persist_set_i32(MYVENDOR_DEVCTL_SOUND_KEY, on ? 1 : 0);
  return 0;
}

bool myvendor_devctl_sound_get(void)
{
  ensure_loaded();
  return g_sound;
}

int myvendor_devctl_autopause_set(bool on)
{
  ensure_loaded();
  g_autopause = on;
  persist_set_i32(MYVENDOR_DEVCTL_AUTOPAUSE_KEY, on ? 1 : 0);
  return 0;
}

bool myvendor_devctl_autopause_get(void)
{
  ensure_loaded();
  return g_autopause;
}

void myvendor_devctl_last_ride_m_set(int32_t m)
{
  ensure_loaded();
  if (m < 0)
    {
      m = 0;
    }

  g_last_ride_m = m;
  persist_set_i32(MYVENDOR_DEVCTL_LAST_RIDE_M_KEY, m);
}

int32_t myvendor_devctl_last_ride_m_get(void)
{
  ensure_loaded();
  return g_last_ride_m;
}

void myvendor_devctl_last_pos_set(int32_t lon_e7, int32_t lat_e7)
{
  ensure_loaded();
  if (!last_pos_ok(lon_e7, lat_e7))
    {
      return;
    }

  g_last_lon_e7 = lon_e7;
  g_last_lat_e7 = lat_e7;
  g_last_pos_have = true;
  persist_set_i32(MYVENDOR_DEVCTL_LAST_LON_E7_KEY, lon_e7);
  persist_set_i32(MYVENDOR_DEVCTL_LAST_LAT_E7_KEY, lat_e7);
}

bool myvendor_devctl_last_pos_get(int32_t *lon_e7, int32_t *lat_e7)
{
  ensure_loaded();
  if (!g_last_pos_have || lon_e7 == NULL || lat_e7 == NULL)
    {
      return false;
    }

  *lon_e7 = g_last_lon_e7;
  *lat_e7 = g_last_lat_e7;
  return true;
}

int myvendor_devctl_eph_auto_set(bool on)
{
  ensure_loaded();
  g_eph_auto = on;
  persist_set_i32(MYVENDOR_DEVCTL_EPH_AUTO_KEY, on ? 1 : 0);
  return 0;
}

bool myvendor_devctl_eph_auto_get(void)
{
  ensure_loaded();
  return g_eph_auto;
}

int myvendor_devctl_gnss_rmc_set(bool on)
{
  ensure_loaded();
  g_gnss_rmc = on;
  persist_set_i32(MYVENDOR_DEVCTL_GNSS_RMC_KEY, on ? 1 : 0);
  return 0;
}

bool myvendor_devctl_gnss_rmc_get(void)
{
  ensure_loaded();
  return g_gnss_rmc;
}

int myvendor_devctl_grade_offset_set(int32_t mdeg)
{
  if (mdeg > 45000) {
    mdeg = 45000;
  } else if (mdeg < -45000) {
    mdeg = -45000;
  }

  ensure_loaded();
  g_grade_off = mdeg;
  persist_set_i32(MYVENDOR_DEVCTL_GRADE_KEY, mdeg);
  return 0;
}

int32_t myvendor_devctl_grade_offset_get(void)
{
  ensure_loaded();
  return g_grade_off;
}

int myvendor_devctl_tz_min_set(int16_t min)
{
  ensure_loaded();
  g_tz_min = clamp_tz((int)min);
  persist_set_i32(MYVENDOR_DEVCTL_TZ_KEY, (int32_t)g_tz_min);
  return 0;
}

int16_t myvendor_devctl_tz_min_get(void)
{
  ensure_loaded();
  return g_tz_min;
}

int16_t myvendor_devctl_tz_min_peek(void)
{
  return g_tz_min;
}

bool myvendor_devctl_tz_hms(long utc_sec, int *hour, int *min)
{
  struct tm tm_buf;
  time_t wall;

  ensure_loaded();
  wall = (time_t)utc_sec + (time_t)g_tz_min * 60;
  if (gmtime_r(&wall, &tm_buf) == NULL) {
    return false;
  }

  if (hour != NULL) {
    *hour = tm_buf.tm_hour;
  }

  if (min != NULL) {
    *min = tm_buf.tm_min;
  }

  return true;
}

int myvendor_devctl_log_mask_set(uint8_t mask, uint8_t *old)
{
  int prev;
  int ret;

  ensure_loaded();

  /* 先改内存再落地：写盘失败也保留本次生效的档位。反过来（先写后改、写失败
   * 就放弃）会让调用方看到"设了却没生效"，比"设了但没记住"更难查。 */

  prev = nx_setlogmask((int)mask);
  g_log_mask = mask;

  if (old != NULL)
    {
      *old = (uint8_t)prev;
    }

  /* 工厂固件不回写：KV 里那份是产品侧调的档位，产测把它改成 0xff 会跟着
   * 产品固件一起带走，产品上就变成刷屏。跟 mtp / radio 同一个理由。 */
  if (myvendor_is_factory())
    {
      return 0;
    }

  ret = persist_set_i32(MYVENDOR_DEVCTL_LOG_KEY, (int32_t)mask);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ctl: log mask 0x%02x not persisted (%d)",
             (unsigned)mask, ret);
    }

  return ret;
}

uint8_t myvendor_devctl_log_mask_get(void)
{
  ensure_loaded();
  return g_log_mask;
}

int myvendor_devctl_dvfs_charge_dyn_set(bool on)
{
  int ret;

  ensure_loaded();

  /* 先改运行期再落盘（同 log mask 的取舍）：写盘失败也保留本次生效的行为。 */
  (void)sf32lb_dvfs_charge_dyn_set(on);
  g_dvfs_charge_dyn = on;

  /* 工厂固件不回写：KV 里那份是产品侧调的，产测改了会跟着产品固件一起带走
   * （跟 log mask / mtp / radio 同一个理由）。 */
  if (myvendor_is_factory())
    {
      return 0;
    }

  ret = persist_set_i32(MYVENDOR_DEVCTL_DVFS_CHARGE_DYN_KEY, on ? 1 : 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ctl: dvfs charge-dyn %d not persisted (%d)",
             on ? 1 : 0, ret);
    }

  return ret;
}

bool myvendor_devctl_dvfs_charge_dyn_get(void)
{
  ensure_loaded();
  return g_dvfs_charge_dyn;
}

uint32_t myvendor_devctl_diag_cnt_mask_get(void)
{
  ensure_loaded();
  return g_diag_cnt_mask;
}

int myvendor_devctl_diag_cnt_mask_set(uint32_t mask)
{
  extern void myvendor_diag_cnt_mask_set(uint32_t mask);

  int ret;

  ensure_loaded();

  /* 先改运行期再落盘（同 log mask / dvfs charge 的取舍）：写盘失败也保留本次行为。 */
  myvendor_diag_cnt_mask_set(mask);
  g_diag_cnt_mask = mask;

  /* 工厂固件不回写：KV 里那份是产品侧调的（同 log mask / mtp / radio 的理由）。 */
  if (myvendor_is_factory())
    {
      return 0;
    }

  ret = persist_set_i32(MYVENDOR_DEVCTL_DIAG_CNT_KEY, (int32_t)mask);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ctl: diag cnt mask 0x%lx not persisted (%d)",
             (unsigned long)mask, ret);
    }

  return ret;
}

static bool favorite_coord_parse(const char *text, double min, double max,
                                 double *value)
{
  char *end = NULL;
  double v;

  if (text == NULL || value == NULL)
    {
      return false;
    }

  errno = 0;
  v = strtod(text, &end);
  if (errno != 0 || end == text || end == NULL || *end != '\0' ||
      !isfinite(v) || v < min || v > max)
    {
      return false;
    }

  *value = v;
  return true;
}

int myvendor_devctl_sensor_recs_get(myvendor_devctl_sensor_rec_t *out,
                                    size_t cap, size_t *count)
{
  size_t n;

  if (count == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_srec_lock);
  srec_ensure_locked();
  n = g_srec_n;
  if (out != NULL && cap > 0 && n > 0)
    {
      if (n > cap)
        {
          n = cap;
        }

      memcpy(out, g_srec, n * sizeof(g_srec[0]));
    }
  else if (out == NULL)
    {
      n = g_srec_n;
    }
  else
    {
      n = 0;
    }

  pthread_mutex_unlock(&g_srec_lock);
  *count = n;
  return 0;
}

uint32_t myvendor_devctl_sensor_rec_gen(void)
{
  uint32_t gen;

  pthread_mutex_lock(&g_srec_lock);
  srec_ensure_locked();
  gen = g_srec_gen;
  pthread_mutex_unlock(&g_srec_lock);
  return gen;
}

int myvendor_devctl_sensor_remember(uint8_t kind, const uint8_t *addr,
                                    uint8_t addr_type, const char *name)
{
  uint8_t i;
  uint8_t idx = 0xffu;
  int ret = 0;
  bool found = false;
  bool dirty = false;
  uint8_t drop_addr[MYVENDOR_DEVCTL_SENSOR_ADDR_LEN];
  uint8_t drop_kind = 0;
  bool have_drop = false;

  if (kind > MYVENDOR_DEVCTL_SENSOR_KIND_CPS || srec_addr_empty(addr))
    {
      return -EINVAL;
    }

  memset(drop_addr, 0, sizeof(drop_addr));
  pthread_mutex_lock(&g_srec_lock);
  srec_ensure_locked();
  for (i = 0; i < g_srec_n; i++)
    {
      if (g_srec[i].autorc && g_srec[i].kind == kind &&
          !srec_addr_eq(g_srec[i].addr, addr))
        {
          memcpy(drop_addr, g_srec[i].addr, sizeof(drop_addr));
          drop_kind = g_srec[i].kind;
          have_drop = true;
          break;
        }
    }

  for (i = 0; i < g_srec_n; i++)
    {
      if (g_srec[i].kind == kind && srec_addr_eq(g_srec[i].addr, addr))
        {
          if (g_srec[i].addr_type != addr_type)
            {
              g_srec[i].addr_type = addr_type;
              dirty = true;
            }

          if (name != NULL && name[0] != '\0')
            {
              char tmp[MYVENDOR_DEVCTL_SENSOR_NAME_MAX];

              srec_name_copy(tmp, sizeof(tmp), name);
              if (memcmp(g_srec[i].name, tmp, sizeof(tmp)) != 0)
                {
                  memcpy(g_srec[i].name, tmp, sizeof(tmp));
                  dirty = true;
                }
            }

          found = true;
          idx = i;
          break;
        }
    }

  if (!found)
    {
      if (g_srec_n >= MYVENDOR_DEVCTL_SENSOR_REC_MAX)
        {
          uint8_t drop = 0xffu;

          for (i = 0; i < g_srec_n; i++)
            {
              if (!g_srec[i].autorc)
                {
                  drop = i;
                  break;
                }
            }

          if (drop == 0xffu)
            {
              drop = 0;
            }

          memmove(&g_srec[drop], &g_srec[drop + 1u],
                  (size_t)(g_srec_n - drop - 1u) * sizeof(g_srec[0]));
          g_srec_n--;
        }

      idx = g_srec_n;
      memset(&g_srec[idx], 0, sizeof(g_srec[0]));
      g_srec[idx].kind = kind;
      g_srec[idx].addr_type = addr_type;
      memcpy(g_srec[idx].addr, addr, MYVENDOR_DEVCTL_SENSOR_ADDR_LEN);
      srec_name_copy(g_srec[idx].name, sizeof(g_srec[idx].name), name);
      g_srec_n++;
      dirty = true;
    }

  if (idx < g_srec_n && !g_srec[idx].autorc)
    {
      dirty = true;
    }

  if (!dirty)
    {
      pthread_mutex_unlock(&g_srec_lock);
      return 0;
    }

  for (i = 0; i < g_srec_n; i++)
    {
      if (g_srec[i].kind == kind)
        {
          g_srec[i].autorc = (i == idx);
        }
    }

  ret = srec_save_locked();
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  /* 已在 READY，不再 post CONNECT。只清掉上一台的 want_link。 */
  if (ret == 0 && have_drop)
    {
      srec_post_clear_locked(drop_addr, drop_kind);
    }
#else
  (void)have_drop;
  (void)drop_kind;
#endif
  pthread_mutex_unlock(&g_srec_lock);
  return ret;
}

int myvendor_devctl_sensor_auto_set(uint8_t idx, bool on)
{
  uint8_t i;
  uint8_t prev;
  uint8_t drop_addr[MYVENDOR_DEVCTL_SENSOR_ADDR_LEN];
  uint8_t drop_kind = 0;
  bool have_drop = false;
  int ret;

  pthread_mutex_lock(&g_srec_lock);
  srec_ensure_locked();
  if (idx >= g_srec_n)
    {
      pthread_mutex_unlock(&g_srec_lock);
      return -EINVAL;
    }

  memset(drop_addr, 0, sizeof(drop_addr));
  prev = 0xffu;
  for (i = 0; i < g_srec_n; i++)
    {
      if (g_srec[i].autorc && g_srec[i].kind == g_srec[idx].kind)
        {
          prev = i;
          break;
        }
    }

  if (on)
    {
      if (prev != 0xffu && prev != idx)
        {
          memcpy(drop_addr, g_srec[prev].addr, sizeof(drop_addr));
          drop_kind = g_srec[prev].kind;
          have_drop = true;
        }

      for (i = 0; i < g_srec_n; i++)
        {
          if (g_srec[i].kind == g_srec[idx].kind)
            {
              g_srec[i].autorc = (i == idx);
            }
        }
    }
  else
    {
      g_srec[idx].autorc = false;
    }

  ret = srec_save_locked();
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  if (ret == 0)
    {
      if (on)
        {
          srec_post_bind_locked(idx, have_drop ? drop_addr : NULL, drop_kind);
        }
      else
        {
          srec_post_clear_locked(g_srec[idx].addr, g_srec[idx].kind);
        }
    }
#else
  (void)have_drop;
  (void)drop_kind;
#endif
  pthread_mutex_unlock(&g_srec_lock);
  return ret;
}

int myvendor_devctl_sensor_auto_get(myvendor_devctl_sensor_rec_t *out)
{
  uint8_t i;

  if (out == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_srec_lock);
  srec_ensure_locked();
  for (i = 0; i < g_srec_n; i++)
    {
      if (g_srec[i].autorc)
        {
          *out = g_srec[i];
          pthread_mutex_unlock(&g_srec_lock);
          return 0;
        }
    }

  pthread_mutex_unlock(&g_srec_lock);
  return -ENOENT;
}

int myvendor_devctl_sensor_delete(uint8_t idx)
{
  myvendor_devctl_sensor_rec_t gone;
  int ret;

  pthread_mutex_lock(&g_srec_lock);
  srec_ensure_locked();
  if (idx >= g_srec_n)
    {
      pthread_mutex_unlock(&g_srec_lock);
      return -EINVAL;
    }

  gone = g_srec[idx];
  if (idx + 1u < g_srec_n)
    {
      memmove(&g_srec[idx], &g_srec[idx + 1u],
              (size_t)(g_srec_n - idx - 1u) * sizeof(g_srec[0]));
    }

  g_srec_n--;
  memset(&g_srec[g_srec_n], 0, sizeof(g_srec[0]));
  ret = srec_save_locked();
#ifdef CONFIG_MYVENDOR_BLE_COMPANION
  if (ret == 0 && gone.autorc)
    {
      srec_post_clear_locked(gone.addr, gone.kind);
    }
#else
  (void)gone;
#endif
  pthread_mutex_unlock(&g_srec_lock);
  return ret;
}

int myvendor_devctl_waypoints_load(const char *path,
                                   myvendor_devctl_favorite_t *out,
                                   size_t cap, size_t *count)
{
  FILE *fp;
  char line[160];
  size_t n = 0;

  if (path == NULL || path[0] == '\0' || out == NULL || cap == 0 ||
      count == NULL)
    {
      return -EINVAL;
    }

  *count = 0;
  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return errno == ENOENT ? 0 : -errno;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      char *name;
      char *lat_text;
      char *lon_text;
      char *extra;
      char *save = NULL;
      size_t len = strlen(line);
      double lat;
      double lon;

      if (len > 0 && line[len - 1] == '\n')
        {
          line[--len] = '\0';
        }
      if (len > 0 && line[len - 1] == '\r')
        {
          line[--len] = '\0';
        }
      if (len == 0)
        {
          continue;
        }

      name = strtok_r(line, "\t", &save);
      lat_text = strtok_r(NULL, "\t", &save);
      lon_text = strtok_r(NULL, "\t", &save);
      extra = strtok_r(NULL, "\t", &save);
      if (name == NULL || name[0] == '\0' ||
          strlen(name) >= MYVENDOR_DEVCTL_FAVORITE_NAME_MAX ||
          lat_text == NULL || lon_text == NULL || extra != NULL ||
          !favorite_coord_parse(lat_text, -90.0, 90.0, &lat) ||
          !favorite_coord_parse(lon_text, -180.0, 180.0, &lon))
        {
          fclose(fp);
          return -EINVAL;
        }
      if (n >= cap)
        {
          fclose(fp);
          return -E2BIG;
        }

      snprintf(out[n].name, sizeof(out[n].name), "%s", name);
      out[n].latitude = lat;
      out[n].longitude = lon;
      n++;
    }

  if (ferror(fp))
    {
      int err = errno != 0 ? errno : EIO;

      fclose(fp);
      return -err;
    }

  fclose(fp);
  *count = n;
  return 0;
}

int myvendor_devctl_favorites_load(myvendor_devctl_favorite_t *out,
                                   size_t cap, size_t *count)
{
  return myvendor_devctl_waypoints_load(MYVENDOR_DEVCTL_FAVORITES_PATH,
                                        out, cap, count);
}

int myvendor_devctl_bat_full_mv_get(void)
{
  int32_t v;

  if (g_loaded)
    {
      return g_bat_full_mv;
    }

  if (persist_try_i32(MYVENDOR_DEVCTL_BAT_FULL_KEY, &v) &&
      bat_full_mv_ok((int)v))
    {
      return (int)v;
    }

  return MYVENDOR_DEVCTL_BAT_FULL_DEFAULT;
}

bool myvendor_devctl_bat_full_have(void)
{
  int32_t v;

  if (g_loaded)
    {
      return g_bat_full_have;
    }

  return persist_try_i32(MYVENDOR_DEVCTL_BAT_FULL_KEY, &v) &&
         bat_full_mv_ok((int)v);
}

void myvendor_devctl_bat_full_note(int mv)
{
  int cal;
  int delta;

  /* 使用中的 3.98 V 不是满电样本；只有充电结束附近的电压才校准。 */
  if (mv < MYVENDOR_DEVCTL_BAT_FULL_OBS_MIN ||
      mv > MYVENDOR_DEVCTL_BAT_FULL_OBS_MAX)
    {
      return;
    }

  cal = mv - MYVENDOR_DEVCTL_BAT_FULL_MARGIN_MV;
  if (!bat_full_mv_ok(cal))
    {
      return;
    }

  if (!g_bat_full_have)
    {
      int32_t stored;

      if (persist_try_i32(MYVENDOR_DEVCTL_BAT_FULL_KEY, &stored) &&
          bat_full_mv_ok((int)stored))
        {
          g_bat_full_mv = (int)stored;
          g_bat_full_have = true;
        }
    }

  if (g_bat_full_have)
    {
      delta = cal - g_bat_full_mv;
      if (delta < 0)
        {
          delta = -delta;
        }

      if (delta < MYVENDOR_DEVCTL_BAT_FULL_DEADBAND_MV)
        {
          return;
        }
    }

  g_bat_full_mv = cal;
  g_bat_full_have = true;
  persist_set_i32(MYVENDOR_DEVCTL_BAT_FULL_KEY, (int32_t)cal);
  syslog(LOG_INFO, "ctl: bat full cal %d mV (obs %d mV)", cal, mv);
}

int myvendor_devctl_pwr_set(bool on)
{
  persist_set_i32(MYVENDOR_DEVCTL_PWR_KEY, on ? 1 : 0);
  return 0;
}

bool myvendor_devctl_pwr_get(void)
{
  return persist_get_i32(MYVENDOR_DEVCTL_PWR_KEY, 0) != 0;
}

void myvendor_devctl_power_cut(void)
{
  unsigned n;

  syslog(LOG_INFO, "ctl: poweroff, PA29 low");
  myvendor_watchdog_halt();
  BSP_GPIO_Set(DEVCTL_PWR_KEY_CTL_PIN, 0, 1);

  for (n = 0; n < 20; n++)
    {
      if (HAL_GPIO_ReadPin(hwp_gpio1, DEVCTL_PWR_KEY_CTL_PIN) == GPIO_PIN_RESET)
        {
          break;
        }

      usleep(1000);
    }

  usleep(100000);
  syslog(LOG_INFO, "ctl: poweroff reset");
  HAL_PMU_Reboot();
  for (;;)
    {
    }
}

void myvendor_devctl_poweroff(void)
{
  myvendor_devctl_bl_commit();
  (void)myvendor_gnss_prepare_poweroff();
  myvendor_mtp_prepare_poweroff();
  myvendor_devctl_power_cut();
}
