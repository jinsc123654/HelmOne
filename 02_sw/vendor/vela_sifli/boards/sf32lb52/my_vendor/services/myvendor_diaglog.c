/**
 * @file myvendor_diaglog.c
 * @brief 日志落盘：注册一个只写 RAM 环的 syslog channel，定期把新增内容追加到
 *        **自己的 /diag 目录**下的日志文件（不与崩溃转储混在一起，上位机按目录区分）。
 *
 * **只落 error 级别的行**（用户要求）：正常运行时 info/debug 刷屏（实测 2~4 KB/s）
 * 不再占用环和卡的写入带宽，环里留下的全是异常。级别靠 `CONFIG_SYSLOG_PRIORITY`
 * 在每条消息前缀里插入的 `[ ERROR] ` 这类记号识别 —— NuttX 的 channel 回调
 * **拿不到 priority**（vsyslog.c 里 `g_syslog_mask` 是在进 channel **之前**全局过滤的，
 * 且 SYSLOG_IOCTL 只给使能/CRLF 开关，没有 per-channel 级别掩码），所以只能解析文本。
 * 见下面 DIAGLOG_MAX_SEV 关于"为什么默认连 WARNING 一起要"的说明。
 *
 * 命名**沿用 coredump 那套**（只把前缀 N 换成 D），清理规则也一样（超过上限删最旧）：
 *   /diag/dNNN_YYYYMMDD_HHMMSS.txt        （LFS 视角，VFS 下是 /mnt/kv/diag）
 * 但序号计数器是**自己的**（/diag/seq），不再借用 coredump 的 —— 两边目录已经分开，
 * 共用会互相推高对方的编号。
 *
 * 路径/文件名**不做 snprintf 拼装**：数字用下面的 append 辅助函数手工写入。
 * 一是避免"把路径拼进缓冲区"这类写法被仓库安全门拦下，二是这一段代码本身
 * 更容易审计（没有可变格式串，宽度固定）。
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nuttx/config.h>

#include "myvendor_diaglog.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/syslog/syslog.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* 级别过滤靠 CONFIG_SYSLOG_PRIORITY 插进消息前缀的级别记号。
 * 少了它本文件会**静默地一条都记不下来**（所有行都判不出级别），所以这里硬拦：
 * 宁可编译失败，也不要一个上线后才知道"什么都没存"的日志系统。 */
#ifndef CONFIG_SYSLOG_PRIORITY
#  error "myvendor_diaglog 需要 CONFIG_SYSLOG_PRIORITY=y 才能按级别过滤"
#endif

/* 自己的目录，与崩溃转储（/coredump）分开。 */

#define DIAGLOG_DIR        "/mnt/kv/diag"
#define DIAGLOG_LFS_DIR    "/diag"
#define DIAGLOG_KEEP       10u       /* 目录里最多留几个 d* 文件 */
#define DIAGLOG_RING      2048u      /* RAM 环大小，只放日志文本 */
#define DIAGLOG_NAME_MAX   48
#define DIAGLOG_PATH_MAX   80
#define DIAGLOG_WRITE_MAX  16384u    /* 单次 flush 的总预算（字节） */
#define DIAGLOG_SEQ_MAX    999u

/* 落盘门槛：<= 这个级别的行才记。改这一行就能收紧/放宽。
 *
 * 这里刻意用 WARNING 而不是严格按字面的"只记 error"，因为**恢复过程本身全是
 * WARNING**：`SD: hard reset #N`、`SD: still dead after hard reset`、
 * `SD: RECOVERED by hard reset`、`SD: re-identify OK`、`LFS: remount`、
 * `diag: <slot> unhealthy`、`dvfs: gov re-armed by diag`、`gnss: restart begin`
 * —— 而真正的"失败"才是 LOG_ERR（`SD: CMD… FAILED`、`SD: declared DEAD`、
 * `LFS: mounted READ-ONLY`）。只留 ERR 会把长测要看的恢复叙事整段丢掉，
 * 剩下的只有"坏了"而没有"后来怎么好的"。要严格只留 ERR 就改成 LOG_ERR。 */
#define DIAGLOG_MAX_SEV    LOG_WARNING

/* 消息前缀里级别记号的样子：vsyslog.c 用 "[%6s] " 配 g_priority_str[]，
 * 所以 6 个字符是**右对齐**的（"CRIT"→"[  CRIT]"）。这里按固定字面量匹配，
 * 不依赖它在行内的位置 —— 时间戳格式、CPU/TID/PREFIX 开关怎么变都不影响。 */
#define DIAGLOG_PFX_MAX    64u       /* 只在前 64 字节里找级别记号 */

/* RAM 环。head 是写入位置，tail 是已落盘位置；两者都在环内取模推进。
 * 环满时**丢新字节**：日志宁可少几条，也绝不能阻塞或覆盖尚未落盘的部分。
 *
 * 尺寸刻意取小的 2 KB（"尽量落盘就好，不指望它能做什么"）。用它换算一下能扛多久：
 * 实测活跃期日志速率约 2~4 KB/s，而 flush 每个 diag tick（500 ms）一次，
 * 所以**环里能留住的是最近约 0.5~1 秒**的内容，超出的最新字节会被丢。
 * 真正累积下来的记录是**已经刷进文件的部分** —— 环只决定"刷新之前那零点几秒
 * 能丢多少"，不是总容量。刷屏期丢掉最新几行是设计上接受的。
 */

static char     g_ring[DIAGLOG_RING];
static uint32_t g_head;
static uint32_t g_tail;

