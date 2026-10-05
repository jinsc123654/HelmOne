/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-11-06     zylx         first version
 */

#include <rtconfig.h>
#include <board.h>
#include <string.h>
#include "stdio.h"
#include "register.h"
#include "../dfu/dfu.h"
#include "../dfu_pan/dfu_pan_macro.h"
#include "boot_flash.h"
#include "secboot.h"
#include "boot_ovnx.h"
#include "boot_msh.h"
#include "boot_lfs.h"
#include "boot_fw.h"
#include "boot_target.h"
#include "boot_hw.h"
#include "boot_sgl.h"
#include "boot_version.h"
#include "ptab_table.h"
#include "sd_nand_drv.h"



// Check if OTA program is valid
int is_ota_program_valid(uint32_t ota_addr)
{
    uint32_t sp_val = 0, pc_val = 0;
    
    // Read the vector table of the OTA program
    g_flash_read(ota_addr, (const int8_t*)&sp_val, sizeof(uint32_t));
    g_flash_read(ota_addr + 4, (const int8_t*)&pc_val, sizeof(uint32_t));
    
    // Simple validation of vector table validity (check if stack pointer is in reasonable range)
    if ((sp_val & 0xFFFF0000) == 0x20000000) // Stack pointer should be in RAM area
    {
        return 1; // OTA program is valid
    }
    
    return 0; // OTA program is invalid
}



int board_boot_src;
struct sec_configuration sec_config_cache;

typedef void (*ram_hook_handler)(void);
void boot_ram(void)
{
    volatile ram_hook_handler hook = (volatile ram_hook_handler)hwp_hpsys_aon->RESERVE0;
    if (hook)
        hook();
}


//#define BOOT_TEST
#ifdef BOOT_TEST
void boot_test(void)
{
    uint32_t delay = (100 << BOOT_PU_Delay_Pos) | (200 << BOOT_PD_Delay_Pos);
    if (HAL_Get_backup(RTC_BACKUP_BOOTOPT + 1) == 0)
    {
        HAL_Set_backup(RTC_BACKUP_BOOTOPT, delay);
        HAL_Set_backup(RTC_BACKUP_BOOTOPT + 1, 1);
        HAL_PMU_Reboot();
    }
    else
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"E", 1);
        __asm("B .");
    }
}
#else
#define boot_test()
#endif

/**************************Efuse**************************************************/
#define boot_efuse_init_stage1(void) \
{ \
    hwp_efusec->TIMR = 0x2D08F; \
    /* Read bank0 */ \
    sifli_hw_efuse_read_bank(0); \
}

#define boot_efuse_init_stage2(void) \
{ \
    /* Read bank3 */ \
    sifli_hw_efuse_read_bank(3); \
}


/************************Boot *****************************************/

void run_img(uint32_t dest)
{
    /* LittleFS caches and SDMMC must not outlive this image.  Every slot
     * (fw / main / factory) jumps through here: unmount, wait DAT0, CMD0.
     */
    boot_lfs_unmount();
    sd_release();
    boot_hw_wdt_handoff();

    /* Clear MSPLIM before switching to the new image's stack. The bootloader's
     * Reset_Handler set MSPLIM to its own stack limit (~0x2004xxxx); the loaded
     * image may use a lower SP (NuttX uses ~0x2001xxxx). If MSPLIM is left set,
     * the image's first PUSH triggers a stack-limit UsageFault before it can
     * run. The SDK RT-Thread app re-sets MSPLIM in its own Reset_Handler, so it
     * was immune; a generic hand-off must clear it here. */
    __asm volatile ("MSR MSPLIM, %0" :: "r"(0) : "memory");
    __asm("LDR SP, [%0]" :: "r"(dest));
    __asm("LDR PC, [%0, #4]" :: "r"(dest));
}

uint8_t is_addr_in_nor(uint32_t addr)
{
    if (boot_handle && boot_handle->isNand == 0 &&
            addr >= boot_handle->base && addr < boot_handle->base + boot_handle->size)
        return 1;
    else
        return 0;
}

/* Copy length for NAND/PSRAM load.
 * ftab[].size comes from ptab.json max_size at build-boot time, so nuttx.bin
 * can grow within the partition without regenerating ftab. Encrypted images
 * still use img_hdr->length (DFU/signature bound to exact payload size).
 */
