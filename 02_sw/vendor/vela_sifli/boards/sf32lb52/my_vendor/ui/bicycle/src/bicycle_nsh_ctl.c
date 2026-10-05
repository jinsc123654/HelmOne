/**
 * @file bicycle_nsh_ctl.c
 * @brief 自行车 UI — nsh_ctl。
 */

#include "bicycle_nsh.h"
#include "bicycle_gpx_sim.h"   /* 全国跳：bicycle_gnss_hop_start/stop（调试负载） */
#include "myvendor_bicycle_ctl.h"
#include "vmap/vmap_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char * name;
    const char * blurb;
    double from_lon;
    double from_lat;
    double to_lon;
    double to_lat;
} bicycle_nsh_nav_sim_demo_t;

static const bicycle_nsh_nav_sim_demo_t g_nav_sim_demos[] = {
    {
        "border",
        "GPX -> east border (~3 km); dest 26 m across lat grid -> overlap single-region",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_BORDER_DEMO_TO_LON, VMAP_ROUTE_BORDER_DEMO_TO_LAT,
    },
    {
        "east",
        "GPX -> east one VREG cell (~8 km); corridor 2 regions",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_CROSS_EAST_DEMO_TO_LON, VMAP_ROUTE_CROSS_EAST_DEMO_TO_LAT,
    },
    {
        "east-long",
        "GPX -> east ~17 km; corridor 2-3 regions",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_CROSS_EAST_LONG_DEMO_TO_LON, VMAP_ROUTE_CROSS_EAST_LONG_DEMO_TO_LAT,
    },
    {
        "west",
        "GPX -> west ~9 km; corridor 2 regions",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_CROSS_WEST_DEMO_TO_LON, VMAP_ROUTE_CROSS_WEST_DEMO_TO_LAT,
    },
    {
        "south",
        "GPX -> south ~12 km; corridor 2 regions (lat grid)",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_CROSS_SOUTH_DEMO_TO_LON, VMAP_ROUTE_CROSS_SOUTH_DEMO_TO_LAT,
    },
    {
        "ne",
        "GPX -> NE diagonal ~25 km; corridor 3+ regions",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_CROSS_NE_DEMO_TO_LON, VMAP_ROUTE_CROSS_NE_DEMO_TO_LAT,
    },
    {
        "far-east",
        "GPX -> east ~30 km; multi-region + rolling refine stress",
        VMAP_ROUTE_GPX_START_LON, VMAP_ROUTE_GPX_START_LAT,
        VMAP_ROUTE_CROSS_FAR_E_DEMO_TO_LON, VMAP_ROUTE_CROSS_FAR_E_DEMO_TO_LAT,
    },
    {
        "reroute",
        "Sparse 恒丰路 start -> east border; reroute / snap stress",
        VMAP_ROUTE_REROUTE_DEMO_FROM_LON, VMAP_ROUTE_REROUTE_DEMO_FROM_LAT,
        VMAP_ROUTE_REROUTE_DEMO_TO_LON, VMAP_ROUTE_REROUTE_DEMO_TO_LAT,
    },
    {
        "cross",
        "GPX -> northwest ~20 km (legacy cross-VREG demo)",
        VMAP_ROUTE_CROSS_DEMO_FROM_LON, VMAP_ROUTE_CROSS_DEMO_FROM_LAT,
        VMAP_ROUTE_CROSS_DEMO_TO_LON, VMAP_ROUTE_CROSS_DEMO_TO_LAT,
    },
    {
        "nanjing-short",
        "GPX -> ESE ~18 km (partial east map)",
        VMAP_ROUTE_NANJING_DEMO_FROM_LON, VMAP_ROUTE_NANJING_DEMO_FROM_LAT,
        VMAP_ROUTE_NANJING_SHORT_TO_LON, VMAP_ROUTE_NANJING_SHORT_TO_LAT,
    },
    {
        "nanjing",
        "GPX -> east toward Nanjing ~48 km (needs east map pack)",
        VMAP_ROUTE_NANJING_DEMO_FROM_LON, VMAP_ROUTE_NANJING_DEMO_FROM_LAT,
        VMAP_ROUTE_NANJING_DEMO_TO_LON, VMAP_ROUTE_NANJING_DEMO_TO_LAT,
    },
};

/**
 * @brief 自行车 nsh nav sim demo find。
 */
static const bicycle_nsh_nav_sim_demo_t * bicycle_nsh_nav_sim_demo_find(const char * name)
{
    if (!name) {
        return NULL;
    }
    for (unsigned i = 0; i < sizeof(g_nav_sim_demos) / sizeof(g_nav_sim_demos[0]); i++) {
        if (strcmp(g_nav_sim_demos[i].name, name) == 0) {
            return &g_nav_sim_demos[i];
        }
    }
    return NULL;
}

