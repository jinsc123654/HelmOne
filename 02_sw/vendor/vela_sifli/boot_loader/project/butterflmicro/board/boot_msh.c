/**
 * @file boot_msh.c
 * @brief NO_OS UART msh: ls/cat/cd on KV LittleFS, boot to NuttX.
 *
 * Not RT-Thread FinSH — same prompt and the commands needed to inspect KV.
 * Autoboot window 2 s overlaps image load: if load already took ≥2 s, jump
 * as soon as CRC passes; if load was faster, wait the remainder. 10
 * consecutive B/b abort the current read into msh; 10 consecutive F/f abort
 * and jump factory.
 * Default autoboot: newest /fw, then partition main, then factory.
 * persist.boot.target (KV) pins fw|main|factory|boot; msh `target` changes it.
 * persist.boot.pwr (ctl pwr on): skip PWR key / charge page, autoboot.
 * Plug-in (STAT low, PWR not held): charge page. KEY1+KEY2 already down
 * at plug, hold 1 s → factory (no KV); release → charge page, combo dead.
 * On the charge page, any key held 1 s boots (KEY1, KEY2, PWR, or several).
 * Both keys together still do not enter factory.
 * 10 consecutive 'B'/'b' stay in msh; 10 consecutive 'F'/'f' jump factory.
 * `app`/`boot` use the same /fw → main → factory chain (ignore KV target).
 * msh `check` CRC-verifies /fw, main, and factory without jumping.
 * sftool talks to Mask ROM UART Debug-IP (START_WORD 0x7E 0x79 + "ATSF32").
 * RTS is not UART data (USB-CDC often never asserts chip RESET).  Seeing that
 * sync triggers a whole-chip WDT reset *without* PMUC_CR_REBOOT so A4+ ROM
 * still waits ~1 s for the next sftool Enter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_msh.h"
#include "boot_lfs.h"
#include "boot_fw.h"
#include "boot_target.h"
#include "boot_hw.h"
#include "boot_sgl.h"
#include "ptab_table.h"
#include "board.h"
#include "bf0_hal.h"
#include "register.h"
#include "secboot.h"

#include <stdint.h>
#include <string.h>

#define MSH_LINE_MAX         128
#define MSH_PATH_MAX         128
#define MSH_AUTOBOOT_SEC     2
#define MSH_HOTKEY_COUNT     10 /* consecutive B/b = msh; F/f = factory */
#define MSH_BOOT_HOLD_MS     1000u

extern void boot_uart_tx(USART_TypeDef *uart, uint8_t *data, int len);

static char g_cwd[MSH_PATH_MAX] = "/";
static int (*g_prepare_fn)(void);
static void (*g_go_fn)(void);
static int g_prepared;
static int g_b_count;
static int g_f_count;
static int g_stopped;
static int g_want_factory;
static int g_factory_skip_kv;
static int g_factory_loading;
static uint32_t g_dwt0;
static uint32_t g_clk_mhz;
static int g_sftool_st; /* 0 idle, 1 saw 0x7E */

static void msh_leave_to_boot(void);
static void msh_enter_msh(const char *msg);

#define MSH_WDT_RELEASE  0x51ff8621u
#define MSH_WDT_START    0x00000076u

/*
 * Pin-like reset into Mask ROM download wait.  HAL_PMU_Reboot() sets
 * PMUC_CR_REBOOT and A4+ ROM skips that wait — do not use it for sftool.
 */
static void msh_download_reset(void)
{
    HAL_DisableInterrupt();
    hwp_pmuc->CR &= ~(PMUC_CR_REBOOT | PMUC_CR_HIBER_EN);
    __HAL_SYSCFG_Enable_WDT_REBOOT(1);
    HAL_PMU_SetWdt(WDT1_BASE);
    hwp_wdt1->WDT_WP = MSH_WDT_RELEASE;
    hwp_wdt1->WDT_CR = 0;
    hwp_wdt1->WDT_CVR0 = 0x80;
    hwp_wdt1->WDT_CVR1 = 0x80;
    hwp_wdt1->WDT_CCR = MSH_WDT_START;
    while (1)
    {
    }
}

static void msh_putc(char c)
{
    uint8_t ch = (uint8_t)c;

    boot_uart_tx(hwp_usart1, &ch, 1);
}

static void msh_puts(const char *s)
{
    boot_uart_tx(hwp_usart1, (uint8_t *)s, (int)strlen(s));
}

static void msh_uart_rx_enable(void)
{
    board_pinmux_uart();
    hwp_usart1->CR1 |= USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
}

static int msh_rx_ready(void)
{
    return (hwp_usart1->ISR & USART_ISR_RXNE) != 0;
}

static int msh_getc(void)
{
    uint8_t c;

    while (!msh_rx_ready())
    {
        boot_hw_wdt_pet();
        HAL_Delay_us(200);
    }
    c = (uint8_t)hwp_usart1->RDR;
    return (int)c;
}

