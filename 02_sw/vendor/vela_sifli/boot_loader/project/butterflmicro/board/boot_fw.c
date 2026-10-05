/**
 * @file boot_fw.c
 * @brief Product firmware from versioned /fw/*.bin on KV_REGION LittleFS.
 *
 * Rank: filename X.Y.Z (optional leading v), then OVNX build_unix.
 * Newest CRC-valid image wins; older files stay as fallback. Never format.
 * NuttX path is /mnt/kv/fw; 2SFBL already has KV mounted as /.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_fw.h"
#include "boot_lfs.h"
#include "boot_msh.h"
#include "boot_target.h"
#include "register.h"
#include "secboot.h"

#include <stddef.h>
#include <string.h>

extern void boot_uart_tx(USART_TypeDef *uart, uint8_t *data, int len);

struct fw_cand
{
    char     name[BOOT_FW_NAME_MAX];
    uint32_t ver[3];
    uint32_t build_unix;
};

struct fw_read_ctx
{
    lfs_file_t *file;
};

static void uart_puts(const char *s)
{
    boot_uart_tx(hwp_usart1, (uint8_t *)s, (int)strlen(s));
}

static void uart_putc(char c)
{
    uint8_t ch = (uint8_t)c;

    boot_uart_tx(hwp_usart1, &ch, 1);
}

static void uart_u32(uint32_t v)
{
    char buf[11];
    int i = 10;

    buf[10] = 0;
    if (v == 0)
    {
        uart_putc('0');
        return;
    }
    while (v && i > 0)
    {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    uart_puts(&buf[i]);
}

static int name_is_bin(const char *name)
{
    size_t n;

    if (name == NULL)
    {
        return 0;
    }
    n = strlen(name);
    if (n < 5 || n >= BOOT_FW_NAME_MAX)
    {
        return 0;
    }
    return name[n - 4] == '.' &&
           (name[n - 3] == 'b' || name[n - 3] == 'B') &&
           (name[n - 2] == 'i' || name[n - 2] == 'I') &&
           (name[n - 1] == 'n' || name[n - 1] == 'N');
}

static void parse_ver_name(const char *name, uint32_t ver[3])
{
    const char *p = name;
    unsigned i;

    ver[0] = ver[1] = ver[2] = 0;
    if (*p == 'v' || *p == 'V')
    {
        p++;
    }
    for (i = 0; i < 3; i++)
    {
        uint32_t v = 0;

        if (*p < '0' || *p > '9')
        {
            break;
        }
        while (*p >= '0' && *p <= '9')
        {
            v = v * 10u + (uint32_t)(*p - '0');
            p++;
        }
        ver[i] = v;
        if (*p != '.')
        {
            break;
        }
        p++;
    }
}

/* 1 if a should be tried before b (newer). */
static int cand_newer(const struct fw_cand *a, const struct fw_cand *b)
{
    unsigned i;

    for (i = 0; i < 3; i++)
    {
        if (a->ver[i] != b->ver[i])
        {
            return a->ver[i] > b->ver[i];
        }
    }
    if (a->build_unix != b->build_unix)
    {
        return a->build_unix > b->build_unix;
    }
    return strcmp(a->name, b->name) > 0;
}

static void cand_insert(struct fw_cand *cands, unsigned *n, unsigned max,
                        const struct fw_cand *in)
{
    unsigned i;
    unsigned j;

    for (i = 0; i < *n; i++)
    {
        if (cand_newer(in, &cands[i]))
        {
            break;
        }
    }
    if (*n < max)
    {
        j = *n;
        (*n)++;
    }
    else if (i >= max)
    {
        return;
    }
    else
    {
        j = max - 1u;
    }
    while (j > i)
    {
        cands[j] = cands[j - 1u];
        j--;
    }
    cands[i] = *in;
}

static void cand_sort(struct fw_cand *cands, unsigned n)
{
    unsigned i;
    unsigned j;

    for (i = 1; i < n; i++)
    {
        struct fw_cand tmp = cands[i];

        j = i;
        while (j > 0 && cand_newer(&tmp, &cands[j - 1u]))
        {
            cands[j] = cands[j - 1u];
            j--;
        }
        cands[j] = tmp;
    }
}

static int make_fw_path(char *dst, size_t dstsz, const char *name)
{
    size_t nlen = strlen(name);

    if (nlen + 5 > dstsz)
    {
        return -1;
    }
    memcpy(dst, BOOT_FW_DIR, 3);
    dst[3] = '/';
    memcpy(dst + 4, name, nlen + 1);
    return 0;
}

