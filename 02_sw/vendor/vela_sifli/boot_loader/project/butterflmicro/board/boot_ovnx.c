/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "boot_ovnx.h"
#include "boot_flash.h"
#include "boot_hw.h"
#include "boot_lfs.h"
#include "boot_msh.h"
#include "boot_sgl.h"
#include "sd_nand_drv.h"
#include "secboot.h"
#include "register.h"
#include <rtconfig.h>
#include <string.h>
#include <stdint.h>

/* One NAND page per step; stage in SRAM then memcpy to PSRAM. */
#define BOOT_OVNX_IO_CHUNK       2048U
#define BOOT_OVNX_DOT_INTERVAL   (16U * 1024U)

typedef char boot_ovnx_file_hdr_size_check[
    (sizeof(struct boot_ovnx_file_hdr) == BOOT_OVNX_FILE_HDR_DESC_SIZE) ? 1 : -1];

static const uint32_t boot_crc32_tab[256] =
{
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static uint32_t boot_ovnx_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        {
            crc = boot_crc32_tab[(crc ^ data[i]) & 0xffU] ^ (crc >> 8);
            if ((i & 0x7ffU) == 0U)
                {
                    boot_hw_wdt_pet();
                    if (boot_msh_abort_load())
                        {
                            return crc;
                        }
                }
        }

    return crc;
}

uint32_t boot_ovnx_crc32(const uint8_t *data, uint32_t len)
{
    return boot_ovnx_crc32_update(0xffffffffU, data, len) ^ 0xffffffffU;
}

static void boot_uart_putc(char c)
{
    uint8_t ch = (uint8_t)c;
    boot_uart_tx(hwp_usart1, &ch, 1);
}

static void boot_uart_puts(const char *s)
{
    while (*s)
        {
            boot_uart_putc(*s++);
        }
}

static void boot_uart_put_hex32(uint32_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    boot_uart_puts("0x");
    for (i = 28; i >= 0; i -= 4)
        {
            boot_uart_putc(hex[(val >> i) & 0xfU]);
        }
}

static void boot_uart_put_u32(uint32_t val)
{
    char buf[11];
    int i = 10;

    buf[10] = '\0';
    if (val == 0U)
        {
            boot_uart_putc('0');
            return;
        }

    while (val > 0U && i > 0)
        {
            buf[--i] = (char)('0' + (val % 10U));
            val /= 10U;
        }

    boot_uart_puts(&buf[i]);
}

static uint32_t boot_read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int boot_ovnx_file_hdr_valid(const struct boot_ovnx_file_hdr *fh)
{
    static const char expect_magic[8] = "OVNXAPP\0";

    if (memcmp(fh->magic, expect_magic, BOOT_OVNX_FILE_MAGIC_LEN) != 0)
        {
            return 0;
        }

    if (fh->hdr_size != BOOT_OVNX_FILE_HDR_SIZE)
        {
            return 0;
        }

    return 1;
}

int boot_ovnx_parse_file_hdr(const struct boot_ovnx_file_hdr *fh,
                             struct boot_ovnx_image_info *info)
{
    if (!fh || !info || !boot_ovnx_file_hdr_valid(fh))
        {
            return -1;
        }

    memset(info, 0, sizeof(*info));
    memcpy(info->version, fh->version, BOOT_OVNX_VERSION_LEN);
    info->version[BOOT_OVNX_VERSION_LEN] = '\0';
    info->build_unix = fh->build_unix;
    memcpy(info->build_date, fh->build_date, BOOT_OVNX_DATE_LEN);
    info->build_date[BOOT_OVNX_DATE_LEN] = '\0';
    info->image_len = fh->image_len;
    info->payload_len = fh->payload_len;

    if (info->payload_len < 8U)
        {
            return -1;
        }

    if (info->image_len != BOOT_OVNX_FILE_HDR_SIZE + info->payload_len +
            BOOT_OVNX_CRC_SIZE)
        {
            return -1;
        }

    return 0;
}

static void boot_ovnx_print_meta_uart(const struct boot_ovnx_image_info *info)
{
    boot_uart_puts("\r\nOVNX app image:\r\n");
    boot_uart_puts("  version     ");
    boot_uart_puts(info->version);
    boot_uart_puts("\r\n");
    boot_uart_puts("  build_date  ");
    boot_uart_puts(info->build_date);
    boot_uart_puts(" (unix ");
    boot_uart_put_u32(info->build_unix);
    boot_uart_puts(")\r\n");
    boot_uart_puts("  image_len   ");
    boot_uart_put_u32(info->image_len);
    boot_uart_puts(" ");
    boot_uart_put_hex32(info->image_len);
    boot_uart_puts("\r\n");
    boot_uart_puts("  payload_len ");
    boot_uart_put_u32(info->payload_len);
    boot_uart_puts(" ");
    boot_uart_put_hex32(info->payload_len);
    boot_uart_puts("\r\n");
}

