/* Cadence model.
 *
 * A CSC sensor reports, per crank revolution, (cumulative revolutions, event
 * time of that revolution in 1/1024 s).  Clients derive cadence from the
 * difference of two such reports, so the reported event times have to be spaced
 * like real revolutions - not like our tick or notification rate.
 *
 * The model integrates crank angle, and when a revolution completes it
 * interpolates the instant it completed to microsecond resolution.  That keeps
 * the derived cadence exact and jitter-free instead of quantised to the tick.
 */
#include "csc_sim.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "csc-sim";

/* One revolution is 60e6 micro-revolutions, so at N rpm the crank advances
 * exactly N micro-revolutions per microsecond: no scale factor in the integrator. */
#define REV_UREV            60000000ULL
#define TICK_US             50000                  /* model tick, 20 Hz */

/* Cadence slew rates, rpm per second. */
#define RAMP_CMD_RPM_S      90.0f                  /* `rpm <n>` */
#define RAMP_STOP_RPM_S     70.0f
#define RAMP_FOLLOW_RPM_S   11.0f                  /* how fast the value chases the wave */

/* `ride` sweeps the cadence as a triangle wave.  A value that walks the whole
 * range is far easier to watch on a display than a constant, and it exercises
 * the client's rendering - and its cut-offs - end to end. */
#define RIDE_MAX_DEFAULT    120
#define TRI_MIN_RPM         40
#define TRI_RAMP_US         (60ULL * 1000000ULL)   /* 60 s from low to high */

/* The battery follows a triangle wave of its own, slow enough to read. */
#define BAT_MIN             20
#define BAT_MAX             100
#define BAT_RAMP_US         (60ULL * 1000000ULL)

#define CSC_SIM_RPM_MAX     250                    /* cadence meters stop well below this */

/* While the crank is still, keep a measurement flowing this often: a CSC client
 * that sees no traffic at all is entitled to probe the link (the Helm One
 * client ATT-reads the sensor after 8 s of silence). */
#define KEEPALIVE_US        (2ULL * 1000000ULL)
#define STATUS_LOG_US       (5ULL * 1000000ULL)

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_tick;

static struct {
    csc_mode_t   mode;
    uint16_t     target;        /* steady cadence / ride triangle top, rpm */
    float        rpm;

    uint64_t     phase_us;      /* time inside the current triangle wave */

    uint64_t     angle;         /* crank angle, 0 .. REV_UREV */
    uint32_t     revs;          /* cumulative revolutions since boot */
    uint16_t     evt_time;      /* last crank event, 1/1024 s */
    uint64_t     evt_us;        /* the same instant in microseconds */

    void       (*tx_cb)(void);
    uint64_t     last_tx_us;
    uint64_t     last_log_us;
    uint64_t     tick_us;

    bool         linked;
    char         peer[24];
    uint32_t     conns;
    uint32_t     ntf_ok;
    uint32_t     ntf_fail;
    uint8_t      battery;
    bool         battery_auto;
    uint64_t     battery_us;
} s;

const char *csc_mode_name(csc_mode_t mode)
{
    switch (mode) {
    case CSC_MODE_STOP:   return "stop";
    case CSC_MODE_STEADY: return "steady";
    case CSC_MODE_RIDE:   return "ride";
    default:              return "?";
    }
}

static float ramp(float cur, float target, float step)
{
    if (cur < target) {
        cur += step;
        if (cur > target) {
            cur = target;
        }
    } else if (cur > target) {
        cur -= step;
        if (cur < target) {
            cur = target;
        }
    }
    return cur;
}

/* Triangle wave between TRI_MIN_RPM and the requested top, one ramp per
 * TRI_RAMP_US.  Called with the lock held. */
static float tri_rpm(uint64_t phase_us)
{
    const uint64_t period = TRI_RAMP_US * 2ULL;
    const uint64_t t = phase_us % period;
    const float lo = (float)TRI_MIN_RPM;
    const float hi = (float)s.target;

    if (t < TRI_RAMP_US) {
        return lo + (hi - lo) * ((float)t / (float)TRI_RAMP_US);
    }
    return hi - (hi - lo) * ((float)(t - TRI_RAMP_US) / (float)TRI_RAMP_US);
}

