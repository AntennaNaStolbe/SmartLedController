#include <string.h>

#include "driver/gpio.h"
#include "driver/pcnt.h"
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

static input_handler_t s_handler;

/* button state */
static bool s_btn_level;         /* debounced level, true = pressed */
static bool s_btn_down;          /* press already registered        */
static bool s_hold_detected;    /* rotate-while-held is active     */
static bool s_menu_fired;
static bool s_factory_fired;
static uint32_t s_btn_down_ms;
static uint32_t s_btn_stable_ms;

static void emit(input_ev_kind_t kind, int8_t detents)
{
    if (!s_handler) {
        return;
    }
    input_event_t ev = {
        .kind = kind,
        .detents = detents,
        .held_ms = s_btn_down ? (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_btn_down_ms : 0,
    };
    s_handler(&ev);
}

static void enc_poll(void)
{
    int32_t raw = 0;
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

    /* The first detent after the hold threshold switches to colour
     * temperature and cancels the pending menu gesture. */
    if (s_btn_down && !s_hold_detected) {
        s_hold_detected = true;
        s_menu_fired = true;   /* consumed by this rotation */
    }
    emit(s_hold_detected ? INPUT_EV_ROTATE_HELD : INPUT_EV_ROTATE, steps);
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
            s_hold_detected = false;
            s_menu_fired = false;
            s_factory_fired = false;
            emit(INPUT_EV_BTN_DOWN, 0);
        }
    } else if (s_btn_down) {
        uint32_t held = now_ms - s_btn_down_ms;
        /* A click is a release that produced no long press. */
        if (!s_menu_fired && !s_factory_fired && held < BTN_CLICK_MAX_MS) {
            emit(INPUT_EV_BTN_UP, 0);
        }
        s_btn_down = false;
        s_hold_detected = false;
        return;
    }

    if (!s_btn_down) {
        return;
    }

    uint32_t held = now_ms - s_btn_down_ms;
    if (!s_menu_fired && !s_hold_detected && held >= BTN_MENU_HOLD_MS) {
        s_menu_fired = true;
        ESP_LOGI(TAG, "long press -> menu");
        emit(INPUT_EV_MENU, 0);
    }
    if (!s_factory_fired && held >= BTN_RESET_HOLD_MS) {
        s_factory_fired = true;
        ESP_LOGW(TAG, "very long press -> factory reset");
        emit(INPUT_EV_FACTORY, 0);
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