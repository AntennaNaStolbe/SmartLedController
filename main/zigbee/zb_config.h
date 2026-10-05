#pragma once

#include "esp_zigbee_core.h"

/* Zigbee Router configuration (mesh router joining the zigbee2mqtt network) */
#define INSTALLCODE_POLICY_ENABLE false

#define HA_ESP_LIGHT_ENDPOINT 10

/* The coordinator (Zigbee2MQTT bridge) serves the Time cluster on endpoint 1. */
#define HA_ESP_COORDINATOR_ENDPOINT 1

/* Scan all Zigbee channels (11..26) on join */
#define ESP_ZB_PRIMARY_CHANNEL_MASK   ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define ESP_ZB_SECONDARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define ESP_ZB_DEFAULT_ROUTER_CONFIG()                                                                                                          \
    {                                                                                                                                          \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,                                                                                              \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,                                                                                      \
        .nwk_cfg = {                                                                                                                           \
            .zczr_cfg = {                                                                                                                      \
                .max_children = 10,                                                                                                            \
            },                                                                                                                                 \
        },                                                                                                                                     \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                                                                                                          \
    {                                                                                                                                          \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                                                                                                    \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                                                                                                           \
    {                                                                                                                                          \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,                                                                                  \
    }