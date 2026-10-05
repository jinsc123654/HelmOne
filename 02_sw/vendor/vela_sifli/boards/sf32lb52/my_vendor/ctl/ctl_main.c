/**
 * @file ctl_main.c
 * @brief NSH `ctl`：产品控制面（persist + owner set）。读走 `sys`。
 *
 *   ctl bl <0-100>      背光
 *   ctl radio on|off    手机 Companion 广播
 *   ctl sensor on|off   HR/CSC/CPS Central 策略
 *   ctl mtp on|off      USB 控制器
 *   ctl notif on|off    手机通知
 *   ctl calls on|off    仅来电
 *   ctl sound on|off    蜂鸣器提示音
 *   ctl autopause on|off 骑行自动暂停
 *   ctl grade [<mdeg>]  坡度零点（毫度；省略则读取）
 *   ctl tz [+8]         时区（小时；省略则读取，默认 +8）
 *   ctl gnss dump_out   旁路倒 MAX-M10S 导航库到 mga_<utc>.ubx
 *   ctl gnss dump_in    把最新 mga_<utc>.ubx 灌进模组
 *   ctl gnss auto [on|off]  开机注入 / 定位后倒库
 *   ctl gnss still [on|off] 停车切 stationary（默认关，见 myvendor_gnss_dyn_still_enable）
 *   ctl idle              打印静止子命令
 *   ctl idle on|off|hour|stay|auto  调试静止；stay=本次开机不自动静止
 *   ctl dvfs auto|144|240  调试 HCPU 调频
 *   ctl cnt [<name>|all] on|off  周期计数器报表开关（diag 那张表；默认全开，落 KV）
 *   ctl cnt now                  立即打一遍选中的计数器
 *   ctl wt <hexaddr> [1|2|4]  DWT 数据观察点：给写坏内存的人点名
 *   ctl wt [clear|test]       全部撤掉 / 自我触发验证交付路径
 *   ctl log [level]     syslog 屏蔽位（err 起，运行时全系统生效，落 KV）
 *   ctl fs ls|get|put   主机 ↔ 板子的文件搬运（走控制台，base64 + 整文件 CRC32）
 *   ctl pwr [on|off]    调试开机（persist.boot.pwr；2SFBL 不判键直接跳）
 *   ctl off             关机（拉低 PA29）
 *
 * 菜单搜表/连接走 C API（myvendor_devctl_sensor_scan 等），不挂 NSH。
 * 外设探针用 `test sensor`，逻辑独立。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_devctl.h"
#include "myvendor_bicycle_ctl.h"
#include "myvendor_gnss.h"
#include "sf32lb_dvfs.h"

#include <dirent.h>  /* ctl fs ls：readdir() */
#include <errno.h>
#include <fcntl.h>   /* open()/O_RDONLY：`ctl gnss listen` 直接读模组串口 */
#include <math.h>
#include <poll.h>   /* ctl fs put：带超时读控制台，见 fs_read_line() */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* ctl fs ls：stat()/S_ISDIR() */
#include <syslog.h>
#include <time.h>    /* clock_gettime(CLOCK_MONOTONIC)：listen 的自带时限 */
#include <unistd.h>

#include <nuttx/syslog/syslog.h>
#include <nuttx/arch.h>    /* up_check_tcbstack()：栈水位自测，见 ctl_stack_report() */
#include <nuttx/sched.h>   /* this_task() / struct tcb_s 的 adj_stack_size */

/* `ctl btlog` 用 KVDB 的 property 接口而不是直接写 persist 文件：框架那边是靠
 * property_monitor 的通知热加载等级的（见 cmd_btlog 的说明）。头部在
 * frameworks/system/utils/include，NSH app 目标够得到（nsh/test/test_kv.c 同款）。 */
#include <kvdb.h>

/* 本文件是 nuttx_add_application（目标 `ctl`），include 路径只有
 * board/include，**芯片层头文件不在里面**，所以只能本地声明。
 * （对照：services/ 下的文件属于 board 目标，可以直接 `#include "sf32lb_sdio.h"`。）
 *
 * 两个函数的完整契约（绕过判死闸门、write 只允许卡尾 1 MiB）见
 * chips/sf32lb52/include/sf32lb_sdio.h 里 sf32lb_sd_raw_read / _raw_write 的
 * Doxygen —— 它们是 `ctl sd read|write` 的落点，用来在**不惊动文件系统**的
 * 前提下直接验卡：判死之后 LFS 层已经不下发 I/O 了，只有裸读能继续测卡。 */
extern int sf32lb_sd_raw_read(uint64_t byte_off, uint32_t nblocks);
extern int sf32lb_sd_raw_write(uint64_t byte_off, uint32_t nblocks);

/* DWT 数据观察点（chips/sf32lb52/myvendor_idle_stat.c）。芯片头在 NSH app 目标里
 * 不可见，沿用本文件既有的"调用点 extern"做法。 */
extern int myvendor_dwt_watch_arm(uint32_t addr, unsigned size_bytes);
extern void myvendor_dwt_watch_clear(void);
extern int myvendor_dwt_watch_status(char *buf, size_t buflen);

/* 路线规划自测（ui/bicycle/src/nav_test.c）：脱离 App 对同一对起终点跑
 * 直规与走廊(rolling)并打印长度/机动，用来复现"rolling 绕远路"那件事。 */
extern int myvendor_nav_test(int argc, char **argv);
extern int sf32lb52_bt_nvds_dump(void);   /* chips/sf32lb52/sf32lb52_bt_adapter.c */
extern int myvendor_bt_pair_cmd(int argc, char **argv);   /* services/myvendor_bt_pair.c */

#define CTL_FAV_PATH      MYVENDOR_DEVCTL_FAVORITES_PATH
#define CTL_FAV_TMP_PATH  "/mnt/kv/.bicycle_favorites.tmp"
#define CTL_FAV_MAX       MYVENDOR_DEVCTL_FAVORITE_MAX
#define CTL_FAV_NAME_MAX  MYVENDOR_DEVCTL_FAVORITE_NAME_MAX

struct ctl_favorite_s
{
  char name[CTL_FAV_NAME_MAX];
  double latitude;
  double longitude;
};

/**
 * @brief 解析 on/1 或 off/0。
 * @return 0 成功，-EINVAL 非法。
 */
static int parse_onoff(const char *s, bool *on)
{
  if (s == NULL)
    {
      return -EINVAL;
    }

  if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0)
    {
      *on = true;
      return 0;
    }

  if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0)
    {
      *on = false;
      return 0;
    }

  return -EINVAL;
}

/** @brief 解析 `AA:BB:CC:DD:EE:FF` / `AA-BB-...` 成 6 字节。 */
static bool sensor_mac_parse(const char *text, uint8_t *out)
{
  unsigned b[6];
  int      n;

  if (text == NULL || out == NULL)
    {
      return false;
    }

  n = sscanf(text, "%x:%x:%x:%x:%x:%x",
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  if (n != 6)
    {
      n = sscanf(text, "%x-%x-%x-%x-%x-%x",
                 &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    }
  if (n != 6)
    {
      return false;
    }

  for (n = 0; n < 6; n++)
    {
      if (b[n] > 0xffu)
        {
          return false;
        }
      out[n] = (uint8_t)b[n];
    }
  return true;
}

/**
 * @brief `ctl sensor bind <kind> <mac> [addr_type]`：按 MAC 重绑一台传感器。
 *
 * 为什么要有：绑定记录（`/mnt/kv/bicycle_sensors.tsv`）被删掉之后设备就不再回连
 * 那台外设，而重绑原本只能在菜单里按好几下键；远端（串口 / monitor-ctl）需要一条
 * 能把现场恢复回去的路。走 UI 点选同一条投递路径（companion 线程执行），
 * NSH 线程不直接碰协议栈。
 */
static int cmd_sensor_bind(int argc, char *argv[])
{
  uint8_t  addr[MYVENDOR_DEVCTL_SENSOR_ADDR_LEN];
  unsigned kind;
  unsigned type;
  int      rc;

  if (argc < 3 || argv[1] == NULL || argv[2] == NULL)
    {
      printf("usage: ctl sensor bind <0HR|1CSC|2CPS> <mac> [addr_type 0pub|1rnd]\n");
      return -EINVAL;
    }

  kind = (unsigned)strtoul(argv[1], NULL, 10);
  if (kind > MYVENDOR_DEVCTL_SENSOR_KIND_CPS)
    {
      printf("ctl sensor bind: kind must be 0/1/2 (got %s)\n", argv[1]);
      return -EINVAL;
    }

  memset(addr, 0, sizeof(addr));
  if (!sensor_mac_parse(argv[2], addr))
    {
      printf("ctl sensor bind: bad mac '%s' (want AA:BB:CC:DD:EE:FF)\n", argv[2]);
      return -EINVAL;
    }

  if (argc >= 4 && argv[3] != NULL)
    {
      type = (unsigned)strtoul(argv[3], NULL, 10);
      if (type > 1u)
        {
          printf("ctl sensor bind: addr_type must be 0(public)/1(random)\n");
          return -EINVAL;
        }
    }
  else
    {
      /* 省掉 type 就按地址位模式猜：静态随机地址最高两位是 11（0xC0…）。 */
      type = (addr[0] & 0xc0u) == 0xc0u ? 1u : 0u;
    }

  /* 显示顺序（`AA:BB:…`）→ 框架顺序（最低位在前）：`ui.found[].addr` 是这个序，
   * 反着填会去连一个不存在的地址（2026-09-20 实测：日志打出来是 `AC:7B:F3:…`）。 */
  for (rc = 0; rc < 3; rc++)
    {
      uint8_t swap = addr[rc];

      addr[rc]     = addr[5 - rc];
      addr[5 - rc] = swap;
    }

  rc = myvendor_devctl_sensor_connect_addr(addr, (uint8_t)type, (uint8_t)kind, NULL);
  if (rc != 0)
    {
      printf("ctl sensor bind: failed rc=%d\n", rc);
      return rc;
    }

  printf("ctl sensor bind: %s kind=%u type=%u -> companion\n",
         argv[2], kind, type);
  return 0;
}

/** @brief 打印用法。 */
/* ---------------------------------------------------------------------------
 * ctl fs — 主机 ↔ 板子的文件搬运（人工，走控制台）
 *
 * 为什么需要：板上的 `mga_*.ubx` 到底对不对、diag 归档里有什么，以前只能靠 MTP
 * 拖拽或拔卡。MTP 会 hold 住 LittleFS（顺带把星历注入挡掉），拔卡要拆机，而
 * 串口/控制台是一直挂在的。
 *
 * 为什么走 base64 而不是 `cat`：控制台是**共享**的，syslog 随时会插进来；二进制
 * 被插一行就再也拼不回来了。base64 是文本、坏行可判别，再加"整文件 size + CRC32"
 * 就能保证**要么完整要么报错**，不会悄悄收下半个文件。
 *
 * 为什么写入先落 `.part` 再 rename：`gnss_eph_src()` 选源只要求"文件名能解出 UTC
 * + 头 6 字节是 UBX + 大小 > 64"。半截文件完全满足这三条，会被当成星历源灌进模组
 * —— 所以绝不能让半成品出现在那个目录里。`mga_<utc>.ubx.part` 解不出 UTC
 * （`gnss_eph_parse_utc()` 要求 `.ubx` 后面就是结尾），注入器看不见它。
 * ------------------------------------------------------------------------- */

/* 57 B -> 76 个 base64 字符：一行的长度离 NSH 的 128 字节行长还有余量。 */
#define FS_LINE_CHUNK 57u
#define FS_LINE_MAX   256u
/** 单次上限：够星历和 diag 归档，防手滑把 16 MB 的 LFS 填满。 */
#define FS_PUT_MAX    (2u * 1024u * 1024u)
/** 攒一行期间的最大静默。正常传输每行间隔只有几毫秒，超过这个就是主机没了。 */
#define FS_PUT_IDLE_MS 15000

static uint32_t g_fs_crc_tab[16];
static bool     g_fs_crc_ready;

static void fs_crc_prepare(void)
{
  unsigned i;
  unsigned j;

  if (g_fs_crc_ready) {
    return;
  }

  for (i = 0; i < 16u; i++) {
    uint32_t c = (uint32_t)i;

    for (j = 0; j < 4u; j++) {
      c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
    }

    g_fs_crc_tab[i] = c;
  }

  g_fs_crc_ready = true;
}

/**
 * @brief 标准 CRC-32（反射、poly 0xEDB88320，初值与终值都取反）。
 *
 * 与主机 `zlib.crc32()` 逐位一致 —— 所以主机可以把这行校验当硬判据用，
 * 不需要自己再实现一遍。
 */
static uint32_t fs_crc32(uint32_t crc, const uint8_t *buf, size_t n)
{
  size_t i;

  fs_crc_prepare();
  for (i = 0; i < n; i++) {
    crc ^= (uint32_t)buf[i];
    crc = (crc >> 4) ^ g_fs_crc_tab[crc & 0x0fu];
    crc = (crc >> 4) ^ g_fs_crc_tab[crc & 0x0fu];
  }

  return crc;
}

static const char g_fs_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t fs_b64_enc(const uint8_t *in, size_t n, char *out)
{
  size_t i = 0;
  size_t o = 0;

  while (i < n) {
    size_t rem = n - i;
    uint32_t v = (uint32_t)in[i] << 16;

    if (rem > 1u) {
      v |= (uint32_t)in[i + 1u] << 8;
    }

    if (rem > 2u) {
      v |= (uint32_t)in[i + 2u];
    }

    out[o++] = g_fs_b64[(v >> 18) & 0x3fu];
    out[o++] = g_fs_b64[(v >> 12) & 0x3fu];
    out[o++] = (rem > 1u) ? g_fs_b64[(v >> 6) & 0x3fu] : '=';
    out[o++] = (rem > 2u) ? g_fs_b64[v & 0x3fu] : '=';
    i += 3u;
  }

  return o;
}

static int fs_b64_val(int c)
{
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }

  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }

  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }

  if (c == '+') {
    return 62;
  }

  if (c == '/') {
    return 63;
  }

  return -1;
}

