/* Status LED: a WS2812 on IO48, kept dim.
 *
 * It answers one question from across the bench - is anything connected - which
 * otherwise only the serial console can tell you, and that needs a cable.
 */
#ifndef LED_H
#define LED_H

typedef enum {
    LED_OFF = 0,   /* not advertising */
    LED_IDLE,      /* connectable, waiting for a client */
    LED_LINKED,    /* a client is connected */
} led_state_t;

/* Brings up the RMT channel driving the strip. */
void led_init(void);

/* Sets the colour for a state.  Cheap to call repeatedly; a repeated state is
 * ignored. */
void led_set(led_state_t state);

#endif /* LED_H */
