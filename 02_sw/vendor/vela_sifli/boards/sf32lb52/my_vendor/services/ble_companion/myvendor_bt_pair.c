/**
 * @file myvendor_bt_pair.c
 * @brief BLE 配对（bonding）测试入口：`ctl pair …`
 *
 * 目的：把"开放配对 / 同意配对 / 主动发起配对 / 查看绑定"做成可以在串口上手动操作的
 * 命令，用来验证 IRK/绑定是否真的落在控制器 NVDS 里（配合 `ctl nvds` dump 对比）。
 *
 * 命令：
 *   ctl pair list              列出当前 LE 连接（idx/地址/状态/安全等级）+ 是否已配对
 *   ctl pair connect <idx>     对第 idx 条连接发起配对（bt_conn_set_security L2）
 *   ctl pair accept on|off     是否自动同意配对请求（注册/注销 auth 回调）
 *
 * 说明：zblue 在**没有注册 auth 回调**时走 "Just Works" 且自动接受；注册了回调
 * 就由我们的 `pairing_confirm` 决定同意与否 —— 所以 `accept off` 会让对端看到
 * "配对被拒绝"，方便你测"同意/拒绝"两条路。
 *
 * **边界（用户 2026-09-20 明确）**：IRK / 绑定**只服务手机**这一条线。传感器
 * （HR/CSC/CPS）一律**按地址连**（`ble_sensor` 那条线不认 IRK，也不该认）：它们
 * 不是我们配对的设备，没有密钥、没有身份地址解析可用。所以本文件里所有"加密/
 * 绑定/记录"的判断都要用 `ble_sensor_owns_addr()` 把传感器链路排除掉。
 *
 * **并发**：本模块的回调分布在三个上下文 —— 协议栈线程（`bt_conn_cb.*`）、
 * companion 线程（每拍 poll）、NSH/UI（`ctl pair …`）。共享的暂存一律上锁
 * （见 `g_secure_lock`），并且**不要在非协议栈线程里遍历连接链表**。
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>   /* BT_HCI_ERR_AUTH_FAIL */
#include <stdlib.h>
#include <unistd.h>

#include "bluetooth.h"                    /* bt_address_t / BT_TRANSPORT_* */
#include "bt_addr.h"                      /* BT_ADDR_STR_LENGTH / bt_addr_ba2str */

#include "myvendor_mono.h"                /* mono_ms()：配对窗口 */
#include "companion_bridge.h"             /* 投命令给 companion 线程 */

/** @brief 是否自动同意配对请求（`ctl pair accept on|off`；默认开）。
 *
 * 本设备是 NoInputNoOutput，Just Works 之外没有可交互的选项，所以默认直接同意；
 * 关掉它用来复现"对端看到配对被拒绝"这条路。 */
static bool g_auto_accept = true;

/** @brief auth 回调是否已注册（注销后 zblue 回到"无回调 = 自动接受"的行为）。 */
static bool g_auth_registered;

/* 前向声明：pair_confirm 在窗口判定函数定义之前就要用它。 */
bool myvendor_pair_window_active(void);
static bool myvendor_pair_reject_new_when_full(struct bt_conn *conn);

/**
 * @brief SMP 配对确认回调：打印对端与决定，并同意（Just Works）。
 *
 * @param conn    LE 连接；仅回调期间有效，本函数不持有引用。
 * @param passkey 本设备不显示也不输入 passkey，仅用于日志。
 */
static void pair_confirm(struct bt_conn *conn, unsigned int passkey)
{
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  /* **窗口外拒绝对新配对**：已绑定手机回连只走 LTK 加密，不会进 pairing_confirm；
   * 能走到这里的是"新配对请求"。以前窗口外也 auto-accept + 30 s 宽限，结果
   * 随便一台手机扫到就能弹配对（用户 2026-09-21：开了 IRK 仍能在非配对时段
   * 被扫到并请求配对）。 */
  if (!myvendor_pair_window_active())
    {
      printf("pair: confirm %s passkey=%u -> reject (no window)\n",
             addr, passkey);
      (void)bt_conn_auth_cancel(conn);
      return;
    }

  /* 已满 3 台且是新地址：拒绝对新配对（先解绑再配）。已在名单里允许刷新。 */
  if (myvendor_pair_reject_new_when_full(conn))
    {
      printf("pair: confirm %s passkey=%u -> reject (slots full)\n",
             addr, passkey);
      (void)bt_conn_auth_cancel(conn);
      return;
    }

  if (!g_auto_accept)
    {
      printf("pair: confirm %s passkey=%u -> reject (accept off)\n",
             addr, passkey);
      (void)bt_conn_auth_cancel(conn);
      return;
    }

  printf("pair: confirm %s passkey=%u -> accept\n", addr, passkey);
  bt_conn_auth_pairing_confirm(conn);
}

/**
 * @brief SMP 配对取消回调（对端取消 / 流程超时）：只留一行日志。
 *
 * @param conn LE 连接；仅回调期间有效。
 */
static void pair_cancel(struct bt_conn *conn)
{
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  printf("pair: cancelled %s\n", addr);
}

__attribute__((unused)) static void pair_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
  char addr[BT_ADDR_LE_STR_LEN];

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  printf("pair: passkey %s -> %06u\n", addr, passkey);
}

static const struct bt_conn_auth_cb g_auth_cb = {
  /* **不要**注册 passkey_display：zblue 用"提供了哪些回调"来决定 IO 能力，
   * 一旦有 passkey_display 就声明成 Display Only，协商结果从 Just Works 变成 PIN/
   * passkey 流程 —— 我们既不能显示也不能输入 → 手机报"PIN 异常"（2026-09-20 现场）。
   * 去掉它（也不加 passkey_entry）即回落 NoInputNoOutput(Just Works)，配合下面的
   * pairing_confirm 自动同意即可。 */
  .cancel = pair_cancel,
  .pairing_confirm = pair_confirm,
};

/**
 * @brief 归一化地址文本：去掉 `:`/空格/逗号并把十六进制转成大写。
 *
 * @param in      原始文本（`AA:BB:…` / `aabbcc…` / 一整行 TSV 都行）。
 * @param out     输出缓冲，保证 NUL 结尾。
 * @param out_len 输出缓冲长度（不足则截断）。
 */
static void pair_norm_addr(const char *in, char *out, size_t out_len)
{
  size_t n = 0;

  while (*in != '\0' && n + 1u < out_len)
    {
      const char c = *in++;

      if (c == ':' || c == ' ' || c == '\t' || c == ',')
        {
          continue;
        }
      out[n++] = (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
    }
  out[n] = '\0';
}

/**
 * @brief 判断一条连接是不是传感器（HR/CSC/CPS），给 `ctl pair list` 的角色列用。
 *
 * 拿连接地址去 `/mnt/kv/bicycle_sensors.tsv`（HR/CSC/CPS 的绑定存档）里找：命中就把
 * 该行首个字段当角色名返回（`hr`/`csc`/`cps`），找不到返回 `"?"`。
 *
 * 匹配放宽两档：TSV 里可能存无冒号形式；对端用 RPA 时连接地址与存档不同，所以还比对
 * "地址后 6 位十六进制"（RPA 前两字节是随机高位）。
 *
 * @param addr_str 连接地址文本（任意分隔形式）。
 * @return 静态缓冲里的角色名；`"?"` = 不是已知传感器 / 读不到存档。
 *
 * @note 返回值指向函数内 static 缓冲，只能当下就用（printf 参数之类）。
 */
static const char *pair_role_hint(const char *addr_str)
{
  static char role[12];
  char want[32];
  FILE *f;
  char line[160];

  role[0] = '?';
  role[1] = '\0';

  pair_norm_addr(addr_str, want, sizeof(want));
  if (strlen(want) < 6u)
    {
      return role;
    }

  f = fopen("/mnt/kv/bicycle_sensors.tsv", "r");
  if (f == NULL)
    {
      return role;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      char have[160];

      pair_norm_addr(line, have, sizeof(have));
      if (strstr(have, want) == NULL && strstr(have, want + strlen(want) - 6u) == NULL)
        {
          continue;
        }

      {
        unsigned n = 0;

        while (line[n] != '\0' && line[n] != '\t' && line[n] != ' '
               && line[n] != ',' && n < sizeof(role) - 1u)
          {
            role[n] = line[n];
            n++;
          }
        role[n] = '\0';
      }
      break;
    }

  fclose(f);
  return role;
}

/** @brief `ctl pair list` 的遍历游标：当前下标 / 要挑的下标 / 已见条数。 */
static struct bt_conn *g_pick;
static int g_want_idx;
static int g_seen;

/**
 * @brief `bt_conn_foreach` 回调：打印一条 LE 连接，并在下标命中时引用取出。
 *
 * 全 `FF:FF:FF:FF:FF:FF` 的条目是"连接刚建立、地址还没解析出来"的中间态，标
 * `invalid(no addr)` 并跳过 —— 按 idx 挑它会挑到空气（现场踩过两次）。
 *
 * @param conn bt_conn_foreach 已经引用过的连接，本函数不额外持有。
 * @param data 未用。
 */
static void pair_conn_cb(struct bt_conn *conn, void *data)
{
  char addr[BT_ADDR_LE_STR_LEN];
  const bt_security_t sec = bt_conn_get_security(conn);
  const int idx = g_seen++;

  (void)data;
  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  if (strncmp(addr, "FF:FF:FF:FF:FF:FF", 17) == 0)
    {
      /* 全 FF 的 public 地址不是真对端（连接对象还没解析出地址 / 残留对象），
       * 标出来免得被当成"某个设备"去配对。 */
      printf("pair: [%d] %s invalid(no addr) -> skip\n", idx, addr);
      return;
    }

  printf("pair: [%d] %s role=%s sec=%u bonded=%d\n", idx, addr,
         pair_role_hint(addr), (unsigned)sec, sec >= BT_SECURITY_L2 ? 1 : 0);

  if (idx == g_want_idx)
    {
      g_pick = bt_conn_ref(conn);
    }
}

/**
 * @brief `ctl pair list`：打印当前 LE 连接（下标 / 地址 / 角色 / 安全等级 / 是否绑定）。
 *
 * @return 恒为 0（命令入口，失败也只用日志表达）。
 */
static int pair_cmd_list(void)
{
  g_seen = 0;
  g_pick = NULL;
  g_want_idx = -1;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_conn_cb, NULL);
  if (g_seen == 0)
    {
      printf("pair: no LE connection\n");
    }
  printf("pair: auto_accept=%d auth_cb=%d conns=%d\n", g_auto_accept ? 1 : 0,
         g_auth_registered ? 1 : 0, g_seen);
  return 0;
}

/* ── 挑一条"可用"的连接（给 `ctl pair connect`）──────────────────────────────
 * 不按裸 idx：链路重建期间那条会显示成 `FF:FF:FF:FF:FF:FF (public) invalid(no addr)`，
 * 按 idx 会挑空（现场两次都这样）。挑法：
 *   `ctl pair connect`              无参数 = auto：挑"不是传感器"的那条（= 手机）
 *   `ctl pair connect <addr 前缀>`   按地址前缀挑（如 `58:81`）
 */
