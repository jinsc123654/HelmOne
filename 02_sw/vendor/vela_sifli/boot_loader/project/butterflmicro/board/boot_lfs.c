/**
 * @file boot_lfs.c
 * @brief Bare-metal LittleFS on KV_REGION or FS_REGION (one mount at a time).
 *
 * Same tree as NuttX (nuttx/fs/littlefs, v2.7 / disk 2.1).
 * Geometry matches sf32lb_sdio.c: read/prog 512, block 4096, name_max 128.
 * Do not format here — blank volumes are autoformatted by NuttX.
 * msh browses KV (ls/cat) and writes persist.* under /db (NuttX /mnt/kv/db).
 * Product /fw stays at KV root.
 * USB MTP lives in the factory NuttX image.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_lfs.h"
#include "boot_hw.h"
#include "board.h"
#include "ptab_table.h"
#include "sd_nand_drv.h"
#include "secboot.h"
#include "lfs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BOOT_LFS_READ_SIZE     512u
#define BOOT_LFS_BLOCK_SIZE    4096u
#define BOOT_LFS_CACHE_SIZE    512u
#define BOOT_LFS_LOOKAHEAD     8u
#define BOOT_LFS_CAT_CHUNK     128u
#define BOOT_LFS_CAT_MAX       4096u
#define BOOT_KV_DB_DIR         "/db"

extern void boot_uart_tx(USART_TypeDef *uart, uint8_t *data, int len);

static lfs_t g_lfs;
static struct lfs_config g_cfg;
static int g_mounted;
static int g_mount_err;
static uint64_t g_region_off;
static uint64_t g_region_size;
static const char *g_region_name;

/* pack-sd-img seeds FS_REGION as if the card were 2 GiB. */
#define BOOT_LFS_PACK_CARD  (2ull * 1024ull * 1024ull * 1024ull)

static uint8_t g_read_buf[BOOT_LFS_CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t g_prog_buf[BOOT_LFS_CACHE_SIZE] __attribute__((aligned(8)));
static uint8_t g_look_buf[BOOT_LFS_LOOKAHEAD] __attribute__((aligned(8)));
static uint8_t g_file_buf[BOOT_LFS_CACHE_SIZE] __attribute__((aligned(8)));

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
    char buf[10];
    int i = 9;

    if (v == 0)
    {
        uart_putc('0');
        return;
    }
    buf[9] = 0;
    while (v && i > 0)
    {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    uart_puts(&buf[i]);
}

static void uart_i32(int v)
{
    if (v < 0)
    {
        uart_putc('-');
        uart_u32((uint32_t)(-v));
        return;
    }
    uart_u32((uint32_t)v);
}

static int card_read512(uint64_t byte_off, void *buf)
{
    if (board_boot_src == BOOT_FROM_EMMC)
    {
        if (byte_off > 0xffffffffull)
        {
            return LFS_ERR_IO;
        }
        return (emmc_read_data((uint32_t)byte_off, buf, BOOT_LFS_READ_SIZE) ==
                (int)BOOT_LFS_READ_SIZE) ? 0 : LFS_ERR_IO;
    }

    return (sd_read_data(byte_off, buf, BOOT_LFS_READ_SIZE) ==
            (int)BOOT_LFS_READ_SIZE) ? 0 : LFS_ERR_IO;
}

static int card_write512(uint64_t byte_off, const void *buf)
{
    if (board_boot_src == BOOT_FROM_EMMC)
    {
        return LFS_ERR_IO;
    }
    return (sd_write_data(byte_off, buf, BOOT_LFS_READ_SIZE) ==
            (int)BOOT_LFS_READ_SIZE) ? 0 : LFS_ERR_IO;
}

static int kv_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint64_t addr = g_region_off + (uint64_t)block * c->block_size + (uint64_t)off;
    uint8_t *dst = (uint8_t *)buffer;
    uint8_t tmp[BOOT_LFS_READ_SIZE] __attribute__((aligned(8)));

    (void)c;
    boot_hw_wdt_pet();
    while (size)
    {
        lfs_size_t n = (size < BOOT_LFS_READ_SIZE) ? size : BOOT_LFS_READ_SIZE;
        uint32_t head = (uint32_t)(addr & (BOOT_LFS_READ_SIZE - 1u));
        uint64_t aligned = addr - head;

        if (card_read512(aligned, tmp) != 0)
        {
            return LFS_ERR_IO;
        }
        memcpy(dst, tmp + head, n);
        dst += n;
        addr += n;
        size -= n;
    }
    return 0;
}