/** @return 解出的字节数；-1 = 有非法字符。'=' 及其之后一律丢弃。 */
static int fs_b64_dec(const char *in, uint8_t *out, size_t max)
{
  uint32_t acc = 0;
  size_t o = 0;
  int bits = 0;

  for (; *in != '\0'; in++) {
    int v;

    if (*in == '=') {
      break;
    }

    v = fs_b64_val((unsigned char)*in);
    if (v < 0) {
      /* 行尾的 CR/LF/空格是行格式的一部分，不是数据；别的才算坏行。 */
      if (*in == '\r' || *in == '\n' || *in == ' ' || *in == '\t') {
        continue;
      }

      return -1;
    }

    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= max) {
        return -1;
      }

      out[o++] = (uint8_t)((acc >> bits) & 0xffu);
    }
  }

  return (int)o;
}

static int fs_cmd_get(const char *path)
{
  uint8_t buf[FS_LINE_CHUNK];
  char line[FS_LINE_CHUNK / 3u * 4u + 4u];
  const char *base;
  uint32_t crc = 0xffffffffu;
  uint32_t total = 0;
  ssize_t n;
  int fd;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("ctl fs get: open %s failed %d\n", path, errno);
    return EXIT_FAILURE;
  }

  base = strrchr(path, '/');
  base = (base != NULL) ? (base + 1) : path;

  /* 第一遍只为把 size/crc 写进 BEGIN 行：主机读到 BEGIN 就知道该收多少字节、
   * 收完立刻能自校验，不必等 END。 */
  for (;;) {
    n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      printf("ctl fs get: read %s failed %d\n", path, errno);
      close(fd);
      return EXIT_FAILURE;
    }

    if (n == 0) {
      break;
    }

    crc = fs_crc32(crc, buf, (size_t)n);
    total += (uint32_t)n;
  }

  crc ^= 0xffffffffu;

  printf("---BEGIN %s size=%lu crc=%08lx\n",
         base, (unsigned long)total, (unsigned long)crc);

  if (lseek(fd, 0, SEEK_SET) < 0) {
    printf("ctl fs get: lseek %s failed %d\n", path, errno);
    close(fd);
    return EXIT_FAILURE;
  }

  for (;;) {
    size_t k;

    n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }

    k = fs_b64_enc(buf, (size_t)n, line);
    line[k] = '\0';
    printf("%s\n", line);
  }

  close(fd);
  printf("---END %s size=%lu crc=%08lx\n",
         base, (unsigned long)total, (unsigned long)crc);
  return EXIT_SUCCESS;
}

/**
 * @brief 带超时地读一行（保留并含换行符）。
 *
 * 为什么不用 `fgets`：**fgets 拿到半行也会一直阻塞，而且阻塞点没有超时**。
 * 主机一崩（拔线 / 脚本被杀 / 传一半退出），这个死等就落在 NSH 的主线程上——
 * 本板没开 `CONFIG_TTY_SIGINT`，Ctrl-C 打断不了，只能重启板子。2026-09-27 实测
 * 就这样把控制台占死过一次。所以改成 poll + 逐字节 read，**攒行期间任何一次
 * 静默超时都算失败**（走 FAIL、删掉 .part，控制台立刻回到 nsh>）。
 *
 * @return 行长（含换行，已补 '\0'）；-1 = 超时 / 断开 / 行太长。
 */
static int fs_read_line(char *buf, size_t n)
{
  size_t used = 0;
  int fd = fileno(stdin);

  for (;;) {
    struct pollfd pfd;
    ssize_t got;

    if (used + 1u >= n) {
      return -1;                        /* 行太长：当坏输入处理 */
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, FS_PUT_IDLE_MS) <= 0) {
      return -1;
    }

    got = read(fd, buf + used, 1);
    if (got <= 0) {
      return -1;
    }

    if (buf[used] == '\n') {
      used++;
      break;
    }

    used++;
  }

  buf[used] = '\0';
  return (int)used;
}

static int fs_cmd_put(const char *path, unsigned long want_size,
                      unsigned long want_crc)
{
  char tmp[192];
  char line[FS_LINE_MAX];
  uint8_t buf[FS_LINE_MAX / 4u * 3u];
  uint32_t crc = 0xffffffffu;
  uint32_t total = 0;
  bool bad = false;
  FILE *fp;

  if (snprintf(tmp, sizeof(tmp), "%s.part", path) >= (int)sizeof(tmp)) {
    printf("ctl fs put: path too long\n");
    return EXIT_FAILURE;
  }

  fp = fopen(tmp, "wb");
  if (fp == NULL) {
    printf("ctl fs put: open %s failed %d\n", tmp, errno);
    return EXIT_FAILURE;
  }

  /* READY 是给主机的握手：**看到它才允许发数据**。
   *
   * 不能省。NSH 在读下一条命令之前，可能已经把主机提前发来的字节收进了自己的
   * 行缓冲；那批字节就永远到不了这里。先发命令、等 READY、再发数据，主机那边
   * 保证 NSH 此刻正阻塞在 waitpid 上，字节直达本函数。 */
  printf("---READY %s size=%lu crc=%08lx\n",
         tmp, want_size, want_crc);
  fflush(stdout);

  for (;;) {
    int got;
    int ln = fs_read_line(line, sizeof(line));

    if (ln < 0) {
      printf("ctl fs put: no data for %u ms after %lu bytes (host gone?)\n",
             (unsigned)FS_PUT_IDLE_MS, (unsigned long)total);
      bad = true;
      break;
    }

    if (strncmp(line, "---END", 6) == 0) {
      break;
    }

    got = fs_b64_dec(line, buf, sizeof(buf));
    if (got < 0) {
      /* 把坏行头几个字节原样打出来：现场 2026-09-27 就是靠这行才看出"读到的是
       * 上一条命令的回显"（而不是数据），否则只能靠猜。 */
      unsigned k;

      printf("ctl fs put: bad base64 after %lu bytes, head=",
             (unsigned long)total);
      for (k = 0; k < 12u && line[k] != '\0' && line[k] != '\n'; k++) {
        printf("%02x ", (unsigned char)line[k]);
      }

      printf("\n");
      bad = true;
      break;
    }

    if (got == 0) {
      continue;
    }

    if (fwrite(buf, 1u, (size_t)got, fp) != (size_t)got) {
      printf("ctl fs put: write failed %d\n", errno);
      bad = true;
      break;
    }

    crc = fs_crc32(crc, buf, (size_t)got);
    total += (uint32_t)got;
    if (total > FS_PUT_MAX) {
      printf("ctl fs put: over %lu bytes, aborted\n",
             (unsigned long)FS_PUT_MAX);
      bad = true;
      break;
    }
  }

  fflush(fp);
  fclose(fp);
  crc ^= 0xffffffffu;

  if (bad || (unsigned long)total != want_size ||
      (unsigned long)crc != (want_crc & 0xffffffffu)) {
    (void)unlink(tmp);
    printf("---FAIL size=%lu/%lu crc=%08lx/%08lx\n",
           (unsigned long)total, want_size,
           (unsigned long)crc, want_crc);
    return EXIT_FAILURE;
  }

  /* 只有校验全过才让这个名字出现：注入器只认 `mga_<utc>.ubx`，半成品永远叫
   * `.part`，所以它即使灌到一半断了，模组那边也不会拿到半份星历。 */
  if (rename(tmp, path) != 0) {
    printf("---FAIL rename %s -> %s failed %d\n", tmp, path, errno);
    (void)unlink(tmp);
    return EXIT_FAILURE;
  }

  printf("---OK %s size=%lu crc=%08lx\n",
         path, (unsigned long)total, (unsigned long)crc);
  return EXIT_SUCCESS;
}