static void bicycle_nsh_print_nav_sim_demos(void)
{
    printf("Named nav sim presets (bicycle_nsh nav sim <name>):\n");
    for (unsigned i = 0; i < sizeof(g_nav_sim_demos) / sizeof(g_nav_sim_demos[0]); i++) {
        const bicycle_nsh_nav_sim_demo_t * d = &g_nav_sim_demos[i];

        printf("  %-14s  %.7f,%.7f -> %.7f,%.7f\n",
            d->name, d->from_lon, d->from_lat, d->to_lon, d->to_lat);
        printf("                  %s\n", d->blurb);
    }
    printf("  <to_lon> <to_lat>              plan from GNSS/GPX\n");
    printf("  <from_lon> <from_lat> <to_lon> <to_lat>  explicit coords\n");
}

static void bicycle_nsh_print_usage(void)
{
    printf("Usage:\n");
    printf("  bicycle_nsh style <name>   post STYLE_SET trigger (LVGL thread applies)\n");
    printf("  bicycle_nsh style list     list theme names\n");
    printf("  bicycle_nsh lang <name>    post LOCALE_SET trigger (LVGL thread applies)\n");
    printf("  bicycle_nsh lang list      list locale names\n");
    printf("  bicycle_nsh notify [title] <text>  top notification (title optional)\n");
    printf("  bicycle_nsh bottom <text>        bottom system toast\n");
    printf("  bicycle_nsh nav trip <lon1> <lat1> [<lon2> <lat2> ...]  multi-waypoint\n");
    printf("  bicycle_nsh nav <to_lon> <to_lat>              plan from GNSS to dest\n");
    printf("  bicycle_nsh nav sim speed [<kph>]           route sim speed (default %.0f)\n",
        VMAP_NAV_SIM_SPEED_KPH);
    printf("  bicycle_nsh nav sim <to_lon> <to_lat>          plan + drive route\n");
    printf("  bicycle_nsh nav <from_lon> <from_lat> <to_lon> <to_lat>  explicit route\n");
    printf("  bicycle_nsh nav sim <name>                       named cross-region preset\n");
    printf("  bicycle_nsh nav sim list                         list sim presets + coords\n");
    printf("  bicycle_nsh nav sim <from_lon> <from_lat> <to_lon> <to_lat>  explicit sim\n");
    printf("  bicycle_nsh nav stop                           cancel navigation\n");
    printf("  bicycle_nsh nav help                           navigation help + examples\n");
    printf("  bicycle_nsh zoom                               show tile z + scale\n");
    printf("  bicycle_nsh zoom in|out                        scale x%.2f step\n", VMAP_SCALE_STEP);
    printf("  bicycle_nsh zoom zin|zout                      tile z +/- 1\n");
    printf("  bicycle_nsh zoom z <11-15>                     set tile zoom\n");
    printf("  bicycle_nsh zoom scale <0.25-8>                set display scale\n");
    printf("  bicycle_nsh zoom reset                         default z + scale\n");
    printf("  bicycle_nsh gnss sim <gpx> [fwd|rev] [kph] [10km|at 10]\n");
    printf("  bicycle_nsh gnss stop                          stop GPX GNSS replay\n");
    printf("  bicycle_nsh demo on|off|toggle                 demo mode (4 steps, loops)\n");
    printf("  bicycle_nsh demo status                        which step it is on\n");
    printf("Themes:");
    for (unsigned i = 0; i < myvendor_bicycle_ctl_style_name_count(); i++) {
        printf(" %s", myvendor_bicycle_ctl_style_name(i));
    }
    printf("\nLocales:");
    for (unsigned i = 0; i < myvendor_bicycle_ctl_locale_name_count(); i++) {
        printf(" %s", myvendor_bicycle_ctl_locale_name(i));
    }
    printf("\n");
    printf("UI must be running (rcS: bicycle &).\n");
}

static void bicycle_nsh_print_nav_usage(void)
{
    printf("Navigation (bicycle_nsh nav ...):\n");
    printf("  nav <to_lon> <to_lat>\n");
    printf("      Plan from current GNSS/GPX to destination.\n");
    printf("  nav sim speed [<kph>]\n");
    printf("      Set route-follow speed (5-360 km/h, default %.0f). No arg: show current.\n",
        VMAP_NAV_SIM_SPEED_KPH);
    printf("  nav sim <to_lon> <to_lat>\n");
    printf("      Plan then drive the route (simulation).\n");
    printf("  nav <from_lon> <from_lat> <to_lon> <to_lat>\n");
    printf("      Plan with explicit start and end.\n");
    printf("  nav sim <name>\n");
    printf("      Named cross-region preset (see: nav sim list).\n");
    printf("  nav sim list\n");
    printf("      List all named sim presets with coordinates.\n");
    printf("  nav sim <from_lon> <from_lat> <to_lon> <to_lat>\n");
    printf("      Explicit start/end + route simulation.\n");
    printf("  nav trip <lon1> <lat1> [<lon2> <lat2> ...]\n");
    printf("      Multi-waypoint route book.\n");
    printf("  nav stop\n");
    printf("      Cancel navigation.\n");
    printf("Examples:\n");
    printf("  bicycle_nsh nav sim list\n");
    printf("  bicycle_nsh nav sim border\n");
    printf("  bicycle_nsh nav sim east-long\n");
    printf("  bicycle_nsh nav sim ne\n");
    printf("  bicycle_nsh nav sim reroute\n");
    printf("  bicycle_nsh nav 118.3604443 32.2316347\n");
    printf("  bicycle_nsh nav sim 118.520000 32.200000\n");
    printf("  bicycle_nsh nav stop\n");
}