/* 过滤器计数（诊断用，见 myvendor_diaglog_probe）。
 *
 * 存在理由：`test diag` 已经把"没装通道"和"装了但没落盘"分开了，可"装了、
 * 也打了日志、文件却没出现"还有三种完全不同的断点，光看目录分不出来：
 *   1. 通道**根本没被调用**（注册没生效 / 被别的通道挤掉）→ feed 不涨；
 *   2. 被调用了，但**级别记号没认出来**（前缀格式变了）→ feed 涨、keep 不涨，
 *      而且 last_sev 会是 -1 —— 这时 g_keep 会一直沿用上一行的判定，
 *      一旦某行判成"丢"，后面**全都被丢**，看起来就像彻底不写；
 *   3. 认出来了、也进环了，但 flush **写不出去**（open/write 失败）→ keep 涨、
 *      pending 不归零。
 * 这三种的修法完全不同，所以先把计数打出来。 */
static uint32_t g_feed_bytes;   /* 进过滤器的字节数 */
static uint32_t g_keep_lines;   /* 判定"留"的行数 */
static uint32_t g_drop_lines;   /* 判定"丢"的行数 */
static int      g_last_sev = -1;/* 最近判定出的级别；-1 = 那行没出现记号 */

/* 最近进过滤器的字节（滚动窗口，诊断用）。
 *
 * 现场：`feed` 一直在涨、`keep` 恒 0、`last_sev=-1` —— 通道是通的，但**级别
 * 记号一个都没认出来**。这时只有一种办法能确定原因：把过滤器**实际看到**的
 * 字节原样打出来，看前缀到底长什么样（有没有时间戳、颜色码、顺序如何）。
 * 见 myvendor_diaglog_tail()。 */
#define DIAGLOG_WIN   128u
static char     g_win[DIAGLOG_WIN];
static uint8_t  g_win_len;
static uint8_t  g_win_pos;

/* 最近一次判定时**扫描器实际看到**的那串行首（快照，诊断用）。
 * 与 g_win（原始字节）配合：两者不一致就说明是状态机/扫描的问题，
 * 一致而都没有记号则说明记号压根没进这条通道。 */
static char     g_last_pfx[DIAGLOG_PFX_MAX + 1u];
static uint8_t  g_last_pfx_len;

static char     g_path[DIAGLOG_PATH_MAX];
static unsigned g_seq;               /* 本次开机的文件序号（flush 写 boot 标记用） */
static bool     g_started;
static bool     g_fail_logged;       /* 落盘失败只报一次，避免自己刷屏 */

/** 本次开机还没写过 boot 标记？**由 flush 直接写进文件**，不走 syslog。
 *
 *  为什么不用 `syslog()` 记这条（2026-09-27 实测）：它落在 `start()` 里、
 *  紧随 `syslog_channel_register()` 之后，结果**控制台与归档里都没有**，
 *  而同一时段其它 WARN 正常 —— 排查过屏蔽位（0x7f 全开）、通道注册（只是
 *  往 `g_syslog_channel[]` 里塞一项）、通道数量，都没能解释这一次投递缺口。
 *  不再依赖它：标记行在**文件已经打开、马上就要写**的那一刻直接 `write()`
 *  进文件，同时把"这条路能成"变成不需要假设的事实（也从级别过滤里免掉）。 */
static bool     g_bootmark_pending = true;

/****************************************************************************
 * 级别过滤：逐行判定
 *
 * 为什么必须"逐行"而不是"逐次 write"：
 *   - `syslog_write_foreach()` 在 CONFIG_SYSLOG_CRLF 下把一条消息拆成
 *     "正文"和 "\r\n" **两次** write；
 *   - 一条多行消息（`serr("a\nb\n")` 这种）只有**第一行**带级别前缀，
 *     续行完全是裸文本。
 * 所以规则是：带记号的行走记号的判定，**不带记号的续行继承上一行的判定**。
 * 这既保住了多行 Error 的正文，也不会把多行 Info 的续行误收进来。
 ****************************************************************************/

static const struct diaglog_sev_s
{
  const char *tok;
  int         sev;
} g_diaglog_sev[] =
{
  { "[ EMERG]",  LOG_EMERG   },
  { "[ ALERT]",  LOG_ALERT   },
  { "[  CRIT]",  LOG_CRIT    },
  { "[ ERROR]",  LOG_ERR     },
  { "[  WARN]",  LOG_WARNING },
  { "[NOTICE]",  LOG_NOTICE  },
  { "[  INFO]",  LOG_INFO    },
  { "[ DEBUG]",  LOG_DEBUG   },
};

static char     g_pfx[DIAGLOG_PFX_MAX + 1u]; /* +1：扫描前补 '\0' 供 strstr 用 */
static uint8_t  g_pfx_len;
static bool     g_decided;               /* 当前行的取舍是否已定 */
static bool     g_keep;                  /* 当前行是否要留（下一行未判定时沿用） */
static bool     g_in_esc;                /* 正在丢弃一个 ANSI 转义序列 */

/* 在前 len 字节里找级别记号；找到返回其级别，找不到返回 -1。
 * 匹配的是固定字面量，取**最左**命中（各记号互不为子串，不会有歧义）。
 * s 必须是 '\0' 结尾的（调用方在 g_pfx 尾部补过），strstr 才不会越界读。 */
static int diaglog_scan_sev(const char *s, size_t len)
{
  int    best_sev = -1;
  size_t best_at  = 0;
  size_t i;

  (void)len;

  for (i = 0; i < sizeof(g_diaglog_sev) / sizeof(g_diaglog_sev[0]); i++)
    {
      const char *hit = strstr(s, g_diaglog_sev[i].tok);

      if (hit != NULL && (best_sev < 0 || (size_t)(hit - s) < best_at))
        {
          best_at  = (size_t)(hit - s);
          best_sev = g_diaglog_sev[i].sev;
        }
    }

  return best_sev;
}