static int fs_cmd_ls(const char *dir)
{
  struct dirent *de;
  unsigned n = 0;
  uint64_t total = 0;
  char path[192];
  DIR *dp;

  dp = opendir(dir);
  if (dp == NULL) {
    printf("ctl fs ls: opendir %s failed %d\n", dir, errno);
    return EXIT_FAILURE;
  }

  while ((de = readdir(dp)) != NULL) {
    struct stat st;

    if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name)
        >= (int)sizeof(path)) {
      continue;
    }

    if (stat(path, &st) != 0) {
      continue;
    }

    printf("%10ld %10ld %s%s\n", (long)st.st_size, (long)st.st_mtime,
           de->d_name, S_ISDIR(st.st_mode) ? "/" : "");
    total += (uint64_t)st.st_size;
    n++;
  }

  closedir(dp);
  printf("ctl fs ls: %u entries %lu bytes\n", n, (unsigned long)total);
  return EXIT_SUCCESS;
}

static int cmd_fs(int argc, char **argv)
{
  static const char dflt_dir[] = "/mnt/lfs/eph";

  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0) {
    printf("usage: ctl fs ls [dir]      list (default %s)\n", dflt_dir);
    printf("       ctl fs get <path>    read out as base64 (+size/crc32)\n");
    printf("       ctl fs put <path> <size> <crc32hex>   write in\n");
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "ls") == 0 || strcmp(argv[1], "dir") == 0) {
    return fs_cmd_ls((argc >= 3 && argv[2] != NULL) ? argv[2] : dflt_dir);
  }

  if (strcmp(argv[1], "get") == 0 || strcmp(argv[1], "read") == 0) {
    if (argc < 3 || argv[2] == NULL) {
      printf("ctl fs get: need <path>\n");
      return EXIT_FAILURE;
    }

    return fs_cmd_get(argv[2]);
  }

  if (strcmp(argv[1], "put") == 0 || strcmp(argv[1], "write") == 0) {
    if (argc < 5 || argv[2] == NULL || argv[3] == NULL || argv[4] == NULL) {
      printf("ctl fs put: need <path> <size> <crc32hex>\n");
      return EXIT_FAILURE;
    }

    return fs_cmd_put(argv[2], strtoul(argv[3], NULL, 10),
                      strtoul(argv[4], NULL, 16));
  }

  printf("ctl fs: unknown '%s'\n", argv[1]);
  return EXIT_FAILURE;
}

static void usage(void)
{
  printf("ctl — write policy (persist + owners)\n");
  printf("  ctl bl <0-100>      backlight PWM percent (0=off)\n");
  printf("  ctl radio <on|off>  phone BLE advertising (upstream / 手机蓝牙)\n");
  printf("  ctl bt <on|off>     alias of radio\n");
  printf("  ctl sensor <on|off> HR/CSC/CPS central (downstream / 外设蓝牙)\n");
  printf("  ctl sensor bind <0HR|1CSC|2CPS> <mac> [type]  按 MAC 重绑（记录被删后恢复）\n");
  printf("  ctl mtp <on|off>    USB controller (PHY+clk; worker stays)\n");
  printf("  ctl notif <on|off>  phone 0xFF17 banners + inbox\n");
  printf("  ctl calls <on|off>  calls-only filter (needs notif on)\n");
  printf("  ctl sound <on|off>  piezo UI beeps (test sound still works)\n");
  printf("  ctl autopause <on|off>  ride auto-pause by speed/cadence\n");
  printf("  ctl grade [<mdeg>]  slope zero (millidegree pitch; omit to read)\n");
  printf("  ctl tz [<hours>]    timezone vs UTC, e.g. +8 (omit to read)\n");
  printf("  ctl fav <name> <lat> <lon>  save/update favorite point\n");
  printf("  ctl fav list              list favorite points\n");
  printf("  ctl fav del <name>        delete favorite point\n");
  printf("  ctl gnss dump_out   dump MAX-M10S nav DB -> /mnt/lfs/eph/mga_<utc>.ubx\n");
  printf("  ctl gnss dump_in    inject newest /mnt/lfs/eph/mga_<utc>.ubx\n");
  printf("  ctl gnss auto [on|off]  boot inject + dump after first fix\n");
  printf("  ctl gnss bdsonly [on|off]  只留北斗（看 gsv 的 bd 项）/ 恢复\n");
  printf("  ctl gnss b1c        try enabling B1C (1575.42, 与 L1 同频)\n");
  printf("  ctl gnss ver        log UBX-MON-VER (fw/hw version)\n");
  printf("  ctl gnss listen [ms]  直接读一次模组串口（自带时限；读线程活着时别用）\n");
  printf("  ctl idle              still-screen help\n");
  printf("  ctl idle on|off|hour|stay|auto  stay=no auto still this boot\n");
  printf("  ctl dvfs auto|144|240  HCPU DVFS (ondemand)\n");
  printf("  ctl dvfs hop [ms]   stress-test: hop gear every ms (def 10000)\n");
  printf("  ctl cnt             periodic counter report: list modules (default all on)\n");
  printf("  ctl cnt <name>|all on|off   toggle a module (persisted in KV)\n");
  printf("  ctl cnt now         dump the selected counters right now\n");
  printf("  ctl log [level]     syslog mask: err|warn|notice|info|all|silent\n");
  printf("  ctl fs ls [dir]     list a dir (default /mnt/lfs/eph)\n");
  printf("  ctl fs get <path>   read a file out (base64 + size/crc32)\n");
  printf("  ctl fs put <path> <size> <crc32>   write in (host waits for ---READY)\n");
  printf("  ctl pwr [on|off]    debug auto-boot (skip PWR key in 2SFBL)\n");
  printf("  ctl boot [fw|main|factory]  boot slot (persist.boot.target; next boot)\n");
  printf("  ctl off             power off (PA29 / PWR_KEY_CTL low)\n");
  printf("Reads: sys / sys bl / sys bat / ...\n");
}

/**
 * @brief `ctl bl <pct>`：只设置，不查询。
 */