static void msh_rx_drain(void)
{
    while (msh_rx_ready())
    {
        (void)hwp_usart1->RDR;
    }
}

/*
 * sftool Debug-IP frames all start 0x7E 0x79 (Enter and MEMRead/Write).
 * Swallow so msh does not echo '~'.  Do not WDT-reset here: stub download
 * uses the same START_WORD after Connected, and a reset kills that session.
 */
static int msh_rx_sftool(uint8_t c)
{
    if (c == 0x7E)
    {
        g_sftool_st = 1;
        return 1;
    }
    if (g_sftool_st)
    {
        g_sftool_st = 0;
        return 1;
    }
    return 0;
}

static void msh_prompt(void)
{
    msh_puts("msh ");
    msh_puts(g_cwd);
    msh_puts("> ");
}

/* Strip /mnt/kv so paths match NuttX (/mnt/kv/db/persist.* → /db/persist.*). */
static const char *strip_mnt(const char *p)
{
    if (p == NULL)
    {
        return "/";
    }
    if (strncmp(p, "/mnt/kv/", 8) == 0)
    {
        return p + 7; /* "/foo" */
    }
    if (strcmp(p, "/mnt/kv") == 0)
    {
        return "/";
    }
    return p;
}

static void join_path(char *dst, size_t dstsz, const char *base, const char *rel)
{
    size_t n;
    const char *r = strip_mnt(rel);

    if (r == NULL || r[0] == 0)
    {
        r = base;
        base = "/";
    }
    if (r[0] == '/')
    {
        n = strlen(r);
        if (n >= dstsz)
        {
            n = dstsz - 1;
        }
        memcpy(dst, r, n);
        dst[n] = 0;
        return;
    }

    n = strlen(base);
    if (n >= dstsz)
    {
        n = dstsz - 1;
    }
    memcpy(dst, base, n);
    dst[n] = 0;
    if (n > 1 && dst[n - 1] != '/')
    {
        if (n + 1 < dstsz)
        {
            dst[n++] = '/';
            dst[n] = 0;
        }
    }
    {
        size_t m = strlen(r);

        if (n + m >= dstsz)
        {
            m = dstsz - 1 - n;
        }
        memcpy(dst + n, r, m);
        dst[n + m] = 0;
    }
}

static void cmd_help(void)
{
    msh_puts(
        "commands:\r\n"
        "  help              this list\r\n"
        "  ver               print 2SFBL version\r\n"
        "  ls [path]         list KV LittleFS\r\n"
        "  cat <file>        print a KV file\r\n"
        "  cd [path]         change directory\r\n"
        "  pwd               print cwd\r\n"
        "  df                KV mount info\r\n"
        "  fw                list KV /fw/*.bin newest-first\r\n"
        "  check             CRC /fw, main, factory (no jump)\r\n"
        "  target [slot]     persist.boot.target: fw|main|factory|boot|off\r\n"
        "  pwr [on|off]      persist.boot.pwr: skip PWR key, autoboot\r\n"
        "  boot              jump /fw then main then factory\r\n"
        "  app               same as boot\r\n"
        "  factory           jump factory (MTP) fw now\r\n"
        "  reboot            PMU reset (skips ROM download window)\r\n"
        "  download          WDT reset into ROM UART download wait\r\n"
        "  <tab>             complete command or KV path\r\n");
}

static void cmd_ver(void)
{
    msh_puts("2SFBL ");
    msh_puts(BOOT_LOADER_VERSION);
    msh_puts("\r\n");
}

static void cmd_ls(const char *arg)
{
    char path[MSH_PATH_MAX];

    if (arg == NULL || arg[0] == 0)
    {
        boot_lfs_ls(g_cwd);
        return;
    }
    join_path(path, sizeof(path), g_cwd, arg);
    boot_lfs_ls(path);
}

static void cmd_cat(const char *arg)
{
    char path[MSH_PATH_MAX];

    if (arg == NULL || arg[0] == 0)
    {
        msh_puts("usage: cat <file>\r\n");
        return;
    }
    join_path(path, sizeof(path), g_cwd, arg);
    boot_lfs_cat(path);
}

static void cmd_cd(const char *arg)
{
    char path[MSH_PATH_MAX];

    if (arg == NULL || arg[0] == 0)
    {
        g_cwd[0] = '/';
        g_cwd[1] = 0;
        return;
    }
    join_path(path, sizeof(path), g_cwd, arg);
    if (path[0] == 0)
    {
        g_cwd[0] = '/';
        g_cwd[1] = 0;
        return;
    }
    if (strlen(path) >= sizeof(g_cwd))
    {
        msh_puts("cd: too long\r\n");
        return;
    }
    memcpy(g_cwd, path, strlen(path) + 1);
}

static const char *const g_cmds[] =
{
    "help", "ver", "ls", "cat", "cd", "pwd", "df", "fw", "check", "target",
    "boot", "app", "factory", "reboot", "download",
};

