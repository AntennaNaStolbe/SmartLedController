#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "clock.h"
#include "config.h"

static const char *TAG = "clock";

/* ZCL Time counts seconds since 2000-01-01 00:00:00 UTC. */
#define ZCL_EPOCH_OFFSET_S 946684800L

static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_resync_timer;
static clock_resync_cb_t s_resync_cb;

static int64_t s_local_base_s;   /* unix seconds at the moment of the last sync */
static int64_t s_sync_mono_us;   /* esp_timer reading taken at the same moment  */
static int32_t s_tz_offset_s = CLOCK_DEFAULT_TZ_OFFSET_S;
static bool s_tz_from_coordinator;
static bool s_synced;

static void on_resync_timer(void *arg)
{
    (void)arg;
    if (s_resync_cb) {
        s_resync_cb();
    }
}

static int64_t now_local_s(void)
{
    if (!s_synced) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_sync_mono_us;
    if (elapsed_us < 0) {
        elapsed_us = 0;
    }
    return s_local_base_s + elapsed_us / 1000000;
}

void clock_set_zcl_time(uint32_t zcl_time_s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* A timestamp of 0 means "not synchronised yet" in ZCL. */
    if (zcl_time_s == 0) {
        s_synced = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "received a zero Time attribute, staying unsynced");
        return;
    }

    s_local_base_s = ZCL_EPOCH_OFFSET_S + (int64_t)zcl_time_s + CLOCK_LEAP_SECONDS + s_tz_offset_s;
    s_sync_mono_us = esp_timer_get_time();
    s_synced = true;

    char buf[16];
    struct tm tmv;
    time_t t = (time_t)s_local_base_s;
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%H:%M", &tmv);
    ESP_LOGI(TAG, "time synced: UTC=%lus tz=%+ds, local %s",
             (unsigned long)zcl_time_s, s_tz_offset_s, buf);
    xSemaphoreGive(s_lock);
}

void clock_set_timezone(int32_t offset_s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tz_offset_s = offset_s;
    s_tz_from_coordinator = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "timezone from coordinator: %+d s", (int)offset_s);
}

void clock_clear_timezone(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tz_offset_s = CLOCK_DEFAULT_TZ_OFFSET_S;
    s_tz_from_coordinator = false;
    xSemaphoreGive(s_lock);
}

bool clock_is_synced(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool synced = s_synced;
    xSemaphoreGive(s_lock);
    return synced;
}

static void clock_format(char *buf, unsigned len, const char *fmt)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t t = now_local_s();
    bool synced = s_synced;
    xSemaphoreGive(s_lock);

    if (!synced) {
        snprintf(buf, len, "--:--");
        return;
    }
    struct tm tmv;
    time_t tt = (time_t)t;
    gmtime_r(&tt, &tmv);
    strftime(buf, len, fmt, &tmv);
}

void clock_get_time_str(char *buf, unsigned len)
{
    clock_format(buf, len, "%H:%M");
}

void clock_get_date_str(char *buf, unsigned len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool synced = s_synced;
    bool tz_valid = s_tz_from_coordinator;
    xSemaphoreGive(s_lock);

    if (!synced) {
        snprintf(buf, len, "no time sync");
        return;
    }
    if (!tz_valid) {
        ESP_LOGD(TAG, "timezone is the built-in fallback");
    }
    clock_format(buf, len, "%d.%m.%Y");
}

void clock_set_resync_cb(clock_resync_cb_t cb)
{
    s_resync_cb = cb;
}

void clock_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "failed to create clock lock");
        abort();
    }

    esp_timer_create_args_t ta = {
        .callback = on_resync_timer,
        .name = "clk_resync",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_resync_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_resync_timer, CLOCK_RESYNC_PERIOD_MS * 1000ULL));

    ESP_LOGI(TAG, "init (fallback tz offset %+d s)", (int)s_tz_offset_s);
}