static struct bt_conn *g_pick_out;

/** @brief 挑连接时想匹配的地址前缀（auto 时为 NULL）。 */
static const char    *g_pick_want;
/** @brief 上述前缀的长度。 */
static unsigned       g_pick_wlen;
/** @brief true = auto 模式（挑非传感器的第一条）。 */
static bool           g_pick_auto;

/**
 * @brief `bt_conn_foreach` 回调：按当前模式挑一条连接并引用取出（只挑第一条）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_pick_cb(struct bt_conn *conn, void *data)
{
  char addr[BT_ADDR_LE_STR_LEN];
  const char *role;

  (void)data;
  if (g_pick_out != NULL) {
    return;                                        /* 已经挑到了 */
  }

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  if (strncmp(addr, "FF:FF:FF", 8) == 0) {
    return;                                        /* 地址未填：跳过 */
  }
  role = pair_role_hint(addr);

  if (!g_pick_auto) {
    if (g_pick_wlen > 0 && strncasecmp(addr, g_pick_want, g_pick_wlen) == 0) {
      g_pick_out = bt_conn_ref(conn);
    }
    return;
  }

  if (role[0] != '?') {
    return;                                        /* auto：传感器不挑 */
  }
  g_pick_out = bt_conn_ref(conn);
}

/**
 * @brief 遍历连接表挑一条可用的（调用方负责 `bt_conn_unref` 归还）。
 *
 * @param want 地址前缀（大小写不敏感）；NULL 或空串 = auto：挑非传感器的那条。
 * @param wlen want 的长度（want 为 NULL 时忽略）。
 * @return 已引用的连接，没挑到返回 NULL。
 */
static struct bt_conn *pair_pick(const char * want, unsigned wlen)
{
  g_pick_out   = NULL;
  g_pick_want  = want;
  g_pick_wlen  = wlen;
  g_pick_auto  = (want == NULL || want[0] == '\0');
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_pick_cb, NULL);
  return g_pick_out;
}

/**
 * @brief `ctl pair connect <addr 前缀>`：挑连接并 `bt_conn_set_security(L2)` 发起配对。
 *
 * 链路刚重建时地址可能还没填，所以最多重试 5 次（每次隔 1 s）。
 *
 * @param arg 地址前缀；NULL = auto（挑非传感器那条）。
 * @return 0 = 已发起配对；1 = 没挑到可用连接 / 发起失败。
 */
static int pair_cmd_connect_arg(const char * arg)
{
  int attempt;
  int ret = -1;

  for (attempt = 0; attempt < 5; attempt++)
    {
      struct bt_conn *pick = pair_pick(arg, arg == NULL ? 0u : (unsigned)strlen(arg));

      if (pick == NULL)
        {
          printf("pair: no usable connection yet (attempt %d)\n", attempt + 1);
          sleep(1);
          continue;
        }

      ret = bt_conn_set_security(pick, BT_SECURITY_L2);
      printf("pair: set_security(L2) ret=%d (attempt %d)\n", ret, attempt + 1);
      bt_conn_unref(pick);
      if (ret == 0)
        {
          return 0;
        }
      sleep(1);
    }

  return 1;
}

/**
 * @brief `ctl pair connect <idx>`：按 `ctl pair list` 的下标挑连接发起配对。
 *
 * @param idx `ctl pair list` 打印的下标。
 * @return 0 = 已发起；1 = 该下标没有连接 / 发起失败。
 */
static int pair_cmd_connect(int idx)
{
  int ret;

  g_seen = 0;
  g_pick = NULL;
  g_want_idx = idx;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_conn_cb, NULL);
  if (g_pick == NULL)
    {
      printf("pair: no connection at idx %d\n", idx);
      return 1;
    }

  ret = bt_conn_set_security(g_pick, BT_SECURITY_L2);
  printf("pair: set_security(L2) ret=%d\n", ret);
  bt_conn_unref(g_pick);
  g_pick = NULL;
  return ret == 0 ? 0 : 1;
}

static int pair_cmd_accept(bool on);
void myvendor_pair_note_bond(const bt_address_t *addr, bool bonded);
static void pair_hex_to_bytes(const char *hex, uint8_t *out, unsigned n);
static void pair_reject_cooldown_clear(const char *hex12);

/* ── 准入 + 手机身份记录（2026-09-20，用户拍板"没配对就不该接受连接"）──────────
 *
 * 改动前：广播是无定向可连接、特征权限全是裸的 `GATT_PERM_READ/WRITE` —— 任何知道
 * 服务 UUID 的中心都能连上并**写**控制/导航/FS 通道（现场实测：手机上 app 已关、
 * 配对已删，仍有一条 `sec=1 bonded=0` 的连接在读写）。现在两道：
 *
 *   ① 连接准入：窗口内放行；窗口外仅身份地址匹配的已绑定手机。有记录但地址对不上
 *      （RPA 回连 **或** 旧手机 A 占坑）给 **3 s** 宽限等 L2；未加密则踢并 **60 s
 *      同地址冷却**（防 A 秒连饿死 B）。**没记录立刻拒**。新配对由 `pair_confirm`
 *      + `bondable_le` 在窗口外拒绝。定向 ADV 不可用时靠此主机侧兜底。
 *   ② 广播三档（决策在 ble_companion.c 的 `companion_start_advertising()`）：
 *      配对窗口内 = 快广播 + 本地名 + bondable；有手机记录窗口外 = **同样快广播**、
 *      无本地名、非 bondable（2026-09-21 去掉 idle 1280：与 sensor 分时叠加后
 *      App 回连极慢）；**没记录 + 窗口外 = 完全不广播**。
 *      **定向 ADV 关闭（2026-09-22 实测）**：`ADV_DIRECT_IND` + `DIR_ADDR_RPA`
 *      在 SF32 上稳定 `adv start failed status=2`（框架 1 s 超时），已从
 *      `companion_start_advertising()` 拿掉；勿再启用。见该函数注释与
 *      `myvendor_pair_directed_peer()`。
 *
 * **IRK / 绑定只服务手机这条线**（用户 2026-09-20 明确）：传感器（HR/CSC/CPS）一律
 * **按地址连** —— 它们不是我们配对的设备，没有密钥、也没有身份地址可解析。所以本
 * 文件里所有"加密 / 绑定 / 记录"的判断都要用 `ble_sensor_owns_addr()` 把传感器链路
 * 排除掉，别把 IRK 那套套到外设上。
 *
 * 记录文件 `/mnt/kv/bt_phone.tsv`（最多 `PAIR_PHONE_MAX` 行，每行 `ADDR<TAB>TYPE`）：
 *   · 谁写：`myvendor_pair_note_bond()` / `myvendor_pair_peer_poll()`（**追加**，不覆盖别的手机）；
 *   · 谁删：解绑（按槽或全部 `bt_unpair` + 改 tsv）；
 *   · 手机侧"忽略设备"**不会**通知设备；换绑靠配对窗口再加一台（满 3 台需先解绑）。
 * 产品（2026-09-22）：IRK 名额 3，允许多台已绑定手机回连，避免"只留 B、A 抢坑饿死"。
 */
#define PAIR_PHONE_TSV     "/mnt/kv/bt_phone.tsv"
#ifndef CONFIG_BT_MAX_PAIRED
#  define PAIR_PHONE_MAX   3u
#else
#  define PAIR_PHONE_MAX   ((unsigned)CONFIG_BT_MAX_PAIRED)
#endif
/** `ctl pair open` / BLE 控制帧不带秒数时的窗口长度。
 *  **开机不再自动开窗**（用户 2026-09-20）：只有 UI / NSH / app 显式操作才开。 */
#define PAIR_WINDOW_DEFAULT_S 60u
/* RPA 回连宽限：有手机记录但连接地址对不上身份时，放行链路等 LTK 升到 L2。
 * 宽限内特征全是 ENCRYPT ⇒ 拿不到数据。到点未加密就踢。
 *
 * **不要再拉回 30 s**：旧手机 A 地址也对不上记录时会吃同一段 grace；一放行就停 ADV。
 * 3 s + 拒连冷却打断秒连环。多手机记录（最多 3）时，名单内身份直接放行。 */
#define PAIR_ADMIT_GRACE_MS       3000u
/** 拒连后同地址冷却：打断旧手机秒连占坑。 */
#define PAIR_REJECT_COOLDOWN_MS   60000u
#define PAIR_REJECT_COOLDOWN_SLOTS 2u

/** @brief 配对窗口的截止时刻（mono_ms 基准）；0 = 没开窗。受 `g_window_lock` 保护。 */
static uint32_t g_pair_window_until_ms;
/** @brief "窗口档位变了，请重播广播"的请求；由 companion 线程取走。受 `g_window_lock` 保护。 */
static bool     g_window_restart_req;

/* 窗口状态是**跨线程**的：UI（LVGL 线程）读倒计时、ctl/NSH 开窗、companion 线程
 * 取重启请求、sal/adv 路径读"在不在窗口内"。全部走这把锁（用户 2026-09-20 明确：
 * 不许无保护地跨线程动共享资源）。*/
static pthread_mutex_t g_window_lock = PTHREAD_MUTEX_INITIALIZER;


/** @brief 取走"开窗后要重播广播"的请求（**只能由 companion 线程调**）。
 *
 * `ctl pair open` 跑在 NSH/ctl 任务里，而 `bt_le_stop_advertising()` 之类的框架调用
 * 只能从蓝牙自己的线程走 —— 2026-09-20 实测：在 ctl 任务里直接调会撞
 * `libuv/src/unix/thread.c:358` 断言（板子当场 panic）。所以这里只置标志，
 * 由 companion 主循环取走后再做（和 `ctl radio` 走 bridge take 的既有做法一致）。 */
bool myvendor_pair_window_take_restart(void)
{
  bool r;

  pthread_mutex_lock(&g_window_lock);
  /* 自然到期也要重播广播 / 关 bondable：以前只看 take 标志，倒计时到 0 后
   * ADV 仍停在窗口内的快广播档，bondable 也还开着。 */
  if (g_pair_window_until_ms != 0u &&
      (int32_t)(g_pair_window_until_ms - myvendor_mono_ms()) <= 0)
    {
      g_pair_window_until_ms = 0u;
      g_window_restart_req   = true;
      printf("pair: window expired\n");
    }
  r = g_window_restart_req;
  g_window_restart_req = false;
  pthread_mutex_unlock(&g_window_lock);
  return r;
}

/**
 * @brief 配对窗口是否开着（判定"允许未配对连接"的两块拼图之一）。
 *
 * @return true = 此刻在窗口内。
 *
 * @note 可从任意线程调用（内部持 `g_window_lock`）。
 */
bool myvendor_pair_window_active(void)
{
  bool active;

  pthread_mutex_lock(&g_window_lock);
  active = g_pair_window_until_ms != 0u &&
           (int32_t)(g_pair_window_until_ms - myvendor_mono_ms()) > 0;
  pthread_mutex_unlock(&g_window_lock);
  return active;
}