/* Source → SRAM staging → PSRAM; CRC is done after the full copy. */
static int boot_ovnx_read_payload_to_psram(boot_ovnx_read_fn readfn, void *ctx,
                                           uint32_t dest, uint32_t payload_len)
{
    static uint8_t staging[BOOT_OVNX_IO_CHUNK];
    uint32_t off = 0U;

    boot_hw_wdt_pet();

    while (off < payload_len)
        {
            uint32_t chunk = payload_len - off;

            if (chunk > BOOT_OVNX_IO_CHUNK)
                {
                    chunk = BOOT_OVNX_IO_CHUNK;
                }

            if (readfn(ctx, BOOT_OVNX_FILE_HDR_SIZE + off, staging, chunk) != 0)
                {
                    boot_uart_puts("  read fail\r\n");
                    return -1;
                }

            memcpy((void *)(dest + off), staging, chunk);
            off += chunk;
            boot_hw_wdt_pet();
            if (boot_msh_abort_load())
                {
                    return -1;
                }
            if ((off % BOOT_OVNX_DOT_INTERVAL) == 0U || off == payload_len)
                {
                    unsigned pct;

                    boot_uart_putc('.');
                    if (payload_len != 0U)
                        {
                            pct = (unsigned)(((uint64_t)off * 100u) / payload_len);
                            /* CRC is still pending: 100% reveals the product
                             * name and hides splash chrome. Keep 99 until the
                             * caller confirms CRC so FW-error / CHIP DAMAGED
                             * can still use the factory label. */
                            if (pct >= 100u)
                                {
                                    pct = 99u;
                                }
                            if ((pct % 5u) == 0u || off == payload_len)
                                {
                                    boot_sgl_progress(pct);
                                }
                        }
                }
        }

    return 0;
}

static void boot_ovnx_print_crc_uart(const struct boot_ovnx_image_info *info,
                                     int ok)
{
    boot_uart_puts("  crc expect  ");
    boot_uart_put_hex32(info->payload_crc);
    boot_uart_puts("\r\n");
    boot_uart_puts("  crc calc    ");
    boot_uart_put_hex32(info->payload_crc_calc);
    boot_uart_puts("\r\n");
    boot_uart_puts(ok ? "  crc result  PASS\r\n" : "  crc result  FAIL\r\n");
}

int boot_ovnx_load_read(boot_ovnx_read_fn readfn, void *ctx,
                        uint32_t dest, uint32_t file_max,
                        struct boot_ovnx_image_info *info_out)
{
    struct boot_ovnx_file_hdr fhdr;
    uint8_t crc_buf[BOOT_OVNX_CRC_SIZE];
    uint32_t payload_len;
    uint32_t image_len;
    int crc_ok;

    if (!readfn || !info_out)
        {
            return -1;
        }

    if (readfn(ctx, 0, &fhdr, sizeof(fhdr)) != 0)
        {
            boot_uart_puts("  hdr read fail\r\n");
            return -1;
        }
    if (boot_ovnx_parse_file_hdr(&fhdr, info_out) != 0)
        {
            boot_uart_puts("  bad OVNX hdr\r\n");
            return -1;
        }

    payload_len = info_out->payload_len;
    image_len = info_out->image_len;

    if (image_len > file_max ||
        image_len < BOOT_OVNX_FILE_HDR_SIZE + BOOT_OVNX_CRC_SIZE)
        {
            boot_uart_puts("  image too large\r\n");
            return -1;
        }

    boot_ovnx_print_meta_uart(info_out);
    boot_uart_puts("  loading");
    boot_sgl_status("loading");
    boot_sgl_progress(0);

    if (boot_msh_abort_load())
        {
            return -1;
        }

    if (boot_ovnx_read_payload_to_psram(readfn, ctx, dest, payload_len) != 0)
        {
            return -1;
        }

    if (boot_msh_abort_load())
        {
            return -1;
        }

    boot_uart_puts("\r\n  loaded\r\n");
    boot_uart_puts("  crc...\r\n");
    boot_hw_wdt_pet();

#if (NAND_BUF_CPY_MODE == 1)
    SCB_CleanInvalidateDCache_by_Addr((void *)dest, payload_len);
    boot_hw_wdt_pet();
#endif

    if (readfn(ctx, image_len - BOOT_OVNX_CRC_SIZE, crc_buf,
               BOOT_OVNX_CRC_SIZE) != 0)
        {
            boot_uart_puts("  crc read fail\r\n");
            return -1;
        }
    info_out->payload_crc = boot_read_u32_le(crc_buf);
    info_out->payload_crc_calc = boot_ovnx_crc32((const uint8_t *)dest, payload_len);
    if (boot_msh_abort_load())
        {
            return -1;
        }
    crc_ok = (info_out->payload_crc == info_out->payload_crc_calc);
    boot_ovnx_print_crc_uart(info_out, crc_ok);
    if (crc_ok)
        {
            boot_sgl_progress(100);
        }

