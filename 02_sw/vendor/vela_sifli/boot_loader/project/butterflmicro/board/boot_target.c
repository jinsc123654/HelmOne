/**
 * @file boot_target.c
 * @brief Read/write persist.boot.target on the mounted KV LittleFS.
 *
 * Matches Vela KVDB file backend: key is the filename, value is a C string
 * (property_set writes strlen+1 including NUL).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_target.h"
#include "boot_lfs.h"
#include "register.h"

#include <string.h>

extern void boot_uart_tx(USART_TypeDef *uart, uint8_t *data, int len);

static void uart_puts(const char *s)
{
    boot_uart_tx(hwp_usart1, (uint8_t *)s, (int)strlen(s));
}

#define BOOT_TARGET_VAL_MAX 32

enum boot_target boot_target_parse(const char *s)
{
    if (s == NULL || s[0] == 0)
    {
        return BOOT_TARGET_NONE;
    }
    if (strcmp(s, "fw") == 0)
    {
        return BOOT_TARGET_FW;
    }
    if (strcmp(s, "main") == 0)
    {
        return BOOT_TARGET_MAIN;
    }
    if (strcmp(s, "factory") == 0)
    {
        return BOOT_TARGET_FACTORY;
    }
    if (strcmp(s, "boot") == 0 || strcmp(s, "msh") == 0)
    {
        return BOOT_TARGET_BOOT;
    }
    return BOOT_TARGET_NONE;
}

const char *boot_target_name(enum boot_target t)
{
    switch (t)
    {
    case BOOT_TARGET_FW:
        return "fw";
    case BOOT_TARGET_MAIN:
        return "main";
    case BOOT_TARGET_FACTORY:
        return "factory";
    case BOOT_TARGET_BOOT:
        return "boot";
    case BOOT_TARGET_NONE:
    default:
        return "default";
    }
}

static void trim_val(char *buf, unsigned *n)
{
    unsigned i = 0;
    unsigned len = *n;

    while (len > 0)
    {
        char c = buf[len - 1];

        if (c != 0 && c != '\n' && c != '\r' && c != ' ' && c != '\t')
        {
            break;
        }
        len--;
    }
    buf[len] = 0;
    while (buf[i] == ' ' || buf[i] == '\t')
    {
        i++;
    }
    if (i > 0)
    {
        memmove(buf, buf + i, len - i + 1);
        len -= i;
    }
    *n = len;
}

enum boot_target boot_target_get(void)
{
    lfs_file_t file;
    char buf[BOOT_TARGET_VAL_MAX];
    lfs_ssize_t n;
    unsigned len;
    enum boot_target t;

    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return BOOT_TARGET_NONE;
        }
    }
    if (boot_lfs_file_open_ro(BOOT_TARGET_PATH, &file) != 0)
    {
        return BOOT_TARGET_NONE;
    }
    n = boot_lfs_file_read(&file, buf, sizeof(buf) - 1u);
    (void)boot_lfs_file_close(&file);
    if (n <= 0)
    {
        return BOOT_TARGET_NONE;
    }
    buf[n] = 0;
    len = (unsigned)n;
    trim_val(buf, &len);
    if (len == 0)
    {
        return BOOT_TARGET_NONE;
    }
    t = boot_target_parse(buf);
    return t;
}

int boot_target_set(enum boot_target t)
{
    const char *name;
    char buf[16];
    unsigned n;
    int err;

    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return -1;
        }
    }
    if (t == BOOT_TARGET_NONE)
    {
        err = boot_lfs_unlink(BOOT_TARGET_PATH);
        if (err == LFS_ERR_NOENT)
        {
            return 0;
        }
        return err;
    }
    name = boot_target_name(t);
    n = (unsigned)strlen(name);
    memcpy(buf, name, n);
    buf[n] = 0; /* match property_set strlen+1 */
    return boot_lfs_put(BOOT_TARGET_PATH, buf, n + 1u);
}