/** @brief 立刻关掉配对窗口（解绑 / 配对成功时都要收掉）。
 *
 * 用户现场：配对完成后 30 s 还没到，这时解绑 —— 剩下的窗口里**谁都能把"配对中 + 倒计时"
 * 走完再配一次**。语义上"允许配对"这个许可不该比绑定关系活得久，所以：
 *   · 解绑（`myvendor_pair_unbind_poll`）先关窗；
 *   · 配对成功（`myvendor_pair_note_bond(…, true)`）也关窗。
 * 窗口一关，`adv` 那三档会按"有没有记录"重新判定（没记录就不再广播）。 */
void myvendor_pair_window_close(void)
{
  bool was_open;

  pthread_mutex_lock(&g_window_lock);
  was_open = g_pair_window_until_ms != 0u;
  g_pair_window_until_ms = 0u;
  /* 让广播立刻按新档位重判一次：解绑后 → `adv off`（没记录）；配对成功后 → 可发现。 */
  g_window_restart_req   = true;
  pthread_mutex_unlock(&g_window_lock);

  if (was_open)
    {
      printf("pair: window closed\n");
    }
}

/** @brief 配对窗口剩余毫秒（0 = 没开窗）。设备 UI 用它显示"配对中 + 倒计时"。 */
uint32_t myvendor_pair_window_left_ms(void)
{
  int32_t left;

  pthread_mutex_lock(&g_window_lock);
  if (g_pair_window_until_ms == 0u)
    {
      pthread_mutex_unlock(&g_window_lock);
      return 0u;
    }

  left = (int32_t)(g_pair_window_until_ms - myvendor_mono_ms());
  pthread_mutex_unlock(&g_window_lock);
  return left > 0 ? (uint32_t)left : 0u;
}

/** @brief 开配对窗口（秒）；0 = 用开机默认 60 s。窗口内允许未配对连接（首次配对靠它）。 */
void myvendor_pair_window_open(unsigned secs)
{
  if (secs == 0u)
    {
      secs = PAIR_WINDOW_DEFAULT_S;
    }

  pthread_mutex_lock(&g_window_lock);
  g_pair_window_until_ms = myvendor_mono_ms() + secs * 1000u;
  g_window_restart_req   = true;
  pthread_mutex_unlock(&g_window_lock);

  /* 开窗清拒连冷却：否则上一轮 grace 踢过的手机窗内仍被挡，系统只报 PIN 错。 */
  pair_reject_cooldown_clear(NULL);

  printf("pair: window open %us\n", secs);
}

/* 十六进制串 → 字节；`hex` 至少 2n 个字符。 */
static void pair_hex_to_bytes(const char *hex, uint8_t *out, unsigned n)
{
  unsigned i;

  for (i = 0; i < n; i++)
    {
      unsigned hi = 0;
      unsigned lo = 0;
      const char c1 = hex[i * 2u];
      const char c2 = hex[i * 2u + 1u];

      hi = (c1 >= '0' && c1 <= '9') ? (unsigned)(c1 - '0') : (unsigned)(c1 - 'A' + 10);
      lo = (c2 >= '0' && c2 <= '9') ? (unsigned)(c2 - '0') : (unsigned)(c2 - 'A' + 10);
      out[i] = (uint8_t)(((hi & 15u) << 4) | (lo & 15u));
    }
}

/* 读 `/mnt/kv/bt_phone.tsv`（最多 PAIR_PHONE_MAX 行）。 */
struct pair_phone_rec
{
  char    hex[13];
  uint8_t type;
};

static unsigned pair_phone_load(struct pair_phone_rec *out, unsigned max)
{
  char  line[96];
  char  raw[40];
  char  norm[40];
  FILE *f;
  unsigned n = 0;

  if (out == NULL || max == 0u)
    {
      return 0u;
    }

  f = fopen(PAIR_PHONE_TSV, "r");
  if (f == NULL)
    {
      return 0u;
    }

  while (n < max && fgets(line, sizeof(line), f) != NULL)
    {
      unsigned type = 1;

      if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r')
        {
          continue;
        }

      if (sscanf(line, "%39s %u", raw, &type) < 1)
        {
          continue;
        }

      pair_norm_addr(raw, norm, sizeof(norm));
      if (strlen(norm) < 12u)
        {
          continue;
        }

      norm[12] = '\0';
      memcpy(out[n].hex, norm, 13u);
      out[n].type = (uint8_t)(type & 0xffu);
      n++;
    }

  fclose(f);
  return n;
}

static void pair_phone_save(const struct pair_phone_rec *rec, unsigned n)
{
  FILE *f;
  unsigned i;

  if (n == 0u)
    {
      (void)remove(PAIR_PHONE_TSV);
      return;
    }

  f = fopen(PAIR_PHONE_TSV, "w");
  if (f == NULL)
    {
      printf("pair: phone record write failed\n");
      return;
    }

  for (i = 0; i < n; i++)
    {
      fprintf(f, "%s\t%u\n", rec[i].hex, (unsigned)rec[i].type);
    }

  fclose(f);
}

/* ── 用密钥池修剪名单（2026-09-23）────────────────────────────────────────
 *
 *  `bt_phone.tsv` 里会混进**配对那次连接的 RPA**：连接建立时 `conn->le.dst`
 *  就定死了，而那一刻密钥池里还没有 IRK ⇒ 主机侧解析不了 ⇒ 两个写入者
 *  （框架的 bond 回调 / `myvendor_pair_peer_poll()`）都只能记下 RPA。
 *  身份地址要等**下一次回连**才会被记上 ⇒ 同一台手机占两个名额。
 *
 *  现场（固件 `202609231925`）：
 *    ctl pair list  → 78:D8:40:4B:F5:20 (public) sec=2 bonded=1   ← 框架侧是对的
 *    ctl pair phone → [0] 66F65E5417BF type=1                     ← 废 RPA
 *                     [1] 78D8404BF520 type=0                     ← 身份地址
 *                     phones 2/3                                  ← 白占一个名额
 *
 *  RPA 会轮换、**永远匹配不上**任何后续连接，对 `pair_phone_contains()` 零贡献，
 *  唯一害处就是占名额（`PAIR_PHONE_MAX` 满了就得靠 FIFO 淘汰兜）。
 *
 *  判据用 `bt_foreach_bond_mc()` —— 这正是框架
 *  `bt_sal_le_get_bonded_devices()` 用的同一份数据，`struct bt_bond_info.addr`
 *  按构造就是**身份地址**。
 *
 *  ⚠ **只能在"密钥池已恢复、且还没有对端能连上"的窗口里调**：遍历密钥池必须
 *  与 BT 线程的增删互斥（`ctl pair list` 就曾在 CTL 线程踩到半更新的链表而
 *  MemFault）。落点 = `companion_restore_keys()`，与
 *  `myvendor_keys_kv_load_all()` 同一窗口（开广播之前，见其注释）。
 *
 *  ⚠ 密钥池为空时**绝不修剪** —— 那说明还没恢复完，会把整张表清掉。
 *  所以先看池里到底有几条，为 0 就原样返回、留给下一次调用。 */

static char     g_key_pool[PAIR_PHONE_MAX][13];
static unsigned g_key_pool_n;
static bool     g_prune_done;

/** 记录**攒多久才落盘**（用户 2026-09-23：「为什么在不稳定的时候落盘，
 *  不能落盘靠后吗」—— 对，落盘就该靠后）。
 *
 *  配对刚开始那会儿状态是不稳定的：密钥池里会**短暂存在一条以 RPA 为地址的
 *  键记录**，之后才归并到身份地址；而 `conn->le.dst` 在连接建立时就定死了、
 *  不会再变。于是 `myvendor_pair_peer_poll()` 会先用 RPA 报一次、约 0.7 s 后
 *  再用身份地址报一次。
 *
 *  ⇒ 两次都**只更新同一份内存待写值**（后到的身份地址直接覆盖掉 RPA），
 *  等 `PAIR_SETTLE_MS` 到点、池也稳了，再**一次性落盘**并当场用"在不在池里"
 *  判据把关。这样：
 *    · 废 RPA **根本不会写进文件**（不是"写了再清"）；
 *    · 名单**不会出现 3/3 的瞬时峰值** ⇒ 不会误触 FIFO 淘汰（那有可能挤掉
 *      另一台手机的身份地址 —— 2 台手机时余量只有一个坑）；
 *    · 附带好处：一次配对只写一次 flash，不是两次。
 *
 *  2 s 的依据：实测 RPA 上报 → 身份地址上报约 **0.7 s**，`bt_keys kv: saved`
 *  从 0 条到 1 条约 **0.4 s**，取 2 s 留足余量。 */
#define PAIR_SETTLE_MS 2000u

/** 队列建立了这么久还挑不出"在密钥池里的地址"就放弃（防止永久挂着）。
 *  60 s 远大于实测的池稳定时间（~0.4 s），正常路径永远走不到这里。 */
#define PAIR_PEND_MAX_MS 60000u

/** 攒住**最近两个**不同地址，落盘时挑"在密钥池里的那个"。
 *
 *  ⚠ 为什么不是"只留最后一个"（第一版就是这么写的，`202609232018` 实测踩了）：
 *  我假设"RPA 先到、身份地址后到"。**首次配对**确实如此；但**回连**时手机可能
 *  先报身份地址、再报一个 RPA（两条连接交替），于是"后到覆盖先到"把身份地址
 *  盖掉，落盘判据又把 RPA 丢掉 ⇒ **一条都没写**（现场日志：
 *  `secured peer 78D8404BF520` 之后跟了 8 次 `secured peer 7F53B7ED5CBB`，
 *  最后 `drop 7F53B7ED5CBB`，名单始终为空）。
 *
 *  留两个槽就同时覆盖两种情形：
 *    · 首次配对 [RPA, 身份]  → 落盘时池里是身份 ⇒ 选身份 ✓
 *    · 回连交替 [身份, RPA]  → 落盘时池里是身份 ⇒ 选身份 ✓ */
#define PAIR_PEND_SLOTS 2u
static char     g_pend_hex[PAIR_PEND_SLOTS][13];
static uint8_t  g_pend_type[PAIR_PEND_SLOTS];
static unsigned g_pend_n;              /* 0..PAIR_PEND_SLOTS，[0] 最老 */
static uint32_t g_pend_due_ms;         /* 第一次入队起算，入队不清零重算 */
static uint32_t g_pend_start_ms;       /**< 队列建立时刻；超 `PAIR_PEND_MAX_MS`
                                        *   仍挑不出身份地址就放弃（免得永久挂着）。 */

static void pair_key_pool_cb(const struct bt_bond_info *info, void *user_data)
{
  char   s[BT_ADDR_LE_STR_LEN];
  char   norm[40];

  (void)user_data;

  if (info == NULL || g_key_pool_n >= PAIR_PHONE_MAX)
    {
      return;
    }

  bt_addr_le_to_str(&info->addr, s, sizeof(s));
  pair_norm_addr(s, norm, sizeof(norm));
  if (strlen(norm) < 12u)
    {
      return;
    }

  norm[12] = '\0';
  memcpy(g_key_pool[g_key_pool_n], norm, 13u);
  g_key_pool_n++;
}