static int probe_hdr(const char *name, uint32_t *build_unix)
{
    char path[4 + BOOT_FW_NAME_MAX];
    lfs_file_t file;
    struct boot_ovnx_file_hdr hdr;
    struct boot_ovnx_image_info info;
    lfs_ssize_t n;

    if (make_fw_path(path, sizeof(path), name) != 0)
    {
        return -1;
    }
    if (boot_lfs_file_open_ro(path, &file) != 0)
    {
        return -1;
    }
    n = boot_lfs_file_read(&file, &hdr, sizeof(hdr));
    (void)boot_lfs_file_close(&file);
    if (n != (lfs_ssize_t)sizeof(hdr))
    {
        return -1;
    }
    if (boot_ovnx_parse_file_hdr(&hdr, &info) != 0)
    {
        return -1;
    }
    *build_unix = info.build_unix;
    return 0;
}

static unsigned scan_fw_cands(struct fw_cand *cands, unsigned max)
{
    lfs_dir_t dir;
    struct lfs_info info;
    unsigned n = 0;
    unsigned i;

    if (boot_lfs_dir_open(BOOT_FW_DIR, &dir) != 0)
    {
        return 0;
    }
    while (boot_lfs_dir_read(&dir, &info) > 0)
    {
        struct fw_cand in;

        if (info.type != LFS_TYPE_REG)
        {
            continue;
        }
        if (!name_is_bin(info.name))
        {
            continue;
        }
        memset(&in, 0, sizeof(in));
        memcpy(in.name, info.name, strlen(info.name) + 1);
        parse_ver_name(in.name, in.ver);
        cand_insert(cands, &n, max, &in);
    }
    (void)boot_lfs_dir_close(&dir);

    i = 0;
    while (i < n)
    {
        uint32_t unix_ts = 0;

        if (probe_hdr(cands[i].name, &unix_ts) != 0)
        {
            cands[i] = cands[n - 1u];
            n--;
            continue;
        }
        cands[i].build_unix = unix_ts;
        i++;
    }
    cand_sort(cands, n);
    return n;
}

static int fw_read_cb(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    struct fw_read_ctx *c = (struct fw_read_ctx *)ctx;
    lfs_ssize_t n;

    if (c == NULL || c->file == NULL)
    {
        return -1;
    }
    if (off > 0x7fffffffu)
    {
        return -1;
    }
    if (boot_lfs_file_seek(c->file, (lfs_soff_t)off) < 0)
    {
        return -1;
    }
    n = boot_lfs_file_read(c->file, buf, len);
    return (n == (lfs_ssize_t)len) ? 0 : -1;
}

static int try_named(const char *name, uint32_t dest, uint32_t dest_max,
                     struct boot_ovnx_image_info *info)
{
    char path[4 + BOOT_FW_NAME_MAX];
    lfs_file_t file;
    struct fw_read_ctx ctx;
    lfs_soff_t sz;
    uint32_t cap;
    int err;

    if (make_fw_path(path, sizeof(path), name) != 0)
    {
        return -1;
    }

    uart_puts("fw ");
    uart_puts(path);
    uart_puts("\r\n");

    if (boot_lfs_file_open_ro(path, &file) != 0)
    {
        uart_puts("  miss\r\n");
        return -1;
    }

    sz = boot_lfs_file_size(&file);
    if (sz < (lfs_soff_t)(BOOT_OVNX_FILE_HDR_SIZE + BOOT_OVNX_CRC_SIZE))
    {
        (void)boot_lfs_file_close(&file);
        uart_puts("  short\r\n");
        return -1;
    }

    cap = dest_max;
    if ((uint32_t)sz < cap)
    {
        cap = (uint32_t)sz;
    }

    ctx.file = &file;
    err = boot_ovnx_load_read(fw_read_cb, &ctx, dest, cap, info);
    (void)boot_lfs_file_close(&file);
    return err;
}

static int verify_named(const char *name, struct boot_ovnx_image_info *info,
                        int *jumpable)
{
    char path[4 + BOOT_FW_NAME_MAX];
    lfs_file_t file;
    struct fw_read_ctx ctx;
    lfs_soff_t sz;
    uint32_t cap;
    int err;

    if (make_fw_path(path, sizeof(path), name) != 0)
    {
        return -1;
    }
    if (boot_lfs_file_open_ro(path, &file) != 0)
    {
        return -1;
    }
    sz = boot_lfs_file_size(&file);
    if (sz < (lfs_soff_t)(BOOT_OVNX_FILE_HDR_SIZE + BOOT_OVNX_CRC_SIZE))
    {
        (void)boot_lfs_file_close(&file);
        return -1;
    }
    cap = (uint32_t)sz;
    ctx.file = &file;
    err = boot_ovnx_verify_read(fw_read_cb, &ctx, cap, info, jumpable);
    (void)boot_lfs_file_close(&file);
    return err;
}

