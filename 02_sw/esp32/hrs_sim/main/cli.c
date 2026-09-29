/* Serial console for the heart-rate simulator.
 *
 * The console shares UART0 with the log output on the USB bridge, so the
 * instrument can be driven and watched over the flashing cable alone.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "gatt_hr.h"
#include "hr_sim.h"

#define BPM_MAX   250   /* the Helm One client ignores anything above this */
#define BPM_MIN   30

static int cmd_status(int argc, char **argv)
{
    hr_status_t st;
    int reset_left;
    uint32_t rlo;
    uint32_t rhi;

    hr_sim_status(&st);
    reset_left = app_reset_remaining_s();
    app_reset_range(&rlo, &rhi);

    printf("mode    : %s\n", hr_mode_name(st.mode));
    printf("heart   : %u bpm (target %u)\n", (unsigned)st.bpm, (unsigned)st.target_bpm);
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

static int cmd_bpm(int argc, char **argv)
{
    int bpm;

    if (argc < 2) {
        printf("usage: bpm <%u..%u>   (0 = strap off the body)\n",
               (unsigned)BPM_MIN, (unsigned)BPM_MAX);
        return 1;
    }

    bpm = atoi(argv[1]);
    if (bpm != 0 && (bpm < BPM_MIN || bpm > BPM_MAX)) {
        printf("bpm: out of range %u..%u (or 0 to take the strap off)\n", BPM_MIN, BPM_MAX);
        return 1;
    }

    hr_sim_steady((uint16_t)bpm);
    printf("heart rate -> %d bpm\n", bpm);

    return 0;
}

static int cmd_ride(int argc, char **argv)
{
    int top = 0;

    if (argc >= 2) {
        top = atoi(argv[1]);
        if (top < 90 || top > BPM_MAX) {
            printf("ride: triangle top out of range 90..%u bpm\n", BPM_MAX);
            return 1;
        }
    }

    hr_sim_ride((uint16_t)top);
    printf("ride profile running: heart rate sweeps 70..%d bpm as a triangle wave\n",
           top ? top : 160);

    return 0;
}

static int cmd_rest(int argc, char **argv)
{
    hr_sim_rest();
    printf("heart rate settling to rest\n");
    return 0;
}

static int cmd_off(int argc, char **argv)
{
    hr_sim_off_body();
    printf("strap off the body (no contact, no reading)\n");
    return 0;
}

static int cmd_battery(int argc, char **argv)
{
    hr_status_t st;

    if (argc >= 2) {
        if (strcmp(argv[1], "auto") == 0) {
            hr_sim_battery_auto();
            gatt_hr_notify_battery();
        } else {
            const int pct = atoi(argv[1]);
            if (pct < 0 || pct > 100) {
                printf("battery: out of range 0..100 (or `auto`)\n");
                return 1;
            }
            hr_sim_battery_set((uint8_t)pct);
            /* Tell a subscribed client about it, like a real gauge would. */
            gatt_hr_notify_battery();
        }
    }

    hr_sim_status(&st);
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
        .help = "show mode, heart rate, link and counters",
        .func = cmd_status,
    }, {
        .command = "bpm",
        .help = "bpm <30..250>: hold a fixed rate (0 takes the strap off)",
        .func = cmd_bpm,
    }, {
        .command = "ride",
        .help = "ride [90..250]: sweep the rate as a triangle wave",
        .func = cmd_ride,
    }, {
        .command = "rest",
        .help = "settle to a resting rate",
        .func = cmd_rest,
    }, {
        .command = "off",
        .help = "strap off the body: no contact, no reading",
        .func = cmd_off,
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

    repl_cfg.prompt = "hrs>";
    repl_cfg.max_cmdline_length = 128;
    repl_cfg.task_stack_size = 4096;

    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    for (i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&s_cmds[i]));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
