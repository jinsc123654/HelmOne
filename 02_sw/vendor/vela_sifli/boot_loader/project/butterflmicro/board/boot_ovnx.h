/*
 * SPDX-License-Identifier: Apache-2.0
 * OVNX flash image (vendor/my_vendor/scripts/wrap_nuttx_image.py).
 *
 * nuttx.flash.bin:
 *   [0 .. 1023]   struct boot_ovnx_file_hdr + zero padding to 1 KiB
 *   [1024 ..]     payload (= nuttx.bin, vectors @ PSRAM dest+0)
 *   [tail 4B]     CRC32/IEEE over payload only
 */
#ifndef BOOT_OVNX_H
#define BOOT_OVNX_H

#include <stdint.h>

#define BOOT_OVNX_FILE_MAGIC          "OVNXAPP"
#define BOOT_OVNX_FILE_MAGIC_LEN      8U
#define BOOT_OVNX_VERSION_LEN         256U
#define BOOT_OVNX_DATE_LEN            12U
#define BOOT_OVNX_FILE_HDR_DESC_SIZE  296U
#define BOOT_OVNX_FILE_HDR_SIZE       1024U
#define BOOT_OVNX_CRC_SIZE            4U

/*
 * Packed descriptor at file offset 0 (must match ovnx_version.py /
 * struct.pack("<8s256sI12sIIII", ...)).
 */
struct boot_ovnx_file_hdr
{
    char     magic[8];
    char     version[256];
    uint32_t build_unix;
    char     build_date[12];
    uint32_t image_len;
    uint32_t payload_len;
    uint32_t hdr_size;
    uint32_t flags;
} __attribute__((packed));

struct boot_ovnx_image_info
{
    uint32_t build_unix;
    uint32_t image_len;
    uint32_t payload_len;
    uint32_t payload_crc;
    uint32_t payload_crc_calc;
    char     build_date[BOOT_OVNX_DATE_LEN + 1];
    char     version[BOOT_OVNX_VERSION_LEN + 1];
};

uint32_t boot_ovnx_crc32(const uint8_t *data, uint32_t len);

/**
 * Read @p len bytes at file/partition offset @p off into @p buf.
 * Return 0 on success, negative on I/O error.
 */
typedef int (*boot_ovnx_read_fn)(void *ctx, uint32_t off, void *buf,
                                 uint32_t len);

int boot_ovnx_load_read(boot_ovnx_read_fn readfn, void *ctx,
                        uint32_t dest, uint32_t file_max,
                        struct boot_ovnx_image_info *info_out);

int boot_ovnx_parse_file_hdr(const struct boot_ovnx_file_hdr *fh,
                             struct boot_ovnx_image_info *info);

int boot_ovnx_load_hcpu(uint32_t src, uint32_t dest, uint32_t part_max,
                        struct boot_ovnx_image_info *info_out);

/**
 * Stream-CRC an OVNX image without copying payload to PSRAM.
 * On success fills @p info_out. @p jumpable_out (optional) is 1 when the
 * reset vector looks like Thumb code in PSRAM/SRAM.
 * Return 0 if header + size + CRC are OK.
 */
int boot_ovnx_verify_read(boot_ovnx_read_fn readfn, void *ctx,
                          uint32_t file_max,
                          struct boot_ovnx_image_info *info_out,
                          int *jumpable_out);

int boot_ovnx_verify_hcpu(uint32_t src, uint32_t part_max,
                          struct boot_ovnx_image_info *info_out,
                          int *jumpable_out);

#endif /* BOOT_OVNX_H */
