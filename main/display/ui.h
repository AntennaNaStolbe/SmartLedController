#pragma once

#include "light_model.h"

typedef void (*ui_get_state_fn_t)(light_state_t *out);

/* Build the 240x240 screen; must be called after display_init(). */
void ui_init(ui_get_state_fn_t get_state);

/* Called whenever the light state changed; reads the state and redraws. */
void ui_request_refresh(void);