static int kv_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size)
{
    uint64_t addr = g_region_off + (uint64_t)block * c->block_size + (uint64_t)off;
    const uint8_t *src = (const uint8_t *)buffer;
    uint8_t tmp[BOOT_LFS_READ_SIZE] __attribute__((aligned(8)));

    (void)c;
    boot_hw_wdt_pet();
    while (size)
    {
        lfs_size_t n = (size < BOOT_LFS_READ_SIZE) ? size : BOOT_LFS_READ_SIZE;
        uint32_t head = (uint32_t)(addr & (BOOT_LFS_READ_SIZE - 1u));
        uint64_t aligned = addr - head;

        if (head != 0 || n != BOOT_LFS_READ_SIZE)
        {
            if (card_read512(aligned, tmp) != 0)
            {
                return LFS_ERR_IO;
            }
            memcpy(tmp + head, src, n);
            if (card_write512(aligned, tmp) != 0)
            {
                return LFS_ERR_IO;
            }
        }
        else if (card_write512(aligned, src) != 0)
        {
            return LFS_ERR_IO;
        }
        src += n;
        addr += n;
        size -= n;
    }
    return 0;
}

static int kv_erase(const struct lfs_config *c, lfs_block_t block)
{
    uint8_t ff[BOOT_LFS_READ_SIZE] __attribute__((aligned(8)));
    uint64_t addr = g_region_off + (uint64_t)block * c->block_size;
    unsigned i;

    (void)c;
    memset(ff, 0xff, sizeof(ff));
    for (i = 0; i < (BOOT_LFS_BLOCK_SIZE / BOOT_LFS_READ_SIZE); i++)
    {
        if (card_write512(addr, ff) != 0)
        {
            return LFS_ERR_IO;
        }
        addr += BOOT_LFS_READ_SIZE;
    }
    return 0;
}

static int kv_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static uint64_t region_window_bytes(const struct ptab_entry *e)
{
    uint64_t nbytes = e->size;
    uint64_t card;

    if (nbytes == 0 || nbytes == 0xffffffffull)
    {
        card = sd_card_bytes();
        if (card <= e->offset)
        {
            return 0;
        }
        nbytes = card - e->offset;
    }
    nbytes &= ~((uint64_t)BOOT_LFS_BLOCK_SIZE - 1ull);
    return nbytes;
}

static uint32_t pack_seed_blocks(const struct ptab_entry *e)
{
    uint64_t seed;

    if ((uint64_t)e->offset >= BOOT_LFS_PACK_CARD)
    {
        return 0;
    }
    seed = (BOOT_LFS_PACK_CARD - e->offset) &
           ~((uint64_t)BOOT_LFS_BLOCK_SIZE - 1ull);
    return (uint32_t)(seed / BOOT_LFS_BLOCK_SIZE);
}

static void fill_lfs_cfg(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.read = kv_read;
    g_cfg.prog = kv_prog;
    g_cfg.erase = kv_erase;
    g_cfg.sync = kv_sync;
    g_cfg.read_size = BOOT_LFS_READ_SIZE;
    g_cfg.prog_size = BOOT_LFS_READ_SIZE;
    g_cfg.block_size = BOOT_LFS_BLOCK_SIZE;
    g_cfg.cache_size = BOOT_LFS_CACHE_SIZE;
    g_cfg.lookahead_size = BOOT_LFS_LOOKAHEAD;
    g_cfg.block_cycles = -1;
    g_cfg.read_buffer = g_read_buf;
    g_cfg.prog_buffer = g_prog_buf;
    g_cfg.lookahead_buffer = g_look_buf;
    g_cfg.name_max = 128; /* CONFIG_FS_LITTLEFS_NAME_MAX */
    g_cfg.file_max = 2147483647;
    g_cfg.attr_max = 1022;
}

