/* Glue between main.c (radio/identity) and cli.c (console). */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

/* Advertising control; implemented in main.c. */
void app_adv_set(bool on);
bool app_adv_on(void);

/* Scheduled board reset; implemented in main.c.  A range picks a fresh random
 * delay inside it for every reset; min_s == 0 turns it off. */
#define APP_RESET_MIN_S     600     /* 10 minutes */
#define APP_RESET_MAX_S     1200    /* 20 minutes */

void app_reset_schedule(uint32_t min_s, uint32_t max_s);
void app_reset_range(uint32_t *min_s, uint32_t *max_s);
int app_reset_remaining_s(void);

/* Starts the serial console REPL; implemented in cli.c. */
void cli_start(void);

#endif /* APP_H */
