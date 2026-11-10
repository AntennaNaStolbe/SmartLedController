#pragma once

#include "lvgl.h"

/* Initialize SPI + GC9A01 + LVGL and start the LVGL task. */
void display_init(void);

/* Display orientation in degrees (0, 90, 180 or 270). The panel is square, so
 * rotation is a MADCTL change only: the LVGL resolution and draw buffers stay
 * the same. display_set_rotation() must be called with the display lock held
 * and repaints the whole screen afterwards. */
uint16_t display_get_rotation(void);
void display_set_rotation(uint16_t deg);

/* LVGL is only safe from one task at a time; take the lock around any UI call. */
void display_lock(void);
void display_unlock(void);