/** @brief 重新遍历密钥池，刷新 `g_key_pool[]`。返回池里条数。 */
static unsigned pair_key_pool_refresh(void)
{
  g_key_pool_n = 0;
  bt_foreach_bond_mc(0, BT_ID_DEFAULT, pair_key_pool_cb, NULL);
  return g_key_pool_n;
}

/* 曾有一个 `pair_addr_in_key_pool()` 用来在写入前拦 RPA —— **已删除**：
 * 配对窗口里密钥池会短暂以 RPA 为地址持有那条键记录，判据会返回 true 而放行，
 * 拦不住（实测 `202609231956`）。现在只保留"池稳定后修剪"这一条路。 */

/** @brief 立刻按密钥池修剪一次 `bt_phone.tsv`。幂等；池空时不动。
 *
 *  ⚠ **只能"池已稳定"的时候调。** 配对过程中密钥池里会**短暂存在一条以 RPA
 *  为地址的键记录**，配对完成后才归到身份地址 —— 那时"地址在不在池里"判不出
 *  真假（实测 `202609231956`：写 RPA 那一刻该判据返回 true，RPA 照样被写进
 *  名单，于是又出现一台手机两条记录）。
 *
 *  ⇒ 所以运行时不"在写入前拦"，而是"**写完等几秒、池稳定后再修剪**"
 *  （见 `myvendor_pair_prune_poll()` 与 `PAIR_PRUNE_DELAY_MS`）。 */
static void pair_prune_now(void)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];
  struct pair_phone_rec keep[PAIR_PHONE_MAX];
  unsigned n;
  unsigned i;
  unsigned k;
  unsigned w = 0;

  if (pair_key_pool_refresh() == 0u)
    {
      return;                 /* 密钥池空 —— 别清表，留给下一次 */
    }

  n = pair_phone_load(rec, PAIR_PHONE_MAX);
  if (n == 0u)
    {
      return;
    }

  for (i = 0; i < n; i++)
    {
      bool in_pool = false;

      for (k = 0; k < g_key_pool_n; k++)
        {
          if (strncmp(rec[i].hex, g_key_pool[k], 12u) == 0)
            {
              in_pool = true;
              break;
            }
        }

      if (in_pool)
        {
          keep[w] = rec[i];
          w++;
        }
      else
        {
          printf("pair: prune %s type=%u (不在密钥池里 —— 陈旧 RPA?)\n",
                 rec[i].hex, (unsigned)rec[i].type);
        }
    }

  if (w != n)
    {
      pair_phone_save(keep, w);
      printf("pair: phone record pruned %u -> %u\n", n, w);
    }
}

/** @brief 开机修剪入口（`companion_restore_keys()` 里调；只跑一次）。 */
void myvendor_pair_prune_against_keys(void)
{
  if (g_prune_done)
    {
      return;
    }

  if (pair_key_pool_refresh() == 0u)
    {
      return;                 /* 密钥池还没恢复完 —— 下次再试，别清表 */
    }

  g_prune_done = true;
  pair_prune_now();
}

static bool pair_pending_commit(void);   /* 定义在下面（延迟落盘的执行体） */

/** @brief **延迟落盘**入口（companion 线程每拍调）。
 *
 *  到 `PAIR_SETTLE_MS` 就把攒下的那条记录按"在不在密钥池里"把关后落盘
 *  —— 那会儿配对已完成、密钥池已把 RPA 归并到身份地址，判据是可靠的。
 *
 *  自证伪：若日志里出现 `pair: drop <身份地址> (不在密钥池…)`（丢掉的是**真正
 *  在用的**那台手机），说明 2 s 不够、池还没稳定 ⇒ 调大 `PAIR_SETTLE_MS`。
 */
void myvendor_pair_settle_poll(void)
{
  if (g_pend_n == 0u ||
      (int32_t)(g_pend_due_ms - myvendor_mono_ms()) > 0)
    {
      return;
    }

  /* 没落盘就把到期时间**拉到当前** ⇒ 下一拍立刻重试（常见原因是"池还没把身份
   * 地址归并好"）。无限重试的上界由 `PAIR_PEND_MAX_MS` 兜住。 */
  if (!pair_pending_commit())
    {
      g_pend_due_ms = myvendor_mono_ms();
    }
}

/** @brief 兼容旧调用：取**第一台**手机身份。 */
static bool pair_phone_identity(char *out12, size_t out12_len, uint8_t *out_type)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];
  unsigned n = pair_phone_load(rec, PAIR_PHONE_MAX);

  if (n == 0u || out12_len < 13u)
    {
      return false;
    }

  memcpy(out12, rec[0].hex, 13u);
  if (out_type != NULL)
    {
      *out_type = rec[0].type;
    }

  return true;
}

static bool pair_phone_contains(const char *hex12, uint8_t *out_type)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];
  unsigned n;
  unsigned i;

  if (hex12 == NULL)
    {
      return false;
    }

  n = pair_phone_load(rec, PAIR_PHONE_MAX);
  for (i = 0; i < n; i++)
    {
      if (strncmp(rec[i].hex, hex12, 12u) == 0)
        {
          if (out_type != NULL)
            {
              *out_type = rec[i].type;
            }

          return true;
        }
    }

  return false;
}

unsigned myvendor_pair_phone_count(void)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];

  return pair_phone_load(rec, PAIR_PHONE_MAX);
}

bool myvendor_pair_phone_at(unsigned idx, char *out12, size_t out12_len,
                            uint8_t *out_type)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];
  unsigned n = pair_phone_load(rec, PAIR_PHONE_MAX);

  if (idx >= n || out12 == NULL || out12_len < 13u)
    {
      return false;
    }

  memcpy(out12, rec[idx].hex, 13u);
  if (out_type != NULL)
    {
      *out_type = rec[idx].type;
    }

  return true;
}

static bool myvendor_pair_reject_new_when_full(struct bt_conn *conn)
{
  char s[BT_ADDR_LE_STR_LEN];
  char norm[40];

  if (conn == NULL)
    {
      return false;
    }

  bt_addr_le_to_str(bt_conn_get_dst(conn), s, sizeof(s));
  pair_norm_addr(s, norm, sizeof(norm));
  if (strlen(norm) >= 12u)
    {
      norm[12] = '\0';
    }

  if (pair_phone_contains(norm, NULL))
    {
      return false;
    }

  return myvendor_pair_phone_count() >= PAIR_PHONE_MAX;
}

/** @brief 新地址且名单已满则拒绝（给 SAL `on_pair_display` 用）。 */
bool myvendor_pair_reject_new_addr(const char *addr_str)
{
  char norm[40];

  if (addr_str == NULL || addr_str[0] == '\0')
    {
      return false;
    }

  pair_norm_addr(addr_str, norm, sizeof(norm));
  if (strlen(norm) >= 12u)
    {
      norm[12] = '\0';
    }

  if (pair_phone_contains(norm, NULL))
    {
      return false;
    }

  return myvendor_pair_phone_count() >= PAIR_PHONE_MAX;
}

/* ── 在 LE 连接里按（归一化）地址找 conn ─────────────────────────────────────
 * 找到就**带引用返回**（调用方负责 `bt_conn_unref`）。
 *
 * @warning 内部走 `bt_conn_foreach()` —— **只能在协议栈线程/会话内用**，不要拿它
 *          做周期性轮询（2026-09-20 的 panic 就是这么来的，见 `myvendor_pair_peer_poll`
 *          的注释）。这里只在 bond 回调 / 控制帧 / 用户命令这些低频路径里调。 */
static char            g_look_hex[13];
/** 命中那条连接在 HCI 层报的地址类型（0 public / 1 random），随查找一起带出。 */
static uint8_t         g_look_type;
/** 命中那条连接的引用（交给调用方 unref）。 */
static struct bt_conn *g_look_conn;

/**
 * @brief `bt_conn_foreach` 回调：地址（归一化后）匹配就记类型并 `bt_conn_ref()`。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_look_cb(struct bt_conn *conn, void *data)
{
  char s[BT_ADDR_LE_STR_LEN];
  char norm[40];

  (void)data;
  bt_addr_le_to_str(bt_conn_get_dst(conn), s, sizeof(s));
  pair_norm_addr(s, norm, sizeof(norm));
  if (strncmp(norm, g_look_hex, 12u) == 0)
    {
      g_look_type = (uint8_t)bt_conn_get_dst(conn)->type;
      g_look_conn = bt_conn_ref(conn);
    }
}

/**
 * @brief 按（12 位归一化）地址查一条 LE 连接。
 *
 * @param hex12    12 位十六进制地址（无冒号，大小写不敏感）。
 * @param out_type 可选；命中时写入该连接在 HCI 层的地址类型（0 public / 1 random）。
 * @return 命中时返回**已带引用**的连接（调用方 `bt_conn_unref`）；没命中返回 NULL。
 */
static struct bt_conn *pair_conn_find(const char *hex12, uint8_t *out_type)
{
  snprintf(g_look_hex, sizeof(g_look_hex), "%s", hex12);
  g_look_conn = NULL;
  g_look_type = 0;                /* BT_ADDR_LE_PUBLIC */

  bt_conn_foreach(BT_CONN_TYPE_LE, pair_look_cb, NULL);

  if (out_type != NULL)
    {
      *out_type = g_look_type;
    }

  return g_look_conn;
}


/**
 * @brief 从"应用侧真的有流量"反推"这就是那台已绑定手机"，并把记录补上。
 *
 * 为什么需要它：`on_bond_state_changed` 那条路实测**没能让记录落盘**
 * （2026-09-20：手机配对成功、能连上，但 `/mnt/kv/bt_phone.tsv` 一直是空的）——
 * 记录空 ⇒ 窗口一关准入就会拒绝这台手机。这里换成**实证**：对端能发控制帧说明
 * 链路已加密（所有特征/CCCD 都是 ENCRYPT 权限，未加密写根本到不了这里），
 * 而加密链路必然来自一次成功的配对 ⇒ 记它。地址此时是 IRK 解析后的**身份地址**
 * （已绑定对端 zblue 会解析），正好是记录与定向广播要的那个。
 */
/* 「刚加密的那条链路」的暂存：`bt_conn_cb.security_changed` **在协议栈线程**里写，
 * companion 线程每拍取走落盘。跨线程共享 ⇒ **必须上锁**：裸写一个 13 字节地址 +
 * 一个标志，取走的一侧就可能读到半截地址（用户明确要求：不许无保护地跨线程动资源）。*/
static pthread_mutex_t g_secure_lock = PTHREAD_MUTEX_INITIALIZER;
static char    g_secure_peer[13];
static uint8_t g_secure_peer_type;
static bool    g_secure_peer_pending;

/** @brief 落盘手机记录：**追加**到最多 PAIR_PHONE_MAX 台，不覆盖已有别的手机。
 *
 * 已在名单里 → 只更新 type（若变了）；名单满且是新地址 → 拒绝（需先解绑）。
 * 新地址成功写入时关配对窗口。 */