int boot_running_set(enum boot_target slot, const char *fw_name)
{
    char buf[BOOT_RUNNING_VAL_MAX];
    unsigned n;
    const char *name;
    int err;

    /* OVNX load uses the same SD; remount so this write is not against a
     * stale LittleFS cache.
     */
    if (boot_lfs_mount() != 0)
    {
        uart_puts("running write fail (kv)\r\n");
        return -1;
    }

    if (slot != BOOT_TARGET_FW && slot != BOOT_TARGET_MAIN &&
        slot != BOOT_TARGET_FACTORY)
    {
        return -1;
    }

    name = boot_target_name(slot);
    n = (unsigned)strlen(name);
    if (n >= sizeof(buf) - 1u)
    {
        n = sizeof(buf) - 1u;
    }

    memcpy(buf, name, n);
    if (slot == BOOT_TARGET_FW && fw_name != NULL && fw_name[0] != 0)
    {
        unsigned fl;

        if (n + 2u < sizeof(buf))
        {
            buf[n++] = ' ';
        }

        fl = (unsigned)strlen(fw_name);
        if (fl > sizeof(buf) - n - 1u)
        {
            fl = sizeof(buf) - n - 1u;
        }

        memcpy(buf + n, fw_name, fl);
        n += fl;
    }

    buf[n] = 0;
    err = boot_lfs_put(BOOT_RUNNING_PATH, buf, n + 1u);
    if (err != 0)
    {
        uart_puts("running write fail\r\n");
        boot_lfs_unmount();
        return err;
    }

    uart_puts("running=");
    uart_puts(buf);
    uart_puts("\r\n");
    /* Unmount flushes gstate and waits DAT0 before the caller reads OVNX. */
    boot_lfs_unmount();
    return 0;
}

int boot_running_get(char *buf, unsigned buf_len)
{
    lfs_file_t file;
    lfs_ssize_t n;
    unsigned len;

    if (buf == NULL || buf_len == 0)
    {
        return -1;
    }
    buf[0] = 0;
    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return -1;
        }
    }
    if (boot_lfs_file_open_ro(BOOT_RUNNING_PATH, &file) != 0)
    {
        return -1;
    }
    n = boot_lfs_file_read(&file, buf, buf_len - 1u);
    (void)boot_lfs_file_close(&file);
    if (n <= 0)
    {
        buf[0] = 0;
        return -1;
    }
    buf[n] = 0;
    len = (unsigned)n;
    trim_val(buf, &len);
    return (len > 0) ? 0 : -1;
}

int boot_version_publish(void)
{
    lfs_file_t file;
    char buf[24];
    lfs_ssize_t n;
    unsigned len;
    unsigned vlen;
    int err;

    vlen = (unsigned)strlen(BOOT_LOADER_VERSION);
    if (vlen == 0 || vlen >= sizeof(buf))
    {
        return -1;
    }

    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return -1;
        }
    }

    if (boot_lfs_file_open_ro(BOOT_VERSION_PATH, &file) == 0)
    {
        n = boot_lfs_file_read(&file, buf, sizeof(buf) - 1u);
        (void)boot_lfs_file_close(&file);
        if (n > 0)
        {
            buf[n] = 0;
            len = (unsigned)n;
            trim_val(buf, &len);
            if (len == vlen && memcmp(buf, BOOT_LOADER_VERSION, vlen) == 0)
            {
                return 0;
            }
        }
    }

    memcpy(buf, BOOT_LOADER_VERSION, vlen);
    buf[vlen] = 0;
    err = boot_lfs_put(BOOT_VERSION_PATH, buf, vlen + 1u);
    if (err != 0)
    {
        uart_puts("boot ver write fail\r\n");
        return err;
    }

    uart_puts("boot=");
    uart_puts(BOOT_LOADER_VERSION);
    uart_puts("\r\n");
    return 0;
}

/** Whole-word ASCII compare, case-insensitive. */
static int pwr_word_is(const char *s, const char *word)
{
    while (*s != '\0' && *word != '\0')
    {
        char c = *s;

        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c != *word)
        {
            return 0;
        }
        s++;
        word++;
    }
    return *s == '\0' && *word == '\0';
}

/** Only an explicit on counts: "1" / "on" / "true". */
static int pwr_val_on(const char *s)
{
    return pwr_word_is(s, "1") || pwr_word_is(s, "on") || pwr_word_is(s, "true");
}

int boot_pwr_auto(void)
{
    lfs_file_t file;
    char buf[32];
    lfs_ssize_t n;
    unsigned len;

    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return 0;
        }
    }
    if (boot_lfs_file_open_ro(BOOT_PWR_PATH, &file) != 0)
    {
        return 0;
    }
    n = boot_lfs_file_read(&file, buf, sizeof(buf) - 1u);
    (void)boot_lfs_file_close(&file);
    if (n <= 0)
    {
        return 0;
    }
    buf[n] = 0;
    len = (unsigned)n;
    trim_val(buf, &len);

    /* Fail closed: missing, empty, "0"/"off" and anything unrecognized all
     * leave the bypass off, so a damaged file cannot force an autoboot. */
    return pwr_val_on(buf) ? 1 : 0;
}

int boot_pwr_set(int on)
{
    const char *s = on ? "1\n" : "0\n";

    if (!boot_lfs_mounted())
    {
        if (boot_lfs_mount() != 0)
        {
            return -1;
        }
    }
    return boot_lfs_put(BOOT_PWR_PATH, s, 2u);
}
