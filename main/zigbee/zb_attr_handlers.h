#pragma once

#include "esp_zigbee_core.h"

/* Called from the Zigbee stack for ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID. The stack
 * translates received standard cluster commands (On/Off, MoveToLevel,
 * MoveToColorTemperature, ...) into attribute-set notifications, so all hub
 * control arrives here. */
esp_err_t zb_attr_handler(const esp_zb_zcl_set_attr_value_message_t *message);