#include "esp_log.h"
#include "nvs_flash.h"

#include "config.h"
#include "display/display_init.h"
#include "display/ui.h"
#include "input/encoder.h"
#include "light_model.h"
#include "output/led_controller.h"
#include "storage.h"
#include "zigbee/zb_app.h"

static const char *TAG = "main";

static void log_firmware_info(void)
{
    ESP_LOGI(TAG, "%s v%s (%s)", MANUFACTURER, MODEL_IDENTIFIER, FW_VERSION);
    ESP_LOGI(TAG, "Model: %s", MODEL_IDENTIFIER);
}

void app_main(void)
{
    log_firmware_info();

    ESP_ERROR_CHECK(nvs_flash_init());
    storage_init();

    lc_init();
    light_model_init();

    encoder_init();
    display_init();
    ui_init(light_model_get);

    light_model_set_change_cb(ui_request_refresh);

    appzb_init();

    ESP_LOGI(TAG, "Application started, device is ready for Zigbee pairing");
}