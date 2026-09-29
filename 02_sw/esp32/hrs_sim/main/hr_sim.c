/* Heart rate model.
 *
 * A chest strap reports beats per minute plus whether it is in contact with a
 * body, roughly once a second - and it reports 0 bpm with no contact when it is
 * off.  The Helm One client keeps the last value when the strap goes quiet, so
 * the interesting states to be able to reproduce on the bench are: resting,
 * working, and off the body.
 *
 * The rates below are faster than physiology (a real heart takes tens of
 * seconds to move 20 bpm); this is an instrument, and a display that reacts
 * within a second or two is far easier to test against.
 */
#include "hr_sim.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "hr-sim";

#define TICK_US             50000        /* model tick, 20 Hz */

#define BPM_MAX             250          /* the client ignores anything above */
#define BPM_REST            58
#define BPM_OFF             0

#define RAMP_BPM_S          12.0f        /* `bpm <n>` */

/* `ride` sweeps the rate as a triangle wave: a value that walks the whole range
 * is far easier to watch on a display than a constant, and it exercises the
 * client's rendering (and its cut-offs) end to end. */
#define RIDE_MAX_DEFAULT    160
#define TRI_MIN_BPM         70
#define TRI_RAMP_US         (60ULL * 1000000ULL)   /* 60 s from low to high */
#define RAMP_FOLLOW_BPM_S   6.0f                   /* how fast the value chases the wave */

/* The battery follows a triangle wave of its own, slow enough to read. */
#define BAT_MIN             20
#define BAT_MAX             100
#define BAT_RAMP_US         (60ULL * 1000000ULL)

/* Straps notify about once a second. */
#define NOTIFY_US           (1ULL * 1000000ULL)
#define STATUS_LOG_US       (5ULL * 1000000ULL)

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_tick;

static struct {
    hr_mode_t    mode;
    uint16_t     target;        /* steady bpm / ride triangle top */
    float        bpm;