static int bicycle_nsh_cmd_style(int argc, char * argv[])
{
    if (argc < 1 || strcmp(argv[0], "list") == 0 ||
        strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
        char ui_style[32];
        bool have_ui = myvendor_bicycle_ctl_state_get(
            MYVENDOR_BICYCLE_CTL_STATE_MAP_STYLE, ui_style, sizeof(ui_style));

        printf("map styles:");
        for (unsigned i = 0; i < myvendor_bicycle_ctl_style_name_count(); i++) {
            const char * name = myvendor_bicycle_ctl_style_name(i);
            const char * mark = " ";

            if (have_ui && strcmp(name, ui_style) == 0) {
                mark = "*";
            }

            printf("\n  %s%s", mark, name);
        }
        if (have_ui) {
            printf("\n  (* = active on running UI / LVGL thread)\n");
        } else if (myvendor_bicycle_ctl_ui_alive()) {
            printf("\n  (UI running; style state not yet published)\n");
        } else {
            printf("\n  (UI not running)\n");
        }
        return 0;
    }

    if (!myvendor_bicycle_ctl_style_name_valid(argv[0])) {
        fprintf(stderr,
            "bicycle_nsh: unknown style \"%s\" (try: bicycle_nsh style list)\n",
            argv[0]);
        return 1;
    }

    if (!myvendor_bicycle_ctl_ui_alive()) {
        fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
        return 1;
    }

    if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_STYLE_SET, argv[0]) != 0) {
        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
        return 1;
    }

    printf("bicycle_nsh: STYLE_SET trigger posted (LVGL thread applies \"%s\")\n",
        argv[0]);
    return 0;
}

static int bicycle_nsh_cmd_lang(int argc, char * argv[])
{
    if (argc < 1 || strcmp(argv[0], "list") == 0 ||
        strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
        char ui_locale[32];
        bool have_ui = myvendor_bicycle_ctl_state_get(
            MYVENDOR_BICYCLE_CTL_STATE_UI_LOCALE, ui_locale, sizeof(ui_locale));

        printf("locales:");
        for (unsigned i = 0; i < myvendor_bicycle_ctl_locale_name_count(); i++) {
            const char * name = myvendor_bicycle_ctl_locale_name(i);
            const char * mark = " ";

            if (have_ui && strcmp(name, ui_locale) == 0) {
                mark = "*";
            }

            printf("\n  %s%s", mark, name);
        }
        if (have_ui) {
            printf("\n  (* = active on running UI / LVGL thread)\n");
        } else if (myvendor_bicycle_ctl_ui_alive()) {
            printf("\n  (UI running; locale state not yet published)\n");
        } else {
            printf("\n  (UI not running)\n");
        }
        return 0;
    }

    if (!myvendor_bicycle_ctl_locale_name_valid(argv[0])) {
        fprintf(stderr,
            "bicycle_nsh: unknown locale \"%s\" (try: bicycle_nsh lang list)\n",
            argv[0]);
        return 1;
    }

    if (!myvendor_bicycle_ctl_ui_alive()) {
        fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
        return 1;
    }

    if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_LOCALE_SET, argv[0]) != 0) {
        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
        return 1;
    }

    printf("bicycle_nsh: LOCALE_SET trigger posted (LVGL thread applies \"%s\")\n",
        argv[0]);
    return 0;
}

static int bicycle_nsh_join_args(int argc, char * argv[], char * out, size_t out_sz)
{
    size_t pos = 0;

    if (argc < 1 || out == NULL || out_sz == 0) {
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        const size_t len = strlen(argv[i]);
        const size_t need = (i == 0 ? 0 : 1) + len;

        if (pos + need >= out_sz) {
            return -1;
        }

        if (i > 0) {
            out[pos++] = ' ';
        }

        memcpy(out + pos, argv[i], len);
        pos += len;
    }

    out[pos] = '\0';
    return 0;
}

static int bicycle_nsh_post_overlay(myvendor_bicycle_ctl_op_t op, const char * arg)
{
    if (!myvendor_bicycle_ctl_ui_alive()) {
        fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
        return 1;
    }

    if (myvendor_bicycle_ctl_post(op, arg) != 0) {
        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
        return 1;
    }

    return 0;
}