static uint32_t boot_img_copy_len(int flashid,
                                  const struct image_header_enc *img_hdr)
{
    if (img_hdr->flags & DFU_FLAG_ENC)
        {
            return img_hdr->length;
        }

    {
        uint32_t part_sz = sec_config_cache.ftab[flashid].size;

        if (part_sz != 0 && part_sz != FLASH_UNINIT_32)
            {
                return part_sz;
            }
    }

    return img_hdr->length;
}

/* UART banner: which storage medium this 2SFBL build is loading from. */
static void boot_uart_print_storage(void)
{
    switch (board_boot_src)
    {
    case BOOT_FROM_NAND:
        boot_uart_tx(hwp_usart1, (uint8_t *)"boot=nand ", 10);
        break;
    case BOOT_FROM_SD:
        boot_uart_tx(hwp_usart1, (uint8_t *)"boot=sd ", 8);
        break;
    case BOOT_FROM_EMMC:
        boot_uart_tx(hwp_usart1, (uint8_t *)"boot=emmc ", 10);
        break;
    case BOOT_FROM_NOR:
        boot_uart_tx(hwp_usart1, (uint8_t *)"boot=nor ", 8);
        break;
    default:
        boot_uart_tx(hwp_usart1, (uint8_t *)"boot=? ", 7);
        break;
    }
}

static int g_defer_jump;
static int g_have_jump;
static uint32_t g_jump_dest;

void boot_images_help(void);
static int boot_factory_load(void);
static int boot_main_load(void);
static int boot_images_prepare_default(void);

static void boot_try_jump(uint32_t dest)
{
    if (g_defer_jump)
    {
        g_jump_dest = dest;
        g_have_jump = 1;
        return;
    }
    run_img(dest);
}

void dfu_boot_img_in_flash(int flashid)
{
    uint32_t src = sec_config_cache.ftab[flashid].base;
    uint32_t dest = sec_config_cache.ftab[flashid].xip_base;
    int coreid = DFU_FLASH_IMG_IDX(flashid);
    struct image_header_enc *img_hdr = &(sec_config_cache.imgs[coreid]);
    struct sec_configuration *sec_config = &sec_config_cache;
    uint32_t copy_len = boot_img_copy_len(flashid, img_hdr);

    if (img_hdr->flags & DFU_FLAG_ENC)
    {
        uint32_t is_flash = 1;

        /* verify public sig_key hash */
        if (sifli_sigkey_pub_verify(sec_config->sig_pub_key, DFU_SIG_KEY_SIZE))
            sifli_secboot_exception(SECBOOT_SIGKEY_PUB_ERR);

        if (coreid < 2 * CORE_MAX)
        {
            coreid %= CORE_MAX;
            // Read Root key
            boot_efuse_init_stage2();
            if (coreid == CORE_HCPU || coreid == CORE_BL || coreid == CORE_LCPU)
            {
                ALIGN(4)
                static uint8_t dfu_key[DFU_KEY_SIZE];
                /** key in plaintext */
                ALIGN(4)
                static uint8_t dfu_key1[DFU_KEY_SIZE];
                if (is_addr_in_nor(dest))
                {
                    memcpy(dfu_key, img_hdr->key, sizeof(dfu_key));
                    sifli_hw_init_xip_key(dfu_key);

                    // Setup XIP for decoding and running
                    HAL_FLASH_NONCE_CFG(boot_handle, dest, dest + copy_len, dfu_get_counter(0));
                    if (is_flash)
                        HAL_FLASH_ALIAS_CFG(boot_handle, dest, copy_len, src - dest);
                    HAL_FLASH_AES_CFG(boot_handle, 1);          /* enable on-the-fly decoder */
                }
                else
                {
                    /* copy encrypted key to ram as AES_ACC cannot access flash */
                    memcpy(dfu_key, img_hdr->key, sizeof(dfu_key));
                    sifli_hw_dec_key(dfu_key, dfu_key1, sizeof(dfu_key1));
                    g_flash_read(src, (const int8_t *)dest, copy_len);
                    sifli_hw_dec(dfu_key1, (uint8_t *)dest, (uint8_t *)dest, copy_len, 0);
                }
#ifdef PKG_SIFLI_MBEDTLS_BOOT
                /* verify image hash signature */
                if (sifli_img_sig_hash_verify(img_hdr->sig, sec_config->sig_pub_key, (uint8_t *)dest, img_hdr->length))
                    sifli_secboot_exception(SECBOOT_IMG_HASH_SIG_ERR);
#endif
                boot_try_jump(dest);
            }
        }
    }
    if (coreid < 2 * CORE_MAX)
    {
        coreid %= CORE_MAX;
        if (coreid == CORE_HCPU || coreid == CORE_BL || coreid == CORE_LCPU)
        {
            if (is_addr_in_nor(dest))
                HAL_FLASH_ALIAS_CFG(boot_handle, dest, copy_len, src - dest);
            else if (src != dest)
                {
                    if (coreid == CORE_HCPU)
                        {
                            struct boot_ovnx_image_info ovnx_info;

                            if (boot_ovnx_load_hcpu(src, dest, copy_len,
                                                    &ovnx_info) != 0)
                                {
                                    /* 10x B/b: stay in msh. Default-chain
                                     * CRC fail: return and try factory. */
                                    if (!boot_msh_abort_load())
                                    {
                                        boot_uart_tx(hwp_usart1,
                                                     (uint8_t *)"MAINFAIL\n", 9);
                                    }
                                    return;
                                }
                        }
                    else
                        {
                            g_flash_read(src, (const int8_t *)dest, copy_len);
                        }
                }

            boot_uart_tx(hwp_usart1, (uint8_t *)"R\n", 2); /* DBG: copied, run_img */
            boot_try_jump(dest);
        }
    }
}