static int cmd_bl(int argc, char *argv[])
{
  int v;
  int ret;

  if (argc < 2)
    {
      fprintf(stderr, "ctl bl: need 0..100 (read: sys bl)\n");
      return EXIT_FAILURE;
    }

  v = atoi(argv[1]);
  if (v < 0 || v > 100)
    {
      fprintf(stderr, "ctl bl: need 0..100\n");
      return EXIT_FAILURE;
    }

  ret = myvendor_devctl_bl_set((uint8_t)v);
  if (ret < 0)
    {
      fprintf(stderr, "ctl bl: set failed %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("ctl bl %d  (sys bl to read)\n", v);
  return 0;
}

/**
 * @brief `ctl radio|sensor|mtp <on|off>`。
 */
static int cmd_switch(const char *name, int argc, char *argv[],
                      int (*setfn)(bool))
{
  bool on;
  int ret;

  if (argc < 2)
    {
      fprintf(stderr, "ctl %s: need on|off (read: sys %s)\n",
              name, strcmp(name, "sensor") == 0 ? "hr" : name);
      return EXIT_FAILURE;
    }

  if (parse_onoff(argv[1], &on) != 0)
    {
      fprintf(stderr, "ctl %s: use on|off\n", name);
      return EXIT_FAILURE;
    }

  ret = setfn(on);
  if (ret == -ENOTSUP)
    {
      fprintf(stderr, "ctl %s: not in this image\n", name);
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl %s: set failed %d\n", name, ret);
      return EXIT_FAILURE;
    }

  printf("ctl %s %s\n", name, on ? "on" : "off");
  return 0;
}

/**
 * @brief `ctl pwr` 读调试开机标记；`ctl pwr on|off` 写 persist.boot.pwr。
 */

static int cmd_pwr(int argc, char *argv[])
{
  bool on;

  if (argc < 2)
    {
      printf("ctl pwr %s\n", myvendor_devctl_pwr_get() ? "on" : "off");
      return 0;
    }

  if (parse_onoff(argv[1], &on) != 0)
    {
      fprintf(stderr, "ctl pwr: use on|off\n");
      return EXIT_FAILURE;
    }

  if (myvendor_devctl_pwr_set(on) < 0)
    {
      fprintf(stderr, "ctl pwr: set failed\n");
      return EXIT_FAILURE;
    }

  printf("ctl pwr %s\n", on ? "on" : "off");
  return 0;
}

/**
 * @brief `ctl boot` 读启动槽位；`ctl boot fw|main|factory` 写 persist.boot.target。
 *
 * 2SFBL 的选择顺序是 `persist.boot.target` → `/fw` → `main` → `factory`。
 * 设备出厂时 `persist.boot.target` 是 `fw`（SD 上的 OTA 文件），所以
 * **`flash` 写完 main 分区之后如果没把 target 切到 `main`，跑的还是 `/fw`
 * 里那个旧镜像** —— 现场踩过：`flash` 明明成功、`build_date` 却不变。
 *
 * 写入**下一次开机**才生效（本命令不复位）：`ctl off` 或 NSH `reboot` 都行。
 * "现在实际跑的是哪个槽 / 哪一版"看 `sys` 输出里的 `slot` 那行。
 */
static int cmd_boot(int argc, char *argv[])
{
  char cur[MYVENDOR_DEVCTL_BOOT_TARGET_MAX];

  if (argc < 2)
    {
      if (myvendor_devctl_boot_target_get(cur, sizeof(cur)))
        {
          printf("ctl boot target=%s\n", cur);
        }
      else
        {
          printf("ctl boot target=(unset; 2SFBL 走默认 /fw > main > factory)\n");
        }

      printf("ctl boot running: see `sys` (slot 行)\n");
      return 0;
    }

  /* 校验在 setter 里（它只认 fw|main|factory，不认就回 -EINVAL）——
   * 写错名字的后果是 2SFBL 认不出、退回默认顺序，用户会以为"改了没生效"，
   * 所以宁可在这里拒掉而不是把任意字符串落盘。 */
  if (myvendor_devctl_boot_target_set(argv[1]) < 0)
    {
      fprintf(stderr, "ctl boot: use fw|main|factory\n");
      return EXIT_FAILURE;
    }

  printf("ctl boot %s (next boot; then `sys` 的 slot 行核对)\n", argv[1]);
  return 0;
}

/**
 * @brief `ctl grade` 读零点；`ctl grade <mdeg>` 写零点（毫度）。
 */
static int cmd_grade(int argc, char *argv[])
{
  int32_t v;
  char *end = NULL;

  if (argc < 2)
    {
      v = myvendor_devctl_grade_offset_get();
      printf("ctl grade  %ld mdeg\n", (long)v);
      return 0;
    }

  v = (int32_t)strtol(argv[1], &end, 0);
  if (end == argv[1] || (end != NULL && *end != '\0'))
    {
      fprintf(stderr, "ctl grade: need millidegrees, e.g. 0 or -2500\n");
      return EXIT_FAILURE;
    }

  if (myvendor_devctl_grade_offset_set(v) < 0)
    {
      fprintf(stderr, "ctl grade: set failed\n");
      return EXIT_FAILURE;
    }

  printf("ctl grade %ld\n", (long)myvendor_devctl_grade_offset_get());
  return 0;
}

/**
 * @brief `ctl tz` 读时区；`ctl tz +8` 写小时偏置。
 */
static int cmd_tz(int argc, char *argv[])
{
  int16_t min;
  int h;
  unsigned mag;
  unsigned hh;
  unsigned mm;
  char sign;
  char *end = NULL;

  if (argc < 2)
    {
      min = myvendor_devctl_tz_min_get();
      if (min < 0)
        {
          sign = '-';
          mag = (unsigned)(-min);
        }
      else
        {
          sign = '+';
          mag = (unsigned)min;
        }

      hh = mag / 60u;
      mm = mag % 60u;
      if (mm == 0)
        {
          printf("ctl tz  UTC%c%u\n", sign, hh);
        }
      else
        {
          printf("ctl tz  UTC%c%u:%02u\n", sign, hh, mm);
        }

      return 0;
    }

  h = (int)strtol(argv[1], &end, 10);
  if (end == argv[1] || (end != NULL && *end != '\0'))
    {
      fprintf(stderr, "ctl tz: need hours, e.g. +8 or -5\n");
      return EXIT_FAILURE;
    }

  if (h < -12 || h > 14)
    {
      fprintf(stderr, "ctl tz: hours -12..+14\n");
      return EXIT_FAILURE;
    }

  if (myvendor_devctl_tz_min_set((int16_t)(h * 60)) < 0)
    {
      fprintf(stderr, "ctl tz: set failed\n");
      return EXIT_FAILURE;
    }

  printf("ctl tz  UTC%+d\n", h);
  return 0;
}

static bool favorite_name_valid(const char *name)
{
  size_t n;

  if (name == NULL)
    {
      return false;
    }

  n = strlen(name);
  if (n == 0 || n >= CTL_FAV_NAME_MAX)
    {
      return false;
    }

  return strchr(name, '\t') == NULL &&
         strchr(name, '\r') == NULL &&
         strchr(name, '\n') == NULL;
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

static int favorite_load(struct ctl_favorite_s *items, size_t cap,
                         size_t *count)
{
  FILE *fp;
  char line[160];
  size_t n = 0;

  if (items == NULL || count == NULL)
    {
      return -EINVAL;
    }

  *count = 0;
  fp = fopen(CTL_FAV_PATH, "r");
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
      if (!favorite_name_valid(name) || lat_text == NULL || lon_text == NULL ||
          extra != NULL ||
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

      snprintf(items[n].name, sizeof(items[n].name), "%s", name);
      items[n].latitude = lat;
      items[n].longitude = lon;
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

static int favorite_save(const struct ctl_favorite_s *items, size_t count)
{
  FILE *fp;
  size_t i;
  int ret = 0;

  fp = fopen(CTL_FAV_TMP_PATH, "w");
  if (fp == NULL)
    {
      return -errno;
    }

  for (i = 0; i < count; i++)
    {
      if (fprintf(fp, "%s\t%.7f\t%.7f\n", items[i].name,
                  items[i].latitude, items[i].longitude) < 0)
        {
          ret = -(errno != 0 ? errno : EIO);
          break;
        }
    }

  if (ret == 0 && fflush(fp) != 0)
    {
      ret = -errno;
    }
  if (ret == 0 && fsync(fileno(fp)) != 0)
    {
      ret = -errno;
    }
  if (fclose(fp) != 0 && ret == 0)
    {
      ret = -errno;
    }

  if (ret == 0 && rename(CTL_FAV_TMP_PATH, CTL_FAV_PATH) != 0)
    {
      ret = -errno;
    }
  if (ret != 0)
    {
      unlink(CTL_FAV_TMP_PATH);
    }

  return ret;
}

static int cmd_fav(int argc, char *argv[])
{
  struct ctl_favorite_s *items;
  const char *name;
  const char *lat_text;
  const char *lon_text;
  size_t count = 0;
  size_t i;
  int ret;
  int result = EXIT_FAILURE;

  items = calloc(CTL_FAV_MAX, sizeof(*items));
  if (items == NULL)
    {
      fprintf(stderr, "ctl fav: out of memory\n");
      return EXIT_FAILURE;
    }
  ret = favorite_load(items, CTL_FAV_MAX, &count);
  if (ret < 0)
    {
      fprintf(stderr, "ctl fav: load %s failed %d\n", CTL_FAV_PATH, ret);
      goto out;
    }

  if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0))
    {
      printf("favorite points: %u (%s)\n", (unsigned)count, CTL_FAV_PATH);
      for (i = 0; i < count; i++)
        {
          printf("  %s  lat %.7f  lon %.7f\n", items[i].name,
                 items[i].latitude, items[i].longitude);
        }
      result = EXIT_SUCCESS;
      goto out;
    }

  if (argc == 3 && (strcmp(argv[1], "del") == 0 ||
                    strcmp(argv[1], "delete") == 0))
    {
      for (i = 0; i < count; i++)
        {
          if (strcmp(items[i].name, argv[2]) == 0)
            {
              memmove(&items[i], &items[i + 1],
                      (count - i - 1) * sizeof(items[0]));
              count--;
              ret = favorite_save(items, count);
              if (ret < 0)
                {
                  fprintf(stderr, "ctl fav: save failed %d\n", ret);
                  goto out;
                }
              printf("ctl fav: deleted %s\n", argv[2]);
              result = EXIT_SUCCESS;
              goto out;
            }
        }

      fprintf(stderr, "ctl fav: point '%s' not found\n", argv[2]);
      goto out;
    }

  if (argc == 5 && strcmp(argv[1], "set") == 0)
    {
      name = argv[2];
      lat_text = argv[3];
      lon_text = argv[4];
    }
  else if (argc == 4)
    {
      name = argv[1];
      lat_text = argv[2];
      lon_text = argv[3];
    }
  else
    {
      fprintf(stderr,
              "ctl fav: use <name> <lat> <lon>, list, or del <name>\n");
      goto out;
    }

  if (!favorite_name_valid(name))
    {
      fprintf(stderr, "ctl fav: name must be 1..%u bytes without tab/newline\n",
              CTL_FAV_NAME_MAX - 1);
      goto out;
    }

  for (i = 0; i < count; i++)
    {
      if (strcmp(items[i].name, name) == 0)
        {
          break;
        }
    }
  if (i == count && count >= CTL_FAV_MAX)
    {
      fprintf(stderr, "ctl fav: at most %u points\n", CTL_FAV_MAX);
      goto out;
    }
  if (!favorite_coord_parse(lat_text, -90.0, 90.0, &items[i].latitude) ||
      !favorite_coord_parse(lon_text, -180.0, 180.0, &items[i].longitude))
    {
      fprintf(stderr,
              "ctl fav: invalid coordinates (latitude -90..90, longitude -180..180)\n");
      goto out;
    }

  snprintf(items[i].name, sizeof(items[i].name), "%s", name);
  if (i == count)
    {
      count++;
    }
  ret = favorite_save(items, count);
  if (ret < 0)
    {
      fprintf(stderr, "ctl fav: save failed %d\n", ret);
      goto out;
    }

  printf("ctl fav: saved %s  lat %.7f  lon %.7f\n", name,
         items[i].latitude, items[i].longitude);
  result = EXIT_SUCCESS;

out:
  free(items);
  return result;
}

/**
 * @brief `ctl gnss dump_in|dump_out|auto`。
 */
/* `ctl gnss listen` 打开的就是这个节点。**必须是字面量**：写入口径要求
 * 路径以整条字面量常量喂 open()，所以这里不直接用 CONFIG_BOARD_L96_GNSS_DEVPATH，
 * 而是**照抄一份 + 每次使用前比对**：ctl_gnss_tty_check() 一旦发现两边不一致就报错，
 * 避免"配置改了、这里忘改"这种最难查的漂移。 */
static const char CTL_GNSS_TTY[] = "/dev/ttyS0";

static unsigned long ctl_mono_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (unsigned long)ts.tv_sec * 1000ul +
         (unsigned long)(ts.tv_nsec / 1000000);
}

static void ctl_gnss_tty_check(void)
{
  if (strcmp(CTL_GNSS_TTY, CONFIG_BOARD_L96_GNSS_DEVPATH) != 0)
    {
      syslog(LOG_ERR, "ctl: GNSS devpath 漂移！本文件=%s 配置=%s\n",
             CTL_GNSS_TTY, CONFIG_BOARD_L96_GNSS_DEVPATH);
    }
}

/**
 * @brief `ctl gnss listen [ms]` —— **自己直接听一次模组的串口**，回答那个
 *        "到底是没有定位数据，还是链路卡死"。
 *
 * 为什么需要它：读线程一旦被卡死/被隔离（2026-09-21 实机那个终态），
 * **就没有任何人读 ttyS0** ⇒ 日志里一条 NMEA 都不会出现 ⇒ 从日志上
 * **完全无法区分**"模组没发"和"我们没收"。这个子命令绕开读线程自己开串口读，
 * 于是能把两半分开：bytes=0 ⇒ 模组没在发；bytes>0 而 GGA 质量=0 ⇒ 在发但没定位；
 * 两者都正常而 reader 那行说卡死 ⇒ 主机链路的问题。
 *
 * **自带时限**（读满 ms 就自己退出）—— 这一条是硬要求，不是讲究：
 * 本 build **没有开 `CONFIG_TTY_SIGINT`**，Ctrl-C **杀不掉正在跑的命令**；
 * 手工 `cat /dev/ttyS0` 试过一次，NSH 直接被卡死、控制台被 NMEA 一直灌到只能复位。
 * 所以这里必须自己到点退出，绝不能依赖用户按什么。
 *
 * ⚠ 读线程活着时**不要用**：两个读者会抢同一串口的字节（读线程会丢数据，
 * 表现为 `reader gap`）。它专给"读线程已死/已停"的现场用。 */
static int gnss_listen(unsigned ms)
{
  char buf[256];
  char line[128];
  char last_gga[96];
  char last_gsa[80];
  unsigned linelen = 0;
  unsigned bytes = 0;
  unsigned sents = 0;
  unsigned long t0;
  bool reader_alive = false;
  int fd;

  ctl_gnss_tty_check();

  /* reader 侧状态先打一行：与下面的字节数一起看才构成完整结论。
   * 顺便记下"reader 是否活着" —— **它活着时 bytes=0 是正常的**（reader 正握着
   * 这个串口在读，独立读自然读不到字节）。2026-09-21 我自己踩过一次：
   * 打出 bytes=0 而判读说"模组没在发"，与同一行的 `alive=1 nmea=218` 自相矛盾。 */
  {
    char st[256];

    if (myvendor_gnss_crash_format(st, sizeof(st)) > 0)
      {
        reader_alive = (strstr(st, "alive=1") != NULL);
        printf("ctl gnss listen: reader %s\n", st);
      }
  }

  fd = open(CTL_GNSS_TTY, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      fprintf(stderr, "ctl gnss listen: open %s failed errno=%d\n",
              CTL_GNSS_TTY, errno);
      return EXIT_FAILURE;
    }

  last_gga[0] = '\0';
  last_gsa[0] = '\0';
  t0 = ctl_mono_ms();

  while (ctl_mono_ms() - t0 < (unsigned long)ms)
    {
      ssize_t n = read(fd, buf, sizeof(buf));

      if (n > 0)
        {
          int i;

          bytes += (unsigned)n;
          for (i = 0; i < (int)n; i++)
            {
              char c = buf[i];

              if (c == '\n' || c == '\r')
                {
                  if (linelen > 0 && line[0] == '$')
                    {
                      line[linelen] = '\0';
                      sents++;
                      if (strstr(line, "GGA") != NULL)
                        {
                          strncpy(last_gga, line, sizeof(last_gga) - 1u);
                          last_gga[sizeof(last_gga) - 1u] = '\0';
                        }
                      else if (strstr(line, "GSA") != NULL)
                        {
                          strncpy(last_gsa, line, sizeof(last_gsa) - 1u);
                          last_gsa[sizeof(last_gsa) - 1u] = '\0';
                        }
                    }

                  linelen = 0;
                }
              else if (linelen < sizeof(line) - 1u)
                {
                  line[linelen++] = c;
                }
            }
        }
      else
        {
          usleep(20000);   /* 20 ms 一探，别把 CPU 烧在这上面 */
        }
    }

  close(fd);

  printf("ctl gnss listen %u ms: bytes=%u nmea=%u\n", ms, bytes, sents);
  if (last_gga[0] != '\0')
    {
      printf("  %s\n", last_gga);
    }

  if (last_gsa[0] != '\0')
    {
      printf("  %s\n", last_gsa);
    }

  fputs("  判读：bytes=0 ⇒ 模组没在发（查供电/接线/波特率）\n", stdout);
  fputs("        bytes>0 而 GGA 质量=0 ⇒ 模组在发、只是还没定位\n", stdout);
  fputs("        两者都正常而 reader 那行说卡死 ⇒ 主机链路的问题\n", stdout);
  if (bytes == 0u && reader_alive)
    {
      fputs("  注意：这次 reader 是活的（alive=1）⇒ bytes=0 属正常，"
            "别据此说模组没发；要看模组就先让 reader 停下来\n", stdout);
    }
  return 0;
}

static int cmd_gnss(int argc, char *argv[])
{
  char path[96];
  uint32_t start;
  uint32_t idle;
  int ret;
  bool dump_out;
  bool on;

  if (argc >= 2 && strcmp(argv[1], "listen") == 0)
    {
      unsigned ms = 1500;

      if (argc >= 3)
        {
          ms = (unsigned)atoi(argv[2]);
          if (ms < 100u)
            {
              ms = 100u;
            }
          else if (ms > 10000u)
            {
              ms = 10000u;   /* 上限 10 s：现场诊断用，不是长录 */
            }
        }

      return gnss_listen(ms);
    }

  if (argc >= 2 && strcmp(argv[1], "auto") == 0)
    {
      if (argc < 3)
        {
          printf("ctl gnss auto %s\n",
                 myvendor_devctl_eph_auto_get() ? "on" : "off");
          return 0;
        }

      if (parse_onoff(argv[2], &on) != 0)
        {
          fprintf(stderr, "ctl gnss auto: use on|off\n");
          return EXIT_FAILURE;
        }

      (void)myvendor_devctl_eph_auto_set(on);
      if (on)
        {
          myvendor_gnss_eph_reload();
        }

      printf("ctl gnss auto %s\n", on ? "on" : "off");
      return 0;
    }

  if (argc >= 2 && (strcmp(argv[1], "bdsonly") == 0 ||
                    strcmp(argv[1], "b1c") == 0 ||
                    strcmp(argv[1], "ver") == 0))
    {
      /* 星座实验：见 myvendor_gnss_probe。真正写串口在 GNSS 线程，
       * 这里只是落请求，所以立刻返回、效果看随后的日志。 */
      if (strcmp(argv[1], "b1c") == 0)
        {
          myvendor_gnss_probe(3);
          printf("ctl gnss b1c: 试开 B1C 中（看 gnss: probe b1c key=… rc=…）\n");
          return 0;
        }

      if (strcmp(argv[1], "ver") == 0)
        {
          myvendor_gnss_probe(4);
          printf("ctl gnss ver: 读 MON-VER 中（看 gnss: mon ver sw=… hw=…）\n");
          return 0;
        }

      on = true;
      if (argc >= 3 && parse_onoff(argv[2], &on) != 0)
        {
          fprintf(stderr, "ctl gnss bdsonly: use on|off\n");
          return EXIT_FAILURE;
        }

      myvendor_gnss_probe(on ? 1 : 2);
      printf("ctl gnss bdsonly %s: 看 link extra 的 gsv gp/gl/ga/bd/gq\n",
             on ? "on" : "off");
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "still") == 0)
    {
      /* 停车切 stationary：默认关；开关在 GNSS 线程 1 s 内生效
       * （见 myvendor_gnss_dyn_still_enable 的说明）。 */
      if (argc < 3 || parse_onoff(argv[2], &on) != 0)
        {
          fprintf(stderr, "ctl gnss still: use on|off\n");
          return EXIT_FAILURE;
        }

      myvendor_gnss_dyn_still_enable(on);
      printf("ctl gnss still %s（看 gnss: still switch / motion still|resume）\n",
             on ? "on" : "off");
      return 0;
    }

  if (argc < 2 ||
      (strcmp(argv[1], "dump_out") != 0 &&
       strcmp(argv[1], "dump_in") != 0 &&
       strcmp(argv[1], "dump") != 0))
    {
      fprintf(stderr,
              "ctl gnss dump_out  — poll UBX-MGA-DBD -> mga_<utc>.ubx\n"
              "ctl gnss dump_in   — inject newest mga_<utc>.ubx\n"
              "ctl gnss auto [on|off]\n"
              "ctl gnss still [on|off]  停车切 stationary（默认关）\n");
      return EXIT_FAILURE;
    }

  dump_out = (strcmp(argv[1], "dump_in") != 0);
  if (dump_out)
    {
      ret = myvendor_gnss_dump_request();
    }
  else
    {
      ret = myvendor_gnss_inject_request();
    }

  if (ret == -ENOTSUP)
    {
      fprintf(stderr, "ctl gnss %s: GNSS not in this image\n", argv[1]);
      return EXIT_FAILURE;
    }

  if (ret == -ENODEV)
    {
      fprintf(stderr, "ctl gnss %s: GNSS thread not started\n", argv[1]);
      return EXIT_FAILURE;
    }

  if (ret == -EBUSY)
    {
      fprintf(stderr, "ctl gnss %s: already running\n", argv[1]);
      return EXIT_FAILURE;
    }

  if (ret == -ENODATA)
    {
      fprintf(stderr, "ctl gnss dump_out: need a live 2D/3D fix first\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl gnss %s: request failed %d\n", argv[1], ret);
      return EXIT_FAILURE;
    }

  printf("ctl gnss %s: started\n", argv[1]);
  fflush(stdout);
  start = 0;
  idle = 0;
  /* 60 s（原来 120 × 100 ms = 12 s，而且判据只有"!busy"）。
   *
   * 原来那句判据是错的：`myvendor_gnss_dump_busy()` 在**倒库进行中**会短暂为 false
   * ⇒ ctl 立刻打 `finished without file` 并返回，而文件 5 秒后正常出现。2026-09-27
   * 现场：t=2899.88 打了失败、t=2905.00 打出 `dumped 154 DBD messages … -> …ubx`。
   * 现在**倒库只在"连续 0.5 s 空闲且已拿到文件路径"或"整体超时"时才收场**，
   * 所以看到 `dump_out: done -> <路径>` 就是真的成了。 */
  while (start < 600)
    {
      bool busy;

      busy = dump_out ? myvendor_gnss_dump_busy() : myvendor_gnss_eph_busy();
      if (busy)
        {
          idle = 0;
        }
      else if (++idle >= 5)
        {
          if (dump_out)
            {
              if (myvendor_gnss_dump_last_path(path, sizeof(path)))
                {
                  printf("ctl gnss dump_out: done -> %s\n", path);
                  return 0;
                }

              /* 空闲但还没有文件：可能只是最后一段还没落盘，继续等（别误报）。 */
            }
          else if (myvendor_gnss_inject_last_path(path, sizeof(path)))
            {
              printf("ctl gnss dump_in: done <- %s (see syslog)\n", path);
              return 0;
            }
          else
            {
              printf("ctl gnss dump_in: finished without file (see syslog)\n");
              return 0;
            }
        }

      usleep(100000);
      start++;
    }

  fprintf(stderr, "ctl gnss %s: timeout, no file (see syslog)\n", argv[1]);
  return EXIT_FAILURE;
}

/**
 * @brief `ctl idle` 无参数打印子命令；`on` 立刻进静止（等同 10 分钟耗尽）；
 *        `hour` 耗尽 1 小时并关机；`off` 唤醒；`stay` 本次开机禁止自动静止；
 *        `auto` 恢复。
 */
static void idle_usage(void)
{
  printf("ctl idle — still-screen debug\n");
  printf("  ctl idle on     enter still now (same as 10 min timeout)\n");
  printf("  ctl idle off    wake backlight + GNSS\n");
  printf("  ctl idle hour   exhaust 1 h still then power off  (1h)\n");
  printf("  ctl idle stay   this boot: no auto still  (hold|never)\n");
  printf("  ctl idle auto   restore auto still\n");
}

static int cmd_idle(int argc, char *argv[])
{
  bool on = true;
  int ret;
  myvendor_bicycle_ctl_op_t op;
  const char *arg = NULL;
  const char *ok = NULL;

  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0)
    {
      idle_usage();
      return 0;
    }

  if (strcmp(argv[1], "hour") == 0 || strcmp(argv[1], "1h") == 0 ||
      strcmp(argv[1], "offhour") == 0)
    {
      op = MYVENDOR_BICYCLE_CTL_OP_IDLE_HOUR;
      ok = "hour";
    }
  else if (strcmp(argv[1], "stay") == 0 ||
           strcmp(argv[1], "hold") == 0 ||
           strcmp(argv[1], "never") == 0)
    {
      op = MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY;
      arg = "1";
      ok = "stay";
    }
  else if (strcmp(argv[1], "auto") == 0)
    {
      op = MYVENDOR_BICYCLE_CTL_OP_IDLE_STAY;
      arg = "0";
      ok = "auto";
    }
  else if (parse_onoff(argv[1], &on) == 0)
    {
      op = on ? MYVENDOR_BICYCLE_CTL_OP_IDLE_ENTER
              : MYVENDOR_BICYCLE_CTL_OP_IDLE_WAKE;
      ok = on ? "on" : "off";
    }
  else
    {
      idle_usage();
      fprintf(stderr, "ctl idle: unknown '%s'\n", argv[1]);
      return EXIT_FAILURE;
    }

  ret = myvendor_bicycle_ctl_post(op, arg);
  if (ret == -ENODEV)
    {
      fprintf(stderr, "ctl idle: UI not running\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl idle: post failed %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("ctl idle %s\n", ok);
  return 0;
}

/**
 * @brief 打印跳频自检状态（没在跑就不打）。
 *
 * @details
 * 跳频期间档位是走强制通道设的，`sys dvfs` 只会显示 `force`，看不出是谁设的；
 * 这里补一行周期与累计跳变次数，方便确认命令生效、估算跑了多久。
 */
static void dvfs_hop_status_line(void)
{
#ifdef CONFIG_MYVENDOR_DVFS
  unsigned period;
  unsigned count;

  if (!sf32lb_dvfs_hop_active())
    {
      return;
    }

  period = (unsigned)sf32lb_dvfs_hop_period_ms();
  count = (unsigned)sf32lb_dvfs_hop_count();
  printf("  dvfs    hop     every %u ms, %u jumps done\n", period, count);
  printf("  dvfs    hop     stop with: ctl dvfs hop off\n");

  /* 调频 worker 心跳。这两行是判"调频是不是已经悄悄死了"的唯一依据：
   * age 应该始终只有个位数毫秒，且 runs 一直在涨；两者任意一个不动，
   * 就说明 gov_worker 的重挂链断了 —— 那时 hop 的 jumps done 也会冻住。 */
  {
    uint32_t age_ms = 0;
    uint32_t runs = 0;

    sf32lb_dvfs_gov_stat(&age_ms, &runs);
    printf("  dvfs    gov     last run %u ms ago, %u runs, %u recoveries\n",
           (unsigned)age_ms, (unsigned)runs,
           (unsigned)sf32lb_dvfs_gov_recoveries());
  }
#endif
}

/**
 * @brief `ctl log [level]`：全系统 syslog 屏蔽位，省略则打印当前值。
 *
 * @details
 * 改的是 `g_syslog_mask`，并且 `nx_setlogmask()` 会**遍历所有已存在的任务**
 * 一起改（drivers/syslog/setlogmask.c 的 task_syslogmask），新任务在
 * sched/tls/task_initinfo.c 里继承这个全局值 —— 所以立刻全系统生效、
 * 不需要重编译，也能运行时随时调回来。
 *
 * **屏蔽的是 syslog 输出**（带 `[  INFO]` / `[  WARN]` 前缀的那些，闸门在
 * libs/libc/syslog/lib_syslog.c 的 `ta_syslog_mask & LOG_MASK(priority)`）。
 * `printf` 不过 syslog（NSH 提示符、test_app、ctl 自己的输出），**不受影响** ——
 * 所以这个开关能压掉驱动/内核的刷屏，但压不掉应用 printf。
 *
 * **档位落 KV**（`persist.log.mask`，落在 `myvendor_devctl_load()` 里恢复）：
 * 以前每次开机都要重敲一遍。写盘失败只警告，不回滚当前档位。
 *
 * 建议值：平时 `err`（只留错误），查问题前 `info` 或 `all`。
 */
static int cmd_log(int argc, char *argv[])
{
  /* POSIX 优先级：数值越小越严重，LOG_UPTO(p) 保留 0..p。 */
  static const char * const names[] =
  {
    "EMERG", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"
  };

  uint8_t mask;
  uint8_t old;
  int want = -1;
  int ret;
  int i;

  if (argc < 2)
    {
      mask = myvendor_devctl_log_mask_get();
      printf("syslog mask 0x%02x  (%s)\n", (unsigned)mask,
             MYVENDOR_DEVCTL_LOG_KEY);
      for (i = 0; i < 8; i++)
        {
          printf("  %-6s %s\n", names[i],
                 (mask & LOG_MASK(i)) != 0 ? "on" : "off");
        }

      return 0;
    }

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0)
    {
      printf("ctl log                 print current syslog mask\n");
      printf("ctl log err|warn|notice|info|all   keep levels up to this\n");
      printf("ctl log silent          only EMERG\n");
      printf("ctl log 0x1f            set raw mask\n");
      printf("  saved as %s, restored on boot\n", MYVENDOR_DEVCTL_LOG_KEY);
      printf("  (printf is NOT syslog; NSH/apps keep printing)\n");
      return 0;
    }

  if (strcmp(argv[1], "err") == 0 || strcmp(argv[1], "error") == 0)
    {
      want = LOG_UPTO(LOG_ERR);
    }
  else if (strcmp(argv[1], "warn") == 0 || strcmp(argv[1], "warning") == 0)
    {
      want = LOG_UPTO(LOG_WARNING);
    }
  else if (strcmp(argv[1], "notice") == 0)
    {
      want = LOG_UPTO(LOG_NOTICE);
    }
  else if (strcmp(argv[1], "info") == 0)
    {
      want = LOG_UPTO(LOG_INFO);
    }
  else if (strcmp(argv[1], "all") == 0 || strcmp(argv[1], "debug") == 0)
    {
      want = 0xff;
    }
  else if (strcmp(argv[1], "silent") == 0 || strcmp(argv[1], "none") == 0)
    {
      want = LOG_MASK(LOG_EMERG);
    }
  else
    {
      char *end = NULL;
      unsigned long raw = strtoul(argv[1], &end, 0);

      if (end == argv[1] || *end != '\0' || raw > 0xff)
        {
          fprintf(stderr, "ctl log: unknown level '%s'\n", argv[1]);
          return EXIT_FAILURE;
        }

      want = (int)raw;
    }

  mask = (uint8_t)want;
  ret = myvendor_devctl_log_mask_set(mask, &old);

  printf("syslog mask 0x%02x -> 0x%02x\n", (unsigned)old, (unsigned)mask);
  for (i = 0; i < 8; i++)
    {
      printf("  %-6s %s\n", names[i],
             (mask & LOG_MASK(i)) != 0 ? "on" : "off");
    }

  if (ret < 0)
    {
      /* 档位已经生效，只是没写进 KV：说清楚，免得下次开机发现"又回去了"。 */
      printf("  WARN  %s write failed (%d), lost after reboot\n",
             MYVENDOR_DEVCTL_LOG_KEY, ret);
    }

  return 0;
}

/**
 * @brief `ctl btlog [off|err|warn|info|debug|<0..7>]`：framework 的 BT_LOG* 档位。
 *
 * @details
 * 与 `ctl log` 是**两道独立的闸**，两个都开才有输出：
 *   - `ctl log`   = NuttX 的 syslog 屏蔽位：哪一档优先级准出控制台。
 *   - `ctl btlog` = framework 的 `g_logger.framework_level`：`log.h` 里那些
 *     `BT_LOGE/W/I/D` 宏会先问 `bt_log_print_check()`，**档不够连 syslog 都不调**。
 *
 * 为什么用 KVDB 的 property 接口，而不是像 `ctl log` 那样直接写 persist 文件：
 * `log_server.c:206-212` 是靠 `property_monitor` 收 KVDB 变更通知来**热加载**的
 * （`FRAMEWORK_LOG_LEVEL_CHANGED` → `bt_log_set_level(new, true)`）。
 * 直接写文件不走 property 层 ⇒ 不触发通知 ⇒ 改完得重启才生效。用 property 接口
 * 立刻生效。
 *
 * 编译期还有一道闸（defconfig）：`CONFIG_BLUETOOTH_LOG` +
 * `CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL`。少了后者，`log.h` 会把所有 BT_LOG*
 * 定义成空宏 —— 那时本命令改了也没用（宏根本不展开）。
 *
 * 档位数值就是 POSIX 优先级：ERROR=3 / WARNING=4 / INFO=6 / DEBUG=7，0=关。
 * 想看 adapter 状态机迁移（`adapter_state.c` 的 ADAPTER_DBG_* 是 BT_LOGD）
 * 要 `debug`；只想看失败原因（`SAL_CHECK_RET` 的 return:%d 是 BT_LOGE）用 `err`。
 */
static int cmd_btlog(int argc, char *argv[])
{
  static const char * const names[] =
  {
    "OFF", "ERROR", "WARNING", "INFO", "DEBUG"
  };

  int cur;
  int want = -1;
  int ret;

  cur = property_get_int32(MYVENDOR_CTL_BTLOG_KEY, -1);

  if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0))
    {
      printf("ctl btlog                      print framework BT log level\n");
      printf("ctl btlog off|err|warn|info|debug   set level (live, no reboot)\n");
      printf("ctl btlog <0..7>               set raw level\n");
      printf("  stored as %s (KVDB property, hot-reloaded)\n",
             MYVENDOR_CTL_BTLOG_KEY);
      printf("  needs CONFIG_BLUETOOTH_LOG + CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL\n");
      printf("  ALSO needs `ctl log` to let that priority out to the console\n");
      return 0;
    }

  if (argc < 2)
    {
      /* 属性**没设过**时（读回 -1）真正生效的是**编译期默认值**
       * `CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL` —— 只报个 "unset/other"
       * 看不出实际档位，所以这里把它显式说出来。 */
      if (cur < 0)
        {
#if defined(CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL)
          printf("framework BT log level = %d (KVDB unset; compile-time default)\n",
                 CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL);
#else
          printf("framework BT log level: KVDB unset, no compile-time default\n");
          printf("  log.h defines every BT_LOG* as a no-op => setting this does nothing\n");
          printf("  enable CONFIG_BLUETOOTH_LOG + CONFIG_BLUETOOTH_SERVICE_LOG_LEVEL first\n");
#endif
        }
      else
        {
          printf("framework BT log level %d  (%s)\n", cur,
                 cur == 3 ? "ERROR" : cur == 4 ? "WARNING" : cur == 6 ? "INFO"
                 : cur == 7 ? "DEBUG" : cur == 0 ? "OFF" : "other");
        }

      printf("  catalog gate: syslog mask 0x%02x (`ctl log`)\n",
             (unsigned)myvendor_devctl_log_mask_get());
      return 0;
    }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "none") == 0)
    {
      want = 0;
    }
  else if (strcmp(argv[1], "err") == 0 || strcmp(argv[1], "error") == 0)
    {
      want = 3;
    }
  else if (strcmp(argv[1], "warn") == 0 || strcmp(argv[1], "warning") == 0)
    {
      want = 4;
    }
  else if (strcmp(argv[1], "info") == 0)
    {
      want = 6;
    }
  else if (strcmp(argv[1], "debug") == 0 || strcmp(argv[1], "all") == 0)
    {
      want = 7;
    }
  else
    {
      char *end = NULL;
      unsigned long raw = strtoul(argv[1], &end, 0);

      if (end == argv[1] || *end != '\0' || raw > 7u)
        {
          fprintf(stderr, "ctl btlog: unknown level '%s'\n", argv[1]);
          return EXIT_FAILURE;
        }

      want = (int)raw;
    }

  ret = property_set_int32(MYVENDOR_CTL_BTLOG_KEY, want);
  if (ret < 0)
    {
      fprintf(stderr, "ctl btlog: write failed %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("framework BT log level %d -> %d\n", cur, want);
  return 0;
}

/**
 * @brief `ctl dvfs auto|144|240|hop [ms]`：强制/自动 HCLK，或跑跳频自检。
 *
 * @details
 * `hop` 每 period（默认 10 s）跳到下一个档位，用来反复走切频路径；档位次序
 * 保证相邻不重复且每轮覆盖全部现役档位（144/240）。`hop off`、`auto` 或显式指定档位
 * 都会停止自检并把控制权交还 governor。
 */
/**
 * @brief `ctl cnt` —— 周期计数器报表的开关（diag 那张模块表）。
 *
 * 计数在各模块里累加（中断侧只计数），打印统一由 diag 线程按这里选的开关
 * 每 60 s 打一遍。用法：
 *   ctl cnt                列出模块与开关（默认全开）
 *   ctl cnt <name> on|off  单个开关（改动落 KV：persist.diag.cnt_mask）
 *   ctl cnt all on|off     全开 / 全关
 *   ctl cnt now            立即打一遍，不用等周期
 *
 * ⚠ **这个开关只管"周期报表"**：真异常一律 `LOG_ERR` 直报，
 *   diag 每拍（500 ms）都做异常判定 —— 关掉某一路也**不会**把它的异常静音
 *   （这样 `ctl log err` 只剩错误级时，异常照旧看得见）。
 */
static int cmd_cnt(int argc, char *argv[])
{
  extern unsigned myvendor_diag_cnt_count(void);
  extern const char *myvendor_diag_cnt_name(unsigned idx);
  extern int myvendor_diag_cnt_find(const char *name);
  extern void myvendor_diag_cnt_dump_now(void);

  uint32_t mask = myvendor_devctl_diag_cnt_mask_get();
  unsigned i;

  if (argc >= 2 && strcmp(argv[1], "now") == 0)
    {
      myvendor_diag_cnt_dump_now();
      printf("cnt: dumped now\n");
      return 0;
    }

  if (argc >= 3)
    {
      bool on = (strcmp(argv[2], "on") == 0) || (strcmp(argv[2], "1") == 0);
      uint32_t m;
      int ret;

      if (strcmp(argv[1], "all") == 0)
        {
          m = on ? 0xffffffffu : 0u;
        }
      else
        {
          int idx = myvendor_diag_cnt_find(argv[1]);

          if (idx < 0)
            {
              printf("cnt: no such module '%s'\n", argv[1]);
              return 1;
            }

          m = on ? (mask | (1u << idx)) : (mask & ~(1u << idx));
        }

      ret = myvendor_devctl_diag_cnt_mask_set(m);
      printf("cnt: %s %s  mask=0x%08lx%s\n", argv[1], on ? "on" : "off",
             (unsigned long)m, ret < 0 ? "  [persist failed]" : "");
      return ret < 0 ? 1 : 0;
    }

  printf("cnt: mask=0x%08lx  period=60 s  kv=%s\n", (unsigned long)mask,
         MYVENDOR_DEVCTL_DIAG_CNT_KEY);
  for (i = 0; i < myvendor_diag_cnt_count(); i++)
    {
      printf("  %-10s %s\n", myvendor_diag_cnt_name(i),
             ((mask >> i) & 1u) ? "on" : "off");
    }

  printf("usage: ctl cnt [<name>|all] on|off | ctl cnt now\n");
  return 0;
}

static int cmd_dvfs(int argc, char *argv[])
{
#ifdef CONFIG_MYVENDOR_DVFS
  int ret;
  uint32_t mhz = 0;

  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0)
    {
      printf("ctl dvfs auto|144|240  (ondemand; not 48/24)\n");
      printf("ctl dvfs hop [ms]   random-ish hop every ms (default %u)\n",
             (unsigned)SF32LB_DVFS_HOP_DEFAULT_MS);
      printf("ctl dvfs hop off    stop hopping, back to ondemand\n");
      printf("ctl dvfs charge on|off   充电时也按负载调频（debug：插电=电池行为）\n");
      sf32lb_dvfs_print();
      return 0;
    }

  /* 「充电时也按负载动态调频」。**和 hop 不是一回事**：hop 走 force 路径、把
   * hold/governor/交互下限全绕过（合成扫描）；本模式只把 CHARGE 位从"钉 240"
   * 的地板掩码里摘掉，其余策略原样 ⇒ 插电跑的就是电池那套负载策略，现场能复现。
   * 落 KVDB（`persist.dvfs.charge_dyn`），复位后仍有效 —— debug 期间会反复烧录，
   * 每次重设太烦。 */
  if (strcmp(argv[1], "charge") == 0 || strcmp(argv[1], "chg") == 0)
    {
      bool want;
      int  pret;

      if (argc >= 3 &&
          (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "0") == 0 ||
           strcmp(argv[2], "no") == 0))
        {
          want = false;
        }
      else if (argc >= 3 &&
               (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0 ||
                strcmp(argv[2], "yes") == 0))
        {
          want = true;
        }
      else if (argc >= 3)
        {
          fprintf(stderr, "ctl dvfs charge: 只接受 on|off\n");
          return EXIT_FAILURE;
        }
      else
        {
          /* 不带参数 = 只查询。 */
          printf("dvfs charge-dyn = %s\n",
                 myvendor_devctl_dvfs_charge_dyn_get() ? "on" : "off");
          sf32lb_dvfs_print();
          return 0;
        }

      pret = myvendor_devctl_dvfs_charge_dyn_set(want);
      printf("dvfs charge-dyn -> %s\n", want ? "on" : "off");
      sf32lb_dvfs_print();
      if (pret < 0)
        {
          /* 运行期已经生效，只是没写进 KV：说清楚，免得下次开机发现"又回去了"。 */
          printf("  WARN  %s write failed (%d), lost after reboot\n",
                 MYVENDOR_DEVCTL_DVFS_CHARGE_DYN_KEY, pret);
        }

      return 0;
    }

  if (strcmp(argv[1], "hop") == 0 || strcmp(argv[1], "rand") == 0)
    {
      if (argc >= 3 &&
          (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "stop") == 0 ||
           strcmp(argv[2], "0") == 0))
        {
          sf32lb_dvfs_hop_stop();
          sf32lb_dvfs_print();
          return 0;
        }

      {
        uint32_t period = SF32LB_DVFS_HOP_DEFAULT_MS;

        if (argc >= 3)
          {
            char *end = NULL;
            unsigned long ms = strtoul(argv[2], &end, 10);

            if (end == argv[2] || *end != '\0')
              {
                fprintf(stderr, "ctl dvfs hop: period must be ms (>=%u)\n",
                        (unsigned)SF32LB_DVFS_HOP_MIN_MS);
                return EXIT_FAILURE;
              }

            period = (uint32_t)ms;
          }

        ret = sf32lb_dvfs_hop_start(period);
        if (ret == -EINVAL)
          {
            fprintf(stderr, "ctl dvfs hop: period %u out of [%u, %u] ms\n",
                    (unsigned)period, (unsigned)SF32LB_DVFS_HOP_MIN_MS,
                    (unsigned)SF32LB_DVFS_HOP_MAX_MS);
            return EXIT_FAILURE;
          }

        if (ret < 0)
          {
            fprintf(stderr, "ctl dvfs hop: start failed %d\n", ret);
            return EXIT_FAILURE;
          }

        sf32lb_dvfs_print();
        dvfs_hop_status_line();
        return 0;
      }
    }

  if (strcmp(argv[1], "auto") == 0 || strcmp(argv[1], "off") == 0)
    {
      mhz = 0;
    }
  else if (strcmp(argv[1], "low") == 0)
    {
      mhz = SF32LB_DVFS_MHZ_LOW;
    }
  else if (strcmp(argv[1], "high") == 0)
    {
      mhz = SF32LB_DVFS_MHZ_240;
    }
  else
    {
      char *end = NULL;
      unsigned long v = strtoul(argv[1], &end, 10);

      if (end == argv[1] || *end != '\0' || v > 240ul)
        {
          fprintf(stderr, "ctl dvfs: need auto|72|96|144|240|hop\n");
          return EXIT_FAILURE;
        }

      mhz = (uint32_t)v;
    }

  /* 走到这里说明是 auto 或显式档位：先停掉跳频自检，否则下一次跳变会把
   * 用户刚设的档位覆盖掉。hop 分支在上面已经 return 了。 */
  sf32lb_dvfs_hop_stop();

  ret = sf32lb_dvfs_force(mhz);
  if (ret == -EINVAL)
    {
      fprintf(stderr, "ctl dvfs: gear must be 72, 96, 144 or 240\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "ctl dvfs: apply failed %d\n", ret);
      return EXIT_FAILURE;
    }

  sf32lb_dvfs_print();
  return 0;
#else
  (void)argc;
  (void)argv;
  fprintf(stderr, "ctl dvfs: CONFIG_MYVENDOR_DVFS is off\n");
  return EXIT_FAILURE;
#endif
}