static void print_cand(const struct fw_cand *c)
{
    uart_puts("  ");
    uart_puts(c->name);
    uart_puts("  ");
    uart_u32(c->ver[0]);
    uart_putc('.');
    uart_u32(c->ver[1]);
    uart_putc('.');
    uart_u32(c->ver[2]);
    uart_puts(" unix=");
    uart_u32(c->build_unix);
    uart_puts("\r\n");
}

static int ensure_kv(void)
{
    if (boot_lfs_mounted())
    {
        return 0;
    }
    return boot_lfs_mount();
}

int boot_fw_try_load(uint32_t dest, uint32_t dest_max,
                     struct boot_ovnx_image_info *info)
{
    struct fw_cand cands[BOOT_FW_MAX];
    unsigned n;
    unsigned i;

    if (info == NULL || dest_max < (BOOT_OVNX_FILE_HDR_SIZE + BOOT_OVNX_CRC_SIZE))
    {
        return -1;
    }

    if (ensure_kv() != 0)
    {
        uart_puts("fw KV mount fail\r\n");
        return -1;
    }

    n = scan_fw_cands(cands, BOOT_FW_MAX);
    uart_puts("fw ");
    uart_u32(n);
    uart_puts(" bin, newest first\r\n");
    for (i = 0; i < n; i++)
    {
        print_cand(&cands[i]);
    }

    for (i = 0; i < n; i++)
    {
        if (boot_msh_abort_load())
        {
            return -1;
        }
        if (try_named(cands[i].name, dest, dest_max, info) == 0)
        {
            uart_puts("fw ok\r\n");
            (void)boot_running_set(BOOT_TARGET_FW, cands[i].name);
            return 0;
        }
        if (boot_msh_abort_load())
        {
            return -1;
        }
    }

    uart_puts("fw none\r\n");
    return -1;
}

void boot_fw_list(void)
{
    struct fw_cand cands[BOOT_FW_MAX];
    unsigned n;
    unsigned i;

    if (ensure_kv() != 0)
    {
        uart_puts("fw KV mount fail\r\n");
        return;
    }
    boot_lfs_print_info();
    n = scan_fw_cands(cands, BOOT_FW_MAX);
    uart_puts("KV /fw *.bin newest first (");
    uart_u32(n);
    uart_puts("):\r\n");
    for (i = 0; i < n; i++)
    {
        print_cand(&cands[i]);
    }
    if (n == 0)
    {
        uart_puts("  (empty — BLE OTA / factory MTP -> /mnt/kv/fw)\r\n");
        (void)boot_lfs_ls(BOOT_FW_DIR);
    }
}

static void copy_name(char *dst, unsigned dst_max, const char *src)
{
    size_t nlen;

    if (dst == NULL || dst_max == 0)
    {
        return;
    }
    nlen = strlen(src);
    if (nlen >= dst_max)
    {
        nlen = dst_max - 1u;
    }
    memcpy(dst, src, nlen);
    dst[nlen] = 0;
}

int boot_fw_check(struct boot_ovnx_image_info *info, char *name_out,
                  unsigned name_max, int *jumpable_out)
{
    struct fw_cand cands[BOOT_FW_MAX];
    struct boot_ovnx_image_info first_info;
    unsigned n;
    unsigned i;
    int jumpable = 0;
    int have_first = 0;

    if (name_out && name_max)
    {
        name_out[0] = 0;
    }
    if (jumpable_out)
    {
        *jumpable_out = 0;
    }
    if (info == NULL)
    {
        return BOOT_FW_CHECK_CRC;
    }

    if (ensure_kv() != 0)
    {
        return BOOT_FW_CHECK_MOUNT;
    }

    n = scan_fw_cands(cands, BOOT_FW_MAX);
    if (n == 0)
    {
        return BOOT_FW_CHECK_EMPTY;
    }

    copy_name(name_out, name_max, cands[0].name);

    for (i = 0; i < n; i++)
    {
        if (verify_named(cands[i].name, info, &jumpable) == 0)
        {
            copy_name(name_out, name_max, cands[i].name);
            if (jumpable_out)
            {
                *jumpable_out = jumpable;
            }
            return BOOT_FW_CHECK_OK;
        }
        if (!have_first)
        {
            first_info = *info;
            have_first = 1;
        }
    }

    if (have_first)
    {
        *info = first_info;
    }
    return BOOT_FW_CHECK_CRC;
}