static int boot_stay_in_msh(void)
{
    if (boot_msh_want_factory())
    {
        return 0;
    }
    if (!boot_msh_poll_stop())
    {
        return 0;
    }
    g_defer_jump = 0;
    g_have_jump = 0;
    return 1;
}

static void boot_uart_msg(const char *s)
{
    if (s == NULL || s[0] == '\0')
    {
        return;
    }
    boot_uart_tx(hwp_usart1, (uint8_t *)s, (int)strlen(s));
}

static void boot_notice_ms(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < ms)
    {
        boot_hw_wdt_pet();
        if (boot_msh_poll_stop() || boot_msh_want_factory())
        {
            return;
        }
    }
}

/** KV persist.boot.target（fw|main）CRC 失败：屏上黄字 + UART，再进 factory。 */
static void boot_prompt_fw_factory(void)
{
    boot_sgl_alert("FW error, factory", 0);
    boot_uart_msg("kv slot CRC fail -> factory\r\n");
    boot_uart_msg("固件异常，进入 factory\r\n");
    boot_notice_ms(800);
}

/** factory CRC 也失败：屏上红字，留 msh。 */
static void boot_prompt_chip_dead(void)
{
    boot_sgl_alert("CHIP DAMAGED", 1);
    boot_uart_msg("factory CRC fail -> chip damaged\r\n");
    boot_uart_msg("芯片损坏\r\n");
    boot_notice_ms(1500);
}

static int boot_slot_crc_to_factory(void)
{
    if (boot_msh_abort_load())
    {
        return -1;
    }
    boot_prompt_fw_factory();
    if (boot_stay_in_msh())
    {
        return -1;
    }
    return boot_factory_load();
}

int boot_images_prepare(void)
{
    enum boot_target t;

    g_defer_jump = 1;
    g_have_jump = 0;

    if (boot_msh_want_factory())
    {
        g_defer_jump = 0;
        return boot_factory_load();
    }

    t = boot_target_get();
    if (t == BOOT_TARGET_BOOT)
    {
        g_defer_jump = 0;
        return -1;
    }
    if (t == BOOT_TARGET_FACTORY)
    {
        g_defer_jump = 0;
        return boot_factory_load();
    }
    if (t == BOOT_TARGET_MAIN)
    {
        g_defer_jump = 0;
        return boot_main_load();
    }
    if (t == BOOT_TARGET_FW)
    {
        const struct ptab_entry *psr = ptab_find_tag("HCPU_PSRAM_CODE");
        uint32_t dest = psr ? psr->addr : 0x10000000u;
        uint32_t dest_max = (psr && psr->size) ? psr->size : 0x00400000u;
        struct boot_ovnx_image_info info;

        boot_uart_tx(hwp_usart1, (uint8_t *)"fw?\n", 4);
        boot_sgl_status("fw");
        board_init_psram();
        (void)boot_msh_poll_stop();
        if (!boot_msh_want_factory() &&
            boot_fw_try_load(dest, dest_max, &info) == 0)
        {
            g_jump_dest = dest;
            g_have_jump = 1;
            g_defer_jump = 0;
            /* boot_fw_try_load already unmounted KV via boot_running_set.
             * Stay unmounted until jump (run_img handoff) or msh_loop remounts.
             */
            if (boot_msh_want_factory())
            {
                return boot_factory_load();
            }
            if (boot_stay_in_msh())
            {
                return -1;
            }
            return 0;
        }
        if (boot_stay_in_msh())
        {
            return -1;
        }
        g_defer_jump = 0;
        return boot_slot_crc_to_factory();
    }

    return boot_images_prepare_default();
}