static int bicycle_nsh_cmd_notify(int argc, char * argv[])
{
    char payload[MYVENDOR_BICYCLE_CTL_ARG_MAX];

    if (argc < 1) {
        if (bicycle_nsh_post_overlay(MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW, NULL) != 0) {
            return 1;
        }
        printf("bicycle_nsh: default top notification posted\n");
        return 0;
    }

    if (argc == 1) {
        if (bicycle_nsh_post_overlay(MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW, argv[0]) != 0) {
            return 1;
        }
        printf("bicycle_nsh: top notification posted\n");
        return 0;
    }

    if (snprintf(payload, sizeof(payload), "%s|", argv[0]) >= (int)sizeof(payload)) {
        fprintf(stderr, "bicycle_nsh: title too long\n");
        return 1;
    }

    if (bicycle_nsh_join_args(argc - 1, argv + 1,
            payload + strlen(payload),
            sizeof(payload) - strlen(payload)) != 0) {
        fprintf(stderr, "bicycle_nsh: notification text too long\n");
        return 1;
    }

    if (bicycle_nsh_post_overlay(MYVENDOR_BICYCLE_CTL_OP_NOTIFY_SHOW, payload) != 0) {
        return 1;
    }

    printf("bicycle_nsh: top notification posted\n");
    return 0;
}

static int bicycle_nsh_cmd_bottom(int argc, char * argv[])
{
    char payload[MYVENDOR_BICYCLE_CTL_ARG_MAX];

    if (argc < 1) {
        if (bicycle_nsh_post_overlay(MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW, NULL) != 0) {
            return 1;
        }
        printf("bicycle_nsh: default bottom toast posted\n");
        return 0;
    }

    if (bicycle_nsh_join_args(argc, argv, payload, sizeof(payload)) != 0) {
        fprintf(stderr, "bicycle_nsh: bottom text too long\n");
        return 1;
    }

    if (bicycle_nsh_post_overlay(MYVENDOR_BICYCLE_CTL_OP_BOTTOM_SHOW, payload) != 0) {
        return 1;
    }

    printf("bicycle_nsh: bottom toast posted\n");
    return 0;
}

/* 起/停骑行（= UI 的"开始记录"）。串口驱动整条测试链的最后一块：
 * NAV_PLAN 只有在地图/骑行页才落到地图上，所以必须先 RIDE_START。 */
static int bicycle_nsh_cmd_ride(int argc, char * argv[])
{
    if (argc < 1) {
        fprintf(stderr, "bicycle_nsh: ride start|stop\n");
        return 1;
    }

    if (strcmp(argv[0], "start") == 0) {
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_RIDE_START, "") != 0) {
            fprintf(stderr, "bicycle_nsh: RIDE_START post failed\n");
            return 1;
        }
        printf("bicycle_nsh: RIDE_START posted\n");
        return 0;
    }

    if (strcmp(argv[0], "stop") == 0) {
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_RIDE_STOP, "") != 0) {
            fprintf(stderr, "bicycle_nsh: RIDE_STOP post failed\n");
            return 1;
        }
        printf("bicycle_nsh: RIDE_STOP posted\n");
        return 0;
    }

    fprintf(stderr, "bicycle_nsh: ride start|stop\n");
    return 1;
}

