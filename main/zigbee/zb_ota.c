#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_zigbee_ota.h"
#include "freertos/FreeRTOS.h"
#include "zdo/esp_zigbee_zdo_command.h"

#include "config.h"
#include "zb_config.h"
#include "zb_ota.h"

#define TAG "zb_ota"

#define OTA_ELEMENT_HEADER_LEN 6
#define OTA_ELEMENT_TAG_UPGRADE_IMAGE 0x0000

static const esp_partition_t *s_ota_partition = NULL;
static esp_ota_handle_t s_ota_handle = 0;
static bool s_tag_received = false;
static uint16_t s_element_tag = 0;
static uint8_t s_element_header[OTA_ELEMENT_HEADER_LEN] = {0};
static uint8_t s_element_header_len = 0;
static uint32_t s_expected_size = 0;
static volatile uint32_t s_stream_expected = 0;
static volatile uint32_t s_received = 0;
static uint32_t s_written = 0;
static volatile bool s_in_progress = false;
static uint32_t s_image_size = 0;
static uint32_t s_file_version = 0;

static void zb_ota_reset(void)
{
    s_ota_partition = NULL;
    s_ota_handle = 0;
    s_tag_received = false;
    s_element_tag = 0;
    s_element_header_len = 0;
    s_expected_size = 0;
    s_stream_expected = 0;
    s_received = 0;
    s_written = 0;
    s_in_progress = false;
}

static void zb_zdo_match_desc_cb(esp_zb_zdp_status_t status, uint16_t addr, uint8_t endpoint, void *ctx)
{
    (void)ctx;

    if (status != ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "OTA server not found on coordinator");
        return;
    }

    ESP_LOGI(TAG, "OTA server found: short addr 0x%04x, endpoint %u", addr, endpoint);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_ota_upgrade_client_query_interval_set(
        HA_ESP_LIGHT_ENDPOINT, OTA_QUERY_INTERVAL_MIN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_zb_ota_upgrade_client_query_image_req(addr, (uint8_t)endpoint));
}

static void zb_ota_match_server(void)
{
    if (!esp_zb_bdb_dev_joined()) {
        ESP_LOGW(TAG, "Cannot query OTA: not joined to a Zigbee network");
        return;
    }

    esp_zb_zdo_match_desc_req_param_t req = {
        .addr_of_interest = 0x0000,
        .dst_nwk_addr = 0x0000,
        .num_in_clusters = 1,
        .num_out_clusters = 0,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_list = (uint16_t[]){ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE},
    };
    esp_zb_zdo_match_cluster(&req, zb_zdo_match_desc_cb, NULL);
}

static esp_err_t zb_ota_extract_payload(const esp_zb_zcl_ota_upgrade_value_message_t *msg,
                                         uint32_t stream_expected, const void **payload, uint16_t *payload_size)
{
    if (!msg->payload || msg->payload_size == 0) {
        return ESP_OK;
    }

    const uint8_t *data = msg->payload;
    uint16_t data_size = msg->payload_size;

    if (!s_tag_received) {
        uint16_t needed = OTA_ELEMENT_HEADER_LEN - s_element_header_len;
        uint16_t copy = data_size < needed ? data_size : needed;
        memcpy(s_element_header + s_element_header_len, data, copy);
        s_element_header_len += copy;
        data += copy;
        data_size -= copy;

        if (s_element_header_len < OTA_ELEMENT_HEADER_LEN) {
            return ESP_OK;
        }

        s_element_tag = (uint16_t)s_element_header[0] | ((uint16_t)s_element_header[1] << 8);
        uint32_t element_len = (uint32_t)s_element_header[2] | ((uint32_t)s_element_header[3] << 8) |
                               ((uint32_t)s_element_header[4] << 16) | ((uint32_t)s_element_header[5] << 24);
        if (element_len + OTA_ELEMENT_HEADER_LEN != stream_expected) {
            ESP_LOGE(TAG, "Unexpected OTA element length %lu (stream %lu, image %lu)",
                     (unsigned long)element_len, (unsigned long)stream_expected,
                     (unsigned long)s_expected_size);
            return ESP_ERR_INVALID_SIZE;
        }
        if (s_element_tag != OTA_ELEMENT_TAG_UPGRADE_IMAGE) {
            ESP_LOGE(TAG, "Unsupported OTA element tag 0x%04x", s_element_tag);
            return ESP_ERR_NOT_SUPPORTED;
        }
        s_tag_received = true;
    }

    *payload = data;
    *payload_size = data_size;
    return ESP_OK;
}