/**
 * @brief 裸块读写自检：不进 LFS、绕过判死闸门。
 *
 * @details 存在的理由：卡一旦判死，**LFS 层就不下发 I/O 了**，常规读写全部
 *          立刻返回 -EIO，于是没法再判断"卡到底还响不响应"。这条命令刻意
 *          绕开闸门，是判死之后唯一还能直接验卡的手段。逐块报时间也是必要的：
 *          坏页/磨损页的典型特征是**某一块明显慢或直接失败**，只看总结果分辨不出来。
 *
 * @note 它同时证伪过一个假设，别再捡回来：曾经怀疑"首次失败总是落在 LFS 块 194
 *       （偏移 0xC2000）"，用 `ctl sd read 200c2000 8` 实测 8 个块全部正常
 *       （约 880 µs），而另一次记录的首次失败偏移是 0x20000000（块 0）。
 *       "卡在某个坏块上"这个方向因此被排除，真正的原因是控制器卡在 CMD_BUSY
 *       （见 docs/sd_recovery.md）。本命令保留为通用的验卡工具，不再代表那个假设。
 */
static int cmd_sd(int argc, char **argv)
{
  uint64_t off;
  uint32_t n;

  if (argc >= 3 && strcmp(argv[1], "read") == 0)
    {
      off = strtoull(argv[2], NULL, 16);
      n = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 1u;
      if (n == 0u || n > 256u)
        {
          n = 1u;
        }

      return sf32lb_sd_raw_read(off, n);
    }

  if (argc >= 3 && strcmp(argv[1], "write") == 0)
    {
      off = strtoull(argv[2], NULL, 16);
      n = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 1u;
      if (n == 0u || n > 256u)
        {
          n = 1u;
        }

      /* 写目标只有驱动器尾部那 1 MiB 是安全的（唯一不属于任何文件系统的
       * 区域）；驱动内部还有一道不可绕过的闸门，这里只是提前给用法。 */
      return sf32lb_sd_raw_write(off, n);
    }

  printf("ctl sd read  <hex_off> [n]    raw 512B block read, per-block timing\n");
  printf("ctl sd write <hex_off> [n]    0xFF program + readback verify\n");
  printf("  both bypass the dead gate on purpose, so they work on a dead card\n");
  printf("  e.g. ctl sd read  200c2000 8        (LFS block 194)\n");
  printf("       ctl sd write <card_size-4096> 32   (sacrificial tail ONLY)\n");
  return -EINVAL;
}