static int bicycle_nsh_cmd_nav(int argc, char * argv[])
{
    char payload[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    int n;

    if (argc < 1 || strcmp(argv[0], "help") == 0 || strcmp(argv[0], "-h") == 0
        || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "list") == 0) {
        bicycle_nsh_print_nav_usage();
        return 0;
    }

    if (argc >= 1 && strcmp(argv[0], "trip") == 0) {
        int pos = 0;
        int i;

        if ((argc - 1) < 2 || ((argc - 1) % 2) != 0) {
            fprintf(stderr,
                "bicycle_nsh: nav trip needs lon/lat pairs: lon1 lat1 [lon2 lat2 ...]\n");
            bicycle_nsh_print_nav_usage();
            return 1;
        }
        if (!myvendor_bicycle_ctl_ui_alive()) {
            fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
            return 1;
        }
        for (i = 1; i < argc; i += 2) {
            if (i > 1) {
                if (pos >= (int)sizeof(payload) - 1) {
                    break;
                }
                payload[pos++] = ';';
            }
            n = snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                "%s,%s", argv[i], argv[i + 1]);
            if (n < 0 || pos + n >= (int)sizeof(payload)) {
                fprintf(stderr, "bicycle_nsh: nav trip payload too long\n");
                return 1;
            }
            pos += n;
        }
        payload[pos] = '\0';
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_NAV_TRIP, payload) != 0) {
            fprintf(stderr, "bicycle_nsh: ctl post failed\n");
            return 1;
        }
        printf("bicycle_nsh: NAV_TRIP posted (%s)\n", payload);
        return 0;
    }

    if (argc >= 1 && (strcmp(argv[0], "stop") == 0 || strcmp(argv[0], "off") == 0)) {
        if (!myvendor_bicycle_ctl_ui_alive()) {
            fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
            return 1;
        }
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_NAV_STOP, NULL) != 0) {
            fprintf(stderr, "bicycle_nsh: ctl post failed\n");
            return 1;
        }
        printf("bicycle_nsh: NAV_STOP posted\n");
        return 0;
    }

    if (argc >= 1 && strcmp(argv[0], "sim") == 0) {
        if (argc >= 2 && strcmp(argv[1], "speed") == 0) {
            if (argc == 2) {
                printf("nav sim speed: default %.0f km/h (set: nav sim speed <kph>)\n",
                    VMAP_NAV_SIM_SPEED_KPH);
                if (myvendor_bicycle_ctl_ui_alive()) {
                    if (myvendor_bicycle_ctl_post(
                            MYVENDOR_BICYCLE_CTL_OP_NAV_SIM_SPEED, NULL) != 0) {
                        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
                        return 1;
                    }
                }
                return 0;
            }
            if (argc != 3) {
                fprintf(stderr,
                    "bicycle_nsh: nav sim speed needs <kph> (5-360), e.g. nav sim speed 120\n");
                return 1;
            }
            if (!myvendor_bicycle_ctl_ui_alive()) {
                fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
                return 1;
            }
            if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_NAV_SIM_SPEED,
                    argv[2]) != 0) {
                fprintf(stderr, "bicycle_nsh: ctl post failed\n");
                return 1;
            }
            printf("bicycle_nsh: nav sim speed %s km/h posted\n", argv[2]);
            return 0;
        }

        myvendor_bicycle_ctl_op_t op = MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN_SIM;
        const bicycle_nsh_nav_sim_demo_t * demo = NULL;

        if (argc == 2 && (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "help") == 0)) {
            bicycle_nsh_print_nav_sim_demos();
            return 0;
        }

        if (argc == 2) {
            demo = bicycle_nsh_nav_sim_demo_find(argv[1]);
            if (demo == NULL) {
                fprintf(stderr,
                    "bicycle_nsh: unknown nav sim preset \"%s\" (try: nav sim list)\n",
                    argv[1]);
                return 1;
            }
            n = snprintf(payload, sizeof(payload), "%.7f,%.7f,%.7f,%.7f",
                demo->from_lon, demo->from_lat, demo->to_lon, demo->to_lat);
        } else if (argc == 3) {
            n = snprintf(payload, sizeof(payload), "%s,%s", argv[1], argv[2]);
        } else if (argc == 5) {
            n = snprintf(payload, sizeof(payload), "%s,%s,%s,%s",
                argv[1], argv[2], argv[3], argv[4]);
        } else {
            fprintf(stderr,
                "bicycle_nsh: nav sim needs list, <name>, <to_lon> <to_lat>, or four coords\n");
            bicycle_nsh_print_nav_usage();
            return 1;
        }

        if (!myvendor_bicycle_ctl_ui_alive()) {
            fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
            return 1;
        }
        if (n < 0 || n >= (int)sizeof(payload)) {
            fprintf(stderr, "bicycle_nsh: nav coords too long\n");
            return 1;
        }
        if (myvendor_bicycle_ctl_post(op, payload) != 0) {
            fprintf(stderr, "bicycle_nsh: ctl post failed\n");
            return 1;
        }
        if (demo != NULL) {
            printf("bicycle_nsh: NAV_PLAN_SIM %s posted (%s)\n",
                demo->name, demo->blurb);
        } else {
            printf("bicycle_nsh: NAV_PLAN_SIM posted (%s)\n", payload);
        }
        return 0;
    }

    if (argc != 2 && argc != 4) {
        fprintf(stderr,
            "bicycle_nsh: nav needs <to_lon> <to_lat> or four coords (see nav help)\n");
        bicycle_nsh_print_nav_usage();
        return 1;
    }

    if (!myvendor_bicycle_ctl_ui_alive()) {
        fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
        return 1;
    }

    if (argc == 2) {
        n = snprintf(payload, sizeof(payload), "%s,%s", argv[0], argv[1]);
    } else {
        n = snprintf(payload, sizeof(payload), "%s,%s,%s,%s",
            argv[0], argv[1], argv[2], argv[3]);
    }

    if (n < 0 || n >= (int)sizeof(payload)) {
        fprintf(stderr, "bicycle_nsh: nav coords too long\n");
        return 1;
    }

    if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_NAV_PLAN, payload) != 0) {
        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
        return 1;
    }

    printf("bicycle_nsh: NAV_PLAN posted (%s)\n", payload);
    return 0;
}

static int bicycle_nsh_post_zoom(const char * payload)
{
    if (!myvendor_bicycle_ctl_ui_alive()) {
        fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
        return 1;
    }

    if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_MAP_ZOOM, payload) != 0) {
        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
        return 1;
    }

    printf("bicycle_nsh: MAP_ZOOM posted (%s)\n", payload);
    return 0;
}

