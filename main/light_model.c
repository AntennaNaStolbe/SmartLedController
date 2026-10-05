#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "light_model.h"
#include "output/led_controller.h"
#include "storage.h"

static const char *TAG = "light_model";

typedef struct {
    bool     on_off;
    uint8_t  brightness;
    uint16_t mireds;
    uint16_t transition_ms;
    uint8_t  startup_behavior;
} model_t;

typedef enum {
    LM_MSG_PERSIST,
    LM_MSG_REPORT,
} lm_msg_t;

static model_t s_model;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_io_q;
static esp_timer_handle_t s_save_timer;
static esp_timer_handle_t s_identify_timer;

static light_change_cb_t s_change_cb;
static light_change_cb_t s_report_cb;
static light_origin_t s_origin = LIGHT_ORIGIN_LOCAL;

static bool s_identify_active;
static bool s_identify_on;
static uint32_t s_identify_remaining_ms;

static void on_save_timer(void *arg);
static void on_identify_timer(void *arg);
static void lm_io_task(void *arg);

/* ------------------------------------------------------------------ */
/* state helpers - must be called with the lock held                    */
/* ------------------------------------------------------------------ */

static void model_clamp(void)
{
    if (s_model.brightness > LM_LEVEL_MAX) {
        s_model.brightness = LM_LEVEL_MAX;
    }
    if (s_model.mireds < LM_CT_MIN_MIREDS) {
        s_model.mireds = LM_CT_MIN_MIREDS;
    } else if (s_model.mireds > LM_CT_MAX_MIREDS) {
        s_model.mireds = LM_CT_MAX_MIREDS;
    }
    if (s_model.transition_ms > 5000) {
        s_model.transition_ms = 5000;
    }
}

/* Effective brightness: a lit light with level 0 is treated as the minimum
 * so that "on" and "level 0" from Zigbee never fight each other. */
static uint8_t model_effective_level(void)
{
    if (!s_model.on_off) {
        return 0;
    }
    return s_model.brightness ? s_model.brightness : LM_LEVEL_MIN;
}

/* Push the model to the PWM driver. Called with the lock held, but it only
 * enqueues a fade request, so it is safe to call from a timer context. */
static void model_apply_locked(void)
{
    uint8_t level = model_effective_level();
    lc_set_fade(s_model.on_off, level, s_model.mireds, s_model.transition_ms);
}

/* ------------------------------------------------------------------ */
/* persistence and reporting (worker task context)                      */
/* ------------------------------------------------------------------ */

static void model_persist(void)
{
    app_settings_t st;
    memset(&st, 0, sizeof(st));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    st.on_off = s_model.on_off;
    st.brightness = s_model.brightness;
    st.color_temp_mireds = s_model.mireds;
    st.on_transition_time = s_model.transition_ms;
    st.off_transition_time = s_model.transition_ms;
    st.startup_behavior = s_model.startup_behavior;
    xSemaphoreGive(s_lock);

    storage_save(&st);
}

static void lm_io_task(void *arg)
{
    (void)arg;

    for (;;) {
        lm_msg_t msg;
        if (xQueueReceive(s_io_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg == LM_MSG_PERSIST) {
            model_persist();
            continue;
        }

        /* Report: coalesce bursts so that rotating the encoder does not flood
         * the mesh, then hand the final state to the coordinator. */
        TickType_t last = xTaskGetTickCount();
        for (;;) {
            TickType_t window = pdMS_TO_TICKS(LM_REPORT_DELAY_MS);
            TickType_t waited = xTaskGetTickCount() - last;
            TickType_t rest = (waited < window) ? (window - waited) : 0;

            lm_msg_t burst;
            if (xQueueReceive(s_io_q, &burst, rest) == pdTRUE) {
                if (burst == LM_MSG_PERSIST) {
                    model_persist();
                }
                last = xTaskGetTickCount();
                continue;
            }
            if (xTaskGetTickCount() - last >= window) {
                break;
            }
        }

        if (s_report_cb) {
            s_report_cb();
        }
    }
}

static void on_save_timer(void *arg)
{
    (void)arg;
    /* Keep the esp_timer task free of flash writes: only hand the request
     * over to the worker. */
    xQueueSend(s_io_q, &(lm_msg_t){LM_MSG_PERSIST}, 0);
}

/* ------------------------------------------------------------------ */
/* change plumbing                                                      */
/* ------------------------------------------------------------------ */

static void model_changed(bool local)
{
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, LM_SAVE_DELAY_MS * 1000);

    if (local && s_report_cb) {
        xQueueSend(s_io_q, &(lm_msg_t){LM_MSG_REPORT}, 0);
    }

    if (s_change_cb) {
        s_change_cb();
    }
}

/* Apply + notify for a mutated model. Called without the lock held. */
static void model_commit(light_origin_t origin)
{
    model_apply_locked();
    model_changed(origin == LIGHT_ORIGIN_LOCAL);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void light_model_init(void)
{
    app_settings_t st;
    (void)storage_load(&st);

    s_lock = xSemaphoreCreateMutex();
    s_io_q = xQueueCreate(8, sizeof(lm_msg_t));
    if (s_lock == NULL || s_io_q == NULL) {
        ESP_LOGE(TAG, "failed to create model resources");
        abort();
    }

    xTaskCreate(lm_io_task, "lm_io", 3072, NULL, 3, NULL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.on_off = st.on_off;
    s_model.brightness = st.brightness;
    s_model.mireds = st.color_temp_mireds;
    s_model.transition_ms = st.on_transition_time ? st.on_transition_time : LM_DEFAULT_TRANSITION_MS;
    s_model.startup_behavior = st.startup_behavior;
    model_clamp();

    esp_timer_create_args_t ta = {
        .callback = on_save_timer,
        .name = "lm_save",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_save_timer));

    esp_timer_create_args_t ti = {
        .callback = on_identify_timer,
        .name = "lm_identify",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ti, &s_identify_timer));

    /* Apply the StartUpOnOff behaviour to the restored state. */
    switch (s_model.startup_behavior) {
    case 0:                                 /* StartUpOnOff: off */
        s_model.on_off = false;
        break;
    case 1:                                 /* StartUpOnOff: on  */
        s_model.on_off = true;
        break;
    default:                                /* previous / toggle: keep stored */
        break;
    }
    model_apply_locked();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Restored: on=%d brightness=%u mireds=%u transition=%ums",
             s_model.on_off, s_model.brightness, s_model.mireds, s_model.transition_ms);
}

