#include <math.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "config.h"
#include "led_controller.h"

#define LC_LS_TIMER        LEDC_TIMER_1
#define LC_LS_MODE         LEDC_LOW_SPEED_MODE
#define LC_DUTY_RES        LEDC_TIMER_11_BIT
#define LC_MAX_DUTY        ((1U << 11) - 1)   /* 2047 */
#define LC_FREQ_HZ         16000
#define LC_GAMMA           1.5f
#define LC_FADE_STEP_MS    10
#define LC_FADE_MAX_MS     5000
#define LC_QUEUE_SIZE      16

#define LC_CH_WARM         LEDC_CHANNEL_0
#define LC_CH_COOL         LEDC_CHANNEL_1

static const char *TAG = "led_controller";

typedef struct {
    uint8_t  on_off;
    uint8_t  brightness;
    uint16_t mireds;
    uint32_t fade_ms;
} lc_req_t;

static QueueHandle_t s_queue;
static volatile int32_t s_duty_warm;
static volatile int32_t s_duty_cool;

static void lc_apply_duty(int32_t warm, int32_t cool)
{
    s_duty_warm = warm;
    s_duty_cool = cool;
    ledc_set_duty(LC_LS_MODE, LC_CH_WARM, (uint32_t)warm);
    ledc_update_duty(LC_LS_MODE, LC_CH_WARM);
    ledc_set_duty(LC_LS_MODE, LC_CH_COOL, (uint32_t)cool);
    ledc_update_duty(LC_LS_MODE, LC_CH_COOL);
}

static void lc_compute_target(const lc_req_t *req, int32_t *warm, int32_t *cool)
{
    *warm = 0;
    *cool = 0;
    if (!req->on_off || req->brightness == 0) {
        return;
    }

    float b = (float)req->brightness / (float)LM_LEVEL_MAX;
    float total = (float)LC_MAX_DUTY * powf(b, 1.0f / LC_GAMMA);
    if (total > LC_MAX_DUTY) {
        total = LC_MAX_DUTY;
    }

    float t = (float)(req->mireds - LM_CT_MIN_MIREDS) / (float)(LM_CT_MAX_MIREDS - LM_CT_MIN_MIREDS);
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }

    *warm = (int32_t)(total * t);
    *cool = (int32_t)(total * (1.0f - t));
}

static void lc_fade_task(void *arg)
{
    (void)arg;
    lc_req_t req;
    int32_t target_warm = 0, target_cool = 0;
    uint32_t fade_ms = 0;
    bool have_target = false;

    for (;;) {
        BaseType_t got = xQueueReceive(s_queue, &req, pdMS_TO_TICKS(LC_FADE_STEP_MS));
        if (got == pdTRUE) {
            /* New request: re-animate from the *actual* current duty. */
            lc_compute_target(&req, &target_warm, &target_cool);
            fade_ms = req.fade_ms;
            if (fade_ms > LC_FADE_MAX_MS) {
                fade_ms = LC_FADE_MAX_MS;
            }
            if (fade_ms <= LC_FADE_STEP_MS) {
                lc_apply_duty(target_warm, target_cool);
                have_target = false;
                continue;
            }
            have_target = true;
        }

        if (!have_target) {
            continue;
        }

        int32_t dw = target_warm - s_duty_warm;
        int32_t dc = target_cool - s_duty_cool;
        if (dw == 0 && dc == 0) {
            have_target = false;
            continue;
        }

        int32_t steps_total = (int32_t)(fade_ms + LC_FADE_STEP_MS - 1) / LC_FADE_STEP_MS;
        int32_t step_w = dw / steps_total;
        int32_t step_c = dc / steps_total;
        if (step_w == 0) {
            step_w = (dw > 0) ? 1 : -1;
        }
        if (step_c == 0) {
            step_c = (dc > 0) ? 1 : -1;
        }

        int32_t nw = s_duty_warm + step_w;
        int32_t nc = s_duty_cool + step_c;
        if ((step_w > 0 && nw > target_warm) || (step_w < 0 && nw < target_warm)) {
            nw = target_warm;
        }
        if ((step_c > 0 && nc > target_cool) || (step_c < 0 && nc < target_cool)) {
            nc = target_cool;
        }

        lc_apply_duty(nw, nc);
        if (nw == target_warm && nc == target_cool) {
            have_target = false;
        }
    }
}

void lc_set_fade(bool on_off, uint8_t brightness, uint16_t mireds, uint32_t fade_ms)
{
    lc_req_t req = {
        .on_off = on_off ? 1 : 0,
        .brightness = brightness,
        .mireds = mireds,
        .fade_ms = fade_ms,
    };
    if (s_queue == NULL) {
        return;
    }
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "fade queue full, applying instantly");
        int32_t w, c;
        lc_compute_target(&req, &w, &c);
        lc_apply_duty(w, c);
    }
}

void lc_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LC_LS_MODE,
        .timer_num = LC_LS_TIMER,
        .duty_resolution = LC_DUTY_RES,
        .freq_hz = LC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_warm = {
        .gpio_num = PIN_LED_WARM,
        .speed_mode = LC_LS_MODE,
        .channel = LC_CH_WARM,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LC_LS_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_warm);

    ledc_channel_config_t ch_cool = {
        .gpio_num = PIN_LED_COOL,
        .speed_mode = LC_LS_MODE,
        .channel = LC_CH_COOL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LC_LS_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_cool);

    s_queue = xQueueCreate(LC_QUEUE_SIZE, sizeof(lc_req_t));
    xTaskCreate(lc_fade_task, "lc_fade", 3072, NULL, 8, NULL);
}