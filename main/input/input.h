#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Input events produced by the EC11 encoder task and consumed by the UI
 * layer. The encoder knows nothing about the light model or the screens: it
 * only translates contacts into gestures, which is what makes a settings
 * menu possible without touching the encoder.
 *
 * Gesture map:
 *   rotate             -> INPUT_EV_ROTATE          adjust active parameter
 *   short click        -> INPUT_EV_BTN_UP          power toggle
 *   double click       -> INPUT_EV_DOUBLE_CLICK    open / close menu
 *   hold (>= 0.5 s)    -> INPUT_EV_MODE_TOGGLE     brightness <-> CCT
 *   hold 5 s           -> INPUT_EV_FACTORY         factory reset
 */
typedef enum {
    INPUT_EV_ROTATE,       /* rotary detent                              */
    INPUT_EV_BTN_DOWN,     /* button pressed (after debounce)            */
    INPUT_EV_BTN_UP,       /* single click (delayed by double-click win) */
    INPUT_EV_DOUBLE_CLICK, /* two quick clicks                           */
    INPUT_EV_MODE_TOGGLE,  /* held >= BTN_MODE_HOLD_MS                  */
    INPUT_EV_FACTORY,      /* held long enough for a factory reset       */
} input_ev_kind_t;

typedef struct {
    input_ev_kind_t kind;
    int8_t          detents;   /* +/- for INPUT_EV_ROTATE          */
    uint8_t         accel;     /* rotation speed multiplier, 1 = slowest.
                                * Only meaningful for INPUT_EV_ROTATE;
                                * always 1 for button events. */
    uint32_t        held_ms;
} input_event_t;

/* Start the encoder task. Call once after light_model_init(). */
void input_init(void);

/* Hand an event to the consumer. Provided by the UI layer. */
typedef void (*input_handler_t)(const input_event_t *ev);
void input_set_handler(input_handler_t handler);

#endif /* INPUT_H */