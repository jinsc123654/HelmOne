#ifndef __MY_VENDOR_BOOT_STORAGE__H__
#define __MY_VENDOR_BOOT_STORAGE__H__

/*
 * Boot storage selection for the app (NuttX) side.
 *
 * AUTO-GENERATED from boot_loader/storage.conf by
 * vendor/my_vendor/build_board.py before each `build`. Do NOT edit by
 * hand — change boot_loader/storage.conf (BOOT_STORAGE=nand|sd|emmc) and
 * rebuild.
 *
 * Exactly one of MY_VENDOR_BOOT_FROM_{NAND,SD,EMMC} is defined; it drives
 * both the partition header choice (ptab.h) and the rootfs mount path
 * (NAND vs SDIO) in board bringup.
 */

#define MY_VENDOR_BOOT_FROM_SD 1

#endif /* __MY_VENDOR_BOOT_STORAGE__H__ */