int boot_images_prepare_chain(void)
{
    g_defer_jump = 1;
    g_have_jump = 0;

    if (boot_msh_want_factory())
    {
        g_defer_jump = 0;
        return boot_factory_load();
    }
    return boot_images_prepare_default();
}

/* Default chain: /fw then ftab main then factory. */
static int boot_images_prepare_default(void)
{
    const struct ptab_entry *psr = ptab_find_tag("HCPU_PSRAM_CODE");
    uint32_t dest = psr ? psr->addr : 0x10000000u;
    uint32_t dest_max = (psr && psr->size) ? psr->size : 0x00400000u;
    struct boot_ovnx_image_info info;

    boot_uart_tx(hwp_usart1, (uint8_t *)"fw?\n", 4);
    boot_sgl_status("fw");
    board_init_psram();
    (void)boot_msh_poll_stop();

    if (!boot_msh_want_factory() && boot_fw_try_load(dest, dest_max, &info) == 0)
    {
        g_jump_dest = dest;
        g_have_jump = 1;
        g_defer_jump = 0;
        /* Keep KV unmounted until jump. msh abort remounts in msh_loop. */
        if (boot_msh_want_factory())
        {
            return boot_factory_load();
        }
        if (boot_stay_in_msh())
        {
            return -1;
        }
        return 0;
    }

    if (boot_msh_want_factory())
    {
        g_defer_jump = 0;
        return boot_factory_load();
    }

    if (boot_stay_in_msh())
    {
        return -1;
    }

    (void)boot_lfs_mount();
    boot_uart_tx(hwp_usart1, (uint8_t *)"part\n", 5);
    boot_sgl_status("loading");
    boot_images_help();
    g_defer_jump = 0;
    (void)boot_msh_poll_stop();

    if (boot_msh_want_factory())
    {
        return boot_factory_load();
    }
    if (boot_stay_in_msh())
    {
        return -1;
    }

    if (g_have_jump)
    {
        return 0;
    }

    if (boot_msh_abort_load())
    {
        return -1;
    }

    /* No KV target: /fw then main then factory, no "firmware error" splash. */
    boot_uart_tx(hwp_usart1, (uint8_t *)"fac?\n", 5);
    return boot_factory_load();
}

/* Load factory OVNX into PSRAM but do not jump. Sets g_jump_dest / g_have_jump. */
static int boot_factory_load(void)
{
    const struct ptab_entry *fac = ptab_find_img("factory");
    const struct ptab_entry *psr = ptab_find_tag("HCPU_PSRAM_CODE");
    uint32_t dest = psr ? psr->addr : 0x10000000u;
    struct boot_ovnx_image_info info;

    if (fac == NULL)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"NOFAC\n", 6);
        if (!boot_msh_abort_load())
        {
            boot_prompt_chip_dead();
        }
        return -1;
    }

    boot_uart_tx(hwp_usart1, (uint8_t *)"P", 1);
    board_init_psram();
    (void)boot_msh_poll_stop();
    boot_uart_tx(hwp_usart1, (uint8_t *)"J", 1);

    if (!boot_msh_factory_skip_kv())
    {
        (void)boot_running_set(BOOT_TARGET_FACTORY, NULL);
    }

    boot_sgl_show_factory();
    boot_msh_factory_load_begin();
    if (boot_ovnx_load_hcpu(fac->addr, dest, fac->size, &info) != 0)
    {
        boot_msh_factory_load_end();
        if (!boot_msh_abort_load())
        {
            boot_uart_tx(hwp_usart1, (uint8_t *)"FACFAIL\n", 8);
            boot_prompt_chip_dead();
        }
        return -1;
    }
    boot_msh_factory_load_end();

    g_jump_dest = dest;
    g_have_jump = 1;
    return 0;
}

