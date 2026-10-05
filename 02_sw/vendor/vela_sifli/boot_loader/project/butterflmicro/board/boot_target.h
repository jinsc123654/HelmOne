/**
 * @file boot_target.h
 * @brief Sticky autoboot slot in KVDB persist.boot.target (file backend).
 *
 * NuttX: /mnt/kv/db/persist.boot.target via setprop persist.boot.target <val>
 * 2SFBL: /db/persist.boot.target (KV mounted at /)
 *
 * Values: fw | main | factory | boot (stay in msh). Missing/invalid → default
 * chain /fw then main then factory. Sticky until msh `target off` or setprop
 * delete. UART 10x B / 10x F still override.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_TARGET_H
#define BOOT_TARGET_H

#include "boot_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_TARGET_KEY  "persist.boot.target"
#define BOOT_TARGET_PATH "/" BOOT_TARGET_KEY /* boot_lfs remaps to /db/ */

/* Last image 2SFBL actually jumped to.  Overwritten every successful
 * load (not a sticky target).  NuttX: /mnt/kv/db/persist.boot.running
 * Value: "fw <filename>" | "main" | "factory"
 */
#define BOOT_RUNNING_KEY  "persist.boot.running"
#define BOOT_RUNNING_PATH "/" BOOT_RUNNING_KEY /* boot_lfs remaps to /db/ */
#define BOOT_RUNNING_VAL_MAX 80

enum boot_target
{
    BOOT_TARGET_NONE = 0, /* no file / invalid → default chain */
    BOOT_TARGET_FW,
    BOOT_TARGET_MAIN,
    BOOT_TARGET_FACTORY,
    BOOT_TARGET_BOOT, /* stay in 2SFBL msh */
};

enum boot_target boot_target_parse(const char *s);
const char      *boot_target_name(enum boot_target t);
enum boot_target boot_target_get(void);
int              boot_target_set(enum boot_target t); /* NONE unlinks */

/** Record the slot that is about to run.  @p fw_name is the /fw basename. */
int boot_running_set(enum boot_target slot, const char *fw_name);

/** Read persist.boot.running into @p buf.  Returns 0 if present. */
int boot_running_get(char *buf, unsigned buf_len);

/**
 * persist.boot.pwr: NuttX `ctl pwr on` writes "1".  Only an explicit on
 * ("1" / "on" / "true") skips the PWR-key / charge-page wait and autoboots
 * (debug reflash); a missing, empty ("0" / "off") or unrecognized value
 * leaves the normal key / charge-page flow in charge.  The banner prints the
 * value this read returns.
 */
#define BOOT_PWR_KEY   "persist.boot.pwr"
#define BOOT_PWR_PATH  "/" BOOT_PWR_KEY /* boot_lfs remaps to /db/ */

/** @return 1 only if persist.boot.pwr reads back an explicit on. */
int boot_pwr_auto(void);

/** Write 1 or 0 (same text as NuttX persist_set_i32). */
int boot_pwr_set(int on);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_TARGET_H */