static void pair_record_write_now(const char *norm, uint8_t type)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];
  unsigned n;
  unsigned i;

  if (norm == NULL || strlen(norm) < 12u)
    {
      return;
    }

  /** ⚠ **不要在写入前用"地址在不在密钥池里"来拦。**
   *
   *  我按这个判据做过一版（`202609231956`），**它在配对窗口里是错的**：
   *  配对过程中密钥池会**短暂存在一条以 RPA 为地址的键记录**，配对完成后才
   *  归并到身份地址 —— 于是"写 RPA 那一刻"该判据返回 **true**，RPA 照样被写
   *  进名单，一台手机又是两条（实测复现）。
   *
   *  当时我拿 `pair: consent 78:D8:40:4B:F5:20` 的位置推断"身份地址已经在池
   *  里"，推错了 —— 那条 printf 与 `bt_keys kv:` 的 syslog 是**两路输出**，
   *  行序不可信。
   *
   *  ⇒ 改法：**照写，写完排一次延迟修剪**（下几行），等池稳定下来再按"不在
   *  池里就清"处理（那条判据在池稳定时一直是准的，开机修剪就是证据）。
   *  宁可有几百毫秒的双记录窗口，也不要拿一条会误判的判据把**身份地址**挡掉
   *  —— 那会让手机下次回连必须开配对窗，代价大得多。 */

  n = pair_phone_load(rec, PAIR_PHONE_MAX);
  for (i = 0; i < n; i++)
    {
      if (strncmp(rec[i].hex, norm, 12u) == 0)
        {
          if (rec[i].type == type)
            {
              return;             /* 已是它，不刷 flash */
            }

          rec[i].type = type;
          pair_phone_save(rec, n);
          printf("pair: phone record %s type=%u (update)\n",
                 norm, (unsigned)type);
          return;
        }
    }

  /** 名单满：**挤掉最老的一条**，而不是丢掉新来的这台。
   *
   *  以前是"满就丢"，配合"记录里存的是轮换 RPA"这条老毛病，现场就变成
   *  `bt_phone.tsv` 3/3 满是三条再也对不上的废地址，**真要用的手机永远进不来**：
   *  它每次回连都落进 `myvendor_pair_admit` 的短 grace，升不上 L2 就被踢 + 冷却
   *  60 s（见 SOAK3 基线 §3）。名单是"准入白名单"，不是"绑定表"——**绑定表在
   *  zblue 的密钥池里**（`CONFIG_BT_MAX_PAIRED=3`，与 `PAIR_PHONE_MAX` 同值），
   *  能走到这里的前提就是"链路已升到 L2"，而 L2 只可能来自我们发出去过的 LTK
   *  ⇒ **能进本函数的对端必然是已配对设备**，让它挤掉一条陈旧记录是安全的。
   *
   *  这样旧固件留下的废 RPA 记录会被真手机逐次挤干净（不需要人去 `ctl pair forget`）。
   */
  if (n >= PAIR_PHONE_MAX)
    {
      unsigned k;

      printf("pair: phone slots full (%u) — evict oldest %s for %s\n",
             (unsigned)PAIR_PHONE_MAX, rec[0].hex, norm);
      for (k = 1u; k < n; k++)
        {
          rec[k - 1u] = rec[k];
        }

      n = PAIR_PHONE_MAX - 1u;
    }

  memcpy(rec[n].hex, norm, 12u);
  rec[n].hex[12] = '\0';
  rec[n].type = type;
  n++;
  pair_phone_save(rec, n);
  /* **关窗放在这里**（真的落盘之后）：语义上"配对完成"只在记录确实写进去之后
   * 才成立。放到上面那个会被每拍调用的排队函数里会把窗口反复关掉。 */
  myvendor_pair_window_close();
  printf("pair: phone record %s type=%u (%u/%u)\n",
         norm, (unsigned)type, n, (unsigned)PAIR_PHONE_MAX);
}

/**
 * @brief 记一条对端 —— **攒在内存里，等状态稳定了再落盘**。
 *
 *  这是"落盘靠后"的入口（用户 2026-09-23 的建议），取代了早先的
 *  "先写、5 s 后再修剪"。配对刚开始时 `myvendor_pair_peer_poll()` 会先用
 *  **本次连接的 RPA** 报一次（密钥池里那一刻只有一条以 RPA 为地址的临时键），
 *  约 0.7 s 后才用身份地址再报一次。
 *
 *  两次都只更新 `g_pend_*`（**后到覆盖先到**）：身份地址自然把 RPA 顶掉，
 *  到点后由 `myvendor_pair_settle_poll()` 用"在不在密钥池里"把关再落盘。
 *  于是废 RPA **根本不进文件**，名单也不会出现 3/3 的瞬时峰值
 *  （⇒ 不会误触 FIFO 淘汰、挤掉另一台手机）。
 *
 *  ⚠ **关窗是语义动作、与磁盘无关，所以立刻做**，不跟着推迟 —— 否则
 *  配对窗会白多开 2 s（窗口期传感器要让位射频）。
 *
 *  @param norm 归一化地址（12 位十六进制，不带分隔符）。
 *  @param type 0 = public / 1 = random（仅用于显示与 `ctl pair phone`）。
 */
static void pair_record_write(const char *norm, uint8_t type)
{
  unsigned i;

  if (norm == NULL || strlen(norm) < 12u)
    {
      return;
    }

  /* 已在这个地址上（`peer_poll` 每拍都会来报一次）⇒ 什么都不做。
   * ⚠ **不要在这里重设 `g_pend_due_ms`**：第一版就是每拍重设，于是到期时间被
   * 无限推后、永远不落盘（现场连打 8 次 `secured peer -> record`）。 */
  for (i = 0; i < g_pend_n; i++)
    {
      if (strncmp(g_pend_hex[i], norm, 12u) == 0)
        {
          return;
        }
    }

  if (g_pend_n >= PAIR_PEND_SLOTS)
    {
      /* 满了：挤掉最老的，给新来的腾位。 */
      memmove(g_pend_hex[0], g_pend_hex[1], sizeof(g_pend_hex[0]));
      g_pend_type[0] = g_pend_type[1];
      g_pend_n       = PAIR_PEND_SLOTS - 1u;
    }

  if (g_pend_n == 0u)
    {
      /* 队列由空变非空：**这时才起算**（新地址中途进来不延长窗口）。 */
      g_pend_due_ms   = myvendor_mono_ms() + PAIR_SETTLE_MS;
      g_pend_start_ms = myvendor_mono_ms();
    }

  memcpy(g_pend_hex[g_pend_n], norm, 12u);
  g_pend_hex[g_pend_n][12] = '\0';
  g_pend_type[g_pend_n]    = type;
  g_pend_n++;

  /* **不在这里关窗**：本函数会被 `peer_poll` 每拍调用，第一版把
   * `myvendor_pair_window_close()` 放这儿，结果窗口被自己反复关掉
   * （`pair window -> adv restart` 连打 8 次 → 随后
   * `adv off (unpaired, no window)` ⇒ 手机再也搜不到设备）。
   * 关窗改到**真的落盘之后**（`pair_record_write_now()` 里）。 */
}

/** @brief 到点把攒下的那条落盘：**在池里才写**，否则丢掉。
 *
 *  `pair_key_pool_refresh()` 拿到的就是身份地址列表（`bt_foreach_bond_mc()`，
 *  与框架 `bt_sal_le_get_bonded_devices()` 同一份数据）。此刻配对已完成、
 *  池已稳定，"在不在池里"这条判据是可靠的 —— 开机修剪一直准，就是因为它跑的
 *  时候池是稳定的。
 *
 *  @return true = 真的落盘了。
 */
static bool pair_pending_commit(void)
{
  unsigned s;
  unsigned i;

  if (pair_key_pool_refresh() == 0u)
    {
      /* 池还没起来：**保留队列**，下次再试（不清）。 */
      return false;
    }

  for (s = g_pend_n; s > 0u; s--)
    {
      for (i = 0; i < g_key_pool_n; i++)
        {
          if (strncmp(g_key_pool[i], g_pend_hex[s - 1u], 12u) == 0)
            {
              char    hex[13];
              uint8_t type = g_pend_type[s - 1u];

              memcpy(hex, g_pend_hex[s - 1u], sizeof(hex));
              g_pend_n = 0u;
              pair_record_write_now(hex, type);   /* 内含关窗 + 落盘 */
              pair_prune_now();                   /* 顺手清历史遗留废条目 */
              return true;
            }
        }
    }

  /* 两个都不是池里的身份地址 —— 可能是"本次连接的 RPA"，也可能只是池还没把
   * 身份地址归并好。**不立刻丢**：留着，下一次 `peer_poll` 报身份地址时会补进
   * 队列。但要有个上界，免得永久挂着。 */
  if ((int32_t)(myvendor_mono_ms() - g_pend_start_ms) >= (int32_t)PAIR_PEND_MAX_MS)
    {
      printf("pair: drop %u 个待写地址（超时仍不在密钥池 —— 不是身份地址）\n",
             g_pend_n);
      g_pend_n = 0u;
      return false;
    }

  return false;
}

/**
 * @brief 链路加密了（`bt_conn_cb.security_changed`，跑在 BT 线程）—— 只记地址。
 *
 * **为什么只置标志**：这条回调在协议栈线程上，落盘（/mnt/kv）+ tsv 读都留到
 * companion 线程的每拍去做（`myvendor_pair_peer_poll`）。FS 在栈线程上阻塞会
 * 顶住整个协议栈（"FS 提交饿死心跳"那条教训）。
 */
void myvendor_pair_note_secure_peer(const bt_address_t *addr, uint8_t addr_type)
{
  char s[BT_ADDR_STR_LENGTH];
  char hex[40];

  if (addr == NULL)
    {
      return;
    }

  bt_addr_ba2str(addr, s);
  pair_norm_addr(s, hex, sizeof(hex));
  if (strlen(hex) < 12u)
    {
      return;
    }
  hex[12] = '\0';

  pthread_mutex_lock(&g_secure_lock);
  snprintf(g_secure_peer, sizeof(g_secure_peer), "%s", hex);
  g_secure_peer_type = addr_type;
  g_secure_peer_pending = true;
  pthread_mutex_unlock(&g_secure_lock);
}

/**
 * @brief 对端能发控制帧 = 链路已加密（所有特征/CCCD 都是 ENCRYPT 权限）⇒ 记它。
 *
 * 现在只置标志，真正的落盘交给 `myvendor_pair_peer_poll`（见上）。
 */
void myvendor_pair_note_peer_if_secure(const bt_address_t *addr)
{
  myvendor_pair_note_secure_peer(addr, 0);
}

/**
 * @brief 维护"已绑定手机"记录（`/mnt/kv/bt_phone.tsv`）。
 *
 * 两个方向：
 *   · `bonded = true`：归一化地址 → 关窗 → **内容变了才**落盘（见 `pair_record_write()`）；
 *     调用点：适配器 bond 回调、控制帧路径、`security_changed` 对账。
 *   · `bonded = false`：只在"确实就是记录里那台"时删记录（别的设备配对失败时
 *     适配器也会发 NONE，不该把手机这条抹掉）；`addr == NULL` 是 `ctl pair forget`
 *     的强制清除路径。
 *
 * @param addr   对端地址；`bonded = false` 时允许为 NULL（= 强制清除）。
 * @param bonded true = 记上 / 更新；false = 清除。
 *
 * @note 会做文件 IO（tsv 读写），**不要**从协议栈的高频回调里直接调；跨线程共享
 *       的暂存都已加锁，但落盘仍尽量留在 companion 线程。
 */
