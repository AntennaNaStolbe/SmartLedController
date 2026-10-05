#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"

#include "config.h"
#include "light_model.h"
#include "time/clock.h"
#include "zb_attr_handlers.h"
#include "zb_config.h"

static const char *TAG = "zb_attr_handlers";

static void handle_on_off_attribute(const esp_zb_zcl_set_attr_value_message_t *message)
{
    switch (message->attribute.id) {
    case ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID:
        if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL && message->attribute.data.value) {
            bool on = *(bool *)message->attribute.data.value;
            ESP_LOGI(TAG, "OnOff -> %s", on ? "On" : "Off");
            light_model_set_power(on);
        } else {
            ESP_LOGW(TAG, "Invalid type for OnOff attr: 0x%x", message->attribute.data.type);
        }
        break;

    case ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF:
        /* StartUpOnOff is read at boot from NVS; the value below only affects the
         * exposed attribute, not the stored startup behaviour. */
        ESP_LOGI(TAG, "StartUpOnOff attr written");
        break;

    default:
        ESP_LOGW(TAG, "Unhandled On/Off attribute: 0x%04x", message->attribute.id);
        break;
    }
}

static void handle_level_attribute(const esp_zb_zcl_set_attr_value_message_t *message)
{
    switch (message->attribute.id) {
    case ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID:
        if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && message->attribute.data.value) {
            uint8_t level = *(uint8_t *)message->attribute.data.value;
            ESP_LOGI(TAG, "CurrentLevel -> %u", level);
            if (level == 0) {
                light_model_set_brightness(0);      /* turns the light off */
            } else {
                if (!light_model_is_on()) {
                    light_model_set_power(true);    /* MoveToLevelWithOnOff semantics */
                }
                light_model_set_brightness(level);
            }
        } else {
            ESP_LOGW(TAG, "Invalid type for CurrentLevel attr: 0x%x", message->attribute.data.type);
        }
        break;

    case ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_TRANSITION_TIME_ID:
    case ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_OFF_TRANSITION_TIME_ID:
        if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && message->attribute.data.value) {
            uint16_t deciseconds = *(uint16_t *)message->attribute.data.value;
            light_model_set_transition_time((uint16_t)deciseconds * 100);
            ESP_LOGI(TAG, "Transition time -> %u ms", (unsigned)deciseconds * 100);
        }
        break;

    default:
        ESP_LOGW(TAG, "Unhandled Level attribute: 0x%04x", message->attribute.id);
        break;
    }
}

static void handle_color_attribute(const esp_zb_zcl_set_attr_value_message_t *message)
{
    switch (message->attribute.id) {
    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID:
        if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && message->attribute.data.value) {
            uint16_t mireds = *(uint16_t *)message->attribute.data.value;
            ESP_LOGI(TAG, "ColorTemperature -> %u mireds (%lu K)", mireds, (unsigned long)(1000000ul / (mireds ? mireds : 1)));
            light_model_set_color_temp(mireds);
        } else {
            ESP_LOGW(TAG, "Invalid type for ColorTemperature attr: 0x%x", message->attribute.data.type);
        }
        break;

    default:
        ESP_LOGW(TAG, "Unhandled Color Control attribute: 0x%04x", message->attribute.id);
        break;
    }
}

static void handle_identify_attribute(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message->attribute.data.value) {
        uint16_t seconds = *(uint16_t *)message->attribute.data.value;
        ESP_LOGI(TAG, "Identify -> %u s", seconds);
        light_model_identify(seconds);
    }
}

esp_err_t zb_attr_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG,
                        "Received message with error status (%d)", message->info.status);

    /* Anything addressed to us is an external actor, so tag it REMOTE and the
     * model will not echo it back onto the mesh. */
    light_model_set_origin(LIGHT_ORIGIN_REMOTE);

    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        switch (message->info.cluster) {
        case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
            handle_on_off_attribute(message);
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
            handle_level_attribute(message);
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL:
            handle_color_attribute(message);
            break;
        case ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY:
            handle_identify_attribute(message);
            break;
        default:
            ESP_LOGW(TAG, "Unhandled cluster 0x%04x on endpoint %d", message->info.cluster, message->info.dst_endpoint);
            break;
        }
    }
    return ESP_OK;
}