/* Load partition main OVNX into PSRAM but do not jump. */
static int boot_main_load(void)
{
    const struct ptab_entry *mainp = ptab_find_img("main");
    const struct ptab_entry *psr = ptab_find_tag("HCPU_PSRAM_CODE");
    uint32_t dest = psr ? psr->addr : 0x10000000u;
    struct boot_ovnx_image_info info;

    if (mainp == NULL)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"NOMAIN\n", 7);
        return boot_slot_crc_to_factory();
    }

    board_init_psram();
    (void)boot_msh_poll_stop();

    (void)boot_running_set(BOOT_TARGET_MAIN, NULL);

    boot_sgl_status("main");
    if (boot_ovnx_load_hcpu(mainp->addr, dest, mainp->size, &info) != 0)
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"MAINFAIL\n", 9);
        return boot_slot_crc_to_factory();
    }

    g_jump_dest = dest;
    g_have_jump = 1;
    return 0;
}

void boot_images_go(void)
{
    if (g_have_jump)
    {
        run_img(g_jump_dest);
    }
    else
    {
        boot_images_help();
    }
}

/* Load and jump to the "factory" firmware slot (ptab img "factory", flashed
 * separately from "main"). Both images run at PSRAM 0x10000000 - only one at a
 * time - so this reuses the OVNX loader with the factory partition as src. */
void boot_factory_boot(void)
{
    /* Same LCD/backlight path as jumping product: combo used to skip this
     * and left the panel off (charge UI never inits the LCD). */
    (void)boot_sgl_start();
    boot_sgl_show_factory();

    if (boot_factory_load() != 0)
    {
        return;
    }

    boot_uart_tx(hwp_usart1, (uint8_t *)"R\n", 2);
    run_img(g_jump_dest);
}

void boot_images_help()
{
    static const char banner[] = "2SFBL " BOOT_LOADER_VERSION " ";

    if (boot_msh_abort_load() && !boot_msh_want_factory())
    {
        return;
    }

    boot_uart_tx(hwp_usart1, (uint8_t *)banner, (int)(sizeof(banner) - 1));
    boot_uart_print_storage();
    if (sec_config_cache.magic == SEC_CONFIG_MAGIC)
    {
#ifdef  CFG_BOOTROM
        if (sec_config_cache.running_imgs[CORE_BL] != (struct image_header_enc *)FLASH_UNINIT_32)
        {
            int flash_id = ((uint32_t)sec_config_cache.running_imgs[CORE_BL] - g_config_addr - 0x1000) / sizeof(struct image_header_enc)
                           + DFU_FLASH_IMG_LCPU;
            dfu_boot_img_in_flash(flash_id);
        }
#else
// dfu_pan logical program： 
        if(DFU_PAN_LOADER_START_ADDR != DFU_PAN_FLASH_UNINIT_32 && DFU_PAN_LOADER_SIZE != DFU_PAN_FLASH_UNINIT_32)
        {
            bool needs_update = 0;
            for (int i = 0; i < MAX_VERSION_FILES; i++) {
                uint32_t needs_update_addr = VERSION_INFO_BASE_ADDR + i * VERSION_INFO_SIZE + NEEDS_UPDATE_OFFSET;
                
                uint32_t needs_update_value = 0;
                int result = g_flash_read(needs_update_addr, (const int8_t*)&needs_update_value, sizeof(uint32_t));
                
                if (result == sizeof(uint32_t) && needs_update_value) {
                    needs_update = 1;
                    break;
                }
            }
            if (needs_update) 
            {       
                
                // Check whether the OTA program is functioning properly
                if (is_ota_program_valid(DFU_PAN_LOADER_START_ADDR))
                {
                    // Directly jump to the OTA program
                    boot_try_jump(DFU_PAN_LOADER_START_ADDR);
                    return;
                }
                else
                {
                    
                }
            }
        }
// OTA logical program end

        dfu_install_info info = {0};
        dfu_install_info info_ext = {0};

        if (DFU_DOWNLOAD_REGION_START_ADDR != FLASH_UNINIT_32)
        {
            g_flash_read(DFU_DOWNLOAD_REGION_START_ADDR, (const int8_t *)&info, sizeof(dfu_install_info));
        }
        if (DFU_INFO_REGION_START_ADDR != FLASH_UNINIT_32)
        {
            g_flash_read(DFU_INFO_REGION_START_ADDR, (const int8_t *)&info_ext, sizeof(dfu_install_info));
        }
        if (info.magic == SEC_CONFIG_MAGIC && info_ext.magic == SEC_CONFIG_MAGIC)
        {
            info = info_ext;
        }

        if (DFU_DOWNLOAD_REGION_START_ADDR != FLASH_UNINIT_32)
        {
            if ((HAL_Get_backup(RTC_BAKCUP_OTA_FORCE_MODE) == DFU_FORCE_MODE_REBOOT_TO_PACKAGE_OTA_MANAGER) ||
                    (info.magic == SEC_CONFIG_MAGIC) && (info.install_state == DFU_PACKAGE_INSTALL))
            {
                sec_config_cache.running_imgs[CORE_HCPU] = (struct image_header_enc *) & (((struct sec_configuration *)FLASH_TABLE_START_ADDR)->imgs[DFU_FLASH_IMG_IDX(DFU_FLASH_IMG_LCPU)]);
            }
        }

        if (sec_config_cache.running_imgs[CORE_HCPU] != (struct image_header_enc *)FLASH_UNINIT_32)
        {
            int flash_id = ((uint32_t)sec_config_cache.running_imgs[CORE_HCPU] - g_config_addr - 0x1000) / sizeof(struct image_header_enc) + DFU_FLASH_IMG_LCPU;
            boot_uart_tx(hwp_usart1, (uint8_t *)"P", 1);  /* DBG: before psram init */
            board_init_psram();
            (void)boot_msh_poll_stop();
            boot_uart_tx(hwp_usart1, (uint8_t *)"J", 1);  /* DBG: psram ok, jump to app */
            dfu_boot_img_in_flash(flash_id);

        }
        else
        {
            boot_uart_tx(hwp_usart1, (uint8_t *)"NOHCPU\n", 7); /* DBG: no HCPU img */
        }
#endif
    }
    else
    {
        boot_uart_tx(hwp_usart1, (uint8_t *)"BADMAGIC\n", 9); /* DBG: ftab magic bad */
    }
}

