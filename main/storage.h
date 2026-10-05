#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* Persistent application settings (PLATFORM.md §3) */
typedef struct {
    bool     on_off;
    uint8_t  brightness;            /* 0..254 */
    uint16_t color_temp_mireds;     /* 154..333 */
    uint16_t on_transition_time;    /* ms */
    uint16_t off_transition_time;   /* ms */
    uint8_t  startup_behavior;      /* ZCL StartUpOnOff: 0=off, 1=on, 2=toggle, 255=previous */
} app_settings_t;

/* Must be called once after nvs_flash_init(). */
void storage_init(void);

/* Returns true if valid stored settings were loaded, false and fills defaults otherwise. */
bool storage_load(app_settings_t *out);

void storage_save(const app_settings_t *in);

/* Erases the application settings namespace (used by factory reset). */
void storage_factory_reset(void);

#endif /* STORAGE_H */