/**
 * @brief DWT 数据观察点：给"谁改坏了这块内存"点名。
 *
 * @details 为什么需要它：n004 的 GATT 回调表、2026-09-18 的 wdog 活动链表都是
 *          被外力改坏的，日志只留下"值变成什么"，没有"谁写的"。挂一个观察点
 *          之后：命中由硬件在 `DWT_FUNCTIONn.MATCHED` 上留痕（事后 `ctl wt`
 *          直接读得到），并且本板无调试器时会交付成异常 → 既有 coredump，
 *          `pc` 就是那条访问指令，按 n004 的老办法 addr2line 反解。
 *
 * @note DWT 接在核的 load/store 通路上，**DMA 写不会命中**："值坏了但
 *       MATCHED=0" 本身就是结论（不是 CPU 写的）。观察点复位即失效，不持久。
 */
/**
 * @brief DWT 观察点只接受 HCPU 自己的两块 RAM。
 *
 * @details 两道理由，缺一不可：
 *          1. 安全：地址来自命令行，不能让任意数值直接变成被监视的内存；
 *             白名单之外一律拒绝（与 wdog 覆写里的 wd_node_sane 同一套边界）。
 *          2. 工程上：观察外设/保留区没有意义，DWT 比较器只有 4 个。
 */
static bool wt_addr_ok(uint32_t addr)
{
  if (addr >= 0x20000000u && addr < 0x20080000u)   /* HCPU SRAM */
    {
      return true;
    }

  if (addr >= 0x60000000u && addr < 0x68000000u)   /* PSRAM / heap */
    {
      return true;
    }

  return false;
}

