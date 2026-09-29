/* Heart rate model: turns a riding profile into Heart Rate measurements.
 *
 * The model runs on its own 20 Hz timer and owns all mutable state behind one
 * mutex.  Everything outside this module (the GATT notifier, the console) only
 * ever sees copies taken with hr_sim_sample() / hr_sim_status().
 */
#ifndef HR_SIM_H
#define HR_SIM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HR_MODE_OFF = 0,    /* strap off the body: no contact, no usable value */
    HR_MODE_REST,       /* sitting still */
    HR_MODE_STEADY,     /* hold the rate set with `bpm <n>` */
    HR_MODE_RIDE,       /* sweep the rate as a triangle wave */
} hr_mode_t;

/* What a Heart Rate Measurement notification carries. */
typedef struct {
    uint8_t bpm;      /* heart rate, 8 bit on the wire (0 = no reading) */
    bool    contact;  /* sensor contact detected */
} hr_sample_t;

/* Everything the console needs to describe the current state. */
typedef struct {
    hr_mode_t mode;
    uint8_t bpm;
    uint8_t target_bpm;
    uint8_t battery;
    bool battery_auto;
    bool linked;
    char peer[24];
    uint32_t conns;
    uint32_t ntf_ok;
    uint32_t ntf_fail;
} hr_status_t;

/* Starts the model timer. */
void hr_sim_init(void);

/* Called (from the model timer, and therefore from a non-BLE task) whenever a
 * measurement should go out; the callee must only hand the work to the NimBLE
 * event queue. */
void hr_sim_set_tx_cb(void (*cb)(void));

/* Snapshots. */
void hr_sim_sample(hr_sample_t *out);
void hr_sim_status(hr_status_t *out);

const char *hr_mode_name(hr_mode_t mode);

/* Heart rate commands. */
void hr_sim_steady(uint16_t bpm);
void hr_sim_ride(uint16_t tri_max_bpm);
void hr_sim_rest(void);
void hr_sim_off_body(void);

/* Simulated battery, 0..100.  It sweeps as a triangle wave by default; setting
 * a value pins it, `auto` hands it back to the wave. */
uint8_t hr_sim_battery_get(void);
void hr_sim_battery_set(uint8_t pct);
void hr_sim_battery_auto(void);

/* Link bookkeeping, called from the BLE task. */
void hr_sim_link_up(const char *peer);
void hr_sim_link_down(void);
void hr_sim_count_notify(bool ok);

#endif /* HR_SIM_H */