#define MSH_NCMD (sizeof(g_cmds) / sizeof(g_cmds[0]))

static void msh_append(char *line, unsigned *n, const char *s)
{
    while (*s && *n + 1u < MSH_LINE_MAX)
    {
        line[*n] = *s;
        msh_putc(*s);
        (*n)++;
        s++;
    }
}

static void msh_append_char(char *line, unsigned *n, char c)
{
    if (*n + 1u < MSH_LINE_MAX)
    {
        line[*n] = c;
        msh_putc(c);
        (*n)++;
    }
}

static void msh_redraw(char *line, unsigned n)
{
    line[n] = 0;
    msh_prompt();
    msh_puts(line);
}

static void split_dir_name(const char *abs, char *dir, size_t dirsz,
                           char *name, size_t namesz)
{
    const char *slash = strrchr(abs, '/');
    size_t dlen;
    size_t nlen;

    if (slash == NULL)
    {
        dir[0] = '/';
        dir[1] = 0;
        nlen = strlen(abs);
        if (nlen >= namesz)
        {
            nlen = namesz - 1;
        }
        memcpy(name, abs, nlen);
        name[nlen] = 0;
        return;
    }

    dlen = (size_t)(slash - abs);
    if (dlen == 0)
    {
        dir[0] = '/';
        dir[1] = 0;
    }
    else
    {
        if (dlen >= dirsz)
        {
            dlen = dirsz - 1;
        }
        memcpy(dir, abs, dlen);
        dir[dlen] = 0;
    }
    nlen = strlen(slash + 1);
    if (nlen >= namesz)
    {
        nlen = namesz - 1;
    }
    memcpy(name, slash + 1, nlen);
    name[nlen] = 0;
}

static void complete_cmd(char *line, unsigned *n, unsigned tok)
{
    unsigned pfxlen = *n - tok;
    const char *pfx = line + tok;
    char common[16];
    unsigned clen = 0;
    unsigned nmatch = 0;
    unsigned i;
    const char *uniq = NULL;

    common[0] = 0;
    for (i = 0; i < MSH_NCMD; i++)
    {
        unsigned k;

        if (strncmp(g_cmds[i], pfx, pfxlen) != 0)
        {
            continue;
        }
        nmatch++;
        uniq = g_cmds[i];
        if (nmatch == 1)
        {
            clen = (unsigned)strlen(g_cmds[i]);
            if (clen >= sizeof(common))
            {
                clen = sizeof(common) - 1;
            }
            memcpy(common, g_cmds[i], clen);
            common[clen] = 0;
        }
        else
        {
            k = 0;
            while (k < clen && common[k] && g_cmds[i][k] == common[k])
            {
                k++;
            }
            clen = k;
            common[clen] = 0;
        }
    }

    if (nmatch == 0)
    {
        msh_putc('\a');
        return;
    }
    if (nmatch == 1)
    {
        msh_append(line, n, uniq + pfxlen);
        msh_append_char(line, n, ' ');
        return;
    }
    if (clen > pfxlen)
    {
        msh_append(line, n, common + pfxlen);
        return;
    }

    msh_puts("\r\n");
    for (i = 0; i < MSH_NCMD; i++)
    {
        if (strncmp(g_cmds[i], pfx, pfxlen) != 0)
        {
            continue;
        }
        msh_puts("  ");
        msh_puts(g_cmds[i]);
        msh_puts("\r\n");
    }
    msh_redraw(line, *n);
}

static void complete_path(char *line, unsigned *n, unsigned tok)
{
    char full[MSH_PATH_MAX];
    char dir[MSH_PATH_MAX];
    char namepfx[BOOT_LFS_NAME_MAX];
    boot_lfs_comp_t comp;
    unsigned pfxlen;
    const char *token = line + tok;

    if (!boot_lfs_mounted())
    {
        msh_putc('\a');
        return;
    }

    if (token[0] == 0)
    {
        memcpy(dir, g_cwd, strlen(g_cwd) + 1);
        namepfx[0] = 0;
    }
    else
    {
        join_path(full, sizeof(full), g_cwd, token);
        split_dir_name(full, dir, sizeof(dir), namepfx, sizeof(namepfx));
    }

    if (boot_lfs_complete(dir, namepfx, &comp) != 0 || comp.nmatch == 0)
    {
        msh_putc('\a');
        return;
    }

    pfxlen = (unsigned)strlen(namepfx);
    if (comp.nmatch == 1)
    {
        msh_append(line, n, comp.common + pfxlen);
        msh_append_char(line, n, comp.unique_dir ? '/' : ' ');
        return;
    }
    if (strlen(comp.common) > pfxlen)
    {
        msh_append(line, n, comp.common + pfxlen);
        return;
    }

    msh_puts("\r\n");
    boot_lfs_complete_list(dir, namepfx);
    msh_redraw(line, *n);
}