/* Triangle wave for the battery: 20 % -> 100 % -> 20 %. */
static uint8_t tri_battery(uint64_t phase_us)
{
    const uint64_t period = BAT_RAMP_US * 2ULL;
    const uint64_t t = phase_us % period;
    const float span = (float)(BAT_MAX - BAT_MIN);

    if (t < BAT_RAMP_US) {
        return (uint8_t)lrintf((float)BAT_MIN + span * ((float)t / (float)BAT_RAMP_US));
    }
    return (uint8_t)lrintf((float)BAT_MAX - span * ((float)(t - BAT_RAMP_US) / (float)BAT_RAMP_US));
}

static void update_rpm(uint32_t dt_us)
{
    const float dt = (float)dt_us / 1000000.0f;

    switch (s.mode) {
    case CSC_MODE_STOP:
        s.rpm = ramp(s.rpm, 0.0f, RAMP_STOP_RPM_S * dt);
        break;

    case CSC_MODE_STEADY:
        s.rpm = ramp(s.rpm, (float)s.target, RAMP_CMD_RPM_S * dt);
        break;

    case CSC_MODE_RIDE:
        s.phase_us += dt_us;
        s.rpm = ramp(s.rpm, tri_rpm(s.phase_us), RAMP_FOLLOW_RPM_S * dt);
        break;

    default:
        break;
    }

    if (s.rpm < 0.0f) {
        s.rpm = 0.0f;
    }
}

/* Advance the crank over one tick; returns the number of revolutions completed. */
static int advance_crank(uint64_t tick_start_us, uint32_t dt_us)
{
    const uint32_t rpm = (uint32_t)lrintf(s.rpm);
    if (rpm == 0) {
        return 0;
    }

    const uint64_t adv = (uint64_t)rpm * dt_us;    /* micro-revolutions this tick */
    uint64_t angle = s.angle + adv;
    uint64_t before = s.angle;
    uint64_t last_cross_us = 0;
    int events = 0;

    while (angle >= REV_UREV) {
        /* Interpolate when inside this tick the revolution completed.  Only the
         * first crossing needs it; more than one per tick would need a crank
         * turning faster than 1200 rpm, which the model never reaches. */
        const uint64_t off_us = (events == 0) ? ((REV_UREV - before) * dt_us) / adv : dt_us;
        last_cross_us = tick_start_us + off_us;
        angle -= REV_UREV;
        before = 0;
        events++;
    }

    s.angle = angle;

    if (events > 0) {
        s.revs += (uint32_t)events;
        s.evt_us = last_cross_us;
        s.evt_time = (uint16_t)((last_cross_us * 1024ULL) / 1000000ULL);
        return events;
    }
    return 0;
}

static void tick(void *arg)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    void (*tx)(void) = NULL;
    char line[160];

    line[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const uint64_t prev_tick_us = s.tick_us;
    const uint32_t dt_us = (uint32_t)(now - prev_tick_us);
    s.tick_us = now;

    int events = 0;
    if (dt_us > 0) {
        update_rpm(dt_us);
        events = advance_crank(prev_tick_us, dt_us);

        s.battery_us += dt_us;
        if (s.battery_auto) {
            s.battery = tri_battery(s.battery_us);
        }
    }

    if (s.linked) {
        if (events > 0 || (now - s.last_tx_us) >= KEEPALIVE_US) {
            tx = s.tx_cb;
            s.last_tx_us = now;
        }
        if ((now - s.last_log_us) >= STATUS_LOG_US) {
            s.last_log_us = now;
            snprintf(line, sizeof(line),
                     "link peer=%s mode=%s rpm=%u revs=%lu ntf=%lu err=%lu",
                     s.peer, csc_mode_name(s.mode), (unsigned)lrintf(s.rpm),
                     (unsigned long)s.revs, (unsigned long)s.ntf_ok,
                     (unsigned long)s.ntf_fail);
        }
    }

    xSemaphoreGive(s_lock);

    if (tx != NULL) {
        tx();
    }
    if (line[0] != '\0') {
        ESP_LOGI(TAG, "%s", line);
    }
}

void csc_sim_set_tx_cb(void (*cb)(void))
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.tx_cb = cb;
    xSemaphoreGive(s_lock);
}