void myvendor_pair_note_bond(const bt_address_t *addr, bool bonded)
{
  char    s[BT_ADDR_STR_LENGTH];
  char    norm[40];
  uint8_t type = 0;

  if (!bonded)
    {
      struct pair_phone_rec rec[PAIR_PHONE_MAX];
      unsigned n;
      unsigned i;
      unsigned w;
      char     peer_s[BT_ADDR_STR_LENGTH];
      char     want[40];

      /* addr == NULL：`ctl pair forget` 清全部。否则只删名单里那一台。 */
      if (addr == NULL)
        {
          n = pair_phone_load(rec, PAIR_PHONE_MAX);
          for (i = 0; i < n; i++)
            {
              /* 密钥在 unbind_poll / forget 路径另清；这里只清 tsv。 */
            }

          if (remove(PAIR_PHONE_TSV) == 0)
            {
              printf("pair: phone record cleared (all)\n");
            }

          return;
        }

      bt_addr_ba2str(addr, peer_s);
      pair_norm_addr(peer_s, want, sizeof(want));
      if (strlen(want) >= 12u)
        {
          want[12] = '\0';
        }

      n = pair_phone_load(rec, PAIR_PHONE_MAX);
      w = 0;
      for (i = 0; i < n; i++)
        {
          if (strncmp(rec[i].hex, want, 12u) == 0)
            {
              continue;           /* 删掉这一台 */
            }

          if (w != i)
            {
              rec[w] = rec[i];
            }

          w++;
        }

      if (w == n)
        {
          return;                 /* 名单里没有它，别误伤 */
        }

      pair_phone_save(rec, w);
      printf("pair: phone record removed %s (%u left)\n", want, w);
      return;
    }

  bt_addr_ba2str(addr, s);
  pair_norm_addr(s, norm, sizeof(norm));
  if (strlen(norm) < 12u)
    {
      printf("pair: phone record bad addr %s\n", s);
      return;
    }
  norm[12] = '\0';

  {
    struct bt_conn *conn = pair_conn_find(norm, &type);

    if (conn != NULL)
      {
        bt_conn_unref(conn);
      }
    else
      {
        type = 0;                 /* 拿不到连接就按 public 记 */
      }
  }

  pair_record_write(norm, type);
}

/** @brief `myvendor_pair_phone_link_up()` 的遍历结果：是否见到"手机侧"链路。 */
static bool g_link_phone;

/**
 * @brief `bt_conn_foreach` 回调：判断这条链路是不是"手机侧"（非传感器）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_link_cb(struct bt_conn *conn, void *data)
{
  extern bool ble_sensor_owns_addr(const bt_address_t *addr);
  bt_address_t a;

  (void)data;
  memset(&a, 0, sizeof(a));
  memcpy(a.addr, bt_conn_get_dst(conn)->a.val, BT_ADDR_LENGTH);

  if (!ble_sensor_owns_addr(&a))
    {
      g_link_phone = true;        /* 手机侧的链路（传感器链路另算） */
    }
}

/** @brief 现在是否真的有一条"手机侧"LE 链路 —— 给 ble_companion 做状态对账。

 *
 * 为什么要它：GATTS 的 disconnect 回调偶尔**不到**（HCI 已报断开、`g_ctx.connected`
 * 却一直是真），现象就是"设备觉得还在连"：不再广播、手机也连不回来。这里给的是
 * zblue 自己的链路表，比"事件有没有到"可靠。 */
bool myvendor_pair_phone_link_up(void)
{
  g_link_phone = false;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_link_cb, NULL);
  return g_link_phone;
}



/** @brief 每拍对账：把"刚加密的那条手机链路"补成一条手机记录。
 *
 * 为什么不再等回调：`on_bond_state_changed` 实测不落盘，而 note_peer_if_secure() 要等
 * 手机真发一帧控制帧 —— 手机连上只读不写时记录永远是空的，UI 于是把「配对中 + 倒计时」
 * 一直显示到 0，也不会有「已配对」（2026-09-20 现场）。链路加密（sec>=L2）本身就是配对
 * 完成的证据：补记录 → 窗口收掉、UI 刷新成设备信息。
 *
 * **地址来自 `bt_conn_cb.security_changed`（BT 线程）**，这里只消费标志 —— 以前这里
 * 每拍 `bt_conn_foreach()` 扫连接链表，撞上协议栈正在增删的链表就 panic：
 *  coredump `n574_20260920_121424`：`pc=bt_conn_ref→stlex`、`cfa=0x82`、`mmfar=0x124`、
 *  `pid=35 ble_companion irq=1`（同一个签名早在 `ctl pair list` 上见过 —— 结论是
 *  **别在非协议栈线程里遍历 conn 链表**，晚一拍由事件驱动就够了）。
 */
void myvendor_pair_peer_poll(void)
{
  char    want[40];
  uint8_t peer_type;
  bool    pending;

  /* 先在锁里把暂存整份取走（地址+类型+清标志），锁外再做 tsv/flash 那套慢活。 */
  pthread_mutex_lock(&g_secure_lock);
  pending = g_secure_peer_pending;
  if (pending)
    {
      g_secure_peer_pending = false;
      peer_type = g_secure_peer_type;
      snprintf(want, sizeof(want), "%s", g_secure_peer);
    }
  pthread_mutex_unlock(&g_secure_lock);

  if (!pending)
    {
      return;
    }

  if (strlen(want) < 12u)
    {
      return;
    }

  if (pair_phone_contains(want, NULL))
    {
      pair_reject_cooldown_clear(want);
      return;                     /* 已在最多 3 台名单里 */
    }

  /** 写入条件：配对窗口内 **或** 这条记录本身已证明是"已绑定手机"。
   *
   *  为什么可以不要窗口：本函数的输入全部来自"链路已加密"——
   *  `le_note_peer_if_secure()` 要求 `bt_conn_get_security(conn) >= BT_SECURITY_L2`，
   *  控制帧那几条路也只在 ENCRYPT 特征上才收得到；而**能升到 L2 只可能持有我们
   *  发出去的 LTK** ⇒ 对端必然已经配对过。窗口本来只是"新手机入名单"的闸门，
   *  对已绑定的手机再多一道闸门只有一个后果：记录里没有它 ⇒ 每次回连都要赌
   *  3 s 宽限（`PAIR_ADMIT_GRACE_MS`），升不上去就被踢 + 冷却 60 s。
   *
   *  现场 2026-09-23（`console.log` 07:24 会话）：记录里已经有身份地址
   *  `78D8404BF520 type=0`，但窗口一关就是成片
   *  `pair: secured peer … ignored (not in list, no window)`；同一条链路还会以
   *  RPA 形式出现（`3C382482454F`，IRK 解析没成功时）⇒ 用户看到的"每次识别的
   *  手机 MAC 都不一样 / app 关闭就无法再次连接"就是这条。窗口只在
   *  `ctl pair open` 时开，所以现场被迫反复开窗续命。
   *
   *  安全性不变档：陌生设备没有 LTK，升不到 L2，走不到这里；窗口内配对新手机
   *  那条路照旧（`myvendor_pair_note_bond()` 也照旧）。
   *
   *  自证伪：若日志里出现"未配对过的陌生设备地址进了名单"，说明 L2 并非只在
   *  已绑定链路上成立 ⇒ 那时应改成"只有 IRK 解析出的身份地址（type=0）才写"。 */
  printf("pair: secured peer %s -> record%s\n", want,
         myvendor_pair_window_active() ? "" : " (bonded, no window)");
  pair_record_write(want, peer_type);
  pair_reject_cooldown_clear(want);
}

/** @brief 有没有"已绑定手机"记录（配对窗口/定向广播/准入都用它）。 */
bool myvendor_pair_has_phone_record(void)
{
  char    hex[13];
  uint8_t type = 0;

  return pair_phone_identity(hex, sizeof(hex), &type);
}

/** @brief 定向广播目标查询（**产品路径已不用**）。
 *
 * 2026-09-22 实测：挂上 `ADV_DIRECT_IND` + SAL `DIR_ADDR_RPA` 后，SF32 稳定
 * `ble_companion: adv start failed status=2`，广播起不来且每秒空转重试。
 * 已在 `companion_start_advertising()` **关闭定向**，改回可连接无名 ADV。
 * 旧手机占坑改由 `myvendor_pair_admit` 短 grace + 拒连冷却兜底（见 board-facts）。
 * 本函数保留：身份字节序/窗口判断仍正确，仅供日后控制器支持时再评估，**勿直接挂回**。
 */
bool myvendor_pair_directed_peer(bt_address_t *out, uint8_t *out_type)
{
  char    hex[13];
  uint8_t bytes[6];
  uint8_t type = 0;
  unsigned i;

  if (myvendor_pair_window_active())
    {
      return false;               /* 窗口内保持可发现，方便首次/重配 */
    }
  if (!pair_phone_identity(hex, sizeof(hex), &type))
    {
      return false;
    }

  /* 记录里是**显示顺序**（`78:D8:40:…`），而 framework/zblue 的 `addr[]` 是最低位
   * 在前 —— 这里要倒过来填，否则广播会指向一个反过来的地址（2026-09-20 实测：
   * 打印成 `20:F5:4B:…`，手机永远看不到）。 */
  pair_hex_to_bytes(hex, bytes, 6u);
  memset(out, 0, sizeof(*out));
  for (i = 0; i < 6u; i++)
    {
      out->addr[i] = bytes[5u - i];
    }

  if (out_type != NULL)
    {
      *out_type = type;
    }
  return true;
}

/**
 * @brief 连接准入：窗口内 / 已记录手机 → 放行；否则断开（AUTH_FAIL）。
 *
 * 由 ble_companion 的 GATT connect 回调在"标记 connected"**之前**调用 ——
 * 被拒时调用方不要把它当连上（断开是异步的，连接对象随后会被栈回收），
 * 也**不要**停 ADV（`companion_stop_advertising` 在 admit 成功之后）。
 *
 * **旧手机占坑（换绑后）**：设备已解绑 A、绑了 B；A 系统里仍有旧绑定先连上。
 * 地址 ≠ 记录 → 以前给 30 s grace 并停 ADV，B 饿死。现：短 grace + 拒连冷却。
 * 定向 ADV 本可射频挡 A，SF32 不支持，靠本函数主机侧兜底。
 */
static uint32_t g_admit_deadline_ms;   /* 0 = 没有待处理的未配对连接 */
static char     g_admit_peer[13];
static bool     g_unbind_req;          /* ctl/UI 侧请求解绑，交给 companion 线程执行 */
static unsigned g_unbind_slot;         /* UINT_MAX=全部；否则 0..PAIR_PHONE_MAX-1 */

