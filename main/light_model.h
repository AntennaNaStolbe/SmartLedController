#ifndef LIGHT_MODEL_H
#define LIGHT_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/* Single source of truth for the light state. The Zigbee cluster layer, the
 * input layer and the UI all feed/read through this module; changes are pushed
 * to the PWM driver and persisted (debounced) to NVS.
 *
 * All setters may be called from any task: the internal state is guarded by a
 * mutex and the callbacks are always invoked *outside* of the lock.
 */

typedef struct {
    bool     on_off;
    uint8_t  brightness;         /* 0..LM_LEVEL_MAX, 0 means "off" */
    uint16_t color_temp_mireds;  /* integer mireds, ZCL/HA boundary value */
    uint16_t color_temp_kelvin;  /* the same colour temperature, exact:
                                  * this is what the display shows and what
                                  * the encoder steps by */
} light_state_t;

/* Who caused the current change. Changes made from Zigbee (HA / Z2M) must not
 * be reported back to the coordinator. */
typedef enum {
    LIGHT_ORIGIN_LOCAL,          /* encoder / menu / identify */
    LIGHT_ORIGIN_REMOTE,         /* Zigbee attribute write    */
} light_origin_t;

typedef void (*light_change_cb_t)(void);

void light_model_init(void);

/* UI refresh callback, invoked on every change. Must not block. */
void light_model_set_change_cb(light_change_cb_t cb);

/* Coordinator reporting callback, invoked only for LIGHT_ORIGIN_LOCAL
 * changes and debounced by LM_REPORT_DELAY_MS. */
void light_model_set_report_cb(light_change_cb_t cb);

/* Set the origin applied to the next change; defaults to LIGHT_ORIGIN_LOCAL. */
void light_model_set_origin(light_origin_t origin);

/* Zigbee identify: blink the strip for the given seconds (0 stops). */
void light_model_identify(uint16_t seconds);

/* Transition time (ms) used by the Zigbee level/on-off handling. */
void light_model_set_transition_time(uint16_t ms);

void light_model_set_power(bool on);
void light_model_toggle_power(void);
void light_model_set_brightness(uint8_t level);
/* detents * accel steps; accel comes from the encoder and scales the step
 * with the rotation speed (1 = slow, precise). */
void light_model_adjust_brightness(int8_t detents, uint8_t accel);
void light_model_set_color_temp(uint16_t mireds);
void light_model_adjust_color_temp(int8_t detents, uint8_t accel);

void light_model_get(light_state_t *out);
bool     light_model_is_on(void);
uint8_t  light_model_get_brightness(void);
uint16_t light_model_get_color_temp(void);
uint16_t light_model_get_transition_time(void);

#endif /* LIGHT_MODEL_H */