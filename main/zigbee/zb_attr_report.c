#include "esp_log.h"
#include "esp_zigbee_core.h"

#include "light_model.h"
#include "time/clock.h"
#include "zb_app.h"
#include "zb_attr_report.h"
#include "zb_config.h"

static const char *TAG = "zb_attr_report";

void zb_attr_report_attr(uint16_t cluster_id, uint16_t attr_id, void *value)
{
    if (!appzb_is_connected()) {
        return;
    }

    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        HA_ESP_LIGHT_ENDPOINT, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, value, false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Failed to set attr cluster 0x%04x / attr 0x%04x (%d)", cluster_id, attr_id, status);
        return;
    }

    esp_zb_zcl_report_attr_cmd_t report_cmd = {
        .zcl_basic_cmd = {
            .dst_endpoint = 1,   /* endpoint 0x01 (remote) */
            .src_endpoint = HA_ESP_LIGHT_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT,
        .clusterID = cluster_id,
        .manuf_specific = 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code = 0,
        .attributeID = attr_id,
    };
    esp_zb_zcl_report_attr_cmd_req(&report_cmd);
}

void zb_attr_report_current_state(void)
{
    bool on = light_model_is_on();
    uint8_t level = light_model_get_brightness();
    uint16_t mireds = light_model_get_color_temp();

    ESP_LOGI(TAG, "reporting on=%d level=%u mireds=%u", on, (unsigned)level, (unsigned)mireds);

    zb_attr_report_attr(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &on);
    zb_attr_report_attr(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &level);
    zb_attr_report_attr(ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &mireds);
}

void zb_report_local_state(void)
{
    /* Invoked from the light model worker, debounced. Only fires when the
     * coordinator is reachable; the shadow attributes are kept in sync by the
     * next join-time report otherwise. */
    zb_attr_report_current_state();
}

void zb_request_time_sync(void)
{
    if (!appzb_is_connected()) {
        return;
    }

    /* The coordinator runs the Time server on endpoint 1; we are the client. */
    static uint16_t time_attr = ESP_ZB_ZCL_ATTR_TIME_TIME_ID;
    esp_zb_zcl_read_attr_cmd_t read_cmd = {
        .zcl_basic_cmd = {
            .dst_endpoint = HA_ESP_COORDINATOR_ENDPOINT,
            .src_endpoint = HA_ESP_LIGHT_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_TIME,
        .manuf_specific = 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .dis_default_resp = 0,
        .manuf_code = 0,
        .attr_number = 1,
        .attr_field = &time_attr,
    };
    esp_zb_zcl_read_attr_cmd_req(&read_cmd);

    ESP_LOGD(TAG, "requested the Time attribute");
}