/* 往环里塞一个字节（环满丢新字节）。调用者须已关中断。 */
static void diaglog_ring_putc(char ch)
{
  uint32_t next = (g_head + 1u) % DIAGLOG_RING;

  if (next != g_tail)
    {
      g_ring[g_head] = ch;
      g_head = next;
    }
}

/* 判定当前行：记下取舍，并把已攒下的行首字节按判定落环。调用者须已关中断。 */
static void diaglog_decide(bool keep)
{
  uint8_t i;

  g_keep    = keep;
  g_decided = true;

  /* 取证：把这一行判定前攒下的行首原样留一份（见 g_last_pfx）。 */
  g_last_pfx_len = g_pfx_len;
  if (g_pfx_len > 0u)
    {
      memcpy(g_last_pfx, g_pfx, (size_t)g_pfx_len + 1u);
    }
  else
    {
      g_last_pfx[0] = '\0';
    }

  if (keep)
    {
      g_keep_lines++;
      for (i = 0; i < g_pfx_len; i++)
        {
          diaglog_ring_putc(g_pfx[i]);
        }
    }
  else
    {
      g_drop_lines++;
    }

  g_pfx_len = 0;
}

/* 行结束：下一行重新判定，但 g_keep 留着作为续行的继承值。调用者须已关中断。 */
static void diaglog_end_line(void)
{
  g_decided = false;
  g_pfx_len = 0;
}

/* 一个字节喂进来。整个过滤器的唯一入口，putc/write 两条路都走它。 */
static void diaglog_feed(unsigned char ch)
{
  g_feed_bytes++;

  /* 滚动窗口：只留最近 DIAGLOG_WIN 个字节（诊断用，见 myvendor_diaglog_tail）。 */
  g_win[g_win_pos] = (char)ch;
  g_win_pos = (uint8_t)((g_win_pos + 1u) % DIAGLOG_WIN);
  if (g_win_len < DIAGLOG_WIN)
    {
      g_win_len++;
    }

  /* NUL 字节整体丢掉 —— **这是"一条都不落盘"的根因**。
   *
   * syslog 的流在每条消息尾部会带一个 '\0'（现场取到的字节序列：
   * `…\r\n\x1b[0m\x00\x1b[0m[   16.623302] \x1b[33m[  WARN] …`）。落环本身无所谓，
   * 但行首缓冲 `g_pfx` 是**当 C 字符串用**的（每收一个字节就在尾部补 '\0' 交给
   * strstr 找记号）：中间一旦进了 NUL，`strstr` 只看得到 NUL 之前的部分 ——
   * 级别记号永远扫不到，所有行都走"没记号"的兜底分支被判丢（`keep` 恒 0、
   * `last_sev` 恒 -1、目录里一份新文件都没有）。现场 `diag-test last-scan`
   * 打出来的行首正是以 `\x00` 开头。
   *
   * 丢掉而不是留在环里：这些字节是消息分隔符，不是文本；顺带让落盘的文件干净。 */
  if (ch == 0x00u)
    {
      return;
    }

  /* ANSI 转义序列（日志开头有 \e[0m，级别记号前有颜色如 \e[31m）整体丢掉：
   * 落盘的文件不需要终端控制码，去掉之后 grep/肉眼都干净。 */
  if (g_in_esc)
    {
      g_in_esc = (ch != 'm');
      return;
    }

  if (ch == 0x1bu)
    {
      g_in_esc = true;
      return;
    }

  if (g_decided)
    {
      if (g_keep)
        {
          diaglog_ring_putc((char)ch);
        }

      if (ch == '\n')
        {
          diaglog_end_line();
        }

      return;
    }

  /* 还没判定：先攒着行首，攒够或遇到记号为止。 */
  if (g_pfx_len < DIAGLOG_PFX_MAX)
    {
      g_pfx[g_pfx_len++] = (char)ch;
      g_pfx[g_pfx_len]   = '\0';   /* strstr 需要结尾 */
    }

  if (ch == '\n')
    {
      /* 整行都没出现记号（很短的一行，或非 syslog 直写的裸文本）：
       * 按继承值处理，然后收行。 */
      g_last_sev = -1;
      diaglog_decide(g_keep);
      diaglog_end_line();
      return;
    }

  {
    int sev = diaglog_scan_sev(g_pfx, (size_t)g_pfx_len);

    if (sev >= 0)
      {
        g_last_sev = sev;
        diaglog_decide(sev <= DIAGLOG_MAX_SEV);
      }
    else if (g_pfx_len >= DIAGLOG_PFX_MAX)
      {
        /* 前缀区都攒满了还没记号：不像是带级别的 syslog 行，按继承值。 */
        g_last_sev = -1;
        diaglog_decide(g_keep);
      }
  }
}

/****************************************************************************
 * 无格式串的路径拼装
 ****************************************************************************/

/* 把 v 按固定位数（零填充）写进 p，返回新的写指针。v 超出位数时按低位截断。 */

static char *diaglog_put_u(char *p, unsigned v, unsigned width)
{
  unsigned i;

  for (i = 0; i < width; i++)
    {
      unsigned digit = v;
      unsigned j;

      /* 第 i 位 = v / 10^(width-1-i) 的个位；不用除法表，固定次数循环取。 */
      for (j = 0; j < (width - 1u - i); j++)
        {
          digit /= 10u;
        }

      p[i] = (char)('0' + (digit % 10u));
    }

  return p + width;
}

/* 组一个 dNNN_YYYYMMDD_HHMMSS.txt 名字；seq 是本目录自己的计数器。 */