static void peek_region(void)
{
    uint8_t peek[BOOT_LFS_READ_SIZE] __attribute__((aligned(8)));
    unsigned i;

    uart_puts(g_region_name ? g_region_name : "?");
    uart_puts(" peek@");
    uart_u32((uint32_t)g_region_off);
    uart_puts(":");
    if (card_read512(g_region_off, peek) == 0)
    {
        for (i = 0; i < 16; i++)
        {
            static const char hex[] = "0123456789ABCDEF";

            uart_putc(' ');
            uart_putc(hex[peek[i] >> 4]);
            uart_putc(hex[peek[i] & 0xf]);
        }
    }
    else
    {
        uart_puts(" read fail");
    }
    uart_puts("\r\n");
}

static int try_mount_blocks(uint32_t block_count)
{
    int err;

    if (block_count < 4u)
    {
        return LFS_ERR_INVAL;
    }
    fill_lfs_cfg();
    g_cfg.block_count = block_count;
    memset(&g_lfs, 0, sizeof(g_lfs));
    err = lfs_mount(&g_lfs, &g_cfg);
    if (err == 0)
    {
        g_region_size = (uint64_t)block_count * BOOT_LFS_BLOCK_SIZE;
        g_mounted = 1;
        g_mount_err = 0;
    }
    return err;
}

void boot_lfs_unmount(void)
{
    if (g_mounted)
    {
        (void)lfs_unmount(&g_lfs);
        g_mounted = 0;
        /* Flush is via kv_prog; wait DAT0 so the next raw OVNX read cannot
         * interrupt a KV commit still programming on the card.
         */
        sd_wait_idle();
    }
}

int boot_lfs_mount_tag(const char *tag)
{
    const struct ptab_entry *e;
    uint64_t window;
    uint32_t blocks;
    uint32_t seed_blocks;
    int err;

    boot_lfs_unmount();
    g_mount_err = 0;
    g_region_name = tag ? tag : "?";
    boot_hw_wdt_pet();

    if (tag == NULL || tag[0] == 0)
    {
        g_mount_err = LFS_ERR_INVAL;
        return LFS_ERR_INVAL;
    }
    e = ptab_find_tag(tag);
    if (e == NULL)
    {
        g_mount_err = LFS_ERR_INVAL;
        return LFS_ERR_INVAL;
    }
    if (board_boot_src != BOOT_FROM_SD && board_boot_src != BOOT_FROM_EMMC)
    {
        g_mount_err = LFS_ERR_IO;
        return LFS_ERR_IO;
    }

    g_region_off = e->offset;
    window = region_window_bytes(e);
    blocks = (uint32_t)(window / BOOT_LFS_BLOCK_SIZE);

    err = try_mount_blocks(blocks);
    if (err != 0 && strcmp(tag, "FS_REGION") == 0)
    {
        seed_blocks = pack_seed_blocks(e);
        if (seed_blocks != 0 && seed_blocks != blocks)
        {
            err = try_mount_blocks(seed_blocks);
        }
    }

    g_mount_err = err;
    if (err != 0)
    {
        peek_region();
    }
    return err;
}

int boot_lfs_mount(void)
{
    return boot_lfs_mount_tag("KV_REGION");
}

int boot_lfs_mount_fs(void)
{
    return boot_lfs_mount_tag("FS_REGION");
}

int boot_lfs_mounted(void)
{
    return g_mounted;
}

void boot_lfs_print_info(void)
{
    uart_puts(g_region_name ? g_region_name : "LFS");
    uart_puts(" ");
    if (!g_mounted)
    {
        uart_puts("not mounted err=");
        uart_i32(g_mount_err);
        uart_puts(" (blank until NuttX autoformat, or SD read fail)\r\n");
        return;
    }
    uart_puts("LittleFS  ");
    uart_u32((uint32_t)(g_region_size / (1024ull * 1024ull)));
    uart_puts(" MiB  rw  blocks=");
    uart_u32(g_cfg.block_count);
    uart_puts("\r\n");
}