    uint64_t     phase_us;      /* time inside the current triangle wave */

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

const char *hr_mode_name(hr_mode_t mode)
{
    switch (mode) {
    case HR_MODE_OFF:    return "off";
    case HR_MODE_REST:   return "rest";
    case HR_MODE_STEADY: return "steady";
    case HR_MODE_RIDE:   return "ride";
    default:             return "?";
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

/* Triangle wave between TRI_MIN_BPM and the requested top, one ramp per
 * TRI_RAMP_US.  Called with the lock held. */
static float tri_bpm(uint64_t phase_us)
{
    const uint64_t period = TRI_RAMP_US * 2ULL;
    const uint64_t t = phase_us % period;
    const float lo = (float)TRI_MIN_BPM;
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

static void update_bpm(uint32_t dt_us)
{
    const float dt = (float)dt_us / 1000000.0f;

    switch (s.mode) {
    case HR_MODE_OFF:
        s.bpm = ramp(s.bpm, (float)BPM_OFF, RAMP_BPM_S * dt);
        break;

    case HR_MODE_REST:
        s.bpm = ramp(s.bpm, (float)BPM_REST, RAMP_BPM_S * dt);
        break;

    case HR_MODE_STEADY: {
        const float t = (float)s.phase_us / 1000000.0f;
        const float wobble = 1.5f * sinf(2.0f * (float)M_PI * t / 11.0f);
        s.phase_us += dt_us;
        /* Ramp to the commanded rate, then let a small oscillation ride on top:
         * a heart does not hold a number exactly. */
        if (fabsf(s.bpm - (float)s.target) > 2.0f) {
            s.bpm = ramp(s.bpm, (float)s.target, RAMP_BPM_S * dt);
        } else {
            s.bpm = (float)s.target + wobble;
        }
        break;
    }

    case HR_MODE_RIDE:
        s.phase_us += dt_us;
        s.bpm = ramp(s.bpm, tri_bpm(s.phase_us), RAMP_FOLLOW_BPM_S * dt);
        break;

    default:
        break;
    }

    if (s.bpm < 0.0f) {
        s.bpm = 0.0f;
    }
    if (s.bpm > (float)BPM_MAX) {
        s.bpm = (float)BPM_MAX;
    }
}

static void tick(void *arg)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    void (*tx)(void) = NULL;
    char line[160];

    line[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const uint32_t dt_us = (uint32_t)(now - s.tick_us);
    s.tick_us = now;

    if (dt_us > 0) {
        update_bpm(dt_us);

        s.battery_us += dt_us;
        if (s.battery_auto) {
            s.battery = tri_battery(s.battery_us);
        }
    }

    if (s.linked) {
        /* 1 Hz, always - including while off the body, where a real strap
         * either reports 0 bpm or goes quiet. */
        if ((now - s.last_tx_us) >= NOTIFY_US) {
            tx = s.tx_cb;
            s.last_tx_us = now;
        }
        if ((now - s.last_log_us) >= STATUS_LOG_US) {
            s.last_log_us = now;
            snprintf(line, sizeof(line), "link peer=%s mode=%s bpm=%u ntf=%lu err=%lu",
                     s.peer, hr_mode_name(s.mode), (unsigned)lrintf(s.bpm),
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

void hr_sim_set_tx_cb(void (*cb)(void))
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.tx_cb = cb;
    xSemaphoreGive(s_lock);
}

void hr_sim_sample(hr_sample_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->bpm = (uint8_t)lrintf(s.bpm);
    out->contact = (s.mode != HR_MODE_OFF);
    xSemaphoreGive(s_lock);
}

void hr_sim_status(hr_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->mode = s.mode;
    out->bpm = (uint8_t)lrintf(s.bpm);
    out->target_bpm = (uint8_t)s.target;
    out->battery = s.battery;
    out->battery_auto = s.battery_auto;
    out->linked = s.linked;
    memcpy(out->peer, s.peer, sizeof(out->peer));
    out->conns = s.conns;
    out->ntf_ok = s.ntf_ok;
    out->ntf_fail = s.ntf_fail;
    xSemaphoreGive(s_lock);
}

void hr_sim_steady(uint16_t bpm)
{
    if (bpm > BPM_MAX) {
        bpm = BPM_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (bpm == 0) {
        s.mode = HR_MODE_OFF;
    } else {
        s.mode = HR_MODE_STEADY;
        s.target = bpm;
    }
    const hr_mode_t mode = s.mode;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=%s target=%u bpm", hr_mode_name(mode), (unsigned)bpm);
}

void hr_sim_ride(uint16_t tri_max_bpm)
{
    if (tri_max_bpm == 0) {
        tri_max_bpm = RIDE_MAX_DEFAULT;
    }
    if (tri_max_bpm > BPM_MAX) {
        tri_max_bpm = BPM_MAX;
    }
    if (tri_max_bpm < TRI_MIN_BPM + 20) {
        tri_max_bpm = TRI_MIN_BPM + 20;     /* keep the wave wide enough to see */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool restarted = (s.mode != HR_MODE_RIDE);
    s.mode = HR_MODE_RIDE;
    s.target = tri_max_bpm;
    if (restarted) {
        s.phase_us = 0;                     /* the sweep starts at its floor */
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=ride triangle=%u..%u bpm", (unsigned)TRI_MIN_BPM,
             (unsigned)tri_max_bpm);
}

void hr_sim_rest(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.mode = HR_MODE_REST;
    s.target = BPM_REST;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=rest");
}

void hr_sim_off_body(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.mode = HR_MODE_OFF;
    s.target = 0;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "command: mode=off (strap off the body)");
}

uint8_t hr_sim_battery_get(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t level = s.battery;
    xSemaphoreGive(s_lock);
    return level;
}

void hr_sim_battery_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery = pct;
    s.battery_auto = false;     /* an explicit value wins until `battery auto` */
    xSemaphoreGive(s_lock);
}

void hr_sim_battery_auto(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.battery_auto = true;
    s.battery_us = 0;           /* rejoin the wave at its floor */
    s.battery = (uint8_t)BAT_MIN;
    xSemaphoreGive(s_lock);
}

void hr_sim_link_up(const char *peer)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = true;
    s.conns++;
    snprintf(s.peer, sizeof(s.peer), "%s", (peer != NULL) ? peer : "-");
    s.last_tx_us = 0;      /* first measurement as soon as the client subscribes */
    s.last_log_us = (uint64_t)esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void hr_sim_link_down(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.linked = false;
    snprintf(s.peer, sizeof(s.peer), "-");
    xSemaphoreGive(s_lock);
}

void hr_sim_count_notify(bool ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s.ntf_ok++;
    } else {
        s.ntf_fail++;
    }
    xSemaphoreGive(s_lock);
}

void hr_sim_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);

    /* Start on the sweep rather than parked: opening this board's console
     * resets it (the USB bridge pulses EN), so the sim has to be producing
     * something the moment it has booted, without waiting for a command a busy
     * console might eat.  `rest` and `off` are one command away. */
    s.mode = HR_MODE_RIDE;
    s.target = RIDE_MAX_DEFAULT;
    s.bpm = (float)TRI_MIN_BPM;
    s.battery = (uint8_t)BAT_MIN;
    s.battery_auto = true;
    s.tick_us = (uint64_t)esp_timer_get_time();
    s.last_tx_us = s.tick_us;
    s.last_log_us = s.tick_us;
    snprintf(s.peer, sizeof(s.peer), "-");

    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "hr-model",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick, TICK_US));
}