static esp_err_t zb_ota_status_handler(const esp_zb_zcl_ota_upgrade_value_message_t *msg)
{
    if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "OTA status callback with ZCL status 0x%02x", msg->info.status);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    switch (msg->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START: {
        uint32_t file_version = msg->ota_header.file_version ? msg->ota_header.file_version : s_file_version;
        uint32_t image_size = msg->ota_header.image_size ? msg->ota_header.image_size : s_image_size;
        ESP_LOGI(TAG, "OTA start: version 0x%08lx, size %lu (header %lu, cached %lu)",
                 (unsigned long)file_version, (unsigned long)image_size,
                 (unsigned long)msg->ota_header.image_size, (unsigned long)s_image_size);
        zb_ota_reset();
        s_expected_size = image_size;
        if (s_expected_size == 0) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition) {
            ESP_LOGE(TAG, "No OTA partition available");
            ret = ESP_ERR_NOT_FOUND;
            break;
        }
        if (s_expected_size > (uint32_t)s_ota_partition->size) {
            ESP_LOGE(TAG, "OTA image %lu exceeds partition %lu",
                     (unsigned long)s_expected_size, (unsigned long)s_ota_partition->size);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        ret = esp_ota_begin(s_ota_partition, 0, &s_ota_handle);
        if (ret == ESP_OK) {
            s_in_progress = true;
        }
        break;
    }

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE: {
        const void *payload = NULL;
        uint16_t payload_size = 0;
        uint32_t stream_expected = msg->ota_header.image_size ? msg->ota_header.image_size : s_expected_size;
        if (s_stream_expected == 0) {
            s_stream_expected = stream_expected;
            ESP_LOGI(TAG, "OTA first receive: block %u, stream %lu, image %lu",
                     msg->payload_size, (unsigned long)s_stream_expected,
                     (unsigned long)s_expected_size);
        }
        s_received += msg->payload_size;
        ESP_LOGD(TAG, "OTA receive: block %u, total %lu / %lu",
                 msg->payload_size, (unsigned long)s_received, (unsigned long)s_stream_expected);
        ret = zb_ota_extract_payload(msg, stream_expected, &payload, &payload_size);
        if (ret != ESP_OK) {
            break;
        }
        if (payload && payload_size) {
            ret = esp_ota_write(s_ota_handle, payload, payload_size);
            if (ret == ESP_OK) {
                s_written += payload_size;
                if (s_received % 16384 < msg->payload_size) {
                    ESP_LOGI(TAG, "OTA progress: %lu / %lu, written %lu",
                             (unsigned long)s_received, (unsigned long)s_stream_expected,
                             (unsigned long)s_written);
                }
            }
        }
        break;
    }

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
        ret = (s_received == s_stream_expected && s_written + OTA_ELEMENT_HEADER_LEN == s_stream_expected)
                  ? ESP_OK : ESP_ERR_INVALID_SIZE;
        ESP_LOGI(TAG, "OTA check: %s (%lu / %lu, written %lu)",
                 ret == ESP_OK ? "OK" : "FAILED",
                 (unsigned long)s_received, (unsigned long)s_stream_expected,
                 (unsigned long)s_written);
        s_tag_received = false;
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        ESP_LOGI(TAG, "OTA apply");
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        ESP_LOGI(TAG, "OTA finished, activating partition %s", s_ota_partition ? s_ota_partition->label : "?");
        ret = esp_ota_end(s_ota_handle);
        if (ret == ESP_OK) {
            ret = esp_ota_set_boot_partition(s_ota_partition);
        }
        if (ret == ESP_OK) {
            s_in_progress = false;
            ESP_LOGW(TAG, "Rebooting into upgraded firmware");
            esp_restart();
        }
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
        ESP_LOGW(TAG, "OTA aborted (status %d), received %lu / %lu, written %lu",
                 msg->upgrade_status, (unsigned long)s_received, (unsigned long)s_stream_expected,
                 (unsigned long)s_written);
        if (s_in_progress) {
            esp_ota_abort(s_ota_handle);
        }
        zb_ota_reset();
        s_image_size = 0;
        s_file_version = 0;
        break;

    default:
        ESP_LOGD(TAG, "OTA status %d", msg->upgrade_status);
        break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA operation failed: %s", esp_err_to_name(ret));
        if (s_in_progress) {
            esp_ota_abort(s_ota_handle);
        }
        zb_ota_reset();
    }
    return ret;
}

static esp_err_t zb_ota_query_resp_handler(const esp_zb_zcl_ota_upgrade_query_image_resp_message_t *msg)
{
    if (msg->info.status == ESP_ZB_ZCL_STATUS_SUCCESS && msg->query_status == 0x00) {
        s_file_version = msg->file_version;
        s_image_size = msg->image_size;
        ESP_LOGI(TAG, "OTA image available: version 0x%08lx, size %lu, server 0x%04x ep %u",
                 (unsigned long)msg->file_version, (unsigned long)msg->image_size,
                 msg->server_addr.u.short_addr, msg->server_endpoint);
    } else {
        s_file_version = 0;
        s_image_size = 0;
        ESP_LOGI(TAG, "No OTA image (query status 0x%02x)", msg->query_status);
    }
    return ESP_OK;
}

void zb_ota_init(void)
{
    ESP_LOGI(TAG, "OTA client v0x%08x, manufacturer 0x%04x, image type 0x%04x",
             OTA_FILE_VERSION, OTA_MANUFACTURER, OTA_IMAGE_TYPE);
}

esp_err_t zb_ota_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        return zb_ota_status_handler((const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
        return zb_ota_query_resp_handler((const esp_zb_zcl_ota_upgrade_query_image_resp_message_t *)message);
    default:
        return ESP_FAIL;
    }
}

void zb_ota_query_now(void)
{
    zb_ota_match_server();
}

bool zb_ota_get_progress(uint8_t *percent)
{
    if (!s_in_progress) {
        return false;
    }
    if (percent != NULL) {
        uint32_t received = s_received;
        uint32_t expected = s_stream_expected;
        *percent = expected ? (uint8_t)((received * 100UL) / expected) : 0;
    }
    return true;
}
