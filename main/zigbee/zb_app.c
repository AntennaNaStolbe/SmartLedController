#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_zigbee_core.h"
#include "freertos/event_groups.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "config.h"
#include "storage.h"
#include "time/clock.h"
#include "zb_app.h"
#include "zb_attr_handlers.h"
#include "zb_attr_report.h"
#include "zb_clusters_config.h"
#include "zb_config.h"
#include "zb_ota.h"

static const char *TAG = "zb_app";

#define CONNECTED_BIT (1 << 0)

static EventGroupHandle_t s_connected_group;

static void report_state_alarm_cb(uint8_t arg)
{
    (void)arg;
    zb_attr_report_current_state();
}

static void request_time_alarm_cb(uint8_t arg)
{
    (void)arg;
    /* Runs in the Zigbee task, so the stack lock is already held. */
    zb_request_time_sync_now();
}

static void ota_query_alarm_cb(uint8_t arg)
{
    (void)arg;
    zb_ota_query_now(false);
    esp_zb_scheduler_alarm(ota_query_alarm_cb, 0, OTA_QUERY_INTERVAL_MS);
}

static void on_joined(void)
{
    xEventGroupSetBits(s_connected_group, CONNECTED_BIT);
    esp_zb_scheduler_alarm(report_state_alarm_cb, 0, 200);
    /* Give the coordinator a moment to answer commissioning before asking
     * for the clock, then the clock module keeps us in sync. */
    esp_zb_scheduler_alarm(request_time_alarm_cb, 0, 2000);
    esp_zb_scheduler_alarm(ota_query_alarm_cb, 0, 3000);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, restored network (short address: 0x%04x, PAN ID: 0x%04x)",
                         esp_zb_get_short_address(), esp_zb_get_pan_id());
                on_joined();
            }
        } else {
            ESP_LOGW(TAG, "%s failed: %s, retrying...", esp_zb_zdo_signal_to_string(sig_type), esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            esp_zb_ieee_addr_t ieee_addr;
            esp_zb_get_long_address(ieee_addr);
            ESP_LOGI(TAG, "Joined network successfully");
            ESP_LOGI(TAG, "IEEE address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
                     ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
            ESP_LOGI(TAG, "Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0]);
            ESP_LOGI(TAG, "PAN ID: 0x%04x, short address: 0x%04x, channel: %d",
                     esp_zb_get_pan_id(), esp_zb_get_short_address(), esp_zb_get_current_channel());
            on_joined();
        } else {
            ESP_LOGW(TAG, "Network steering was not successful (status: %s), retrying...", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGI(TAG, "Left the network, factory reset and rebooting");
        storage_factory_reset();
        esp_zb_factory_reset();
        esp_restart();
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), (unsigned)sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;

    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attr_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
        break;

    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *resp =
            (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
        if (resp && resp->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_TIME) {
            esp_zb_zcl_read_attr_resp_variable_t *var = resp->variables;
            while (var) {
                if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS && var->attribute.data.value) {
                    if (var->attribute.id == ESP_ZB_ZCL_ATTR_TIME_TIME_ID) {
                        uint32_t t = 0;
                        memcpy(&t, var->attribute.data.value, sizeof(uint32_t));
                        clock_set_zcl_time(t);
                    } else if (var->attribute.id == ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID) {
                        int32_t tz = 0;
                        memcpy(&tz, var->attribute.data.value, sizeof(int32_t));
                        clock_set_timezone(tz);
                    }
                }
                var = var->next;
            }
        }
        ret = ESP_OK;
        break;
    }

    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
        ret = zb_ota_action_handler(callback_id, message);
        break;

    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
        const esp_zb_zcl_report_attr_message_t *rep =
            (const esp_zb_zcl_report_attr_message_t *)message;
        if (rep && rep->cluster == ESP_ZB_ZCL_CLUSTER_ID_TIME && rep->status == ESP_ZB_ZCL_STATUS_SUCCESS && rep->attribute.data.value) {
            if (rep->attribute.id == ESP_ZB_ZCL_ATTR_TIME_TIME_ID) {
                uint32_t t = 0;
                memcpy(&t, rep->attribute.data.value, sizeof(uint32_t));
                clock_set_zcl_time(t);
            } else if (rep->attribute.id == ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID) {
                int32_t tz = 0;
                memcpy(&tz, rep->attribute.data.value, sizeof(int32_t));
                clock_set_timezone(tz);
            }
        }
        ret = ESP_OK;
        break;
    }

    default:
        ESP_LOGW(TAG, "Unhandled Zigbee action (0x%x)", callback_id);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    (void)pvParameters;

    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_DEFAULT_ROUTER_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_ep_list_t *ep_list = slctr_ep_list_create();
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(zb_action_handler);
    zb_ota_init();
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_set_secondary_network_channel_set(ESP_ZB_SECONDARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void appzb_init(void)
{
    s_connected_group = xEventGroupCreate();

    /* Let the clock module poll the coordinator on a timer. */
    clock_set_resync_cb(zb_request_time_sync);

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}

bool appzb_is_connected(void)
{
    if (s_connected_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_connected_group) & CONNECTED_BIT) != 0;
}

static void factory_reset_alarm_cb(uint8_t arg)
{
    (void)arg;
    /* Runs in the Zigbee task: the Zigbee APIs below need the stack lock. */
    ESP_LOGW(TAG, "Factory reset: clearing settings and Zigbee network");
    storage_factory_reset();
    esp_zb_factory_reset();
    esp_restart();
}

void appzb_factory_reset(void)
{
    /* Wipe both the stored settings and the Zigbee network, then reboot. The
     * work is handed to the Zigbee task because the SDK is not thread safe. */
    esp_zb_scheduler_alarm(factory_reset_alarm_cb, 0, 0);
}

static void pair_alarm_cb(uint8_t arg)
{
    (void)arg;
    /* Runs in the Zigbee task. Reset only the Zigbee network (the light
     * settings survive), which makes the next boot factory-new and starts
     * network steering so the device can join again. */
    ESP_LOGW(TAG, "Pairing: leaving network and restarting to steer");
    esp_zb_factory_reset();
    esp_restart();
}

void appzb_start_pairing(void)
{
    /* Clear the Zigbee network and reboot; on boot the stack is factory-new
     * and the DEVICE_FIRST_START path runs network steering. Local settings
     * (brightness, CCT, timezone) are left untouched. */
    esp_zb_scheduler_alarm(pair_alarm_cb, 0, 2000);
}

static void ota_check_alarm_cb(uint8_t arg)
{
    (void)arg;
    zb_ota_query_now(true);
}

void appzb_check_ota(void)
{
    /* Schedule on the Zigbee task: all stack APIs are single-threaded. */
    esp_zb_scheduler_alarm(ota_check_alarm_cb, 0, 0);
}
