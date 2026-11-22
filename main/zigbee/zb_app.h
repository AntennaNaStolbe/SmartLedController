#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bring up the Zigbee stack, join and start serving the light endpoint. */
void appzb_init(void);

/* True once the device has joined or rejoined a network. */
bool appzb_is_connected(void);

/* Report the current light state to the coordinator. Called by the light
 * model for locally originated changes only, so remote writes are not echoed
 * back onto the mesh. */
void zb_report_local_state(void);

/* Ask the coordinator for a fresh Time attribute. Called by the clock module
 * on a timer; also issued right after joining. */
void zb_request_time_sync(void);

/* Wipe the Zigbee network and the stored settings, then reboot. */
void appzb_factory_reset(void);

/* Factory-reset the Zigbee network only (keeping the light settings) and
 * reboot so the device can be paired again. */
void appzb_start_pairing(void);

/* Ask the coordinator's OTA server whether a newer image is available. */
void appzb_check_ota(void);