static void msh_tab(char *line, unsigned *n)
{
    unsigned tok;
    unsigned i;
    int is_first = 1;

    line[*n] = 0;
    tok = *n;
    while (tok > 0 && line[tok - 1] != ' ' && line[tok - 1] != '\t')
    {
        tok--;
    }
    for (i = 0; i < tok; i++)
    {
        if (line[i] != ' ' && line[i] != '\t')
        {
            is_first = 0;
            break;
        }
    }

    if (is_first)
    {
        complete_cmd(line, n, tok);
    }
    else
    {
        complete_path(line, n, tok);
    }
}

static void msh_put_u32(uint32_t v)
{
    char buf[11];
    int i = 10;

    buf[10] = 0;
    if (v == 0)
    {
        msh_putc('0');
        return;
    }
    while (v && i > 0)
    {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    msh_puts(&buf[i]);
}

static void msh_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[8];
    int i;

    for (i = 7; i >= 0; i--)
    {
        buf[i] = hex[v & 0xfU];
        v >>= 4;
    }
    boot_uart_tx(hwp_usart1, (uint8_t *)buf, 8);
}

static void msh_pad_label(const char *s, unsigned width)
{
    unsigned n = (unsigned)strlen(s);

    msh_puts("  ");
    msh_puts(s);
    while (n < width)
    {
        msh_putc(' ');
        n++;
    }
}

static void check_print_result(int crc_ok, int jumpable, const char *name,
                               const struct boot_ovnx_image_info *info)
{
    msh_puts(crc_ok ? " OK  " : " FAIL");
    msh_puts("  jump=");
    msh_puts((crc_ok && jumpable) ? "yes" : "no ");
    if (name && name[0])
    {
        msh_puts("  ");
        msh_puts(name);
    }
    if (info && info->payload_len)
    {
        msh_puts("  size=");
        msh_put_u32(info->payload_len);
        msh_puts("  crc=");
        msh_put_hex32(info->payload_crc);
        if (!crc_ok)
        {
            msh_puts(" calc=");
            msh_put_hex32(info->payload_crc_calc);
        }
        if (info->version[0])
        {
            msh_puts("  ");
            msh_puts(info->version);
        }
    }
    else if (!crc_ok)
    {
        msh_puts("  (bad OVNX hdr)");
    }
    msh_puts("\r\n");
}

static int check_part(const char *img, int *ok_out)
{
    const struct ptab_entry *e;
    struct boot_ovnx_image_info info;
    int jumpable = 0;
    int ok;

    msh_pad_label(img, 8);
    e = ptab_find_img(img);
    if (e == NULL)
    {
        msh_puts(" FAIL  (no ptab)\r\n");
        *ok_out = 0;
        return -1;
    }

    memset(&info, 0, sizeof(info));
    ok = (boot_ovnx_verify_hcpu(e->addr, e->size, &info, &jumpable) == 0);
    *ok_out = ok;
    check_print_result(ok, jumpable, NULL, &info);
    return ok ? 0 : -1;
}

static void cmd_check(void)
{
    struct boot_ovnx_image_info info;
    char fw_name[BOOT_FW_NAME_MAX];
    int jump_fw = 0;
    int ok_fw = 0;
    int ok_main = 0;
    int ok_fac = 0;
    int rc;
    enum boot_target t;

    msh_puts("check (CRC, no jump)\r\n");

    msh_pad_label("/fw", 8);
    memset(&info, 0, sizeof(info));
    fw_name[0] = 0;
    rc = boot_fw_check(&info, fw_name, sizeof(fw_name), &jump_fw);
    if (rc == BOOT_FW_CHECK_OK)
    {
        ok_fw = 1;
        check_print_result(1, jump_fw, fw_name, &info);
    }
    else if (rc == BOOT_FW_CHECK_MOUNT)
    {
        msh_puts(" FAIL  (KV mount)\r\n");
    }
    else if (rc == BOOT_FW_CHECK_EMPTY)
    {
        msh_puts(" FAIL  (empty /fw)\r\n");
    }
    else
    {
        check_print_result(0, 0, fw_name[0] ? fw_name : NULL, &info);
    }

    (void)check_part("main", &ok_main);
    (void)check_part("factory", &ok_fac);

    msh_puts("autoboot would jump: ");
    t = boot_target_get();
    if (t == BOOT_TARGET_BOOT)
    {
        msh_puts("msh (kv target=boot)\r\n");
    }
    else if (t == BOOT_TARGET_FW)
    {
        msh_puts(ok_fw ? "/fw (kv)\r\n" : "none (kv target=fw failed)\r\n");
    }
    else if (t == BOOT_TARGET_MAIN)
    {
        msh_puts(ok_main ? "main (kv)\r\n" : "none (kv target=main failed)\r\n");
    }
    else if (t == BOOT_TARGET_FACTORY)
    {
        msh_puts(ok_fac ? "factory (kv)\r\n" : "none (kv target=factory failed)\r\n");
    }
    else if (ok_fw)
    {
        msh_puts("/fw\r\n");
    }
    else if (ok_main)
    {
        msh_puts("main\r\n");
    }
    else if (ok_fac)
    {
        msh_puts("factory\r\n");
    }
    else
    {
        msh_puts("none\r\n");
    }
}

