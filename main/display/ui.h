#ifndef UI_H
#define UI_H

#include <stdint.h>

#include "input/input.h"

/* Build the 240x240 screen and start the UI task. Must be called after
 * display_init() and before input_init(). */
void ui_init(void);

/* Non-blocking: request a redraw from the current light state. Safe to call
 * from any task, including the Zigbee stack task. */
void ui_request_refresh(void);

/* Route an encoder gesture. Called from the encoder task; must not block. */
void ui_handle_input(const input_event_t *ev);

#endif /* UI_H */