/* WS2812 status LED on IO48, driven straight from an RMT TX channel.
 *
 * One frame is 24 bits (the strip takes GRB order) plus a low latch period, and
 * the whole frame is pre-encoded into RMT symbols and handed to the copy encoder
 * - no custom encoder vtable needed for a 25-symbol frame.
 *
 * The colour says which sensor this board is; the animation says what the link
 * is doing: nothing connected blinks, a live link breathes.  Both are done by
 * scaling the colour per frame, an addressable strip having no PWM of its own.
 */
#include "led.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#define WS2812_GPIO         48
#define RMT_RES_HZ          10000000    /* 0.1 us per tick */
#define WS2812_BITS         24
#define WS2812_FRAME_SYMS   (WS2812_BITS + 1)

/* WS2812 bit timing at 0.1 us resolution: 0 is a short high, 1 a long one. */
#define WS2812_T0H          3           /* 0.30 us */
#define WS2812_T0L          9           /* 0.90 us */
#define WS2812_T1H          9           /* 0.90 us */
#define WS2812_T1L          3           /* 0.30 us */
#define WS2812_LATCH_TICKS  500         /* 50 us low to latch the frame */

/* This board's colour: red = heart rate (cadence is green, power amber/yellow).
 * Peak brightness is kept low on purpose - this sits next to the bike
 * computer's own screen. */
#define LED_RGB             28, 0, 0
#define LED_COLOR_NAME      "red"

/* Animation: 40 fps is plenty for a fade that lasts seconds. */
#define LED_TICK_MS         25
#define LED_BLINK_MS        1000        /* idle: half a second on, half off */
#define LED_IDLE_LEVEL      35          /* % of the colour while the blink is on */
#define LED_BREATH_MS       3200        /* linked: one full fade in and out */
#define LED_BREATH_MIN      15          /* % of the colour at the bottom of it */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static rmt_symbol_word_t s_frame[WS2812_FRAME_SYMS];

/* Written by whichever task changes the link state, read by the LED task. */
static volatile led_state_t s_state = LED_OFF;

static void build_frame(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t grb[3] = { green, red, blue };
    int i = 0;
    int byte;
    int bit;

    for (byte = 0; byte < 3; byte++) {
        for (bit = 7; bit >= 0; bit--) {
            const int one = (grb[byte] >> bit) & 1;

            s_frame[i].level0 = 1;
            s_frame[i].duration0 = one ? WS2812_T1H : WS2812_T0H;
            s_frame[i].level1 = 0;
            s_frame[i].duration1 = one ? WS2812_T1L : WS2812_T0L;
            i++;
        }
    }

    s_frame[i].level0 = 0;
    s_frame[i].duration0 = WS2812_LATCH_TICKS;
    s_frame[i].level1 = 0;
    s_frame[i].duration1 = 0;
}

static void led_write(uint8_t red, uint8_t green, uint8_t blue)
{
    const rmt_transmit_config_t cfg = { .loop_count = 0 };

    if (s_chan == NULL) {
        return;
    }

    build_frame(red, green, blue);
    if (rmt_transmit(s_chan, s_encoder, s_frame, sizeof(s_frame), &cfg) != ESP_OK) {
        return;
    }
    (void)rmt_tx_wait_all_done(s_chan, 100);
}

/* Writes the board colour at `percent` of its full value. */
static void led_write_scaled(unsigned percent)
{
    static const uint8_t full[3] = { LED_RGB };

    if (percent > 100u) {
        percent = 100u;
    }

    led_write((uint8_t)(full[0] * percent / 100u),
              (uint8_t)(full[1] * percent / 100u),
              (uint8_t)(full[2] * percent / 100u));
}

/* How bright the colour should be right now, in percent. */
static unsigned led_level(led_state_t state, uint32_t now_ms)
{
    if (state == LED_LINKED) {
        /* Breathing: a slow triangle between nothing much and full. */
        const uint32_t half = LED_BREATH_MS / 2u;
        const uint32_t phase = now_ms % LED_BREATH_MS;
        const uint32_t rise = (phase < half) ? phase : (LED_BREATH_MS - phase);

        return LED_BREATH_MIN + (100u - LED_BREATH_MIN) * rise / half;
    }

    if (state == LED_IDLE) {
        /* Blinking: hard on/off, which cannot be mistaken for a fade. */
        return ((now_ms % LED_BLINK_MS) < (LED_BLINK_MS / 2u)) ? LED_IDLE_LEVEL : 0u;
    }

    return 0u;
}

static void led_task(void *arg)
{
    for (;;) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        led_write_scaled(led_level(s_state, now_ms));
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

static const char *led_state_name(led_state_t state)
{
    switch (state) {
    case LED_IDLE:   return "blinking (advertising)";
    case LED_LINKED: return "breathing (linked)";
    default:         return "off";
    }
}

void led_set(led_state_t state)
{
    if (state == s_state) {
        return;
    }

    s_state = state;    /* the LED task picks it up within one frame */
    ESP_LOGI(TAG, "%s, %s", LED_COLOR_NAME, led_state_name(state));
}

void led_init(void)
{
    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = (gpio_num_t)WS2812_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    const rmt_copy_encoder_config_t enc_cfg = { };

    if (rmt_new_tx_channel(&tx_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "no RMT channel for the status LED");
        s_chan = NULL;
        return;
    }
    if (rmt_new_copy_encoder(&enc_cfg, &s_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "no copy encoder for the status LED");
        s_chan = NULL;
        return;
    }
    if (rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "RMT channel would not start");
        s_chan = NULL;
        return;
    }

    s_state = LED_IDLE;
    if (xTaskCreate(led_task, "led", 2560, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task for the status LED animation");
        return;
    }

    ESP_LOGI(TAG, "status LED on IO%d: %s, blinks while advertising, breathes when linked",
             WS2812_GPIO, LED_COLOR_NAME);
}
