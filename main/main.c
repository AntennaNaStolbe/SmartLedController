#include "esp_log.h"
#include "esp_zigbee_trace.h"
#include "nvs_flash.h"

#include "config.h"
#include "display/display_init.h"
#include "display/ui.h"
#include "input/input.h"
#include "light_model.h"
#include "output/led_controller.h"
#include "storage.h"
#include "time/clock.h"
#include "zigbee/zb_app.h"

static const char *TAG = "main";

static void log_firmware_info(void)
{
    ESP_LOGI(TAG, "%s %s v%s (%s)", PRODUCT_NAME, MANUFACTURER, MODEL_IDENTIFIER, FW_VERSION);
}

/* NVS may survive a partition resize or a downgrade; recover instead of
 * aborting on boot. */
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    log_firmware_info();

    init_nvs();
    storage_init();

    lc_init();
    light_model_init();

    clock_init();

    display_init();
    ui_init();

    /* The UI owns input routing: the encoder only produces gestures. */
    input_set_handler(ui_handle_input);
    light_model_set_change_cb(ui_request_refresh);
    light_model_set_report_cb(zb_report_local_state);

    input_init();

    esp_zb_set_trace_level_mask(ESP_ZB_TRACE_LEVEL_DEBUG,
                                ESP_ZB_TRACE_SUBSYSTEM_OTA | ESP_ZB_TRACE_SUBSYSTEM_ZCL);
    appzb_init();

    ESP_LOGI(TAG, "Application started, device is ready for Zigbee pairing");
}