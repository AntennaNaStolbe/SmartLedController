#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_zigbee_core.h"

/* Register the OTA upgrade callback with the Zigbee stack. */
void zb_ota_init(void);

/* Handle OTA-related core action callbacks. Called from the Zigbee task. */
esp_err_t zb_ota_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

/* Ask the coordinator's OTA server for a newer image. Must be called from
 * the Zigbee task (or through its scheduler). */
void zb_ota_query_now(void);

/* True while an image is being written; percent is 0..100. */
bool zb_ota_get_progress(uint8_t *percent);

/* Result of the last finished upgrade. Read (and clear) it when progress
 * stops, so the UI can replace "OTA 100%" with a final status. */
typedef enum {
    ZB_OTA_RESULT_NONE = 0,
    ZB_OTA_RESULT_SUCCESS,
    ZB_OTA_RESULT_FAIL,
} zb_ota_result_t;

zb_ota_result_t zb_ota_take_result(void);
