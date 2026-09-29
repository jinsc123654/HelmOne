/* Serial console for the cadence simulator.
 *
 * The console shares UART0 with the log output on the CH9102 bridge, so the
 * instrument can be driven and watched over the flashing cable alone.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csc_sim.h"
#include "esp_console.h"
#include "gatt_csc.h"

#define RPM_MAX   250

static int cmd_status(int argc, char **argv)
{
    csc_status_t st;
    int reset_left;
    uint32_t rlo;
    uint32_t rhi;

    csc_sim_status(&st);
    reset_left = app_reset_remaining_s();
    app_reset_range(&rlo, &rhi);

    printf("mode    : %s\n", csc_mode_name(st.mode));
    printf("cadence : %u rpm (target %u)\n", (unsigned)st.rpm, (unsigned)st.target_rpm);
    printf("crank   : %lu revs, last event %u (1/1024 s)\n",
           (unsigned long)st.revs, (unsigned)st.evt_time);
    printf("link    : %s peer=%s conns=%lu ntf=%lu err=%lu\n", st.linked ? "up" : "down",
           st.peer, (unsigned long)st.conns, (unsigned long)st.ntf_ok, (unsigned long)st.ntf_fail);
    printf("radio   : advertise=%s\n", app_adv_on() ? "on" : "off");
    printf("battery : %u%% (%s)\n", (unsigned)st.battery,
           st.battery_auto ? "triangle wave" : "fixed");
    if (reset_left < 0) {
        printf("reset   : off\n");
    } else if (rhi > rlo) {
        printf("reset   : in %d s (random %u..%u s)\n", reset_left,
               (unsigned)rlo, (unsigned)rhi);
    } else {
        printf("reset   : in %d s (fixed %u s)\n", reset_left, (unsigned)rlo);
    }

    return 0;
}

static int cmd_rpm(int argc, char **argv)
{
    int rpm;

    if (argc < 2) {
        printf("usage: rpm <0..%u>   (0 = stop pedalling)\n", (unsigned)RPM_MAX);
        return 1;
    }

    rpm = atoi(argv[1]);
    if (rpm < 0 || rpm > RPM_MAX) {
        printf("rpm: out of range 0..%u\n", RPM_MAX);
        return 1;
    }

    csc_sim_steady((uint16_t)rpm);
    printf("cadence -> %d rpm\n", rpm);

    return 0;
}

static int cmd_ride(int argc, char **argv)
{
    int top = 0;

    if (argc >= 2) {
        top = atoi(argv[1]);
        if (top < 60 || top > RPM_MAX) {
            printf("ride: triangle top out of range 60..%u rpm\n", (unsigned)RPM_MAX);
            return 1;
        }
    }

    csc_sim_ride((uint16_t)top);
    printf("ride profile running: cadence sweeps 40..%d rpm as a triangle wave\n",
           top ? top : 120);

    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    csc_sim_stop();
    printf("crank stopping\n");
    return 0;
}

static int cmd_battery(int argc, char **argv)
{
    csc_status_t st;

    if (argc >= 2) {
        if (strcmp(argv[1], "auto") == 0) {
            csc_sim_battery_auto();
            gatt_csc_notify_battery();
        } else {
            const int pct = atoi(argv[1]);
            if (pct < 0 || pct > 100) {
                printf("battery: out of range 0..100 (or `auto`)\n");
                return 1;
            }
            csc_sim_battery_set((uint8_t)pct);
            /* Tell a subscribed client about it, like a real gauge would. */
            gatt_csc_notify_battery();
        }
    }

    csc_sim_status(&st);
    printf("battery -> %u%% (%s)\n", (unsigned)st.battery,
           st.battery_auto ? "triangle wave" : "fixed");

    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    uint32_t lo;
    uint32_t hi;
    int left;
    int lo_s;
    int hi_s;

    if (argc < 2) {
        app_reset_range(&lo, &hi);
        left = app_reset_remaining_s();
        if (left < 0) {
            printf("scheduled reset: off\n");
        } else if (hi > lo) {
            printf("scheduled reset: in %d s (random %u..%u s)\n", left,
                   (unsigned)lo, (unsigned)hi);
        } else {
            printf("scheduled reset: in %d s (fixed %u s)\n", left, (unsigned)lo);
        }
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        app_reset_schedule(0, 0);
        printf("scheduled reset: off\n");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        app_reset_schedule(APP_RESET_MIN_S, APP_RESET_MAX_S);
        printf("scheduled reset: random %u..%u s\n",
               (unsigned)APP_RESET_MIN_S, (unsigned)APP_RESET_MAX_S);
        return 0;
    }

    lo_s = atoi(argv[1]);
    hi_s = (argc >= 3) ? atoi(argv[2]) : lo_s;
    if (lo_s < 10 || hi_s < lo_s || hi_s > 86400) {
        printf("usage: reset <10..86400> [max] | on | off\n");
        return 1;
    }

    app_reset_schedule((uint32_t)lo_s, (uint32_t)hi_s);
    printf("scheduled reset: %s %d..%d s\n", (hi_s > lo_s) ? "random" : "fixed", lo_s, hi_s);

    return 0;
}

static int cmd_adv(int argc, char **argv)
{
    if (argc < 2) {
        printf("advertising: %s\n", app_adv_on() ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        app_adv_set(true);
    } else if (strcmp(argv[1], "off") == 0) {
        app_adv_set(false);
    } else {
        printf("usage: adv <on|off>\n");
        return 1;
    }

    printf("advertising: %s\n", app_adv_on() ? "on" : "off");
    return 0;
}

static const esp_console_cmd_t s_cmds[] = {
    {
        .command = "status",
        .help = "show mode, cadence, link and counters",
        .func = cmd_status,
    }, {
        .command = "rpm",
        .help = "rpm <0..250>: hold a fixed cadence (0 stops)",
        .func = cmd_rpm,
    }, {
        .command = "ride",
        .help = "ride [60..250]: sweep the cadence as a triangle wave",
        .func = cmd_ride,
    }, {
        .command = "stop",
        .help = "stop pedalling (cadence -> 0)",
        .func = cmd_stop,
    }, {
        .command = "battery",
        .help = "battery [0..100|auto]: pin the level, or sweep it",
        .func = cmd_battery,
    }, {
        .command = "reset",
        .help = "reset [min] [max] | on | off: scheduled board resets (default random 600..1200 s)",
        .func = cmd_reset,
    }, {
        .command = "adv",
        .help = "adv <on|off>: start or stop connectable advertising",
        .func = cmd_adv,
    },
};

void cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    size_t i;

    repl_cfg.prompt = "csc>";
    repl_cfg.max_cmdline_length = 128;
    repl_cfg.task_stack_size = 4096;

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    for (i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&s_cmds[i]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
