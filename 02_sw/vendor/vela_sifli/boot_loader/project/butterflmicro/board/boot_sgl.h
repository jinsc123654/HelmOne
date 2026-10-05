/**
 * @file boot_sgl.h
 * @brief Trimmed SGL splash (logo + label + progress) on the boot LCD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_SGL_H
#define BOOT_SGL_H

#ifdef __cplusplus
extern "C" {
#endif

/** LCD + SGL. Failure is non-fatal: UART boot continues. */
int boot_sgl_start(void);

int boot_sgl_ready(void);

void boot_sgl_status(const char *text);

/** Show "factory" above the loading label. Loading text stays as-is. */
void boot_sgl_show_factory(void);

/**
 * Splash alert (ASCII, consolas14). @p danger 0 = yellow, 1 = red.
 * Does not require the boot chrome to still be visible.
 */
void boot_sgl_alert(const char *text, int danger);

/** 0..100. No-op until boot_sgl_start succeeds. */
void boot_sgl_progress(unsigned pct);

/** Animate the charging battery. Call from the charge-wait loop, not WDT pet. */
void boot_sgl_charge_tick(void);

/** No-op. hw_preinit0 feeds WDT before BSS; do not put SGL work here. */
void boot_sgl_tick(void);

/** Hide status/progress; leave logo + product name for NuttX. */
void boot_sgl_hold(void);

/**
 * LCD + battery fill, no logo enter. Fill follows SOC (phone-style);
 * below 10% still draws a 10% red bar. Bottom shows SOC (voltage 100%
 * without charge-full IO stays 99%). Full (charge IO only): fill 100%,
 * breathes alpha, bottom "100%". No percentage inside the battery.
 */
int boot_sgl_charge_begin(void);

/** Hide the charger UI and play the normal logo enter. */
void boot_sgl_charge_end(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_SGL_H */
