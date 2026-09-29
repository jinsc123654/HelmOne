/* Cadence model: turns a riding profile into CSC crank events.
 *
 * The model runs on its own 20 Hz timer and owns all mutable state behind one
 * mutex.  Everything outside this module (the GATT notifier, the console) only
 * ever sees copies taken with csc_sim_sample() / csc_sim_status().
 */
#ifndef CSC_SIM_H
#define CSC_SIM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CSC_MODE_STOP = 0,  /* crank still */
    CSC_MODE_STEADY,    /* hold the cadence set with `rpm <n>` */
    CSC_MODE_RIDE,      /* sweep the cadence as a triangle wave */
} csc_mode_t;

/* The two numbers a CSC Measurement notification carries. */
typedef struct {
    uint32_t revs;      /* cumulative crank revolutions (16 bit on the wire) */
    uint16_t evt_time;  /* last crank event time, 1/1024 s units (16 bit) */
    uint16_t rpm;       /* instantaneous cadence, for logs only */
} csc_sample_t;

/* Everything the console needs to describe the current state. */
typedef struct {
    csc_mode_t mode;
    uint16_t rpm;
    uint16_t target_rpm;
    uint32_t revs;
    uint16_t evt_time;
    uint8_t battery;
    bool battery_auto;
    bool linked;
    char peer[24];
    uint32_t conns;
    uint32_t ntf_ok;
    uint32_t ntf_fail;
} csc_status_t;

/* Starts the model timer. */
void csc_sim_init(void);

const char *csc_mode_name(csc_mode_t mode);

/* Called (from the model timer, and therefore from a non-BLE task) whenever a
 * measurement should go out; the callee must only hand the work to the NimBLE
 * event queue. */
void csc_sim_set_tx_cb(void (*cb)(void));

/* Snapshots. */
void csc_sim_sample(csc_sample_t *out);
void csc_sim_status(csc_status_t *out);

/* Cadence commands. */
void csc_sim_steady(uint16_t rpm);   /* 0 selects CSC_MODE_STOP */
void csc_sim_ride(uint16_t tri_max_rpm);
void csc_sim_stop(void);

/* Simulated battery, 0..100.  It sweeps as a triangle wave by default; setting
 * a value pins it, `auto` hands it back to the wave. */
uint8_t csc_sim_battery_get(void);
void csc_sim_battery_set(uint8_t pct);
void csc_sim_battery_auto(void);

/* Link bookkeeping, called from the BLE task. */
void csc_sim_link_up(const char *peer);
void csc_sim_link_down(void);
void csc_sim_count_notify(bool ok);

#endif /* CSC_SIM_H */