int boot_lfs_ls(const char *path)
{
    lfs_dir_t dir;
    struct lfs_info info;
    int err;

    if (!g_mounted)
    {
        uart_puts("lfs: not mounted\r\n");
        return LFS_ERR_IO;
    }
    if (path == NULL || path[0] == 0)
    {
        path = "/";
    }

    err = lfs_dir_open(&g_lfs, &dir, path);
    if (err)
    {
        uart_puts("ls: open failed ");
        uart_u32((uint32_t)(-err));
        uart_puts("\r\n");
        return err;
    }

    uart_puts("Directory ");
    uart_puts(path);
    uart_puts(":\r\n");
    while (1)
    {
        err = lfs_dir_read(&g_lfs, &dir, &info);
        if (err <= 0)
        {
            break;
        }
        uart_puts("  ");
        uart_puts(info.name);
        if (info.type == LFS_TYPE_DIR)
        {
            uart_puts("  <DIR>");
        }
        else
        {
            uart_puts("  ");
            uart_u32(info.size);
        }
        uart_puts("\r\n");
    }
    lfs_dir_close(&g_lfs, &dir);
    return 0;
}

int boot_lfs_cat(const char *path)
{
    lfs_file_t file;
    struct lfs_file_config fcfg;
    uint8_t buf[BOOT_LFS_CAT_CHUNK];
    lfs_ssize_t n;
    uint32_t total = 0;
    int err;

    if (!g_mounted)
    {
        uart_puts("lfs: not mounted\r\n");
        return LFS_ERR_IO;
    }
    if (path == NULL || path[0] == 0)
    {
        uart_puts("cat: need path\r\n");
        return LFS_ERR_INVAL;
    }

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.buffer = g_file_buf;
    err = lfs_file_opencfg(&g_lfs, &file, path, LFS_O_RDONLY, &fcfg);
    if (err)
    {
        uart_puts("cat: open failed\r\n");
        return err;
    }

    while (total < BOOT_LFS_CAT_MAX)
    {
        n = lfs_file_read(&g_lfs, &file, buf, sizeof(buf));
        if (n < 0)
        {
            lfs_file_close(&g_lfs, &file);
            uart_puts("\r\ncat: read error\r\n");
            return (int)n;
        }
        if (n == 0)
        {
            break;
        }
        boot_uart_tx(hwp_usart1, buf, (int)n);
        total += (uint32_t)n;
        if ((uint32_t)n < sizeof(buf))
        {
            break;
        }
    }
    lfs_file_close(&g_lfs, &file);
    if (total >= BOOT_LFS_CAT_MAX)
    {
        uart_puts("\r\n[truncated]\r\n");
    }
    uart_puts("\r\n");
    return 0;
}