static int bicycle_nsh_cmd_zoom(int argc, char * argv[])
{
    char payload[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    char zoom_state[48];

    if (argc < 1 || strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
        if (myvendor_bicycle_ctl_state_get(MYVENDOR_BICYCLE_CTL_STATE_MAP_ZOOM,
                zoom_state, sizeof(zoom_state))) {
            printf("map zoom: %s\n", zoom_state);
        } else if (myvendor_bicycle_ctl_ui_alive()) {
            printf("map zoom: (UI running; state not yet published)\n");
        } else {
            printf("map zoom: (UI not running)\n");
        }
        return 0;
    }

    if (strcmp(argv[0], "in") == 0 || strcmp(argv[0], "out") == 0
        || strcmp(argv[0], "reset") == 0 || strcmp(argv[0], "zin") == 0
        || strcmp(argv[0], "zout") == 0) {
        return bicycle_nsh_post_zoom(argv[0]);
    }

    if (strcmp(argv[0], "z") == 0) {
        int z;

        if (argc < 2) {
            fprintf(stderr, "bicycle_nsh: zoom z needs level %d..%d\n",
                VMAP_ZOOM_MIN, VMAP_ZOOM_MAX);
            return 1;
        }

        z = atoi(argv[1]);
        if (z < VMAP_ZOOM_MIN || z > VMAP_ZOOM_MAX) {
            fprintf(stderr, "bicycle_nsh: tile zoom must be %d..%d\n",
                VMAP_ZOOM_MIN, VMAP_ZOOM_MAX);
            return 1;
        }

        if (snprintf(payload, sizeof(payload), "z,%d", z) >= (int)sizeof(payload)) {
            return 1;
        }
        return bicycle_nsh_post_zoom(payload);
    }

    if (strcmp(argv[0], "scale") == 0) {
        if (argc < 2) {
            fprintf(stderr, "bicycle_nsh: zoom scale needs value 0.25..8\n");
            return 1;
        }

        if (snprintf(payload, sizeof(payload), "s,%s", argv[1]) >= (int)sizeof(payload)) {
            return 1;
        }
        return bicycle_nsh_post_zoom(payload);
    }

    {
        char * end = NULL;
        long z = strtol(argv[0], &end, 10);

        if (end != NULL && *end == '\0' && z >= VMAP_ZOOM_MIN && z <= VMAP_ZOOM_MAX) {
            if (snprintf(payload, sizeof(payload), "z,%ld", z) >= (int)sizeof(payload)) {
                return 1;
            }
            return bicycle_nsh_post_zoom(payload);
        }
    }

    fprintf(stderr,
        "bicycle_nsh: zoom: use in|out|zin|zout|reset|z <level>|scale <value>\n");
    return 1;
}

static bool bicycle_nsh_token_is_kph(const char * s, float * out_kph)
{
    char * end = NULL;
    float v;

    if (s == NULL || s[0] == '\0') {
        return false;
    }

    v = (float)strtod(s, &end);
    if (end == s || *end != '\0') {
        return false;
    }

    if (out_kph != NULL) {
        *out_kph = v;
    }

    return true;
}

static bool bicycle_nsh_parse_skip_km(const char * s, float * out_km)
{
    char * end = NULL;
    const char * p = s;
    float v;

    if (s == NULL || s[0] == '\0') {
        return false;
    }

    if (strncmp(s, "at", 2) == 0 && s[2] != '\0' && s[2] != '-') {
        p = s + 2;
        if (*p == '=' || *p == ':') {
            p++;
        }
    } else if (strncmp(s, "skip", 4) == 0 && s[4] != '\0') {
        p = s + 4;
        if (*p == '=' || *p == ':') {
            p++;
        }
    }

    v = (float)strtod(p, &end);
    if (end == p) {
        return false;
    }

    if (*end == '\0') {
        if (p != s) {
            if (out_km) {
                *out_km = v;
            }
            return true;
        }
        return false;
    }

    if (strcmp(end, "km") == 0 || strcmp(end, "KM") == 0
        || strcmp(end, "k") == 0) {
        if (out_km) {
            *out_km = v;
        }
        return true;
    }

    return false;
}

static bool bicycle_nsh_token_is_skip_kw(const char * s)
{
    return s != NULL
        && (strcmp(s, "at") == 0 || strcmp(s, "skip") == 0
            || strcmp(s, "from") == 0);
}

static bool bicycle_nsh_token_is_dir(const char * s, bool * out_rev)
{
    if (s == NULL) {
        return false;
    }

    if (strcmp(s, "rev") == 0 || strcmp(s, "reverse") == 0
        || strcmp(s, "back") == 0) {
        if (out_rev != NULL) {
            *out_rev = true;
        }
        return true;
    }

    if (strcmp(s, "fwd") == 0 || strcmp(s, "forward") == 0
        || strcmp(s, "seq") == 0) {
        if (out_rev != NULL) {
            *out_rev = false;
        }
        return true;
    }

    return false;
}

static void bicycle_nsh_print_gnss_usage(void)
{
    printf("GNSS GPX replay (bicycle_nsh gnss ...):\n");
    printf("  gnss sim <gpx> [fwd|rev] [kph] [10km|at 10|skip 10] [loop]\n");
    printf("      Replay track points as onboard GNSS (overrides MAX-M10S).\n");
    printf("      fwd = start to finish (default); rev = finish to start.\n");
    printf("      kph optional (1-360); omit = 15-40 km/h ramp.\n");
    printf("      10km / at 10: jump there; prefix is already ridden.\n");
    printf("      loop: at the track end TURN AROUND and keep riding (soak tests;\n");
    printf("            without it the replay just stops at the last point).\n");
    printf("  gnss stop\n");
    printf("      Stop replay; onboard GNSS resumes.\n");
    printf("  gnss hop [period_ms] [count]\n");
    printf("      Debug: jump between far-apart cities (fixed sequence) to force\n");
    printf("      full tile reload + repaint -- a REPEATABLE heavy load for A/B.\n");
    printf("      period_ms default 2000, count 0 = until 'gnss hop stop'.\n");
    printf("Examples:\n");
    printf("  bicycle_nsh gnss sim /mnt/lfs/mtp/record/TRK_20260831_102750.gpx\n");
    printf("  bicycle_nsh gnss sim /mnt/lfs/mtp/record/TRK_20260831_102750.gpx rev\n");
    printf("  bicycle_nsh gnss sim /mnt/lfs/mtp/record/TRK_20260831_102750.gpx fwd 30\n");
    printf("  bicycle_nsh gnss sim /mnt/lfs/mtp/record/TRK_20260912_193207.gpx 10km\n");
    printf("  bicycle_nsh gnss sim /mnt/lfs/mtp/record/TRK_20260912_193207.gpx at 10\n");
    printf("  bicycle_nsh gnss stop\n");
}

static int bicycle_nsh_cmd_gnss(int argc, char * argv[])
{
    char payload[MYVENDOR_BICYCLE_CTL_ARG_MAX];
    const char * path = NULL;
    bool reverse = false;
    bool loop = false;
    float kph = 0.0f;
    float skip_km = 0.0f;
    int i;
    int n;

    if (argc < 1 || strcmp(argv[0], "help") == 0 || strcmp(argv[0], "-h") == 0
        || strcmp(argv[0], "--help") == 0) {
        bicycle_nsh_print_gnss_usage();
        return 0;
    }

    /*
     * 全国跳：调试用合成定位（固定城市序列大跳），用来造**可重复的大渲染负载**
     * —— 小幅平移常常一个新瓦片都不需要，`[vperf] frame` 就不出帧。
     * 实现在 bicycle_gpx_sim.c（不建线程，getter 里按时间换点）。
     */
    if (strcmp(argv[0], "hop") == 0) {
        unsigned period_ms = 2000u;
        unsigned count = 0u;

        if (argc >= 2 && (strcmp(argv[1], "stop") == 0 || strcmp(argv[1], "off") == 0)) {
            bicycle_gnss_hop_stop();
            printf("bicycle_nsh: gnss hop stop\n");
            return 0;
        }

        if (argc >= 2) {
            period_ms = (unsigned)strtoul(argv[1], NULL, 10);
        }
        if (argc >= 3) {
            count = (unsigned)strtoul(argv[2], NULL, 10);
        }

        bicycle_gnss_hop_start(period_ms, count);
        printf("bicycle_nsh: gnss hop start period=%u ms count=%u (0=until stop)\n",
            period_ms, count);
        return 0;
    }

    if (strcmp(argv[0], "stop") == 0 || strcmp(argv[0], "off") == 0) {
        if (!myvendor_bicycle_ctl_ui_alive()) {
            fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
            return 1;
        }
        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_GNSS_SIM, "stop") != 0) {
            fprintf(stderr, "bicycle_nsh: ctl post failed\n");
            return 1;
        }
        printf("bicycle_nsh: GNSS_SIM stop posted\n");
        return 0;
    }

    if (strcmp(argv[0], "sim") != 0) {
        fprintf(stderr, "bicycle_nsh: gnss needs sim <gpx> or stop\n");
        bicycle_nsh_print_gnss_usage();
        return 1;
    }

    for (i = 1; i < argc; i++) {
        bool rev = false;
        float km = 0.0f;

        if (bicycle_nsh_token_is_dir(argv[i], &rev)) {
            reverse = rev;
            continue;
        }

        if (bicycle_nsh_token_is_skip_kw(argv[i])) {
            if (i + 1 >= argc
                || !bicycle_nsh_parse_skip_km(argv[i + 1], &km)) {
                if (i + 1 >= argc
                    || !bicycle_nsh_token_is_kph(argv[i + 1], &km)) {
                    fprintf(stderr, "bicycle_nsh: %s needs a km value\n",
                        argv[i]);
                    bicycle_nsh_print_gnss_usage();
                    return 1;
                }
            }
            skip_km = km;
            i++;
            continue;
        }

        if (bicycle_nsh_parse_skip_km(argv[i], &km)) {
            skip_km = km;
            continue;
        }

        if (bicycle_nsh_token_is_kph(argv[i], &kph)) {
            continue;
        }

        /* 长测用：`loop` ⇒ 到端点掉头继续骑（默认关闭 = 到端点停住）。 */
        if (strcmp(argv[i], "loop") == 0) {
            loop = true;
            continue;
        }

        if (path != NULL) {
            fprintf(stderr, "bicycle_nsh: extra gnss sim arg \"%s\"\n", argv[i]);
            bicycle_nsh_print_gnss_usage();
            return 1;
        }

        path = argv[i];
    }

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "bicycle_nsh: gnss sim needs a GPX path\n");
        bicycle_nsh_print_gnss_usage();
        return 1;
    }

    if (!myvendor_bicycle_ctl_ui_alive()) {
        fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
        return 1;
    }

    if (skip_km > 0.0f) {
        n = snprintf(payload, sizeof(payload), "%s|%s|%.1f|%.0f",
            reverse ? "rev" : "fwd", path, (double)kph,
            (double)skip_km * 1000.0);
    } else if (kph > 0.0f) {
        n = snprintf(payload, sizeof(payload), "%s|%s|%.1f",
            reverse ? "rev" : "fwd", path, (double)kph);
    } else {
        n = snprintf(payload, sizeof(payload), "%s|%s",
            reverse ? "rev" : "fwd", path);
    }

    if (n < 0 || n >= (int)sizeof(payload)) {
        fprintf(stderr, "bicycle_nsh: gnss sim path too long\n");
        return 1;
    }

    /* 循环标志要在投递之前设好（投递只是让 UI 去 start，跨任务，标志只在到端点时读）。*/
    bicycle_gpx_sim_set_loop(loop);

    if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_GNSS_SIM, payload) != 0) {
        fprintf(stderr, "bicycle_nsh: ctl post failed\n");
        return 1;
    }

    printf("bicycle_nsh: GNSS_SIM posted (%s) loop=%d\n", payload, (int)loop);
    return 0;
}

