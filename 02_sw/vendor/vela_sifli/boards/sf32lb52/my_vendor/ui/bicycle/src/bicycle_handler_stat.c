/**
 * @file bicycle_handler_stat.c
 * @brief 自行车 UI — handler_stat。
 */

#include "bicycle_handler_stat.h"

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#ifndef BICYCLE_HANDLER_STAT_PERIOD_MS
#define BICYCLE_HANDLER_STAT_PERIOD_MS 5000u
#endif

static struct timespec s_ts0;
static struct timespec s_usleep_ts0;
static struct timespec s_win_ts;
static bool s_win_valid;

static uint32_t s_calls;
static uint64_t s_handler_ns;
static uint32_t s_handler_max_ns;
static uint64_t s_sleep_req_ms;

static uint64_t s_usleep_act_ns;
static uint32_t s_usleep_min_ns;
static uint32_t s_usleep_max_ns;

static uint64_t bicycle_ts_delta_ns(const struct timespec * a,
    const struct timespec * b)
{
    int64_t sec = (int64_t)b->tv_sec - (int64_t)a->tv_sec;
    int64_t nsec = (int64_t)b->tv_nsec - (int64_t)a->tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }

    return (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
}

/**
 * @brief 自行车 handler stat reset。
 */
static void bicycle_handler_stat_reset(const struct timespec * now_ts)
{
    s_win_ts = *now_ts;
    s_win_valid = true;
    s_calls = 0;
    s_handler_ns = 0;
    s_handler_max_ns = 0;
    s_sleep_req_ms = 0;
    s_usleep_act_ns = 0;
    s_usleep_min_ns = UINT32_MAX;
    s_usleep_max_ns = 0;
}

static void bicycle_handler_stat_emit(const struct timespec * now_ts)
{
    uint64_t win_ns = bicycle_ts_delta_ns(&s_win_ts, now_ts);
    uint32_t win_ms = (uint32_t)(win_ns / 1000000ULL);
    uint32_t pct_x10 = win_ns ? (uint32_t)(s_handler_ns * 1000ULL / win_ns) : 0;
    uint32_t handler_ms = (uint32_t)(s_handler_ns / 1000000ULL);
    uint32_t loop_hz = win_ms ? (uint32_t)((uint64_t)s_calls * 1000ULL / win_ms) : 0;
    uint32_t usleep_act_ms = (uint32_t)(s_usleep_act_ns / 1000000ULL);
    uint32_t usleep_avg_us = s_calls ? (uint32_t)(s_usleep_act_ns / 1000ULL / s_calls) : 0;
    uint32_t usleep_min_us = (s_usleep_min_ns == UINT32_MAX) ? 0 :
        (uint32_t)(s_usleep_min_ns / 1000u);
    uint32_t usleep_max_us = (uint32_t)(s_usleep_max_ns / 1000u);

    LV_LOG_INFO(
        "[handler] t=%llu.%06llu win=%u.%03us calls=%u loop=%uHz "
        "handler=%u.%03ums share=%u.%u%% max=%uus "
        "sleep_req=%llums sleep_act=%ums avg=%uus min=%uus max=%uus",
        (unsigned long long)now_ts->tv_sec,
        (unsigned long long)(now_ts->tv_nsec / 1000LL),
        win_ms / 1000u, win_ms % 1000u,
        (unsigned)s_calls, (unsigned)loop_hz,
        handler_ms / 1000u, handler_ms % 1000u,
        pct_x10 / 10u, pct_x10 % 10u,
        (unsigned)(s_handler_max_ns / 1000u),
        (unsigned long long)s_sleep_req_ms,
        (unsigned)usleep_act_ms,
        (unsigned)usleep_avg_us,
        (unsigned)usleep_min_us,
        (unsigned)usleep_max_us);

    syslog(LOG_NOTICE,
        "[handler] t=%llu.%06llu win=%u.%03us calls=%u loop=%uHz "
        "handler=%u.%03ums share=%u.%u%% "
        "sleep_req=%llums sleep_act=%ums avg=%uus min=%uus max=%uus",
        (unsigned long long)now_ts->tv_sec,
        (unsigned long long)(now_ts->tv_nsec / 1000LL),
        win_ms / 1000u, win_ms % 1000u,
        (unsigned)s_calls, (unsigned)loop_hz,
        handler_ms / 1000u, handler_ms % 1000u,
        pct_x10 / 10u, pct_x10 % 10u,
        (unsigned long long)s_sleep_req_ms,
        (unsigned)usleep_act_ms,
        (unsigned)usleep_avg_us,
        (unsigned)usleep_min_us,
        (unsigned)usleep_max_us);
}

/**
 * @brief 自行车 handler stat init。
 * @return 0 成功，负 errno 失败。
 */
void bicycle_handler_stat_init(void)
{
    s_win_valid = false;
}

/**
 * @brief 自行车 handler stat begin。
 */
void bicycle_handler_stat_begin(void)
{
    clock_gettime(CLOCK_MONOTONIC, &s_ts0);
}

/**
 * @brief 自行车 handler stat after handler。
 */
void bicycle_handler_stat_after_handler(uint32_t idle_ms, uint32_t sleep_ms)
{
    struct timespec ts1;
    uint64_t handler_ns;

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    handler_ns = bicycle_ts_delta_ns(&s_ts0, &ts1);

    if (!s_win_valid) {
        bicycle_handler_stat_reset(&ts1);
    }

    s_calls++;
    s_handler_ns += handler_ns;
    if (handler_ns > s_handler_max_ns) {
        s_handler_max_ns = (uint32_t)handler_ns;
    }
    s_sleep_req_ms += sleep_ms;
    (void)idle_ms;

    if (bicycle_ts_delta_ns(&s_win_ts, &ts1) >=
        (uint64_t)BICYCLE_HANDLER_STAT_PERIOD_MS * 1000000ULL) {
        bicycle_handler_stat_emit(&ts1);
        bicycle_handler_stat_reset(&ts1);
    }
}

/**
 * @brief 自行车 handler stat usleep begin。
 */
void bicycle_handler_stat_usleep_begin(void)
{
    clock_gettime(CLOCK_MONOTONIC, &s_usleep_ts0);
}

/**
 * @brief 自行车 handler stat usleep end。
 */
void bicycle_handler_stat_usleep_end(void)
{
    struct timespec ts1;
    uint64_t act_ns;

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    act_ns = bicycle_ts_delta_ns(&s_usleep_ts0, &ts1);

    s_usleep_act_ns += act_ns;
    if (act_ns < s_usleep_min_ns) {
        s_usleep_min_ns = (uint32_t)act_ns;
    }
    if (act_ns > s_usleep_max_ns) {
        s_usleep_max_ns = (uint32_t)act_ns;
    }
}