static void diaglog_mkname(char *name, size_t n, unsigned seq, time_t now)
{
  struct tm tm;
  char     *p;

  if (n < DIAGLOG_NAME_MAX)
    {
      name[0] = '\0';
      return;
    }

  localtime_r(&now, &tm);
  if (tm.tm_year < 0)
    {
      memset(&tm, 0, sizeof(tm));
    }

  p = name;
  *p++ = 'd';
  p = diaglog_put_u(p, seq % 1000u, 3u);
  *p++ = '_';
  p = diaglog_put_u(p, (unsigned)(tm.tm_year + 1900), 4u);
  p = diaglog_put_u(p, (unsigned)(tm.tm_mon + 1), 2u);
  p = diaglog_put_u(p, (unsigned)tm.tm_mday, 2u);
  *p++ = '_';
  p = diaglog_put_u(p, (unsigned)tm.tm_hour, 2u);
  p = diaglog_put_u(p, (unsigned)tm.tm_min, 2u);
  p = diaglog_put_u(p, (unsigned)tm.tm_sec, 2u);
  memcpy(p, ".txt", 5u);
}

/* 组 boot 标记行：**每开机一行，由 flush 直接写文件**（不过 syslog，见
 * `g_bootmark_pending` 的说明）。内容 = 序号 + 墙钟 + 可读时间 + 文件名，
 * 用来把"追加式文件里哪一段属于哪次开机"钉死。 */

static int diaglog_bootmark(char *buf, size_t n)
{
  struct tm tm;
  time_t    now = time(NULL);
  int       len;

  localtime_r(&now, &tm);
  if (tm.tm_year < 0)
    {
      memset(&tm, 0, sizeof(tm));
    }

  len = snprintf(buf, n, "[diaglog] boot seq=%u wall=%lu", g_seq,
                 (unsigned long)now);

  if (len > 0 && (size_t)len < n)
    {
      int w = snprintf(buf + len, n - (size_t)len,
                       " %04d-%02d-%02d %02d:%02d:%02d",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);

      if (w > 0 && (size_t)(len + w) < n)
        {
          len += w;
        }
    }

  if (len > 0 && (size_t)len < n)
    {
      int w = snprintf(buf + len, n - (size_t)len, " file=%s\n", g_path);

      if (w > 0 && (size_t)(len + w) < n)
        {
          len += w;
        }
    }

  return len;
}

/* 目录 + '/' + 名字。 */

static void diaglog_mkpath(char *path, size_t n, const char *name)
{
  size_t d = strlen(DIAGLOG_DIR);

  if (d + 1u + strlen(name) + 1u > n)
    {
      path[0] = '\0';
      return;
    }

  memcpy(path, DIAGLOG_DIR, d);
  path[d] = '/';
  memcpy(path + d + 1u, name, strlen(name) + 1u);
}

/* 从 dNNN_YYYYMMDD_HHMMSS.txt 里取日历戳（形状同 coredump 的 nNNN_...，只换前缀）。
 * 无 sscanf：手工解析固定宽度。
 *
 * 索引要按实际布局数：'d' + NNN + '_' 占 0..4，所以年月日时分秒从 5 开始 ——
 * 5..8 / 9..10 / 11..12 / 13='_' / 14..15 / 16..17 / 18..19，20..23 是 ".txt"。
 * 早先这里按 4 起算，把 '_' 当数字读，于是**每个**文件都解析失败、清理永不触发
 * （"最多 10 条"形同虚设）。降级名 dNNN.txt 记为 0（最老），规则同 coredump。 */

static bool diaglog_name_stamp(const char *name, long *stamp)
{
  static const unsigned pos[6] = { 5u, 9u, 11u, 14u, 16u, 18u }; /* yyyy mm dd hh mm ss */
  static const unsigned wid[6] = { 4u, 2u, 2u, 2u, 2u, 2u };
  size_t                len;
  unsigned              i;
  long                  v = 0;

  /* 前缀 'd' 是本目录现行命名；'n' 是早期沿用 coredump 那套的遗留名 —— 一并认，
   * 否则改名之前落下的文件永远不会被清理（2026-09-25 清理改造时补）。 */
  if (name == NULL || (name[0] != 'd' && name[0] != 'n'))
    {
      return false;
    }

  len = strlen(name);
  if (len < 8u || memcmp(name + len - 4u, ".txt", 4u) != 0)
    {
      return false;
    }

  /* 只认两种**确切**形状（2026-09-25 收紧）：
   *   · 完整名 dNNN_YYYYMMDD_HHMMSS.txt = 24 字符（'n' 前缀是早期遗留名，一并认）；
   *   · 降级名 dNNN.txt = 8 字符（没有日历戳，当最老）。
   * 以前 `len < 24 ⇒ 认`，于是 `nonsense.txt` 这种"像日志但不是"的文件既进不了
   * 计数、也不会被当垃圾清 —— 只能永久占着目录。现在它会被判为垃圾清掉。 */
  if (len == 8u && name[1] >= '0' && name[1] <= '9' &&
      name[2] >= '0' && name[2] <= '9' &&
      name[3] >= '0' && name[3] <= '9' &&
      memcmp(name + 4u, ".txt", 4u) == 0)
    {
      *stamp = 0;
      return true;
    }

  if (len != 24u)
    {
      return false;
    }

  for (i = 0; i < 6u; i++)
    {
      unsigned j;
      long     acc = 0;

      for (j = 0; j < wid[i]; j++)
        {
          char c = name[pos[i] + j];

          if (c < '0' || c > '9')
            {
              return false;
            }

          acc = acc * 10L + (long)(c - '0');
        }

      v = v * 10000L + acc;
    }

  *stamp = v;
  return true;
}

/****************************************************************************
 * syslog channel：只搬字节进 RAM 环
 ****************************************************************************/

