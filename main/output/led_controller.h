#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

/* Two-channel PWM CCT driver (warm/cool) with a smooth fade queue.
 * Channels: PIN_LED_WARM / PIN_LED_COOL (ESP32-C6 SuperMini GPIO4 / GPIO5).
 *
 * CCT mapping (3000 K warm .. 6500 K cold):
 *   warm_frac = (mireds - MIN) / (MAX - MIN)
 *   cool_frac = 1 - warm_frac
 * so at 3000 K the warm channel is fully on and at 6500 K the cool channel is.
 */

void lc_init(void);

/* Schedule a fade to the given state. fade_ms <= lc_fade_step applies instantly. */
void lc_set_fade(bool on_off, uint8_t brightness, uint16_t mireds, uint32_t fade_ms);

#endif /* LED_CONTROLLER_H */