static void cmd_target_usage(void)
{
    msh_puts("usage: target [fw|main|factory|boot|off]\r\n");
    msh_puts("  fw        KV /fw\r\n");
    msh_puts("  main      partition main\r\n");
    msh_puts("  factory   partition factory\r\n");
    msh_puts("  boot      stay in msh\r\n");
    msh_puts("  off       default /fw then main then factory\r\n");
}

static void cmd_target(const char *arg)
{
    enum boot_target t;
    int err;

    if (arg == NULL || arg[0] == 0)
    {
        char running[BOOT_RUNNING_VAL_MAX];

        t = boot_target_get();
        msh_puts("persist.boot.target: ");
        msh_puts(boot_target_name(t));
        msh_puts("\r\n");
        msh_puts("persist.boot.running: ");
        if (boot_running_get(running, sizeof(running)) == 0)
        {
            msh_puts(running);
        }
        else
        {
            msh_puts("(none)");
        }
        msh_puts("\r\n");
        cmd_target_usage();
        return;
    }

    if (strcmp(arg, "off") == 0 || strcmp(arg, "default") == 0 ||
        strcmp(arg, "del") == 0 || strcmp(arg, "none") == 0)
    {
        err = boot_target_set(BOOT_TARGET_NONE);
        if (err == 0)
        {
            msh_puts("target cleared (default chain)\r\n");
        }
        else
        {
            msh_puts("target: unlink failed\r\n");
        }
        return;
    }

    t = boot_target_parse(arg);
    if (t == BOOT_TARGET_NONE)
    {
        cmd_target_usage();
        return;
    }
    err = boot_target_set(t);
    if (err == 0)
    {
        msh_puts("target=");
        msh_puts(boot_target_name(t));
        msh_puts("\r\n");
    }
    else
    {
        msh_puts("target: write failed\r\n");
    }
}

static void cmd_pwr(const char *arg)
{
    if (arg == NULL || arg[0] == 0)
    {
        msh_puts("persist.boot.pwr: ");
        msh_puts(boot_pwr_auto() ? "on" : "off");
        msh_puts("\r\n");
        return;
    }
    if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0)
    {
        if (boot_pwr_set(1) == 0)
        {
            msh_puts("pwr=on (skip key, autoboot)\r\n");
        }
        else
        {
            msh_puts("pwr: write failed\r\n");
        }
        return;
    }
    if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0)
    {
        if (boot_pwr_set(0) == 0)
        {
            msh_puts("pwr=off\r\n");
        }
        else
        {
            msh_puts("pwr: write failed\r\n");
        }
        return;
    }
    msh_puts("pwr [on|off]\r\n");
}

static void cmd_jump(void)
{
    msh_puts("booting (/fw then main then factory)...\r\n");
    msh_leave_to_boot();
    g_prepared = (boot_images_prepare_chain() == 0);
    if (!g_prepared)
    {
        msh_puts("load failed\r\n");
        return;
    }
    if (g_go_fn)
    {
        g_go_fn();
    }
    msh_puts("boot returned\r\n");
}

static void dispatch(char *line)
{
    char *cmd = line;
    char *arg = NULL;
    char *p;

    while (*cmd == ' ' || *cmd == '\t')
    {
        cmd++;
    }
    if (*cmd == 0)
    {
        return;
    }
    p = cmd;
    while (*p && *p != ' ' && *p != '\t')
    {
        p++;
    }
    if (*p)
    {
        *p++ = 0;
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p)
        {
            arg = p;
        }
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
    {
        cmd_help();
    }
    else if (strcmp(cmd, "ver") == 0)
    {
        cmd_ver();
    }
    else if (strcmp(cmd, "ls") == 0)
    {
        cmd_ls(arg);
    }
    else if (strcmp(cmd, "cat") == 0)
    {
        cmd_cat(arg);
    }
    else if (strcmp(cmd, "cd") == 0)
    {
        cmd_cd(arg);
    }
    else if (strcmp(cmd, "pwd") == 0)
    {
        msh_puts(g_cwd);
        msh_puts("\r\n");
    }
    else if (strcmp(cmd, "df") == 0)
    {
        boot_lfs_print_info();
    }
    else if (strcmp(cmd, "fw") == 0)
    {
        boot_fw_list();
    }
    else if (strcmp(cmd, "check") == 0 || strcmp(cmd, "verify") == 0)
    {
        cmd_check();
    }
    else if (strcmp(cmd, "target") == 0)
    {
        cmd_target(arg);
    }
    else if (strcmp(cmd, "pwr") == 0)
    {
        cmd_pwr(arg);
    }
    else if (strcmp(cmd, "boot") == 0 || strcmp(cmd, "app") == 0)
    {
        cmd_jump();
    }
    else if (strcmp(cmd, "factory") == 0)
    {
        msh_puts("booting factory (MTP) fw...\r\n");
        boot_factory_boot();
        msh_puts("factory returned\r\n");
    }
    else if (strcmp(cmd, "reboot") == 0)
    {
        HAL_PMU_Reboot();
    }
    else if (strcmp(cmd, "download") == 0)
    {
        msh_download_reset();
    }
    else
    {
        msh_puts("unknown: ");
        msh_puts(cmd);
        msh_puts("  (help)\r\n");
    }
}

