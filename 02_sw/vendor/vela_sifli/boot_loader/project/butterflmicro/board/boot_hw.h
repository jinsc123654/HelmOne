/**
 * @file boot_hw.h
 * @brief 2SFBL board GPIO: piezo off, power hold, ETA9184, factory combo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_HW_H
#define BOOT_HW_H

#ifdef __cplusplus
extern "C" {
#endif

/** PA40 ceramic piezo as GPIO low so it cannot buzz while boot runs. */
void boot_hw_buzzer_off(void);

/**
 * Hold system power: PA29 PWR_KEY_CTL high (same as NuttX bsp_pinmux).
 * Call as early as buzzer mute so releasing the physical key does not
 * drop the rail before the app starts.
 */
void boot_hw_power_hold(void);

/**
 * Drop PA29 while USB/VIN still feeds the rail. Charge UI uses this so
 * unplug is a hardware power-off, not a software latch release.
 */
void boot_hw_power_release(void);

/**
 * Battery / PWR boot: hold PA29. USB plug-in without PWR (charging or
 * full): release so unplug is hardware power-off. PWR pressed at boot
 * keeps the latch even on cable (no charge page).
 * Call from entry() after BSS, not from hw_preinit0.
 */
void boot_hw_power_apply(void);

/** Drop PA29 so the latch can release. Does not return if the rail dies. */
void boot_hw_power_off(void);

/**
 * Reprogram WDT1/WDT2/IWDT to 5 s (unlock, STOP, CVR, START). Handoff stops
 * WDT1/WDT2 and leaves IWDT at 15 s for NuttX.
 */
void boot_hw_wdt_pet(void);

/** 15 s timeout before jumping to NuttX so bringup can take over IWDT. */
void boot_hw_wdt_handoff(void);

/**
 * Mux ETA9184 STAT/DISCHRG/PULSE and KEY1/KEY2. ENBST stays off.
 * Safe to call more than once.
 */
void boot_hw_eta_pins(void);

/** PA25 STAT low: ETA9184 is charging. */
int boot_hw_charging(void);

/** STAT high and DISCHRG low: cable gone, battery is feeding the MCU. */
int boot_hw_unplugged(void);

/** STAT high, DISCHRG high: full while still on the cable. PULSE is untrusted. */
int boot_hw_charge_full(void);

/**
 * GPADC CH7 VBATS + load persist.battery.full_mv (same file as NuttX).
 * Internal divider + factory vbat_mv/vbat_reg. Busy-waits ~300 ms once
 * for analog settle. Call from msh, never WDT pet.
 */
void boot_hw_bat_init(void);

/**
 * Count PA26 PULSE rising edges. Untrusted; SOC is VBATS.
 */
void boot_hw_eta_poll(void);

/**
 * VBATS percent, 3.00 V to calibrated full (Li-ion OCV curve).
 * Charging subtracts ~80 mV and will not drop the shown percent.
 * Charging→full, sitting at full IO, or stale ~4.10 V KV while already at
 * CV (≥4.30 V on this ADC) writes the same KV as the app (−20 mV, ±50 mV
 * deadband, both directions).
 * @return -1 until ADC works, else 0～100.
 */
int boot_hw_bat_pct(void);

/** Last VBATS millivolts; <0 if ADC has not sampled yet. */
int boot_hw_bat_mv(void);

/** Snapshot KEY1/KEY2 so the next falling edge is a new press. */
void boot_hw_keys_arm(void);

/** KEY1 (PA30) and KEY2 (PA33) both active-low right now. */
int boot_hw_both_keys(void);

/**
 * Any of KEY1 / KEY2 / PWR (including several at once). Charge page 1 s
 * hold uses this to boot; KEY1+KEY2 is not factory after the charge page.
 */
int boot_hw_boot_key(void);

/** PA34 PWR_KEY_READ high: user is holding the power button. */
int boot_hw_pwr_key(void);

/** KEY1 (PA30), KEY2 (PA33), or PWR (PA34) is pressed right now. */
int boot_hw_any_key(void);

/** KEY1 or KEY2 just went active-low since boot_hw_keys_arm() / last call. */
int boot_hw_key_edge(void);

/**
 * Charging (PA25 STAT low) and KEY1 (PA30) + KEY2 (PA33) both pressed
 * (active-low). Sampled a few times. Unused by msh (factory is a 1 s
 * hold at plug-in, before the charge page).
 */
int boot_hw_factory_combo(void);

/** KEY1+KEY2 seen while charging (including LCD init). Unused by msh. */
int boot_hw_factory_pending(void);

/** Sample KEY1+KEY2 during LCD delays. Safe after BSS (not in WDT pet). */
void boot_hw_factory_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_HW_H */
