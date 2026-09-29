/* Power model.
 *
 * A power meter reports instantaneous power plus, optionally, the accumulated
 * crank revolutions at the last crank event.  The Helm One client reads the
 * power at bytes 2..3 of the measurement, so this model keeps power and cadence
 * consistent with each other: the cranks turn like a rider's, and the reported
 * power follows that cadence.
 */
#include "cps_sim.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cps-sim";

/* One crank revolution is 60e6 micro-revolutions, so at N rpm the crank
 * advances exactly N micro-revolutions per microsecond. */
#define REV_UREV            60000000ULL
#define TICK_US             50000        /* model tick, 20 Hz */

/* W per rpm: 85 rpm lands around 195 W, which is where a fit rider sits. */
#define WATTS_PER_RPM       2.3f
#define WATTS_MAX           3000         /* the client drops anything above this */
#define RPM_MAX             250

/* Slew rates. */
#define RAMP_WATTS_S        150.0f
#define RAMP_STOP_WATTS_S   220.0f

/* `ride` sweeps the power as a triangle wave: a value that walks the whole
 * range is far easier to watch on a display than a constant, and it exercises
 * the client's rendering - and its cut-offs - end to end.  The cadence the
 * meter reports is derived from the power, so it sweeps in step. */
#define RIDE_MAX_DEFAULT    300
#define TRI_MIN_WATTS       100
#define TRI_RAMP_US         (60ULL * 1000000ULL)   /* 60 s from low to high */
#define RAMP_FOLLOW_WATTS_S 60.0f                  /* how fast the value chases the wave */

/* The battery follows a triangle wave of its own, slow enough to read. */
#define BAT_MIN             20
#define BAT_MAX             100
#define BAT_RAMP_US         (60ULL * 1000000ULL)

/* Power meters notify about once a second. */
#define NOTIFY_US           (1ULL * 1000000ULL)
#define STATUS_LOG_US       (5ULL * 1000000ULL)

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_tick;

static struct {
    cps_mode_t   mode;
    uint16_t     target;        /* steady watts / ride triangle top */
    float        watts;
    float        rpm;

    uint64_t     phase_us;      /* time inside the current triangle wave */

