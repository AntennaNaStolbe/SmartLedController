#pragma once

#include <stdint.h>

/* Update the Zigbee attribute store and push a report to the coordinator.
 * Must run in the Zigbee task or with the stack lock held. */
void zb_attr_report_attr(uint16_t cluster_id, uint16_t attr_id, void *value);

/* Report the current on/off, brightness and CCT state.
 * Must run in the Zigbee task or with the stack lock held. */
void zb_attr_report_current_state(void);

/* Request a fresh Time attribute from the coordinator.
 * Must run in the Zigbee task or with the stack lock held. */
void zb_request_time_sync_now(void);