    return crc_ok ? 0 : -1;
}

static int boot_ovnx_reset_looks_jumpable(uint32_t sp, uint32_t pc)
{
    uint32_t entry = pc & ~1u;

    (void)sp;
    if ((pc & 1u) == 0u)
    {
        return 0;
    }
    /* Product / factory run at PSRAM 0x10000000; 2SFBL itself is SRAM. */
    if (entry >= 0x10000000u && entry < 0x14000000u)
    {
        return 1;
    }
    if (entry >= 0x20000000u && entry < 0x20400000u)
    {
        return 1;
    }
    return 0;
}

int boot_ovnx_verify_read(boot_ovnx_read_fn readfn, void *ctx,
                          uint32_t file_max,
                          struct boot_ovnx_image_info *info_out,
                          int *jumpable_out)
{
    struct boot_ovnx_file_hdr fhdr;
    static uint8_t staging[BOOT_OVNX_IO_CHUNK];
    uint8_t crc_buf[BOOT_OVNX_CRC_SIZE];
    uint8_t vec[8];
    uint32_t payload_len;
    uint32_t image_len;
    uint32_t crc;
    uint32_t off;
    int crc_ok;
    int jumpable = 0;

    if (jumpable_out)
    {
        *jumpable_out = 0;
    }
    if (!readfn || !info_out)
    {
        return -1;
    }

    memset(info_out, 0, sizeof(*info_out));
    boot_hw_wdt_pet();

    if (readfn(ctx, 0, &fhdr, sizeof(fhdr)) != 0)
    {
        return -1;
    }
    if (boot_ovnx_parse_file_hdr(&fhdr, info_out) != 0)
    {
        return -1;
    }

    payload_len = info_out->payload_len;
    image_len = info_out->image_len;
    if (image_len > file_max ||
        image_len < BOOT_OVNX_FILE_HDR_SIZE + BOOT_OVNX_CRC_SIZE)
    {
        return -1;
    }

    if (readfn(ctx, BOOT_OVNX_FILE_HDR_SIZE, vec, sizeof(vec)) == 0)
    {
        jumpable = boot_ovnx_reset_looks_jumpable(boot_read_u32_le(vec),
                                                  boot_read_u32_le(vec + 4));
    }

    crc = 0xffffffffU;
    off = 0U;
    while (off < payload_len)
    {
        uint32_t chunk = payload_len - off;

        if (chunk > BOOT_OVNX_IO_CHUNK)
        {
            chunk = BOOT_OVNX_IO_CHUNK;
        }
        if (readfn(ctx, BOOT_OVNX_FILE_HDR_SIZE + off, staging, chunk) != 0)
        {
            return -1;
        }
        crc = boot_ovnx_crc32_update(crc, staging, chunk);
        off += chunk;
        boot_hw_wdt_pet();
        if (boot_msh_abort_load())
        {
            return -1;
        }
        if ((off % BOOT_OVNX_DOT_INTERVAL) == 0U || off == payload_len)
        {
            boot_uart_putc('.');
        }
    }

    if (readfn(ctx, image_len - BOOT_OVNX_CRC_SIZE, crc_buf,
               BOOT_OVNX_CRC_SIZE) != 0)
    {
        return -1;
    }
    info_out->payload_crc = boot_read_u32_le(crc_buf);
    info_out->payload_crc_calc = crc ^ 0xffffffffU;
    crc_ok = (info_out->payload_crc == info_out->payload_crc_calc);
    if (!crc_ok)
    {
        jumpable = 0;
    }
    if (jumpable_out)
    {
        *jumpable_out = jumpable;
    }
    return crc_ok ? 0 : -1;
}

static int boot_ovnx_flash_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    uint32_t src = (uint32_t)(uintptr_t)ctx;
    int nread;

    if (!g_flash_read)
        {
            return -1;
        }
    nread = g_flash_read(src + off, (const int8_t *)buf, len);
    return (nread == (int)len) ? 0 : -1;
}

/* Raw partition I/O must not run against a mounted LittleFS cache. */
static void boot_ovnx_sd_claim(void)
{
    boot_lfs_unmount();
    sd_wait_idle();
}

int boot_ovnx_load_hcpu(uint32_t src, uint32_t dest, uint32_t part_max,
                        struct boot_ovnx_image_info *info_out)
{
    boot_ovnx_sd_claim();
    return boot_ovnx_load_read(boot_ovnx_flash_read, (void *)(uintptr_t)src,
                               dest, part_max, info_out);
}

int boot_ovnx_verify_hcpu(uint32_t src, uint32_t part_max,
                          struct boot_ovnx_image_info *info_out,
                          int *jumpable_out)
{
    boot_ovnx_sd_claim();
    return boot_ovnx_verify_read(boot_ovnx_flash_read, (void *)(uintptr_t)src,
                                 part_max, info_out, jumpable_out);
}