    uint64_t     angle;         /* crank angle, 0 .. REV_UREV */
    uint32_t     crank_revs;
    uint16_t     crank_evt_time;
    uint64_t     crank_evt_us;

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

const char *cps_mode_name(cps_mode_t mode)
{
    switch (mode) {
    case CPS_MODE_STOP:   return "stop";
    case CPS_MODE_STEADY: return "steady";
    case CPS_MODE_RIDE:   return "ride";
    default:              return "?";
    }
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
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

/* Triangle wave between TRI_MIN_WATTS and the requested top, one ramp per
 * TRI_RAMP_US.  Called with the lock held. */
static float tri_watts(uint64_t phase_us)
{
    const uint64_t period = TRI_RAMP_US * 2ULL;
    const uint64_t t = phase_us % period;
    const float lo = (float)TRI_MIN_WATTS;
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

static void update_watts(uint32_t dt_us)
{
    const float dt = (float)dt_us / 1000000.0f;

    switch (s.mode) {
    case CPS_MODE_STOP:
        s.watts = ramp(s.watts, 0.0f, RAMP_STOP_WATTS_S * dt);
        s.rpm = ramp(s.rpm, 0.0f, RAMP_STOP_WATTS_S * dt);
        break;

    case CPS_MODE_STEADY:
        s.watts = ramp(s.watts, (float)s.target, RAMP_WATTS_S * dt);
        /* Keep the cranks turning in step with the power, so the crank fields
         * of the measurement stay meaningful whatever the console asks for. */
        s.rpm = clampf(s.watts / WATTS_PER_RPM, 0.0f, (float)RPM_MAX);
        break;

    case CPS_MODE_RIDE:
        s.phase_us += dt_us;
        s.watts = ramp(s.watts, tri_watts(s.phase_us), RAMP_FOLLOW_WATTS_S * dt);
        /* The cadence rides along with the power, so the crank fields of the
         * measurement stay meaningful while the wave sweeps. */
        s.rpm = clampf(s.watts / WATTS_PER_RPM, 0.0f, (float)RPM_MAX);
        break;

    default:
        break;
    }

    s.watts = clampf(s.watts, 0.0f, (float)WATTS_MAX);
    s.rpm = clampf(s.rpm, 0.0f, (float)RPM_MAX);
}

/* Advance the crank over one tick. */
static void advance_crank(uint64_t tick_start_us, uint32_t dt_us)
{
    const uint32_t rpm = (uint32_t)lrintf(s.rpm);
    if (rpm == 0) {
        return;
    }

    const uint64_t adv = (uint64_t)rpm * dt_us;    /* micro-revolutions this tick */
    uint64_t angle = s.angle + adv;
    uint64_t before = s.angle;
    uint64_t last_cross_us = 0;
    int events = 0;

    while (angle >= REV_UREV) {
        /* Interpolate when inside this tick the revolution completed, so the
         * reported crank event times keep real spacing. */
        const uint64_t off_us = (events == 0) ? ((REV_UREV - before) * dt_us) / adv : dt_us;
        last_cross_us = tick_start_us + off_us;
        angle -= REV_UREV;
        before = 0;
        events++;
    }

    s.angle = angle;

    if (events > 0) {
        s.crank_revs += (uint32_t)events;
        s.crank_evt_us = last_cross_us;
        s.crank_evt_time = (uint16_t)((last_cross_us * 1024ULL) / 1000000ULL);
    }
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

    if (dt_us > 0) {
        update_watts(dt_us);
        advance_crank(prev_tick_us, dt_us);

        s.battery_us += dt_us;
        if (s.battery_auto) {
            s.battery = tri_battery(s.battery_us);
        }
    }

    if (s.linked) {
        /* A fixed 1 Hz stream: that is what power meters do, and it also keeps
         * the link obviously alive while the rider is coasting at 0 W. */
        if ((now - s.last_tx_us) >= NOTIFY_US) {
            tx = s.tx_cb;
            s.last_tx_us = now;
        }
        if ((now - s.last_log_us) >= STATUS_LOG_US) {
            s.last_log_us = now;
            snprintf(line, sizeof(line),
                     "link peer=%s mode=%s watts=%u rpm=%u revs=%lu ntf=%lu err=%lu",
                     s.peer, cps_mode_name(s.mode), (unsigned)lrintf(s.watts),
                     (unsigned)lrintf(s.rpm), (unsigned long)s.crank_revs,
                     (unsigned long)s.ntf_ok, (unsigned long)s.ntf_fail);
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

void cps_sim_set_tx_cb(void (*cb)(void))
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.tx_cb = cb;
    xSemaphoreGive(s_lock);
}

void cps_sim_sample(cps_sample_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->watts = (uint16_t)lrintf(s.watts);
    out->crank_revs = s.crank_revs;
    out->crank_evt_time = s.crank_evt_time;
    out->rpm = (uint16_t)lrintf(s.rpm);
    xSemaphoreGive(s_lock);
}

void cps_sim_status(cps_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->mode = s.mode;
    out->watts = (uint16_t)lrintf(s.watts);
    out->target_watts = s.target;
    out->rpm = (uint16_t)lrintf(s.rpm);
    out->crank_revs = s.crank_revs;
    out->battery = s.battery;
    out->battery_auto = s.battery_auto;
    out->linked = s.linked;
    memcpy(out->peer, s.peer, sizeof(out->peer));
    out->conns = s.conns;
    out->ntf_ok = s.ntf_ok;
    out->ntf_fail = s.ntf_fail;
    xSemaphoreGive(s_lock);
}

void cps_sim_steady(uint16_t watts)
{
    if (watts > WATTS_MAX) {
        watts = WATTS_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (watts == 0) {
        s.mode = CPS_MODE_STOP;
    } else {
        s.mode = CPS_MODE_STEADY;
        s.target = watts;
    }
    const cps_mode_t mode = s.mode;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=%s target=%u W", cps_mode_name(mode), (unsigned)watts);
}

void cps_sim_ride(uint16_t tri_max_watts)
{
    if (tri_max_watts == 0) {
        tri_max_watts = RIDE_MAX_DEFAULT;
    }
    if (tri_max_watts > WATTS_MAX) {
        tri_max_watts = WATTS_MAX;
    }
    if (tri_max_watts < TRI_MIN_WATTS + 50) {
        tri_max_watts = TRI_MIN_WATTS + 50;    /* keep the wave wide enough to see */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool restarted = (s.mode != CPS_MODE_RIDE);
    s.mode = CPS_MODE_RIDE;
    s.target = tri_max_watts;
    if (restarted) {
        s.phase_us = 0;                        /* the sweep starts at its floor */
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=ride triangle=%u..%u W", (unsigned)TRI_MIN_WATTS,
             (unsigned)tri_max_watts);
}

void cps_sim_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.mode = CPS_MODE_STOP;
    s.target = 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=stop");
}

uint8_t cps_sim_battery_get(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t level = s.battery;
    xSemaphoreGive(s_lock);
    return level;
}

void cps_sim_battery_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery = pct;
    s.battery_auto = false;     /* an explicit value wins until `battery auto` */
    xSemaphoreGive(s_lock);
}

void cps_sim_battery_auto(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery_auto = true;
    s.battery_us = 0;           /* rejoin the wave at its floor */
    s.battery = (uint8_t)BAT_MIN;
    xSemaphoreGive(s_lock);
}

void cps_sim_link_up(const char *peer)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = true;
    s.conns++;
    snprintf(s.peer, sizeof(s.peer), "%s", (peer != NULL) ? peer : "-");
    s.last_tx_us = 0;      /* first measurement as soon as the client subscribes */
    s.last_log_us = (uint64_t)esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void cps_sim_link_down(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = false;
    snprintf(s.peer, sizeof(s.peer), "-");
    xSemaphoreGive(s_lock);
}

void cps_sim_count_notify(bool ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s.ntf_ok++;
    } else {
        s.ntf_fail++;
    }
    xSemaphoreGive(s_lock);
}

void cps_sim_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    /* Start riding rather than parked: opening this board's console resets it
     * (the USB bridge pulses EN), so the sim has to be useful the moment it has
     * booted instead of waiting for a command a busy console might eat. */
    s.mode = CPS_MODE_RIDE;
    s.target = RIDE_MAX_DEFAULT;
    s.battery = (uint8_t)BAT_MIN;
    s.battery_auto = true;
    s.tick_us = (uint64_t)esp_timer_get_time();
    s.last_tx_us = s.tick_us;
    s.last_log_us = s.tick_us;
    snprintf(s.peer, sizeof(s.peer), "-");

    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "cps-model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick, TICK_US));
}
