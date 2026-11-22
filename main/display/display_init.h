#pragma once

#include "lvgl.h"

/* Initialize SPI + GC9A01 + LVGL and start the LVGL task. */
void display_init(void);

/* Display rotation in 90 degree steps: 0, 90, 180 or 270 degrees. */
uint8_t display_get_rotation(void);
uint8_t display_rotate_90(void);

/* LVGL is only safe from one task at a time; take the lock around any UI call. */
void display_lock(void);
void display_unlock(void);