static int name_is_dot(const char *name)
{
    return name[0] == '.' &&
           (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

int boot_lfs_complete(const char *dir, const char *prefix, boot_lfs_comp_t *out)
{
    lfs_dir_t d;
    struct lfs_info info;
    unsigned plen;
    int err;

    if (out == NULL)
    {
        return LFS_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    if (!g_mounted || dir == NULL || prefix == NULL)
    {
        return LFS_ERR_IO;
    }
    if (dir[0] == 0)
    {
        dir = "/";
    }
    plen = (unsigned)strlen(prefix);

    err = lfs_dir_open(&g_lfs, &d, dir);
    if (err)
    {
        return err;
    }

    while (lfs_dir_read(&g_lfs, &d, &info) > 0)
    {
        unsigned nlen;
        unsigned k;

        if (name_is_dot(info.name))
        {
            continue;
        }
        if (strncmp(info.name, prefix, plen) != 0)
        {
            continue;
        }

        nlen = (unsigned)strlen(info.name);
        if (nlen >= BOOT_LFS_NAME_MAX)
        {
            nlen = BOOT_LFS_NAME_MAX - 1;
        }

        out->nmatch++;
        if (out->nmatch == 1)
        {
            memcpy(out->common, info.name, nlen);
            out->common[nlen] = 0;
            out->unique_dir = (info.type == LFS_TYPE_DIR);
        }
        else
        {
            k = 0;
            while (out->common[k] && info.name[k] &&
                   out->common[k] == info.name[k])
            {
                k++;
            }
            out->common[k] = 0;
            out->unique_dir = 0;
        }
    }
    lfs_dir_close(&g_lfs, &d);
    return 0;
}

void boot_lfs_complete_list(const char *dir, const char *prefix)
{
    lfs_dir_t d;
    struct lfs_info info;
    unsigned plen;

    if (!g_mounted || dir == NULL || prefix == NULL)
    {
        return;
    }
    if (dir[0] == 0)
    {
        dir = "/";
    }
    plen = (unsigned)strlen(prefix);
    if (lfs_dir_open(&g_lfs, &d, dir) != 0)
    {
        return;
    }
    while (lfs_dir_read(&g_lfs, &d, &info) > 0)
    {
        if (name_is_dot(info.name))
        {
            continue;
        }
        if (strncmp(info.name, prefix, plen) != 0)
        {
            continue;
        }
        uart_puts("  ");
        uart_puts(info.name);
        if (info.type == LFS_TYPE_DIR)
        {
            uart_puts("/");
        }
        uart_puts("\r\n");
    }
    lfs_dir_close(&g_lfs, &d);
}

uint64_t boot_lfs_size(void)
{
    return g_region_size;
}

uint64_t boot_lfs_free(void)
{
    lfs_ssize_t used;

    if (!g_mounted)
    {
        return 0;
    }
    used = lfs_fs_size(&g_lfs);
    if (used < 0)
    {
        return 0;
    }
    return (uint64_t)(g_cfg.block_count - (lfs_size_t)used) * BOOT_LFS_BLOCK_SIZE;
}

/**
 * @brief True for `/persist.*` at KV root (NuttX `/mnt/kv/persist.*`).
 * Canonical files live in `/db/` only (NuttX `/mnt/kv/db`).
 */
static int persist_at_kv_root(const char *path)
{
    const char *p;

    if (path == NULL || path[0] != '/')
    {
        return 0;
    }
    p = path + 1;
    if (strncmp(p, "persist.", 8) != 0)
    {
        return 0;
    }
    while (*p != '\0')
    {
        if (*p == '/')
        {
            return 0;
        }
        p++;
    }
    return 1;
}

static int persist_db_path(char *dst, size_t n, const char *root_path)
{
    size_t i = 0;
    const char *p = BOOT_KV_DB_DIR;

    if (dst == NULL || n < 16 || root_path == NULL)
    {
        return LFS_ERR_INVAL;
    }
    while (*p != '\0')
    {
        if (i + 1 >= n)
        {
            return LFS_ERR_NAMETOOLONG;
        }
        dst[i++] = *p++;
    }
    p = root_path;
    while (*p != '\0')
    {
        if (i + 1 >= n)
        {
            return LFS_ERR_NAMETOOLONG;
        }
        dst[i++] = *p++;
    }
    dst[i] = '\0';
    return 0;
}

static int boot_lfs_mkdir_db(void)
{
    int err;

    if (!g_mounted)
    {
        return LFS_ERR_IO;
    }
    err = lfs_mkdir(&g_lfs, BOOT_KV_DB_DIR);
    if (err == LFS_ERR_EXIST)
    {
        return 0;
    }
    return err;
}

static int boot_lfs_file_open_ro_raw(const char *path, lfs_file_t *file)
{
    struct lfs_file_config fcfg;

    if (!g_mounted || path == NULL || file == NULL)
    {
        return LFS_ERR_IO;
    }
    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.buffer = g_file_buf;
    return lfs_file_opencfg(&g_lfs, file, path, LFS_O_RDONLY, &fcfg);
}

int boot_lfs_file_open_ro(const char *path, lfs_file_t *file)
{
    char dbp[96];

    if (persist_at_kv_root(path) && persist_db_path(dbp, sizeof(dbp), path) == 0)
    {
        return boot_lfs_file_open_ro_raw(dbp, file);
    }
    return boot_lfs_file_open_ro_raw(path, file);
}

lfs_ssize_t boot_lfs_file_read(lfs_file_t *file, void *buf, lfs_size_t size)
{
    if (!g_mounted || file == NULL || buf == NULL)
    {
        return LFS_ERR_IO;
    }
    return lfs_file_read(&g_lfs, file, buf, size);
}

lfs_soff_t boot_lfs_file_seek(lfs_file_t *file, lfs_soff_t off)
{
    if (!g_mounted || file == NULL)
    {
        return LFS_ERR_IO;
    }
    return lfs_file_seek(&g_lfs, file, off, LFS_SEEK_SET);
}

lfs_soff_t boot_lfs_file_size(lfs_file_t *file)
{
    if (!g_mounted || file == NULL)
    {
        return LFS_ERR_IO;
    }
    return lfs_file_size(&g_lfs, file);
}

int boot_lfs_file_close(lfs_file_t *file)
{
    if (file == NULL)
    {
        return LFS_ERR_INVAL;
    }
    return lfs_file_close(&g_lfs, file);
}

static int boot_lfs_put_raw(const char *path, const void *data, lfs_size_t size)
{
    lfs_file_t file;
    struct lfs_file_config fcfg;
    lfs_ssize_t n;
    int err;

    if (!g_mounted || path == NULL || (data == NULL && size != 0))
    {
        return LFS_ERR_IO;
    }

    memset(&fcfg, 0, sizeof(fcfg));
    fcfg.buffer = g_file_buf;
    err = lfs_file_opencfg(&g_lfs, &file, path,
                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fcfg);
    if (err)
    {
        return err;
    }
    n = 0;
    if (size > 0)
    {
        n = lfs_file_write(&g_lfs, &file, data, size);
        if (n < 0)
        {
            (void)lfs_file_close(&g_lfs, &file);
            return (int)n;
        }
    }
    err = lfs_file_sync(&g_lfs, &file);
    (void)lfs_file_close(&g_lfs, &file);
    if (n != (lfs_ssize_t)size)
    {
        return LFS_ERR_IO;
    }
    return err;
}

static int boot_lfs_unlink_raw(const char *path)
{
    if (!g_mounted || path == NULL)
    {
        return LFS_ERR_IO;
    }
    return lfs_remove(&g_lfs, path);
}

int boot_lfs_put(const char *path, const void *data, lfs_size_t size)
{
    char dbp[96];
    int err;

    if (persist_at_kv_root(path))
    {
        err = boot_lfs_mkdir_db();
        if (err != 0)
        {
            return err;
        }
        err = persist_db_path(dbp, sizeof(dbp), path);
        if (err != 0)
        {
            return err;
        }
        return boot_lfs_put_raw(dbp, data, size);
    }
    if (strncmp(path, BOOT_KV_DB_DIR "/", 4) == 0)
    {
        err = boot_lfs_mkdir_db();
        if (err != 0)
        {
            return err;
        }
    }
    return boot_lfs_put_raw(path, data, size);
}

int boot_lfs_unlink(const char *path)
{
    char dbp[96];
    int err;

    if (persist_at_kv_root(path) && persist_db_path(dbp, sizeof(dbp), path) == 0)
    {
        err = boot_lfs_unlink_raw(dbp);
        if (err == LFS_ERR_NOENT)
        {
            return 0;
        }
        return err;
    }
    return boot_lfs_unlink_raw(path);
}

int boot_lfs_dir_open(const char *path, lfs_dir_t *dir)
{
    if (!g_mounted || path == NULL || dir == NULL)
    {
        return LFS_ERR_IO;
    }
    if (path[0] == 0)
    {
        path = "/";
    }
    return lfs_dir_open(&g_lfs, dir, path);
}

int boot_lfs_dir_read(lfs_dir_t *dir, struct lfs_info *info)
{
    if (!g_mounted || dir == NULL || info == NULL)
    {
        return LFS_ERR_IO;
    }
    return lfs_dir_read(&g_lfs, dir, info);
}

int boot_lfs_dir_close(lfs_dir_t *dir)
{
    if (dir == NULL)
    {
        return LFS_ERR_INVAL;
    }
    return lfs_dir_close(&g_lfs, dir);
}
