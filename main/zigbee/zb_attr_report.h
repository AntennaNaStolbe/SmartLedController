#pragma once

#include <stdint.h>

/* Update the Zigbee attribute store and push a report to the coordinator. */
void zb_attr_report_attr(uint16_t cluster_id, uint16_t attr_id, void *value);

/* Report the current on/off, brightness and CCT state (used on join/reboot). */
void zb_attr_report_current_state(void);