static void msh_loop(void)
{
    char line[MSH_LINE_MAX];
    unsigned n = 0;

    /* boot_running_set unmounts KV so OVNX can DMA the same SD. */
    (void)boot_lfs_mount();
    msh_puts("type help; app=/fw then main then factory\r\n");
    msh_prompt();
    while (1)
    {
        int c = msh_getc();

        if (msh_rx_sftool((uint8_t)c))
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            msh_puts("\r\n");
            line[n] = 0;
            dispatch(line);
            n = 0;
            /* factory/boot/check unmount KV for raw SD; msh needs it back. */
            if (!boot_lfs_mounted())
            {
                (void)boot_lfs_mount();
            }
            msh_prompt();
            continue;
        }
        if (c == 0x08 || c == 0x7f)
        {
            if (n > 0)
            {
                n--;
                msh_puts("\b \b");
            }
            continue;
        }
        if (c == '\t')
        {
            msh_tab(line, &n);
            continue;
        }
        if (c < 32)
        {
            continue;
        }
        if (n + 1 < sizeof(line))
        {
            line[n++] = (char)c;
            msh_putc((char)c);
        }
    }
}

/* Consecutive B/b stay in msh; consecutive F/f jump factory. Other bytes reset both. */
static void count_hotkeys(void)
{
    while (msh_rx_ready())
    {
        uint8_t c = (uint8_t)hwp_usart1->RDR;

        if (msh_rx_sftool(c))
        {
            continue;
        }
        if (c == 'B' || c == 'b')
        {
            g_b_count++;
            g_f_count = 0;
            if (g_b_count >= MSH_HOTKEY_COUNT)
            {
                g_stopped = 1;
            }
        }
        else if (c == 'F' || c == 'f')
        {
            g_f_count++;
            g_b_count = 0;
            if (g_f_count >= MSH_HOTKEY_COUNT)
            {
                g_want_factory = 1;
            }
        }
        else
        {
            g_b_count = 0;
            g_f_count = 0;
        }
    }
}

int boot_msh_poll_stop(void)
{
    count_hotkeys();
    return g_stopped;
}

int boot_msh_want_factory(void)
{
    count_hotkeys();
    return g_want_factory;
}

int boot_msh_abort_load(void)
{
    int want_fac;
    int was = (g_stopped || (g_want_factory && !g_factory_loading));

    count_hotkeys();
    want_fac = (g_want_factory && !g_factory_loading);
    if (!was && g_stopped)
    {
        msh_puts("\r\nBBB abort load\r\n");
        boot_sgl_status("stopped");
    }
    else if (!was && want_fac)
    {
        msh_puts("\r\nFFF abort load\r\n");
        boot_sgl_show_factory();
    }
    return (g_stopped || want_fac);
}

void boot_msh_factory_load_begin(void)
{
    g_factory_loading = 1;
}

void boot_msh_factory_load_end(void)
{
    g_factory_loading = 0;
}

void boot_msh_request_factory(int write_kv)
{
    g_want_factory = 1;
    g_factory_skip_kv = write_kv ? 0 : 1;
}

int boot_msh_factory_skip_kv(void)
{
    return g_factory_skip_kv;
}

static void msh_time_start(void)
{
    HAL_Delay_us(0);
#ifdef DWT
    if (HAL_DBG_DWT_IsInit() == 0)
    {
        HAL_DBG_DWT_Init();
    }
    g_clk_mhz = HAL_RCC_GetHCLKFreq(CORE_ID_DEFAULT) / 1000000u;
    if (g_clk_mhz == 0)
    {
        g_clk_mhz = 48;
    }
    g_dwt0 = HAL_DBG_DWT_GetCycles();
#else
    g_clk_mhz = 0;
    g_dwt0 = 0;
#endif
}

static uint32_t msh_elapsed_us(void)
{
#ifdef DWT
    if (g_clk_mhz == 0)
    {
        return 0;
    }
    return (HAL_DBG_DWT_GetCycles() - g_dwt0) / g_clk_mhz;
#else
    return 0;
#endif
}

/* Wait until 2 s from msh_time_start (overlaps OVNX load). If the image
 * read already used up that window, return immediately. */