void light_model_set_change_cb(light_change_cb_t cb)
{
    /* called once from app_main before the tasks start driving the model */
    s_change_cb = cb;
}

void light_model_set_report_cb(light_change_cb_t cb)
{
    s_report_cb = cb;
}

void light_model_set_origin(light_origin_t origin)
{
    s_origin = origin;
}

void light_model_set_power(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_model.on_off == on) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_model.on_off = on;
    model_commit(s_origin);
    xSemaphoreGive(s_lock);
}

void light_model_toggle_power(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = !s_model.on_off;
    xSemaphoreGive(s_lock);

    light_model_set_power(on);
}

void light_model_set_brightness(uint8_t level)
{
    if (level > LM_LEVEL_MAX) {
        level = LM_LEVEL_MAX;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (level == s_model.brightness && (level != 0 || !s_model.on_off)) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_model.brightness = level;
    /* ZCL semantics: level 0 means off. */
    if (level == 0) {
        s_model.on_off = false;
    }
    model_commit(s_origin);
    xSemaphoreGive(s_lock);
}

void light_model_adjust_brightness(int8_t detents)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int level = (int)s_model.brightness + ((int)detents * LM_BRIGHTNESS_STEP);
    if (level < LM_LEVEL_MIN) {
        level = LM_LEVEL_MIN;
    } else if (level > LM_LEVEL_MAX) {
        level = LM_LEVEL_MAX;
    }
    if (level == s_model.brightness) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_model.brightness = (uint8_t)level;
    model_commit(s_origin);
    xSemaphoreGive(s_lock);
}

void light_model_set_color_temp(uint16_t mireds)
{
    if (mireds < LM_CT_MIN_MIREDS) {
        mireds = LM_CT_MIN_MIREDS;
    } else if (mireds > LM_CT_MAX_MIREDS) {
        mireds = LM_CT_MAX_MIREDS;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (mireds == s_model.mireds) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_model.mireds = mireds;
    model_commit(s_origin);
    xSemaphoreGive(s_lock);
}

void light_model_adjust_color_temp(int8_t detents)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int mireds = (int)s_model.mireds + ((int)detents * LM_CT_STEP_MIREDS);
    if (mireds < LM_CT_MIN_MIREDS) {
        mireds = LM_CT_MIN_MIREDS;
    } else if (mireds > LM_CT_MAX_MIREDS) {
        mireds = LM_CT_MAX_MIREDS;
    }
    if (mireds == s_model.mireds) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_model.mireds = (uint16_t)mireds;
    model_commit(s_origin);
    xSemaphoreGive(s_lock);
}

void light_model_set_transition_time(uint16_t ms)
{
    if (ms > 5000) {
        ms = 5000;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_model.transition_ms = ms;
    xSemaphoreGive(s_lock);
}

static void identify_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_identify_active = false;
    model_apply_locked();
    bool local = s_origin == LIGHT_ORIGIN_LOCAL;
    xSemaphoreGive(s_lock);

    model_changed(local);
}

static void on_identify_timer(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_identify_active) {
        xSemaphoreGive(s_lock);
        return;
    }

    s_identify_on = !s_identify_on;
    /* Blink the warm channel at full brightness with a short fade. */
    lc_set_fade(s_identify_on, LM_LEVEL_MAX, LM_CT_MAX_MIREDS, 100);

    bool finished = false;
    if (s_identify_remaining_ms > LM_IDENTIFY_BLINK_MS) {
        s_identify_remaining_ms -= LM_IDENTIFY_BLINK_MS;
    } else {
        finished = true;
    }
    xSemaphoreGive(s_lock);

    if (finished) {
        esp_timer_stop(s_identify_timer);
        identify_stop();
    }
}

void light_model_identify(uint16_t seconds)
{
    if (seconds == 0) {
        if (s_identify_active) {
            esp_timer_stop(s_identify_timer);
            identify_stop();
        }
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_identify_remaining_ms = (uint32_t)seconds * 1000;
    if (!s_identify_active) {
        s_identify_active = true;
        s_identify_on = false;
        esp_timer_start_periodic(s_identify_timer, LM_IDENTIFY_BLINK_MS * 1000);
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Identify for %u s", (unsigned)seconds);
}

void light_model_get(light_state_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->on_off = s_model.on_off;
    out->brightness = model_effective_level();
    out->color_temp_mireds = s_model.mireds;
    xSemaphoreGive(s_lock);
}

bool light_model_is_on(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_model.on_off;
    xSemaphoreGive(s_lock);
    return on;
}

uint8_t light_model_get_brightness(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t level = model_effective_level();
    xSemaphoreGive(s_lock);
    return level;
}

uint16_t light_model_get_color_temp(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t mireds = s_model.mireds;
    xSemaphoreGive(s_lock);
    return mireds;
}

uint16_t light_model_get_transition_time(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t ms = s_model.transition_ms;
    xSemaphoreGive(s_lock);
    return ms;
}