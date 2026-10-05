#pragma once

#include <stdbool.h>

/* Start the Zigbee stack task (router role). */
void appzb_init(void);

/* True once the device has a working network connection. */
bool appzb_is_connected(void);

/* Clear settings and network, then reboot. */
void appzb_factory_reset(void);