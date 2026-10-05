#pragma once

#include "lvgl.h"

/* Initialize SPI + GC9A01 + LVGL and start the LVGL task. */
void display_init(void);

/* LVGL is only safe from one task at a time; take the lock around any UI call. */
void display_lock(void);
void display_unlock(void);