void hw_preinit0(void)
{
    if (__HAL_SYSCFG_GET_REVID() < HAL_CHIP_REV_ID_A4)
    {
        /* lower power on threshold and set VBAT_LDO output voltage to default 3.3V*/
        MODIFY_REG(hwp_pmuc->AON_LDO, PMUC_AON_LDO_VBAT_POR_TH_Msk | PMUC_AON_LDO_VBAT_LDO_SET_VOUT_Msk,
                   MAKE_REG_VAL(0, PMUC_AON_LDO_VBAT_POR_TH_Msk, PMUC_AON_LDO_VBAT_POR_TH_Pos)
                   | MAKE_REG_VAL(6, PMUC_AON_LDO_VBAT_LDO_SET_VOUT_Msk, PMUC_AON_LDO_VBAT_LDO_SET_VOUT_Pos));

        /* auto power down if VCC is low */
        hwp_pmuc->WER |= PMUC_WER_LOWBAT;
    }

    HAL_Delay_us(0);
    boot_hw_wdt_pet();
    boot_hw_power_hold();
    boot_hw_buzzer_off();

    // 1. Read efuse bank0 first to take efuse effect.
    boot_efuse_init_stage1();

    // 2. If ram hook existed, just jump to ram.
    boot_ram();
}

/**************************main**************************************/

#if defined(__CC_ARM) || defined(__CLANG_ARM)
    int main(void)
#elif defined(__ICCARM__)
    int __low_level_init(void)
#elif defined(__GNUC__)
    int entry(void)
#endif
{
    HAL_Delay_us(0);
    boot_hw_wdt_pet();
    boot_hw_power_apply();
    boot_hw_buzzer_off();

    if (__HAL_SYSCFG_GET_REVID() >= HAL_CHIP_REV_ID_A4)
    {
        // 3. Power on flash.
        board_flash_power_on();

        // 4. Check boot mode.
        HAL_MspInit();

        // 5. Boot images
#ifdef CFG_BOOTROM
        if (hwp_hpsys_cfg->BMR == 0)
#endif
        {
            // 6. Read boot options
            board_boot_src = board_boot_from();

            /* init AES_ACC as normal mode */
            __HAL_SYSCFG_CLEAR_SECURITY();
            dfu_flash_init();
            boot_msh_start(boot_images_prepare, boot_images_go);
        }
    }
    else
    {
        // 3. Read boot options
        board_boot_src = board_boot_from();

        // 4. Power on flash.
        board_flash_power_on();

        // 5. Check boot mode.
        HAL_MspInit();

        // 6. Boot images
#ifdef CFG_BOOTROM
        if (hwp_hpsys_cfg->BMR == 0)
#endif
        {
            /* init AES_ACC as normal mode */
            __HAL_SYSCFG_CLEAR_SECURITY();
            dfu_flash_init();
            boot_msh_start(boot_images_prepare, boot_images_go);
        }
    }

    while (1)
        ;

    return HAL_OK;
}