static void pair_unpair_hex(const char *hex, uint8_t type)
{
  bt_addr_le_t addr;
  unsigned     i;
  unsigned     bytes[6];

  pair_hex_to_bytes(hex, (uint8_t *)bytes, 6u);
  memset(&addr, 0, sizeof(addr));
  for (i = 0; i < 6u; i++)
    {
      addr.a.val[i] = ((const uint8_t *)bytes)[5u - i];
    }

  addr.type = type;
  printf("pair: unbind %s (type=%u)\n", hex, (unsigned)type);
  (void)bt_unpair(BT_ID_DEFAULT, &addr);
}

/** @brief 请求解除绑定（全部）。**只置标志**，执行见 poll。 */
void myvendor_pair_unbind_request(void)
{
  g_unbind_slot = UINT_MAX;
  g_unbind_req = true;
}

/** @brief 请求解除第 idx 台（0-based）。越界则当全部清。 */
void myvendor_pair_unbind_request_slot(unsigned idx)
{
  g_unbind_slot = idx;
  g_unbind_req = true;
}

/** @brief 是否还有排队的"配对动作"（解绑请求 / 宽限中的准入）。
 *
 *  给 companion 主循环在**跳过**这两个 poll 时打好原因用（见
 *  `ble_companion.c` 的调用点：控制器不应答时不做同步 HCI）。
 *
 *  @return true = 有排队中的动作。
 *
 *  @note 可从 companion 线程调用；两个标志都只在 companion 线程改。
 */
bool myvendor_pair_pending(void)
{
  return g_unbind_req || g_admit_deadline_ms != 0u;
}

/** @brief 执行解绑（companion 线程每拍调）。`bt_unpair` 只能在蓝牙自己的线程里调。
 *
 *  ⚠ **调用方必须先确认适配器可用**：本函数走 `bt_unpair()`，而解析列表真的
 *  写进控制器之后，那条路会经 `bt_keys_clear() → bt_id_del() →
 *  addr_res_enable()` 发**同步 HCI**；控制器静默时会把 companion 主循环钉死。
 *  见 `ble_companion.c` 的调用点与 `myvendor_pair_pending()`。 */
void myvendor_pair_unbind_poll(void)
{
  struct pair_phone_rec rec[PAIR_PHONE_MAX];
  unsigned n;
  unsigned i;

  if (!g_unbind_req)
    {
      return;
    }

  g_unbind_req = false;
  myvendor_pair_window_close();

  n = pair_phone_load(rec, PAIR_PHONE_MAX);
  if (n == 0u)
    {
      printf("pair: unbind: no phone record\n");
      return;
    }

  if (g_unbind_slot == UINT_MAX || g_unbind_slot >= n)
    {
      for (i = 0; i < n; i++)
        {
          pair_unpair_hex(rec[i].hex, rec[i].type);
        }

      (void)remove(PAIR_PHONE_TSV);
      printf("pair: phone record cleared (all %u)\n", n);
      return;
    }

  pair_unpair_hex(rec[g_unbind_slot].hex, rec[g_unbind_slot].type);
  for (i = g_unbind_slot + 1u; i < n; i++)
    {
      rec[i - 1u] = rec[i];
    }

  pair_phone_save(rec, n - 1u);
  printf("pair: phone record removed slot %u (%u left)\n",
         g_unbind_slot, n - 1u);
}

/** 拒连冷却：踢掉后同 12-hex 地址一段时间立刻拒，打断秒连占坑。 */
struct pair_reject_slot
{
  char     hex[13];
  uint32_t until_ms;              /* 0 = 空槽 */
};

static struct pair_reject_slot g_reject_cool[PAIR_REJECT_COOLDOWN_SLOTS];

static void pair_reject_cooldown_clear(const char *hex12)
{
  unsigned i;

  /* hex12 == NULL：清空全部冷却槽（配对窗口打开时用）。 */
  if (hex12 == NULL || hex12[0] == '\0')
    {
      if (hex12 == NULL)
        {
          for (i = 0; i < PAIR_REJECT_COOLDOWN_SLOTS; i++)
            {
              g_reject_cool[i].until_ms = 0u;
              g_reject_cool[i].hex[0] = '\0';
            }
        }

      return;
    }

  for (i = 0; i < PAIR_REJECT_COOLDOWN_SLOTS; i++)
    {
      if (g_reject_cool[i].until_ms != 0u &&
          strncmp(g_reject_cool[i].hex, hex12, 12u) == 0)
        {
          g_reject_cool[i].until_ms = 0u;
          g_reject_cool[i].hex[0] = '\0';
        }
    }
}

static void pair_reject_cooldown_arm(const char *hex12)
{
  unsigned i;
  unsigned victim = 0;
  uint32_t now;
  uint32_t oldest_left;

  if (hex12 == NULL || strlen(hex12) < 12u)
    {
      return;
    }

  now = myvendor_mono_ms();

  /* 已有同地址槽：刷新截止。 */
  for (i = 0; i < PAIR_REJECT_COOLDOWN_SLOTS; i++)
    {
      if (g_reject_cool[i].until_ms != 0u &&
          strncmp(g_reject_cool[i].hex, hex12, 12u) == 0)
        {
          g_reject_cool[i].until_ms = now + PAIR_REJECT_COOLDOWN_MS;
          return;
        }
    }

  /* 空槽优先；否则挤掉剩余冷却最短的。 */
  oldest_left = UINT32_MAX;
  for (i = 0; i < PAIR_REJECT_COOLDOWN_SLOTS; i++)
    {
      int32_t left;

      if (g_reject_cool[i].until_ms == 0u)
        {
          victim = i;
          break;
        }

      left = (int32_t)(g_reject_cool[i].until_ms - now);
      if (left < 0)
        {
          victim = i;
          break;
        }

      if ((uint32_t)left < oldest_left)
        {
          oldest_left = (uint32_t)left;
          victim = i;
        }
    }

  snprintf(g_reject_cool[victim].hex, sizeof(g_reject_cool[victim].hex),
           "%s", hex12);
  g_reject_cool[victim].hex[12] = '\0';
  g_reject_cool[victim].until_ms = now + PAIR_REJECT_COOLDOWN_MS;
  printf("pair: reject cooldown %s %us\n",
         g_reject_cool[victim].hex,
         (unsigned)(PAIR_REJECT_COOLDOWN_MS / 1000u));
}

static bool pair_reject_cooldown_hit(const char *hex12)
{
  unsigned i;
  uint32_t now;

  if (hex12 == NULL || hex12[0] == '\0')
    {
      return false;
    }

  now = myvendor_mono_ms();
  for (i = 0; i < PAIR_REJECT_COOLDOWN_SLOTS; i++)
    {
      if (g_reject_cool[i].until_ms == 0u)
        {
          continue;
        }

      if ((int32_t)(g_reject_cool[i].until_ms - now) <= 0)
        {
          g_reject_cool[i].until_ms = 0u;
          g_reject_cool[i].hex[0] = '\0';
          continue;
        }

      if (strncmp(g_reject_cool[i].hex, hex12, 12u) == 0)
        {
          return true;
        }
    }

  return false;
}

static void pair_admit_kick(const char *hex12, const char *why)
{
  struct bt_conn *conn;

  printf("pair: reject %s (%s)\n", hex12, why);
  conn = pair_conn_find(hex12, NULL);
  if (conn != NULL)
    {
      (void)bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
      bt_conn_unref(conn);
    }

  pair_reject_cooldown_arm(hex12);
  g_admit_deadline_ms = 0u;
}

bool myvendor_pair_admit(const bt_address_t *addr)
{
  char s[BT_ADDR_STR_LENGTH];
  char want[40];

  if (myvendor_pair_window_active())
    {
      return true;
    }

  bt_addr_ba2str(addr, s);
  pair_norm_addr(s, want, sizeof(want));
  if (strlen(want) >= 12u)
    {
      want[12] = '\0';
    }

  /* 冷却期内：立刻拒，不进 grace、调用方不标 connected、不停 ADV。 */
  if (pair_reject_cooldown_hit(want))
    {
      pair_admit_kick(want, "cooldown");
      return false;
    }

  /* 名单内任一台（最多 3）：直接放行。 */
  if (pair_phone_contains(want, NULL))
    {
      pair_reject_cooldown_clear(want);
      return true;
    }

  /* **有手机记录但地址对不上**：可能是名单内手机的 RPA，也可能是陌生人。
   * 短 grace 等 L2；升不上去 → admit_poll 踢 + 冷却。 */
  if (myvendor_pair_phone_count() > 0u)
    {
      snprintf(g_admit_peer, sizeof(g_admit_peer), "%s", want);
      g_admit_deadline_ms = myvendor_mono_ms() + PAIR_ADMIT_GRACE_MS;
      printf("pair: %s not in list -> grace %us (await L2 for bonded RPA)\n",
             want, (unsigned)(PAIR_ADMIT_GRACE_MS / 1000u));
      return true;
    }

  /* **没记录 + 窗口外**：立刻拒。 */
  pair_admit_kick(want, "no window, no record");
  return false;
}

/** @brief 宽限到期仍未加密 → 断开 + 冷却。由 companion 线程每拍调。 */
void myvendor_pair_admit_poll(void)
{
  struct bt_conn *conn;

  if (g_admit_deadline_ms == 0u ||
      (int32_t)(g_admit_deadline_ms - myvendor_mono_ms()) > 0)
    {
      return;
    }

  g_admit_deadline_ms = 0u;

  conn = pair_conn_find(g_admit_peer, NULL);
  if (conn == NULL)
    {
      /* 对端已自行断开：仍冷却该地址，防旧手机立刻再占坑。 */
      pair_reject_cooldown_arm(g_admit_peer);
      return;
    }

  /* 已绑定手机 RPA 回连：宽限内 LTK 升到 L2。清冷却，别误伤真 B。 */
  if (bt_conn_get_security(conn) >= BT_SECURITY_L2)
    {
      printf("pair: %s secured in grace (bonded reconnect)\n", g_admit_peer);
      pair_reject_cooldown_clear(g_admit_peer);
      bt_conn_unref(conn);
      return;
    }

  /* 未加密：旧手机 / 陌生人。踢掉后 gatts 断链 → companion_phone_gone →
   * adv_restart（grace 路径曾标过 connected 并停过 ADV）。 */
  bt_conn_unref(conn);
  pair_admit_kick(g_admit_peer, "grace over, not encrypted");
}


/* ── ctl 侧的"看"和"动作"都改走 companion 线程（2026-09-20）─────────────────
 *
 * 现场：`ctl pair list` 在 **ctl 线程**里 `bt_conn_foreach()` → 踩到被 BT 线程改到
 * 一半的连接链表 → `bt_conn_ref()` 的 `stlex` 打在野指针上 → MemManage panic
 * （dump: cfsr=0x82 mmfar=0x124 pc=bt_conn_ref←bt_conn_foreach_mc pid=ctl）。
 * 所以下面这两个函数**只能由 companion 线程调**（节点里的 COMPANION_TEST_PAIR_*），
 * ctl 侧只负责投命令 + 打印。
 */