static int cmd_watch(int argc, char **argv)
{
  bool acted = false;

  if (argc >= 2 && strcmp(argv[1], "clear") == 0)
    {
      myvendor_dwt_watch_clear();
      printf("wt: cleared\n");
      acted = true;
    }
  else if (argc >= 2 && strcmp(argv[1], "test") == 0)
    {
      /* 自我触发：挂在一个我们自己写的变量上，然后写它。2026-09-18 实测：
       * 本板 **不会**把它交付成异常（这一行后面照常打印），所以能靠的只有
       * MATCHED 位 —— 它同样证明了"比较器抓到了这次写"。 */
      static volatile uint32_t probe __attribute__((aligned(4)));
      int ret = myvendor_dwt_watch_arm((uint32_t)(uintptr_t)&probe, 4);

      if (ret != 0)
        {
          printf("wt test: arm failed %d\n", ret);
          return ret;
        }

      printf("wt test: armed on probe(%08lx), writing now\n",
             (unsigned long)(uintptr_t)&probe);
      probe = 0x12345678u;
      printf("wt test: write returned (no fault on this part), check MATCHED\n");
      acted = true;
    }
  else if (argc >= 2 && argv[1][0] != '\0')
    {
      /* 裸十六进制地址（不带 0x 也接受，和 ctl sd 一致）。**白名单是真正的
       * 安全边界**：地址来自命令行，只允许 HCPU SRAM / PSRAM。 */
      char *end = NULL;
      unsigned long parsed = strtoul(argv[1], &end, 16);
      unsigned size = (argc >= 3) ? (unsigned)strtoul(argv[2], NULL, 10) : 4u;
      uint32_t addr = (uint32_t)parsed;
      int ret;

      if (end == NULL || *end != '\0' || parsed > 0xfffffffful)
        {
          printf("wt: '%s' is not a hex address\n", argv[1]);
          return -EINVAL;
        }

      if (!wt_addr_ok(addr))
        {
          printf("wt: only HCPU SRAM 0x20000000-0x2007ffff or PSRAM 0x60000000-0x67ffffff\n");
          return -EINVAL;
        }

      ret = myvendor_dwt_watch_arm(addr, size);
      if (ret != 0)
        {
          printf("wt: arm %08lx size=%u failed %d (comparators full?)\n",
                 (unsigned long)addr, size, ret);
          return ret;
        }

      printf("wt: armed %08lx size=%u\n", (unsigned long)addr, size);
      acted = true;
    }

  {
    char buf[256];

    myvendor_dwt_watch_status(buf, sizeof(buf));
    printf("%s", buf);
  }

  if (!acted || argc < 2)
    {
      printf("ctl wt <hexaddr> [1|2|4]   arm a DWT data watchpoint (RAM only)\n");
      printf("ctl wt clear               remove all watchpoints\n");
      printf("ctl wt test                self-test (may fault/reset on purpose)\n");
      printf("ctl wt                     show armed watchpoints + MATCHED bits\n");
      printf("  ON THIS BOARD a hit sets MATCHED but is NOT delivered as a fault,\n");
      printf("  so MATCHED answers \"did the CPU touch this word\" (DMA never hits);\n");
      printf("  naming the writer's pc needs a debugger attached\n");
    }

  return 0;
}

