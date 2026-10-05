#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Input events produced by the EC11 encoder task and consumed by the UI
 * layer. The encoder knows nothing about the light model or the screens: it
 * only translates contacts into gestures, which is what makes a settings
 * menu possible without touching the encoder.
 */
typedef enum {
    INPUT_EV_ROTATE,       /* rotary detent, no button held          */
    INPUT_EV_ROTATE_HELD,  /* rotary detent while the button is held  */
    INPUT_EV_BTN_DOWN,     /* button pressed (after debounce)        */
    INPUT_EV_BTN_UP,       /* button released, no long press fired    */
    INPUT_EV_MENU,         /* held long enough without rotating       */
    INPUT_EV_FACTORY,      /* held long enough for a factory reset    */
} input_ev_kind_t;

typedef struct {
    input_ev_kind_t kind;
    int8_t          detents;   /* +/- for INPUT_EV_ROTATE*   */
    uint32_t        held_ms;
} input_event_t;

/* Start the encoder task. Call once after light_model_init(). */
void input_init(void);

/* Hand an event to the consumer. Provided by the UI layer. */
typedef void (*input_handler_t)(const input_event_t *ev);
void input_set_handler(input_handler_t handler);

#endif /* INPUT_H */