void csc_sim_sample(csc_sample_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->revs = s.revs;
    out->evt_time = s.evt_time;
    out->rpm = (uint16_t)lrintf(s.rpm);
    xSemaphoreGive(s_lock);
}

void csc_sim_status(csc_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->mode = s.mode;
    out->rpm = (uint16_t)lrintf(s.rpm);
    out->target_rpm = s.target;
    out->revs = s.revs;
    out->evt_time = s.evt_time;
    out->battery = s.battery;
    out->battery_auto = s.battery_auto;
    out->linked = s.linked;
    memcpy(out->peer, s.peer, sizeof(out->peer));
    out->conns = s.conns;
    out->ntf_ok = s.ntf_ok;
    out->ntf_fail = s.ntf_fail;
    xSemaphoreGive(s_lock);
}

void csc_sim_steady(uint16_t rpm)
{
    if (rpm > CSC_SIM_RPM_MAX) {
        rpm = CSC_SIM_RPM_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (rpm == 0) {
        s.mode = CSC_MODE_STOP;
    } else {
        s.mode = CSC_MODE_STEADY;
        s.target = rpm;
    }
    const csc_mode_t mode = s.mode;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=%s target=%u rpm", csc_mode_name(mode), (unsigned)rpm);
}

void csc_sim_ride(uint16_t tri_max_rpm)
{
    if (tri_max_rpm == 0) {
        tri_max_rpm = RIDE_MAX_DEFAULT;
    }
    if (tri_max_rpm > CSC_SIM_RPM_MAX) {
        tri_max_rpm = CSC_SIM_RPM_MAX;
    }
    if (tri_max_rpm < TRI_MIN_RPM + 20) {
        tri_max_rpm = TRI_MIN_RPM + 20;     /* keep the wave wide enough to see */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool restarted = (s.mode != CSC_MODE_RIDE);
    s.mode = CSC_MODE_RIDE;
    s.target = tri_max_rpm;
    if (restarted) {
        s.phase_us = 0;                     /* the sweep starts at its floor */
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=ride triangle=%u..%u rpm", (unsigned)TRI_MIN_RPM,
             (unsigned)tri_max_rpm);
}

void csc_sim_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.mode = CSC_MODE_STOP;
    s.target = 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=stop");
}

uint8_t csc_sim_battery_get(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t level = s.battery;
    xSemaphoreGive(s_lock);
    return level;
}

void csc_sim_battery_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery = pct;
    s.battery_auto = false;     /* an explicit value wins until `battery auto` */
    xSemaphoreGive(s_lock);
}

void csc_sim_battery_auto(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery_auto = true;
    s.battery_us = 0;           /* rejoin the wave at its floor */
    s.battery = (uint8_t)BAT_MIN;
    xSemaphoreGive(s_lock);
}

void csc_sim_link_up(const char *peer)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = true;
    s.conns++;
    snprintf(s.peer, sizeof(s.peer), "%s", (peer != NULL) ? peer : "-");
    /* Send the first measurement soon after subscribe rather than waiting for a
     * revolution: a client seeds its baseline from the first packet it gets. */
    s.last_tx_us = 0;
    s.last_log_us = (uint64_t)esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void csc_sim_link_down(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = false;
    snprintf(s.peer, sizeof(s.peer), "-");
    xSemaphoreGive(s_lock);
}

void csc_sim_count_notify(bool ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s.ntf_ok++;
    } else {
        s.ntf_fail++;
    }
    xSemaphoreGive(s_lock);
}

void csc_sim_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    /* Start riding rather than parked: opening this board's console resets it
     * (the USB bridge pulses EN), so the sim has to be useful the moment it has
     * booted instead of waiting for a command that a busy console might eat. */
    s.mode = CSC_MODE_RIDE;
    s.rpm = 0.0f;
    s.target = RIDE_MAX_DEFAULT;
    s.battery = (uint8_t)BAT_MIN;
    s.battery_auto = true;
    s.tick_us = (uint64_t)esp_timer_get_time();
    s.last_tx_us = s.tick_us;
    s.last_log_us = s.tick_us;
    snprintf(s.peer, sizeof(s.peer), "-");

    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "csc-model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick, TICK_US));
}
