/* Power model: turns a riding profile into Cycling Power measurements.
 *
 * The model runs on its own 20 Hz timer and owns all mutable state behind one
 * mutex.  Everything outside this module (the GATT notifier, the console) only
 * ever sees copies taken with cps_sim_sample() / cps_sim_status().
 */
#ifndef CPS_SIM_H
#define CPS_SIM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CPS_MODE_STOP = 0,  /* not pedalling: 0 W */
    CPS_MODE_STEADY,    /* hold the power set with `watts <n>` */
    CPS_MODE_RIDE,      /* sweep the power as a triangle wave */
} cps_mode_t;

/* What a Cycling Power Measurement notification carries. */
typedef struct {
    uint16_t watts;          /* instantaneous power */
    uint32_t crank_revs;     /* cumulative crank revolutions (16 bit on the wire) */
    uint16_t crank_evt_time; /* last crank event, 1/1024 s (16 bit) */
    uint16_t rpm;            /* cadence the power follows, for logs */
} cps_sample_t;

/* Everything the console needs to describe the current state. */
typedef struct {
    cps_mode_t mode;
    uint16_t watts;
    uint16_t target_watts;
    uint16_t rpm;
    uint32_t crank_revs;
    uint8_t battery;
    bool battery_auto;
    bool linked;
    char peer[24];
    uint32_t conns;
    uint32_t ntf_ok;
    uint32_t ntf_fail;
} cps_status_t;

/* Starts the model timer. */
void cps_sim_init(void);

/* Called (from the model timer, and therefore from a non-BLE task) whenever a
 * measurement should go out; the callee must only hand the work to the NimBLE
 * event queue. */
void cps_sim_set_tx_cb(void (*cb)(void));

/* Snapshots. */
void cps_sim_sample(cps_sample_t *out);
void cps_sim_status(cps_status_t *out);

const char *cps_mode_name(cps_mode_t mode);

/* Power commands. */
void cps_sim_steady(uint16_t watts);   /* 0 selects CPS_MODE_STOP */
void cps_sim_ride(uint16_t tri_max_watts);
void cps_sim_stop(void);

/* Simulated battery, 0..100.  It sweeps as a triangle wave by default; setting
 * a value pins it, `auto` hands it back to the wave. */
uint8_t cps_sim_battery_get(void);
void cps_sim_battery_set(uint8_t pct);
void cps_sim_battery_auto(void);

/* Link bookkeeping, called from the BLE task. */
void cps_sim_link_up(const char *peer);
void cps_sim_link_down(void);
void cps_sim_count_notify(bool ok);

#endif /* CPS_SIM_H */
