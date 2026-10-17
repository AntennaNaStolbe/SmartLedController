#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "input.h"

#define ENC_POLL_MS 5

static const char *TAG = "encoder";

static pcnt_unit_handle_t s_unit;
static int32_t s_raw_last;
static int32_t s_detent_acc;

/* Rotation rate, detents per second, exponentially smoothed so a single
 * burst does not jump the step size. Only updated by actual movement. */
static uint16_t s_rate;
static uint32_t s_last_move_ms;
static bool     s_rate_valid;

static input_handler_t s_handler;

/* button state */
static bool s_btn_level;         /* debounced level, true = pressed */
static bool s_btn_down;          /* press already registered        */
static bool s_mode_fired;        /* hold threshold already hit      */
static bool s_factory_fired;
static uint32_t s_btn_down_ms;
static uint32_t s_btn_stable_ms;

/* single click is held back for BTN_DOUBLE_CLICK_WINDOW_MS so that a second
 * click can turn it into INPUT_EV_DOUBLE_CLICK instead. */
static bool     s_pending_single;
static uint32_t s_pending_single_ms;

static uint8_t accel_from_rate(uint16_t rate)
{
    if (rate < ENC_RATE_T1) {
        return 1;
    }
    if (rate < ENC_RATE_T2) {
        return 2;
    }
    if (rate < ENC_RATE_T3) {
        return 4;
    }
    if (rate < ENC_RATE_T4) {
        return 8;
    }
    return ENC_ACCEL_MAX;
}

static void emit(input_ev_kind_t kind, int8_t detents, uint8_t accel)
{
    if (!s_handler) {
        return;
    }
    input_event_t ev = {
        .kind = kind,
        .detents = detents,
        .accel = accel,
        .held_ms = s_btn_down ? (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_btn_down_ms : 0,
    };
    s_handler(&ev);
}

static uint8_t update_rate(int steps)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t dt = now - s_last_move_ms;
    s_last_move_ms = now;

    uint16_t rate = 0;
    if (s_rate_valid && dt > 0 && dt <= ENC_RATE_RESET_MS) {
        rate = (uint16_t)(((uint32_t)steps * 1000U) / dt);
    }
    /* A pause longer than ENC_RATE_RESET_MS counts as a fresh start, so the
     * first detent after stopping always uses the slow, precise step. */
    s_rate_valid = true;

    s_rate = (uint16_t)(((uint32_t)s_rate * 3U + rate) / 4U);
    return accel_from_rate(s_rate);
}

static void enc_poll(void)
{
    /* The new PCNT API takes a plain int; keep the accumulator wide. */
    int raw = 0;
    if (pcnt_unit_get_count(s_unit, &raw) != ESP_OK) {
        return;
    }

    s_detent_acc += raw - s_raw_last;
    s_raw_last = raw;

    int8_t steps = 0;
    while (s_detent_acc >= ENC_COUNTS_PER_DETENT) {
        steps++;
        s_detent_acc -= ENC_COUNTS_PER_DETENT;
    }
    while (s_detent_acc <= -ENC_COUNTS_PER_DETENT) {
        steps--;
        s_detent_acc += ENC_COUNTS_PER_DETENT;
    }

    if (steps == 0) {
        return;
    }

    if (steps > 127) {
        steps = 127;
    } else if (steps < -127) {
        steps = -127;
    }

    uint8_t accel = update_rate(steps > 0 ? steps : -steps);

    /* Rotation while the button is held is swallowed: the old hold+rotate
     * gesture is gone. A pending single click is dropped too - the user is
     * clearly adjusting, not clicking. */
    if (s_btn_down) {
        s_pending_single = false;
        return;
    }
    s_pending_single = false;
    emit(INPUT_EV_ROTATE, steps, accel);
}

static void btn_poll(void)
{
    bool pressed = gpio_get_level(PIN_ENC_KEY) == 0;   /* active low */
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (pressed != s_btn_level) {
        /* level just flipped: restart the debounce window */
        s_btn_level = pressed;
        s_btn_stable_ms = now_ms;
        return;
    }
    if (now_ms - s_btn_stable_ms < BTN_DEBOUNCE_MS) {
        return;
    }

    if (pressed) {
        if (!s_btn_down) {
            s_btn_down = true;
            s_btn_down_ms = now_ms;
            s_mode_fired = false;
            s_factory_fired = false;
            emit(INPUT_EV_BTN_DOWN, 0, 1);
        }
    } else if (s_btn_down) {
        s_btn_down = false;
        /* The hold gesture already fired while the button was down; the
         * release only has to cancel a click that preceded the hold. */
        if (s_mode_fired || s_factory_fired) {
            s_pending_single = false;
            return;
        }

        /* Short press: candidate for single or double click. The actual
         * BTN_UP is deferred until the double-click window expires. */
        if (s_pending_single && (now_ms - s_pending_single_ms) <= BTN_DOUBLE_CLICK_WINDOW_MS) {
            s_pending_single = false;
            ESP_LOGI(TAG, "double click");
            emit(INPUT_EV_DOUBLE_CLICK, 0, 1);
        } else {
            s_pending_single = true;
            s_pending_single_ms = now_ms;
        }
        return;
    }

    if (s_btn_down) {
        uint32_t held = now_ms - s_btn_down_ms;
        if (!s_mode_fired && held >= BTN_MODE_HOLD_MS) {
            s_mode_fired = true;
            s_pending_single = false;
            ESP_LOGI(TAG, "hold -> mode toggle");
            emit(INPUT_EV_MODE_TOGGLE, 0, 1);
        }
        if (!s_factory_fired && held >= BTN_RESET_HOLD_MS) {
            s_factory_fired = true;
            s_pending_single = false;
            ESP_LOGW(TAG, "very long press -> factory reset");
            emit(INPUT_EV_FACTORY, 0, 1);
        }
    } else if (s_pending_single &&
               (now_ms - s_pending_single_ms) > BTN_DOUBLE_CLICK_WINDOW_MS) {
        /* Double-click window expired with no second click: this was a
         * single click, so let it through now. */
        s_pending_single = false;
        ESP_LOGI(TAG, "single click");
        emit(INPUT_EV_BTN_UP, 0, 1);
    }
}

static void encoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        enc_poll();
        btn_poll();
        vTaskDelay(pdMS_TO_TICKS(ENC_POLL_MS));
    }
}

void input_set_handler(input_handler_t handler)
{
    s_handler = handler;
}

void input_init(void)
{
    /* All three encoder lines need explicit pull-ups: the PCNT driver does not
     * configure the level/edge input GPIOs itself. */
    gpio_config_t enc_cfg = {
        .pin_bit_mask = (1ULL << PIN_ENC_CLK) | (1ULL << PIN_ENC_DT) | (1ULL << PIN_ENC_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&enc_cfg));

    /* Quadrature decoder: A = CLK (edge), B = DT (direction) */
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -32768,
        .high_limit = 32767,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = PIN_ENC_CLK,
        .level_gpio_num = PIN_ENC_DT,
    };
    pcnt_channel_handle_t chan = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_cfg, &chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan,
                                                PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan,
                                                 PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                 PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    pcnt_glitch_filter_config_t glitch_cfg = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &glitch_cfg));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));

    xTaskCreate(encoder_task, "encoder", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "started (CLK=%d DT=%d KEY=%d)", PIN_ENC_CLK, PIN_ENC_DT, PIN_ENC_KEY);
}