/** @brief 拼 `ctl pair list` 输出的累积器（buf/cap/写入偏移/当前下标）。 */
struct pair_report_s
{
  char    *buf;
  size_t   cap;
  unsigned off;
  int      idx;
};

/**
 * @brief 往报告缓冲追加一行（printf 风格）；写满就丢弃后续内容。
 *
 * @param r   报告累积器。
 * @param fmt printf 格式串。
 */
static void pair_report_add(struct pair_report_s *r, const char *fmt, ...)
{
  va_list ap;
  int     n;

  if (r->off + 8u >= r->cap)
    {
      return;
    }

  va_start(ap, fmt);
  n = vsnprintf(r->buf + r->off, r->cap - r->off, fmt, ap);
  va_end(ap);

  if (n > 0)
    {
      r->off += (unsigned)n;
      if (r->off >= r->cap)
        {
          r->off = (unsigned)(r->cap - 1u);
        }
    }
}

/**
 * @brief `bt_conn_foreach` 回调：把一条 LE 连接渲染成报告行（给 `ctl pair list`）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data `struct pair_report_s *`（累积器）。
 */
static void pair_report_cb(struct bt_conn *conn, void *data)
{
  struct pair_report_s *r = data;
  char                  addr[BT_ADDR_LE_STR_LEN];
  const bt_security_t   sec = bt_conn_get_security(conn);

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

  if (strncmp(addr, "FF:FF:FF:FF:FF:FF", 17) == 0)
    {
      pair_report_add(r, "pair: [%d] %s invalid(no addr) -> skip\n", r->idx++, addr);
      return;
    }

  pair_report_add(r, "pair: [%d] %s role=%s sec=%u bonded=%d\n", r->idx++, addr,
                  pair_role_hint(addr), (unsigned)sec,
                  sec >= BT_SECURITY_L2 ? 1 : 0);
}

/** @brief 填"配对/连接快照"文本（**companion 线程**；ctl 只打印）。 */
void myvendor_pair_report(char *buf, size_t cap)
{
  struct pair_report_s r;

  if (buf == NULL || cap == 0u)
    {
      return;
    }

  buf[0] = '\0';
  r.buf = buf;
  r.cap = cap;
  r.off = 0;
  r.idx = 0;

  bt_conn_foreach(BT_CONN_TYPE_LE, pair_report_cb, &r);

  if (r.idx == 0)
    {
      pair_report_add(&r, "pair: no LE connection\n");
    }
  pair_report_add(&r, "pair: conns=%d\n", r.idx);
}

/* 挑一条"可用"的连接（跳过全 FF 的中间态、跳过传感器）。 */
static struct bt_conn *g_auto_pick;

/**
 * @brief `bt_conn_foreach` 回调：挑第一条"不是传感器"的连接（供设备侧主动配对）。
 *
 * @param conn bt_conn_foreach 已引用的连接。
 * @param data 未用。
 */
static void pair_auto_cb(struct bt_conn *conn, void *data)
{
  char addr[BT_ADDR_LE_STR_LEN];
  bt_address_t a;

  (void)data;
  if (g_auto_pick != NULL)
    {
      return;
    }

  bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
  if (strncmp(addr, "FF:FF:FF:FF:FF:FF", 17) == 0)
    {
      return;
    }

  memset(&a, 0, sizeof(a));
  memcpy(a.addr, bt_conn_get_dst(conn)->a.val, BT_ADDR_LENGTH);
  {
    extern bool ble_sensor_owns_addr(const bt_address_t *addr);

    if (ble_sensor_owns_addr(&a))
      {
        return;                 /* 心率带/踏频/功率计不是配对对象 */
      }
  }

  g_auto_pick = bt_conn_ref(conn);
}

/** @brief 设备侧发起配对：挑"不是传感器"的那条连接做 set_security(L2)。
 *  @return true = 请求已提交。 */
bool myvendor_pair_connect_auto(void)
{
  char addr[BT_ADDR_LE_STR_LEN];

  g_auto_pick = NULL;
  bt_conn_foreach(BT_CONN_TYPE_LE, pair_auto_cb, NULL);

  if (g_auto_pick == NULL)
    {
      return false;
    }

  bt_addr_le_to_str(bt_conn_get_dst(g_auto_pick), addr, sizeof(addr));
  printf("pair: connect %s set_security(L2) ret=%d\n", addr,
         bt_conn_set_security(g_auto_pick, BT_SECURITY_L2));
  bt_conn_unref(g_auto_pick);
  g_auto_pick = NULL;
  return true;
}

/**
 * @brief `ctl pair accept on|off`：注册/注销 auth 回调并设置"自动同意"。
 *
 * 注销（`on=false`）后 zblue 回到"无回调 = 自动接受"，`g_auto_accept` 也跟着变，
 * 用来复现"对端看到配对被拒绝"的路径。
 *
 * @param on true = 注册回调并自动同意；false = 注销并拒绝。
 * @return 0 = 成功；1 = 框架返回错误。
 */
static int pair_cmd_accept(bool on)
{
  int ret = 0;

  if (on && !g_auth_registered)
    {
      ret = bt_conn_auth_cb_register(&g_auth_cb);
      if (ret == 0)
        {
          g_auth_registered = true;
        }
    }
  else if (!on && g_auth_registered)
    {
      ret = bt_conn_auth_cb_register(NULL);
      if (ret == 0)
        {
          g_auth_registered = false;
        }
    }

  g_auto_accept = on;
  printf("pair: accept=%d auth_cb=%d ret=%d\n", on ? 1 : 0,
         g_auth_registered ? 1 : 0, ret);
  return ret == 0 ? 0 : 1;
}

/**
 * @brief `ctl pair …` 的 NSH 入口（`list` / `connect` / `accept` / `open` / `phone` /
 *        `unbind` / `forget`）。
 *
 * 两条纪律在这里体现：
 *   · **不在 ctl 线程遍历连接链表** —— `list` 只投
 *     `COMPANION_TEST_PAIR_LIST`，等 companion 线程把快照填进 bridge 后打印
 *     （2026-09-20 的 `bt_conn_ref→stlex` panic 就是 ctl 线程直接遍历造成的）；
 *   · **框架调用只置标志** —— `open` / `unbind` 落到
 *     `myvendor_pair_window_take_restart()` / `myvendor_pair_unbind_poll()`，
 *     由 companion 线程真正执行（在 ctl 线程直接调会撞 libuv 断言）。
 *
 * @param argc 参数个数（不含 "pair"）。
 * @param argv 参数数组；argv[0] 为子命令。
 * @return 0 = 成功；1 = 用法错误 / 失败。
 */
int myvendor_bt_pair_cmd(int argc, char ** argv)
{
  /* 默认就自动同意配对：`g_auth_registered` 是 RAM 里的（重启后清零），
   * 所以每次用这个命令前先把回调注册上 —— 否则重启后 `accept=1` 但 `auth_cb=0`，
   * 实际走的是 zblue 的默认（无人应答）路径。 */
  if (!g_auth_registered)
    {
      (void)pair_cmd_accept(true);
    }

  if (argc < 1)
    {
      printf("usage: ctl pair list | connect <idx|addr> | accept on|off | open [秒] | phone | unbind | forget\n");
      return 1;
    }

  if (strcmp(argv[0], "list") == 0)
    {
      /* **不要在 ctl 线程遍历连接**（会踩半截链表 panic）：让 companion 线程填快照，
       * 这里只等一拍再打印（见 myvendor_pair_report 的注释 + dump 证据）。 */
      char     out[320];
      uint32_t gen = 0;
      unsigned i;

      companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_LIST);
      for (i = 0; i < 30u; i++)
        {
          if (companion_bridge_pair_report_get(out, sizeof(out), &gen))
            {
              break;
            }
          usleep(20000);        /* 20 ms × 30 = 最长等 600 ms */
        }
      companion_bridge_pair_report_get(out, sizeof(out), &gen);
      printf("%s", out);
      printf("pair: auto_accept=%d auth_cb=%d\n", g_auto_accept ? 1 : 0,
             g_auth_registered ? 1 : 0);
      return 0;
    }

  if (strcmp(argv[0], "open") == 0)
    {
      myvendor_pair_window_open(argc >= 2 ? (unsigned)atoi(argv[1]) : 0u);
      return 0;
    }

  if (strcmp(argv[0], "phone") == 0)
    {
      struct pair_phone_rec rec[PAIR_PHONE_MAX];
      unsigned n = pair_phone_load(rec, PAIR_PHONE_MAX);
      unsigned i;

      if (n == 0u)
        {
          printf("pair: phone record absent (window=%d)\n",
                 myvendor_pair_window_active() ? 1 : 0);
        }
      else
        {
          printf("pair: phones %u/%u window=%d\n", n,
                 (unsigned)PAIR_PHONE_MAX,
                 myvendor_pair_window_active() ? 1 : 0);
          for (i = 0; i < n; i++)
            {
              printf("  [%u] %s type=%u\n", i, rec[i].hex,
                     (unsigned)rec[i].type);
            }
        }

      return 0;
    }

  if (strcmp(argv[0], "unbind") == 0)
    {
      /* 真解绑（清密钥池）+ 清记录；实际执行在 companion 线程（框架约束）。 */
      myvendor_pair_unbind_request();
      return 0;
    }

  if (strcmp(argv[0], "forget") == 0)
    {
      /* 只清"已绑定手机"这条记录（定向广播/准入用），不动 zblue 的密钥池 —— 真解绑
       * 还得让手机侧"忽略设备"或走 bt_unpair。 */
      myvendor_pair_note_bond(NULL, false);
      return 0;
    }

  if (strcmp(argv[0], "connect") == 0)
    {
      /* 设备侧发起配对：**同样交给 companion 线程**（要遍历连接挑一条，ctl 线程不能碰）。
       * 现在的语义是 auto（跳过全 FF 中间态、跳过传感器）；带地址前缀的挑法留给设备 UI。 */
      (void)argc;
      (void)argv;
      companion_bridge_sensor_cmd_post(COMPANION_TEST_PAIR_CONNECT);
      printf("pair: connect requested (companion 执行；结果看日志 / ctl pair list)\n");
      return 0;
    }

  if (strcmp(argv[0], "accept") == 0 && argc >= 2)
    {
      return pair_cmd_accept(strcmp(argv[1], "on") == 0);
    }

  printf("usage: ctl pair list | connect <idx|addr> | accept on|off | open [秒] | phone | unbind | forget\n");
  return 1;
}

/* 开机默认：注册 auth 回调 + 自动同意。这样 app / 系统的配对请求会被自动确认，
 * 不必每次重启后手工 `ctl pair accept on`（2026-09-19 实测需要它才能配上手机）。 */
void myvendor_bt_pair_autoinit(void)
{
  if (g_auth_registered)
    {
      return;
    }

  if (bt_conn_auth_cb_register(&g_auth_cb) == 0)
    {
      g_auth_registered = true;
      g_auto_accept     = true;
      printf("pair: autoinit ok (auth_cb=1 auto_accept=1)\n");
    }
}