/**
 * @brief `bicycle_nsh demo on|off|toggle|status` —— 演示模式（四步循环）。
 *
 * @details 真正的动作在 UI 线程（`bicycle_demo_poll()`），这里只投一条 ctl 命令，
 *          再读回状态槽里的"当前那一步"（UI 侧每步都会写，见 bicycle_demo.c）。
 *          四步：GPX 导航 → 坐标点导航 → 全国跳点 → 换主题，跑完回到第一步。
 */
static int bicycle_nsh_cmd_demo(int argc, char * argv[])
{
    char state[64];
    const char * arg = (argc >= 1) ? argv[0] : NULL;

    if (argc >= 1 && (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "-h") == 0)) {
        printf("Demo mode (bicycle_nsh demo ...):\n");
        printf("  demo on        start the 4-step loop (GPX nav / coord nav / China hops / theme)\n");
        printf("  demo off       stop, restore theme + zoom\n");
        printf("  demo toggle    flip it\n");
        printf("  demo status    print the current step (no change)\n");
        return 0;
    }

    if (arg == NULL || arg[0] == '\0') {
        arg = "on";
    }

    if (strcmp(arg, "status") != 0) {
        if (!myvendor_bicycle_ctl_ui_alive()) {
            fprintf(stderr, "bicycle_nsh: UI not running (start via rcS / bicycle &)\n");
            return 1;
        }

        if (myvendor_bicycle_ctl_post(MYVENDOR_BICYCLE_CTL_OP_DEMO, arg) != 0) {
            fprintf(stderr, "bicycle_nsh: ctl post failed\n");
            return 1;
        }

        printf("bicycle_nsh: DEMO %s posted\n", arg);
        /* 起/停要等 UI 那一拍跑完才有状态可读，给一小段（这一步本来就在 UI 循环里）。 */
        usleep(200000);
    }

    if (myvendor_bicycle_ctl_state_get(MYVENDOR_BICYCLE_CTL_STATE_DEMO, state,
            sizeof(state))) {
        printf("bicycle_nsh: demo = %s\n", state);
    } else {
        printf("bicycle_nsh: demo = off\n");
    }

    return 0;
}

/**
 * @brief 自行车 nsh dispatch。
 */
int bicycle_nsh_dispatch(int argc, char * argv[])
{
    if (argc < 1) {
        bicycle_nsh_print_usage();
        return 1;
    }

    if (strcmp(argv[0], "demo") == 0) {
        return bicycle_nsh_cmd_demo(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "style") == 0) {
        return bicycle_nsh_cmd_style(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "lang") == 0) {
        return bicycle_nsh_cmd_lang(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "notify") == 0) {
        return bicycle_nsh_cmd_notify(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "bottom") == 0 || strcmp(argv[0], "botton") == 0) {
        return bicycle_nsh_cmd_bottom(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "nav") == 0) {
        return bicycle_nsh_cmd_nav(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "ride") == 0) {
        return bicycle_nsh_cmd_ride(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "zoom") == 0) {
        return bicycle_nsh_cmd_zoom(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "gnss") == 0) {
        return bicycle_nsh_cmd_gnss(argc - 1, argv + 1);
    }

    if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
        bicycle_nsh_print_usage();
        return 0;
    }

    fprintf(stderr, "bicycle_nsh: unknown command \"%s\"\n", argv[0]);
    bicycle_nsh_print_usage();
    return 1;
}