static int wait_remainder(void)
{
    const uint32_t total_us = (uint32_t)MSH_AUTOBOOT_SEC * 1000000u;
    uint32_t elapsed;
    int last_sec = -1;

    if (boot_msh_poll_stop() || boot_msh_want_factory())
    {
        return 1;
    }

    elapsed = msh_elapsed_us();
    if (g_clk_mhz == 0 || elapsed >= total_us)
    {
        return 0;
    }

    msh_puts("wait 10x B=msh F=fac: ");
    while ((elapsed = msh_elapsed_us()) < total_us)
    {
        int sec_left;

        if (boot_msh_poll_stop() || boot_msh_want_factory())
        {
            msh_puts("\r\n");
            return 1;
        }
        sec_left = (int)((total_us - elapsed + 999999u) / 1000000u);
        if (sec_left < 1)
        {
            sec_left = 1;
        }
        if (sec_left != last_sec)
        {
            last_sec = sec_left;
            msh_putc('0' + (char)sec_left);
            msh_putc(' ');
        }
        boot_hw_wdt_pet();
        HAL_Delay_us(20);
    }
    msh_puts("\r\n");
    return 0;
}

static void msh_clear_hotkeys(void)
{
    g_b_count = 0;
    g_f_count = 0;
    g_stopped = 0;
    g_want_factory = 0;
    g_factory_skip_kv = 0;
    g_factory_loading = 0;
}

static void msh_enter_msh(const char *msg)
{
    msh_puts(msg);
    boot_sgl_status("msh");
    msh_rx_drain();
    msh_clear_hotkeys();
    msh_loop();
}

static void msh_factory_now(void)
{
    if (boot_msh_factory_skip_kv())
    {
        msh_puts("combo CHG+KEY1+KEY2... factory (no kv)\r\n");
    }
    else
    {
        msh_puts("hotkey FFF... factory\r\n");
    }
    boot_sgl_show_factory();
    boot_factory_boot();
    msh_puts("factory returned, entering msh\r\n");
    msh_clear_hotkeys();
    msh_loop();
}

/*
 * Shared by PWR boot, leaving the charge page, factory combo, msh `app`.
 * Hold PA29, drop charge widgets, LCD + backlight + splash. Jump/load
 * after this is the same as a battery power-on.
 */
static void msh_leave_to_boot(void)
{
    boot_hw_power_hold();
    boot_sgl_charge_end();
    if (boot_sgl_start() != 0)
    {
        msh_puts("sgl fail\r\n");
    }
    else
    {
        msh_puts("sgl ok\r\n");
    }
}

static int msh_charge_uart_sel(void)
{
    if (boot_msh_poll_stop())
    {
        return 1;
    }
    if (boot_msh_want_factory())
    {
        return 2;
    }
    return -1;
}

/*
 * KEY1+KEY2 already down at charging boot: keep sampling until 1 s or
 * lost. Does not show the charge page. Returns 2 factory, 1 msh, 0 charge.
 */
static int msh_factory_hold_at_plug(void)
{
    uint32_t t0;

    if (!boot_hw_both_keys())
    {
        return 0;
    }

    msh_puts("charge: KEY1+KEY2 at plug, hold 1s for factory\r\n");
    t0 = HAL_GetTick();
    for (;;)
    {
        int uart;

        boot_hw_wdt_pet();
        uart = msh_charge_uart_sel();
        if (uart >= 0)
        {
            return uart;
        }
        if (boot_hw_unplugged())
        {
            msh_puts("unplug, power off\r\n");
            boot_hw_power_off();
        }
        if (!boot_hw_both_keys())
        {
            msh_puts("charge: KEY1+KEY2 lost, charge page\r\n");
            return 0;
        }
        if ((HAL_GetTick() - t0) >= MSH_BOOT_HOLD_MS)
        {
            msh_puts("KEY1+KEY2 hold 1s -> factory (no kv)\r\n");
            boot_msh_request_factory(0);
            return 2;
        }
        HAL_Delay_us(10000);
    }
}

/*
 * USB plug-in without PWR: charge animation, PA29 released.
 * Returns 0 to autoboot, 1 to stay in msh, 2 to jump factory.
 * Does not hold power or start the splash: msh_leave_to_boot() does that
 * for every path (PWR, charge-leave, factory).
 */
static int msh_charge_wait(void)
{
    uint32_t hold0 = 0;
    int holding = 0;
    int plug;

    msh_puts("charging: any key 1s boot (not factory)\r\n");
    boot_hw_power_release();

    plug = msh_factory_hold_at_plug();
    if (plug != 0)
    {
        return plug;
    }

    (void)boot_lfs_mount();
    boot_hw_bat_init();
    if (boot_sgl_charge_begin() != 0)
    {
        msh_puts("sgl fail (charge)\r\n");
    }
    else
    {
        msh_puts("sgl charge\r\n");
    }
    boot_hw_keys_arm();

    for (;;)
    {
        int uart;

        boot_hw_eta_poll();
        boot_sgl_charge_tick();
        boot_hw_wdt_pet();

        uart = msh_charge_uart_sel();
        if (uart >= 0)
        {
            return uart;
        }
        if (boot_hw_unplugged())
        {
            msh_puts("unplug, power off\r\n");
            boot_hw_power_off();
        }
        /* Charge page: any key 1 s boots. Both keys do not enter factory. */
        if (boot_hw_boot_key())
        {
            if (!holding)
            {
                holding = 1;
                hold0 = HAL_GetTick();
            }
            else if ((HAL_GetTick() - hold0) >= MSH_BOOT_HOLD_MS)
            {
                msh_puts("key hold 1s -> boot\r\n");
                return 0;
            }
        }
        else
        {
            holding = 0;
        }
        HAL_Delay_us(10000);
    }
}