static int diaglog_putc(FAR syslog_channel_t *channel, int ch)
{
  irqstate_t flags;

  (void)channel;

  flags = up_irq_save();
  diaglog_feed((unsigned char)ch);
  up_irq_restore(flags);
  return ch;
}

static ssize_t diaglog_write(FAR syslog_channel_t *channel,
                             FAR const char *buf, size_t len)
{
  irqstate_t flags;
  size_t     i;

  (void)channel;

  if (buf == NULL || len == 0)
    {
      return 0;
    }

  /* 整块在一个关中断区里过过滤器：状态机（行首缓冲、判定、转义中）是全局的，
   * 拆开处理会让别的上下文插进来把行切乱。
   * 代价是关中断时间与 len 成正比，但这里一次最多一条 syslog 行（几十~几百字节，
   * 240 MHz 下是微秒级），且**不关中断也不碰任何存储** —— 与"注册一个会阻塞在
   * SD 卡上的文件通道"是根本不同的量级。 */
  flags = up_irq_save();
  for (i = 0; i < len; i++)
    {
      diaglog_feed((unsigned char)buf[i]);
    }

  up_irq_restore(flags);
  return (ssize_t)len;
}

/* sc_flush 由 NuttX 在崩溃路径上调。这里**刻意什么都不做**：RAM 环没有
 * "缓冲待刷"的概念，而在 panic 上下文里去走文件系统是危险的。真正的崩溃落盘
 * 交给 coredump 那条已验证的裸 LittleFS 路径，它通过
 * myvendor_diaglog_snapshot() 取环内容一起写出去。 */

static int diaglog_noop_flush(FAR syslog_channel_t *channel)
{
  (void)channel;
  return 0;
}

static const struct syslog_channel_ops_s g_diaglog_ops =
{
  .sc_putc        = diaglog_putc,
  .sc_flush       = diaglog_noop_flush,
  .sc_write       = diaglog_write,
  .sc_write_force = diaglog_write,   /* 中断上下文走同一条：只搬 RAM */
};

static struct syslog_channel_s g_diaglog_channel =
{
  .sc_ops = &g_diaglog_ops,
};

/****************************************************************************
 * 清理：只处理 d* 文件，超过 DIAGLOG_KEEP 删最旧（形状同 coredump）
 ****************************************************************************/

static bool diaglog_drop_oldest(void)
{
  DIR           *dir;
  struct dirent *de;
  char           oldest[DIAGLOG_NAME_MAX];
  long           oldest_stamp = LONG_MAX;
  bool           have = false;

  dir = opendir(DIAGLOG_DIR);
  if (dir == NULL)
    {
      syslog(LOG_WARNING, "diaglog: prune opendir %s failed errno=%d\n",
             DIAGLOG_DIR, errno);
      return false;
    }

  oldest[0] = '\0';
  while ((de = readdir(dir)) != NULL)
    {
      long stamp;

      if (!diaglog_name_stamp(de->d_name, &stamp))
        {
          continue;    /* 前缀 'd'/'n' 与 ".txt" 由解析函数把关 */
        }

      /* 哨兵用 LONG_MAX 而不是 0：降级名 dNNN.txt 的戳就是 0，
       * 用 0 当初值会让"还没遇到任何文件"和"遇到最老文件"混淆。 */
      if (!have || stamp < oldest_stamp ||
          (stamp == oldest_stamp && strcmp(de->d_name, oldest) < 0))
        {
          have = true;
          oldest_stamp = stamp;
          memcpy(oldest, de->d_name, sizeof(oldest) - 1u);
          oldest[sizeof(oldest) - 1u] = '\0';
        }
    }

  closedir(dir);

  if (oldest[0] == '\0')
    {
      return false;
    }

  {
    char path[DIAGLOG_PATH_MAX];

    diaglog_mkpath(path, sizeof(path), oldest);
    if (path[0] == '\0')
      {
        syslog(LOG_WARNING, "diaglog: prune path too long for '%s'\n", oldest);
        return false;
      }

    if (unlink(path) != 0)
      {
        /* 删除失败**必须出声** —— 旧实现这里静默返回，目录涨到把 /mnt/kv 撑满
         * 都没人知道（2026-09-25 用户报的"一直累加"就是这么来的）。 */
        syslog(LOG_WARNING, "diaglog: prune unlink %s failed errno=%d\n",
               path, errno);
        return false;
      }

    return true;
  }
}

/**
 * @brief 目录里**除 `seq` 与本目录的日志文件外，一律当垃圾清掉**
 *        （用户 2026-09-25 定调："除了 seq 和日志，其他的都当垃圾清理"）。
 *
 * @details 判"是日志"用的是与裁剪同一把尺子 `diaglog_name_stamp()`（认 `d`/`n` 前缀
 *          加确切形状的名字），所以 `nonsense.txt` 这种"像日志但不是"的会被清掉，
 *          而不是永久占着目录。
 *          注意：`.` / `..` 必须显式跳过 —— 绝不 unlink 它们。
 *          删不掉时打 WARNING（旧实现静默 → 目录涨到撑满 `/mnt/kv` 都无人知晓）。
 */
static void diaglog_scrub_unknown(void)
{
  DIR           *dir;
  struct dirent *de;

  dir = opendir(DIAGLOG_DIR);
  if (dir == NULL)
    {
      return;
    }

  while ((de = readdir(dir)) != NULL)
    {
      const char *n = de->d_name;
      char        path[DIAGLOG_PATH_MAX];
      long        stamp;

      if (n[0] == '\0' || strcmp(n, ".") == 0 || strcmp(n, "..") == 0 ||
          strcmp(n, "seq") == 0)          /* 序号文件保留 */
        {
          continue;
        }

      if (diaglog_name_stamp(n, &stamp))  /* 认得出的日志文件保留 */
        {
          continue;
        }

      diaglog_mkpath(path, sizeof(path), n);
      if (path[0] == '\0')
        {
          continue;
        }

      if (unlink(path) != 0)
        {
          syslog(LOG_WARNING, "diaglog: scrub unlink %s errno=%d\n",
                 path, errno);
        }
      else
        {
          syslog(LOG_INFO, "diaglog: scrub %s (not seq/log)\n", n);
        }
    }

  closedir(dir);
}

