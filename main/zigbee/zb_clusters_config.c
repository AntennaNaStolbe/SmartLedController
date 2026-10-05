#include <string.h>

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "config.h"
#include "light_model.h"
#include "zb_clusters_config.h"
#include "zb_config.h"

static void zcl_string(uint8_t *buf, size_t cap, const char *text)
{
    size_t len = strlen(text);
    if (len > cap - 1) {
        len = cap - 1;
    }
    buf[0] = (uint8_t)len;
    memcpy(&buf[1], text, len);
    if (len + 1 < cap) {
        buf[len + 1] = 0;   /* ZCL strings are length-prefixed, keep a terminator for safety */
    }
}

static esp_zb_attribute_list_t *slctr_basic_cluster(void)
{
    esp_zb_basic_cluster_cfg_t cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01,   /* mains (single phase) */
    };

    static uint8_t manufacturer_name[32];
    static uint8_t model_identifier[32];
    static uint8_t date_code[16];
    static uint32_t app_version = 1;    /* FW v0.1.0 */
    static uint32_t stack_version = 2;
    static uint32_t hw_version = 1;

    zcl_string(manufacturer_name, sizeof(manufacturer_name), MANUFACTURER);
    zcl_string(model_identifier, sizeof(model_identifier), MODEL_IDENTIFIER);
    zcl_string(date_code, sizeof(date_code), FW_DATE_CODE);

    esp_zb_attribute_list_t *cl = esp_zb_basic_cluster_create(&cfg);
    esp_zb_basic_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
    esp_zb_basic_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
    esp_zb_basic_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
    esp_zb_basic_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer_name);
    esp_zb_basic_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_identifier);
    esp_zb_basic_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
    return cl;
}

static esp_zb_attribute_list_t *slctr_identify_cluster(void)
{
    esp_zb_identify_cluster_cfg_t cfg = {.identify_time = 0};
    return esp_zb_identify_cluster_create(&cfg);
}

static esp_zb_attribute_list_t *slctr_groups_cluster(void)
{
    return esp_zb_groups_cluster_create(NULL);
}

static esp_zb_attribute_list_t *slctr_on_off_cluster(void)
{
    esp_zb_on_off_cluster_cfg_t cfg = {.on_off = light_model_is_on() ? 1 : 0};
    esp_zb_attribute_list_t *cl = esp_zb_on_off_cluster_create(&cfg);

    static uint16_t startup_on_off = 255;   /* previous state */
    esp_zb_on_off_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &startup_on_off);
    return cl;
}

static esp_zb_attribute_list_t *slctr_level_cluster(void)
{
    esp_zb_attribute_list_t *cl = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL);

    static uint8_t level = 0;
    static uint16_t on_transition_time = 0;
    static uint16_t off_transition_time = 0;

    level = light_model_get_brightness();
    on_transition_time = (uint16_t)(light_model_get_transition_time() / 10);   /* attr unit: 1/10 s */
    off_transition_time = (uint16_t)(light_model_get_transition_time() / 10);

    esp_zb_level_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &level);
    esp_zb_level_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_TRANSITION_TIME_ID, &on_transition_time);
    esp_zb_level_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_OFF_TRANSITION_TIME_ID, &off_transition_time);
    return cl;
}

static esp_zb_attribute_list_t *slctr_color_cluster(void)
{
    esp_zb_color_cluster_cfg_t cfg = {
        .current_x = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_X_DEF_VALUE,
        .current_y = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_Y_DEF_VALUE,
        .color_mode = 0x0002,   /* color temperature */
        .options = ESP_ZB_ZCL_COLOR_CONTROL_OPTIONS_DEFAULT_VALUE,
        .enhanced_color_mode = ESP_ZB_ZCL_COLOR_CONTROL_ENHANCED_COLOR_MODE_DEFAULT_VALUE,
        .color_capabilities = 0x0010,   /* COLOR_TEMPERATURE */
    };
    esp_zb_attribute_list_t *cl = esp_zb_color_control_cluster_create(&cfg);

    static uint16_t color_temperature = 0;
    static uint16_t min_mireds = LM_CT_MIN_MIREDS;
    static uint16_t max_mireds = LM_CT_MAX_MIREDS;

    color_temperature = light_model_get_color_temp();

    esp_zb_color_control_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &color_temperature);
    esp_zb_color_control_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_MIN_MIREDS_ID, &min_mireds);
    esp_zb_color_control_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_MAX_MIREDS_ID, &max_mireds);
    esp_zb_color_control_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &min_mireds);
    esp_zb_color_control_cluster_add_attr(cl, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &max_mireds);
    return cl;
}

esp_zb_cluster_list_t *slctr_clusters_create(void)
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(list, slctr_basic_cluster(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(list, slctr_identify_cluster(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_groups_cluster(list, slctr_groups_cluster(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_on_off_cluster(list, slctr_on_off_cluster(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_level_cluster(list, slctr_level_cluster(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_color_control_cluster(list, slctr_color_cluster(), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    return list;
}

esp_zb_ep_list_t *slctr_ep_list_create(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = HA_ESP_LIGHT_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 4,
    };
    esp_zb_ep_list_add_ep(ep_list, slctr_clusters_create(), ep_cfg);
    return ep_list;
}