void boot_msh_start(int (*prepare_fn)(void), void (*go_fn)(void))
{
    enum boot_target t;
    int stay_msh = 0;
    int go_factory = 0;
    int pwr_auto;

    g_prepare_fn = prepare_fn;
    g_go_fn = go_fn;
    g_prepared = 0;
    msh_clear_hotkeys();

    msh_uart_rx_enable();

    /* KV first: the banner reports the live persist.boot.pwr value and the
     * PWR/charge decision below uses the same read. */
    (void)boot_lfs_mount();
    pwr_auto = boot_pwr_auto();

    msh_puts("\r\n2SFBL msh ");
    msh_puts(BOOT_LOADER_VERSION);
    msh_puts("\r\n");
    msh_puts("autoboot: kv persist.boot.target, else /fw then main then factory\r\n");
    msh_puts("10x B/b = msh, 10x F/f = factory\r\n");
    msh_puts("pwr: splash and boot (no charge page)\r\n");
    msh_puts("plug-in: charge page, any key 1s to boot\r\n");
    msh_puts("KEY1+KEY2 already down at plug, hold 1s = factory (no kv)\r\n");
    if (pwr_auto)
    {
        msh_puts("persist.boot.pwr=on: skip key, autoboot\r\n");
    }
    else
    {
        msh_puts("persist.boot.pwr=off: pwr key boots, plug-in charge page\r\n");
    }
    boot_hw_wdt_pet();
    msh_time_start();
    (void)boot_msh_poll_stop();

    /* PWR boot skips charge even if the cable is in. USB plug-in without
     * PWR is the only path that waits on the charge page. persist.boot.pwr
     * (ctl pwr on) is the debug bypass: no key, jump. */
    if (boot_hw_pwr_key())
    {
        msh_puts("pwr: boot\r\n");
    }
    else if (pwr_auto)
    {
        msh_puts("pwr: persist.boot.pwr, skip key\r\n");
    }
    else if (boot_hw_charging())
    {
        int chg = msh_charge_wait();

        if (chg == 1)
        {
            stay_msh = 1;
        }
        else if (chg == 2)
        {
            go_factory = 1;
        }
    }

    msh_leave_to_boot();

    if (stay_msh)
    {
        msh_enter_msh("stopped by BBB... staying in msh\r\n");
        return;
    }

    (void)boot_lfs_mount();
    boot_lfs_print_info();
    (void)boot_version_publish();
    t = boot_target_get();
    msh_puts("kv target: ");
    msh_puts(boot_target_name(t));
    msh_puts("\r\n");
    (void)boot_msh_poll_stop();

    if (boot_msh_poll_stop())
    {
        msh_enter_msh("stopped by BBB... staying in msh\r\n");
        return;
    }

    if (go_factory || boot_msh_want_factory())
    {
        msh_factory_now();
        return;
    }

    if (t == BOOT_TARGET_BOOT)
    {
        msh_puts("kv target=boot, staying in msh\r\n");
        msh_clear_hotkeys();
        msh_loop();
        return;
    }

    if (g_prepare_fn)
    {
        /* 2 s autoboot window runs in parallel with OVNX copy/CRC. */
        msh_time_start();
        g_prepared = (g_prepare_fn() == 0);
    }

    if (boot_msh_poll_stop())
    {
        msh_enter_msh("stopped by BBB... staying in msh\r\n");
        return;
    }

    if (boot_msh_want_factory())
    {
        msh_factory_now();
        return;
    }

    if (!g_prepared)
    {
        msh_puts("load failed, entering msh\r\n");
        msh_clear_hotkeys();
        msh_loop();
        return;
    }

    if (wait_remainder())
    {
        if (boot_msh_poll_stop())
        {
            msh_enter_msh("stopped by BBB... staying in msh\r\n");
            return;
        }
        if (boot_msh_want_factory())
        {
            msh_factory_now();
            return;
        }
    }

    if (boot_msh_poll_stop())
    {
        msh_enter_msh("stopped by BBB... staying in msh\r\n");
        return;
    }
    if (boot_msh_want_factory())
    {
        msh_factory_now();
        return;
    }

    msh_puts("autoboot\r\n");
    boot_sgl_hold();
    if (g_go_fn)
    {
        g_go_fn();
    }
    msh_puts("boot returned, entering msh\r\n");
    msh_clear_hotkeys();
    msh_loop();
}