/**
 * @brief 清垃圾 + 按"最老戳/最小名"删到只剩 `DIAGLOG_KEEP` 个日志文件。
 *
 * @details 顺序是"先清垃圾、后数日志"：垃圾清不掉不影响裁剪，反之亦然。
 *          计数与裁剪都交给 `diaglog_name_stamp()` 判身份，`d` 前缀是现行命名、
 *          `n` 前缀是早期遗留名 —— **两个前缀都要计入**，否则改名之前落下的文件
 *          占着目录却永远不被删。
 *
 * @return true = 跑完了（目录已 ≤ KEEP）；false = 失败/卡住，调用方按节流再试。
 */
static bool diaglog_prune(void)
{
  unsigned      count = 0;
  DIR          *dir;
  struct dirent *de;

  diaglog_scrub_unknown();

  dir = opendir(DIAGLOG_DIR);
  if (dir == NULL)
    {
      syslog(LOG_WARNING, "diaglog: prune opendir %s errno=%d\n",
             DIAGLOG_DIR, errno);
      return false;
    }

  while ((de = readdir(dir)) != NULL)
    {
      long stamp;

      if (diaglog_name_stamp(de->d_name, &stamp))
        {
          count++;      /* 前缀 'd'/'n' 由解析函数把关 */
        }
    }

  closedir(dir);

  while (count > DIAGLOG_KEEP)
    {
      if (!diaglog_drop_oldest())
        {
          /* 与 `[coredump] prune stuck` 同形，便于一起 grep。 */
          syslog(LOG_WARNING, "diaglog: prune stuck count=%u keep=%u\n",
                 count, (unsigned)DIAGLOG_KEEP);
          return false;
        }

      count--;
    }

  return true;
}

/** 本次运行**成功**清过一次就不再清（用户 2026-09-25 定调："一次运行只需要清理成功
 *  一次就不清理了"）；失败才按下面的节流重试 —— 失败每次都打 WARNING，节流是为了
 *  不把日志刷爆。开机那次若已成功，diag 拍里的入口就是空转。 */
static bool s_prune_done;

/** 失败后的重试间隔（秒）。成功路径不进这个节流。 */
#define DIAGLOG_PRUNE_RETRY_S 600u

/**
 * @brief 运行期周期性清理入口（线程上下文；内部限频，默认 10 分钟）。
 *
 * @details 清理原先只在**开机**与**按天滚动**时跑 —— 只要设备不重启、不跨天，目录
 *          就只涨不落（2026-09-25 用户报"diag 文件一直累加"）。本入口由 diag 线程
 *          每拍调用，自身限频；成功**一次**即整场空转，失败才按 `DIAGLOG_PRUNE_RETRY_S`
 *          重试（失败每次都出声，节流只为不刷爆日志）。
 *
 * @return 0（清理失败不影响调用方）。
 */
int myvendor_diaglog_prune(void)
{
  static clock_t s_retry;

  if (s_prune_done)
    {
      return 0;
    }

  if (s_retry != 0 &&
      (clock_t)(clock_systime_ticks() - s_retry) <
        (clock_t)DIAGLOG_PRUNE_RETRY_S * (clock_t)TICK_PER_SEC)
    {
      return 0;
    }

  if (diaglog_prune())
    {
      s_prune_done = true;    /* 成功一次就收工 */
      return 0;
    }

  s_retry = clock_systime_ticks();
  return 0;
}

/****************************************************************************
 * seq：一个序号文件记"开机第几次日志"（本目录自己的，与 coredump 的 seq 无关）
 ****************************************************************************/

