#pragma once

#include "esp_zigbee_core.h"

/* Cluster list for the device endpoint (HA color dimmable light, CCT only). */
esp_zb_cluster_list_t *slctr_clusters_create(void);

/* Endpoint list for the whole device. */
esp_zb_ep_list_t *slctr_ep_list_create(void);