/* 每次 ctl 退出前报一次**自己的栈水位**。
 *
 * 前提 CONFIG_STACK_COLORATION=y（本板是 y）：内核建任务时把整块栈刷成
 * STACK_COLOR，`up_check_tcbstack()` 从栈底往上数到第一个被写过的字，
 * 就是**历史最高水位**（不是"当前 sp"）。
 *
 * 为什么加这一行：2026-09-19 02:43~03:00 那 12 份 ctl 快照（占崩溃普查 60%）
 * 是"栈写到底"（sp=0x60400078 距 PSRAM 堆底只剩 120 B、pc=lr=0），
 * 而 **-fstack-usage 全量量过：ctl 自己的最大栈帧只有 288 B，整条调用链里
 * 没有任何 ≥512 B 的局部缓冲区** ⇒ 吃栈的是**调用深度**，4096 对这个任务
 * 就是不够（兄弟命令 sys/ble_companion 都是 8192）。栈已提到 8192，
 * **这一行把"到底用了多少"从猜变成数字**：再跑一遍当时崩过的命令，
 * 看百分比就知道 8192 够不够；≥75% 会多打一句提醒。 */
static void ctl_stack_report(void)
{
#ifdef CONFIG_STACK_COLORATION
  FAR struct tcb_s *tcb = this_task();
  size_t size;
  size_t used;
  unsigned pct;

  if (tcb == NULL || tcb->adj_stack_size == 0 || tcb->stack_base_ptr == NULL)
    {
      return;
    }

  size = (size_t)tcb->adj_stack_size;
  used = up_check_tcbstack(tcb, size);
  pct  = (unsigned)((used * 100u) / size);

  printf("ctl: stack used %u/%u B (%u%%)%s\n",
         (unsigned)used, (unsigned)size, pct,
         pct >= 75u ? "  ==== 水位偏高：这条命令离栈底很近了" : " ok");
#else
  /* 没开栈着色就不报 —— 不猜数字。 */
#endif
}

/**
 * @brief NSH 入口。
 */
static int ctl_run(int argc, FAR char *argv[]);

int main(int argc, FAR char *argv[])
{
  int ret = ctl_run(argc, argv);

  ctl_stack_report();
  return ret;
}

static int ctl_run(int argc, FAR char *argv[])
{
  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "help") == 0)
    {
      usage();
      return 0;
    }

  if (strcmp(argv[1], "bl") == 0 || strcmp(argv[1], "backlight") == 0)
    {
      return cmd_bl(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "radio") == 0 || strcmp(argv[1], "bt") == 0)
    {
      return cmd_switch("radio", argc - 1, argv + 1,
                        myvendor_devctl_radio_set);
    }

  if (strcmp(argv[1], "sensor") == 0)
    {
      /* `ctl sensor bind <kind 0/1/2> <mac> [addr_type]`：按 MAC 重绑一台传感器。
       * 存在的意义：记录删掉之后设备就不再回连它，而重绑要进菜单按好几下键 ——
       * 远端（串口/monitor）要能把这条恢复回去。走的是 UI 点的同一条投递路径
       * （`myvendor_devctl_sensor_connect_addr` → companion 线程执行），
       * NSH 线程不直接碰协议栈。 */
      if (argc >= 3 && strcmp(argv[2], "bind") == 0)
        {
          return cmd_sensor_bind(argc - 2, argv + 2);
        }

      return cmd_switch("sensor", argc - 1, argv + 1,
                        myvendor_devctl_sensor_set);
    }

  if (strcmp(argv[1], "sd") == 0)
    {
      return cmd_sd(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "wt") == 0 || strcmp(argv[1], "watch") == 0)
    {
      return cmd_watch(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "mtp") == 0)
    {
      return cmd_switch("mtp", argc - 1, argv + 1,
                        myvendor_devctl_mtp_set);
    }

  if (strcmp(argv[1], "notif") == 0)
    {
      return cmd_switch("notif", argc - 1, argv + 1,
                        myvendor_devctl_notif_set);
    }

  if (strcmp(argv[1], "calls") == 0)
    {
      return cmd_switch("calls", argc - 1, argv + 1,
                        myvendor_devctl_notif_calls_only_set);
    }

  if (strcmp(argv[1], "sound") == 0)
    {
      return cmd_switch("sound", argc - 1, argv + 1,
                        myvendor_devctl_sound_set);
    }

  if (strcmp(argv[1], "autopause") == 0)
    {
      return cmd_switch("autopause", argc - 1, argv + 1,
                        myvendor_devctl_autopause_set);
    }

  if (strcmp(argv[1], "grade") == 0)
    {
      return cmd_grade(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "tz") == 0)
    {
      return cmd_tz(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "fav") == 0 ||
      strcmp(argv[1], "favorite") == 0)
    {
      return cmd_fav(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "fs") == 0 || strcmp(argv[1], "file") == 0)
    {
      return cmd_fs(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "gnss") == 0)
    {
      return cmd_gnss(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "pair") == 0)
    {
      return myvendor_bt_pair_cmd(argc - 2, argv + 2);
    }

  if (strcmp(argv[1], "nvds") == 0)
    {
      return sf32lb52_bt_nvds_dump();
    }

  if (strcmp(argv[1], "nav") == 0)
    {
      return myvendor_nav_test(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "idle") == 0 || strcmp(argv[1], "still") == 0)
    {
      return cmd_idle(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "dvfs") == 0 || strcmp(argv[1], "hclk") == 0)
    {
      return cmd_dvfs(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "cnt") == 0 || strcmp(argv[1], "counters") == 0)
    {
      return cmd_cnt(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "log") == 0 || strcmp(argv[1], "loglevel") == 0)
    {
      return cmd_log(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "btlog") == 0)
    {
      return cmd_btlog(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "pwr") == 0 || strcmp(argv[1], "power") == 0)
    {
      return cmd_pwr(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "boot") == 0)
    {
      return cmd_boot(argc - 1, argv + 1);
    }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "poweroff") == 0)
    {
      myvendor_devctl_poweroff();
      return EXIT_FAILURE;
    }

  fprintf(stderr, "ctl: unknown '%s'\n", argv[1]);
  usage();
  return EXIT_FAILURE;
}