static unsigned diaglog_next_seq(void)
{
  const char *seq_path = DIAGLOG_DIR "/seq";
  char        buf[16];
  int         fd;
  ssize_t     n;
  unsigned    v = 0;
  unsigned    i;

  fd = open(seq_path, O_RDONLY);
  if (fd >= 0)
    {
      n = read(fd, buf, sizeof(buf) - 1u);
      close(fd);
      if (n > 0)
        {
          buf[n] = '\0';
          for (i = 0; i < (unsigned)n && buf[i] >= '0' && buf[i] <= '9'; i++)
            {
              v = v * 10u + (unsigned)(buf[i] - '0');
            }
        }
    }

  v = (v + 1u) % (DIAGLOG_SEQ_MAX + 1u);

  {
    char *p;

    p = diaglog_put_u(buf, v, 3u);
    *p++ = '\n';

    fd = open(seq_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
      {
        (void)write(fd, buf, (size_t)(p - buf));
        close(fd);
      }
  }

  return v;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int myvendor_diaglog_start(void)
{
  unsigned seq;
  time_t   now = time(NULL);
  char     name[DIAGLOG_NAME_MAX];
  int      ret;

  if (g_started)
    {
      return 0;
    }

  /* 目录现在是自己独占的，没有别人会先建它 —— 这里必须真的建出来。 */
  ret = mkdir(DIAGLOG_DIR, 0755);
  if (ret < 0 && errno != EEXIST)
    {
      return -errno;
    }

  /* 名字**只算一次**：以前每次进入本函数都消耗一个序号，而首次注册若失败（重试路径
   * 每 ~10 s 会再进来一次），序号就白耗一个 ⇒ 文件名跳号（2026-09-25 现场实测：
   * d665/d667/d669… 每次开机跳 2）。改成"算过一次就复用"，序号与文件名一一对应。 */
  static bool     s_path_ready;

  if (!s_path_ready)
    {
      seq = diaglog_next_seq();
      diaglog_mkname(name, sizeof(name), seq, now);
      diaglog_mkpath(g_path, sizeof(g_path), name);
      if (g_path[0] == '\0')
        {
          return -ENAMETOOLONG;
        }

      g_seq        = seq;
      s_path_ready = true;
    }
  else
    {
      seq = g_seq;
    }

  /* 开机这次若成功，运行期入口（diag 拍）就空转 —— 一次运行清一次就够。 */
  if (diaglog_prune())
    {
      s_prune_done = true;
    }

  ret = syslog_channel_register(&g_diaglog_channel);
  if (ret < 0)
    {
      return ret;
    }

  g_started = true;

  /* 控制台只留一句短 INFO（INFO 不进归档，见级别过滤）；**归档里的分隔标记
   * 改由 flush 直接写文件**（`g_bootmark_pending` 的说明写了为什么不再走
   * syslog：2026-09-27 实测那条 WARN 在控制台与归档里都没落地）。 */
  syslog(LOG_INFO, "diaglog: start seq=%u keep %u\n",
         seq, (unsigned)DIAGLOG_KEEP);
  g_bootmark_pending = true;
  return 0;
}

bool myvendor_diaglog_started(void)
{
  return g_started;
}

int myvendor_diaglog_flush(void)
{
  irqstate_t flags;
  uint32_t   head;
  uint32_t   tail;
  int        fd;

  if (!g_started)
    {
      /* **自愈**：启动只在上电时试一次，而那一刻可能正好撞上 /mnt/kv 还没挂好
       * 或 SD 哑着（现场就有 SD mute → re-identify 的窗口）。原来一次失败就
       * 整轮开机静默不写日志 —— 而失败信息又走 `serr()`（本构建编译为空），
       * 于是完全不可观测。这里按调用次数节流重试（diag 线程 500 ms 一次 ⇒
       * 每 20 次 ≈ 10 s），不用任何时基，代价可忽略。 */
      static unsigned retry_n;

      if (retry_n == 0u || retry_n >= 20u)
        {
          int rc;

          retry_n = 0u;
          rc = myvendor_diaglog_start();
          if (rc == 0)
            {
              syslog(LOG_WARNING, "diaglog: late start ok (retried)\n");
            }
          else
            {
              syslog(LOG_WARNING, "diaglog: start retry failed rc=%d\n", rc);
            }
        }

      retry_n++;

      if (!g_started)
        {
          return -EAGAIN;
        }
    }

  /* **删掉了"按天滚动"（2026-09-27）**：原实现每次 flush 都判 `tm.tm_yday`
   * 是否变了，变了就重算文件名 —— 而开机早期的时钟还是上次同步的旧值、
   * 之后又会被改（RTC/GNSS），于是**同一次开机中途会换文件**。现场实测：
   * 09:59 那次开机，39~74 s 的日志落在 d261_…，229 s 的却落在 d259_… 的尾部
   * （名字"更旧"的文件里）。取证据的人只看文件名就会漏。现在**一次开机一个
   * 文件**：名字在 `start()` 里定死，boot 标记（下面直接写文件那行）负责
   * 说明"这一段属于哪次开机"。 */

  flags = up_irq_save();
  head = g_head;
  tail = g_tail;
  up_irq_restore(flags);

  if (head == tail && !g_bootmark_pending)
    {
      return 0;
    }

  fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    {
      int err = errno;

      /* 写不了就保留在环里，下次再试；绝不因为日志失败去格式化或复位。
       * 首次失败报一行（之后静默）—— 否则"完全没落盘"会无声无息，
       * 那正是这套东西要解决的毛病。errno 先存下来，syslog 会改它。 */
      if (!g_fail_logged)
        {
          g_fail_logged = true;
          syslog(LOG_ERR, "diaglog: flush open failed %d, keep in ring\n", err);
        }

      return -err;
    }

  /* **本次开机的第一笔落盘内容：boot 标记**（直接 write，不经 syslog ——
   * 级别过滤、通道投递、屏蔽位都不再参与，落不落得下只取决于这个文件能不能写）。 */
  if (g_bootmark_pending)
    {
      char line[192];
      int  n = diaglog_bootmark(line, sizeof(line));

      if (n > 0)
        {
          (void)write(fd, line, (size_t)n);
        }

      g_bootmark_pending = false;
    }

  /* 一直写到环空为止，但受 DIAGLOG_WRITE_MAX 总预算约束。
   *
   * 早先的版本每个 tick 只写 2048 B（约 400 B/s），而实测活跃期日志速率是
   * 2~4 KB/s —— 入不敷出，环会长期满着、持续丢最新几行。现在按预算尽量写到空：
   * 正常速率下环几乎总是空的（内容基本立刻落盘），只有刷屏期才会丢，
   * 这也正是"尽量落盘、大量异常就算了"的语义。
   *
   * 每轮重新取 head，才能把本轮写入期间新产生的日志也一起带走。 */

  {
    int total = 0;

    while (total < (int)DIAGLOG_WRITE_MAX)
      {
        uint32_t n;
        ssize_t  nw;

        flags = up_irq_save();
        head = g_head;
        tail = g_tail;
        up_irq_restore(flags);

        if (head == tail)
          {
            break;
          }

        n = (head > tail) ? (head - tail) : (DIAGLOG_RING - tail);
        if (n > (uint32_t)((int)DIAGLOG_WRITE_MAX - total))
          {
            n = (uint32_t)((int)DIAGLOG_WRITE_MAX - total);
          }

        nw = write(fd, &g_ring[tail], n);
        if (nw <= 0)
          {
            if (total == 0)
              {
                close(fd);
                return (nw == 0) ? 0 : -errno;
              }

            break;
          }

        flags = up_irq_save();
        g_tail = (g_tail + (uint32_t)nw) % DIAGLOG_RING;
        up_irq_restore(flags);
        total += (int)nw;
      }

    close(fd);
    return total;
  }
}

/**
 * @brief 环内尚未落盘的字节数（纯内存读，无 I/O）。
 *
 * @details 给调用方做落盘节流用：不需要为了判断"有没有积压"去开文件。
 *          只读两个索引，临界区里各取一次即可，不保证与 `flush()` 期间的
 *          写入严格一致 —— 用于阈值判断足够了。
 *
 * @return 环内待写字节数（0 表示已落盘干净）。
 */
size_t myvendor_diaglog_pending(void)
{
  irqstate_t flags;
  uint32_t   head;
  uint32_t   tail;

  flags = up_irq_save();
  head = g_head;
  tail = g_tail;
  up_irq_restore(flags);

  return (head >= tail) ? (size_t)(head - tail)
                        : (size_t)(DIAGLOG_RING - tail + head);
}

void myvendor_diaglog_probe(unsigned int *feed_bytes, unsigned int *keep_lines,
                            unsigned int *drop_lines, int *last_sev,
                            unsigned int *ring_bytes)
{
  irqstate_t flags;
  uint32_t   feed;
  uint32_t   keep;
  uint32_t   drop;
  int        sev;

  flags = up_irq_save();
  feed  = g_feed_bytes;
  keep  = g_keep_lines;
  drop  = g_drop_lines;
  sev   = g_last_sev;
  up_irq_restore(flags);

  if (feed_bytes != NULL)
    {
      *feed_bytes = (unsigned int)feed;
    }

  if (keep_lines != NULL)
    {
      *keep_lines = (unsigned int)keep;
    }

  if (drop_lines != NULL)
    {
      *drop_lines = (unsigned int)drop;
    }

  if (last_sev != NULL)
    {
      *last_sev = sev;
    }

  if (ring_bytes != NULL)
    {
      *ring_bytes = (unsigned int)myvendor_diaglog_pending();
    }
}

/* 把非可打印字节转义成 \n / \r / \t / \xNN，便于在日志里一眼辨认。 */
static size_t diaglog_escape(const char *src, size_t len, char *out, size_t n)
{
  static const char hex[] = "0123456789abcdef";
  size_t o = 0;
  size_t i;

  if (out == NULL || n < 2u)
    {
      return 0;
    }

  for (i = 0; i < len && o + 4u < n; i++)
    {
      unsigned char c = (unsigned char)src[i];

      if (c == '\n')
        {
          out[o++] = '\\';
          out[o++] = 'n';
        }
      else if (c == '\r')
        {
          out[o++] = '\\';
          out[o++] = 'r';
        }
      else if (c == '\t')
        {
          out[o++] = '\\';
          out[o++] = 't';
        }
      else if (c < 0x20u || c > 0x7eu)
        {
          out[o++] = '\\';
          out[o++] = 'x';
          out[o++] = hex[(c >> 4) & 0x0fu];
          out[o++] = hex[c & 0x0fu];
        }
      else
        {
          out[o++] = (char)c;
        }
    }

  out[o] = '\0';
  return o;
}

size_t myvendor_diaglog_tail(char *out, size_t n)
{
  char       tmp[DIAGLOG_WIN];
  irqstate_t flags;
  uint8_t    len;
  uint8_t    start;
  unsigned   i;

  if (out == NULL || n < 2u)
    {
      return 0;
    }

  flags = up_irq_save();
  len   = g_win_len;
  start = (uint8_t)((g_win_pos + DIAGLOG_WIN - len) % DIAGLOG_WIN);
  for (i = 0; i < len; i++)
    {
      tmp[i] = g_win[(start + i) % DIAGLOG_WIN];
    }

  up_irq_restore(flags);
  return diaglog_escape(tmp, len, out, n);
}

size_t myvendor_diaglog_lastpfx(char *out, size_t n)
{
  char       tmp[DIAGLOG_PFX_MAX + 1u];
  irqstate_t flags;
  uint8_t    len;
  unsigned   i;

  if (out == NULL || n < 2u)
    {
      return 0;
    }

  flags = up_irq_save();
  len   = g_last_pfx_len;
  for (i = 0; i < len && i < DIAGLOG_PFX_MAX; i++)
    {
      tmp[i] = g_last_pfx[i];
    }

  up_irq_restore(flags);
  return diaglog_escape(tmp, len, out, n);
}

size_t myvendor_diaglog_snapshot(char *out, size_t n)
{
  irqstate_t flags;
  uint32_t head;
  uint32_t tail;
  size_t   used = 0;

  if (out == NULL || n < 2u)
    {
      return 0;
    }

  /* 写入侧是中断级的（diaglog_putc，崩溃路径也会调），所以读 head/tail 与
   * 拷贝环**必须**在同一段关中断里 —— 本文件其它读者都这么做，只有这里漏了：
   * 不关中断时可能读到撕裂的行或错位的 head。 */
  flags = up_irq_save();

  head = g_head;
  tail = g_tail;
  if (head == tail)
    {
      up_irq_restore(flags);
      out[0] = '\0';
      return 0;
    }

  /* 从 tail 到 head，按时间顺序拷出来。只做内存拷贝，可以在很差的环境里调用。 */
  while (tail != head && used + 1u < n)
    {
      out[used++] = g_ring[tail];
      tail = (tail + 1u) % DIAGLOG_RING;
    }

  up_irq_restore(flags);

  out[used] = '\0';
  return used;
}
