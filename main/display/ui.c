#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "config.h"
#include "display/display_init.h"
#include "display/ui.h"
#include "input/input.h"
#include "light_model.h"
#include "time/clock.h"
#include "zigbee/zb_app.h"

#define UI_COLOR_BG        0x0e1116
#define UI_COLOR_ARC_BG    0x2a3038
#define UI_COLOR_BR        0x42a5f5
#define UI_COLOR_CT        0xffa726
#define UI_COLOR_TEXT      0xf2f4f7
#define UI_COLOR_DIM       0x79838f
#define UI_COLOR_OFF       0x8d949d

/* One complete ring, split into two halves. Both arcs share the geometry so
 * they read as a single dial. */
#define RING_DIAMETER      212
#define RING_THICKNESS     12
#define ARC_TOP_START      270     /* 12 o'clock -> 3 o'clock  */
#define ARC_TOP_END        450
#define ARC_BOT_START      90
#define ARC_BOT_END        270

#define CENTER_TOP_Y       (-26)   /* colour temperature readout  */
#define CENTER_BOT_Y       (26)    /* brightness readout          */

typedef enum {
    UI_SCREEN_MAIN,        /* both scales visible (spec "state 1")     */
    UI_SCREEN_FOCUS_BR,    /* brightness only, big value in the middle */
    UI_SCREEN_FOCUS_CT,    /* colour temperature only                  */
    UI_SCREEN_OFF,         /* OFF + clock                              */
    UI_SCREEN_MENU,        /* settings menu, navigation only           */
} ui_screen_t;

typedef struct {
    uint8_t kind;          /* 0 = refresh, 1 = input event */
    input_event_t ev;
} ui_cmd_t;

#define UI_CMD_REFRESH 0
#define UI_CMD_EVENT   1

static const char *TAG = "ui";

static QueueHandle_t s_ui_q;
static volatile bool s_refresh_pending;

static ui_screen_t s_screen = UI_SCREEN_OFF;

/* main group */
static lv_obj_t *s_grp_main;
static lv_obj_t *s_layer_ct;
static lv_obj_t *s_layer_br;
static lv_obj_t *s_arc_ct;
static lv_obj_t *s_arc_br;
static lv_obj_t *s_lbl_ct;
static lv_obj_t *s_lbl_br;
static lv_obj_t *s_lbl_ct_small;
static lv_obj_t *s_lbl_br_small;

/* off group */
static lv_obj_t *s_grp_off;
static lv_obj_t *s_lbl_off;
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_date;

/* menu group */
static lv_obj_t *s_grp_menu;
static lv_obj_t *s_lbl_menu_title;
static lv_obj_t *s_lbl_menu_items;
static lv_obj_t *s_lbl_menu_hint;

static lv_timer_t *s_focus_timer;
static lv_timer_t *s_clock_timer;

/* ------------------------------------------------------------------ */
/* animation helpers - these run inside LVGL, so the lock is already held */
/* ------------------------------------------------------------------ */

static void anim_arc_cb(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v);
}

static void arc_to(lv_obj_t *arc, int32_t value, uint32_t dur)
{
    if (dur == 0) {
        lv_arc_set_value(arc, value);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, anim_arc_cb);
    lv_anim_set_values(&a, lv_arc_get_value(arc), value);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_coord_t)v, 0);
}

static void anim_opa_ready_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void fade_to(lv_obj_t *obj, lv_coord_t target, uint32_t dur, bool hide_at_end)
{
    lv_anim_del(obj, anim_opa_cb);

    if (target > 0) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, target);
    lv_anim_set_time(&a, dur ? dur : 1);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    if (hide_at_end && target == LV_OPA_TRANSP) {
        lv_anim_set_ready_cb(&a, anim_opa_ready_cb);
    }
    lv_anim_start(&a);
}

static void obj_show(lv_obj_t *obj, uint32_t dur)
{
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    fade_to(obj, LV_OPA_COVER, dur, false);
}

static void obj_hide(lv_obj_t *obj, uint32_t dur)
{
    fade_to(obj, LV_OPA_TRANSP, dur, true);
}

/* ------------------------------------------------------------------ */
/* object construction                                                 */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, LCD_H_RES, LCD_V_RES);
    lv_obj_align(layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    return layer;
}

static lv_obj_t *make_arc(lv_obj_t *parent, int16_t start, int16_t end)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, RING_DIAMETER, RING_DIAMETER);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(arc, start, end);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_range(arc, 0, 100);

    lv_obj_set_style_arc_width(arc, RING_THICKNESS, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_ARC_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_set_style_arc_width(arc, RING_THICKNESS, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_BR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            lv_coord_t y_off, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, y_off);
    lv_label_set_text(lbl, text);
    return lbl;
}

static void build_main_group(lv_obj_t *scr)
{
    s_grp_main = make_layer(scr);

    s_layer_ct = make_layer(s_grp_main);
    s_arc_ct = make_arc(s_layer_ct, ARC_TOP_START, ARC_TOP_END);
    lv_obj_set_style_arc_color(s_arc_ct, lv_color_hex(UI_COLOR_CT), LV_PART_INDICATOR);
    s_lbl_ct = make_label(s_layer_ct, &lv_font_montserrat_24, lv_color_hex(UI_COLOR_TEXT),
                          CENTER_TOP_Y, "--");
    s_lbl_ct_small = make_label(s_layer_ct, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                                CENTER_TOP_Y + 20, "CCT");

    s_layer_br = make_layer(s_grp_main);
    s_arc_br = make_arc(s_layer_br, ARC_BOT_START, ARC_BOT_END);
    lv_obj_set_style_arc_color(s_arc_br, lv_color_hex(UI_COLOR_BR), LV_PART_INDICATOR);
    s_lbl_br = make_label(s_layer_br, &lv_font_montserrat_24, lv_color_hex(UI_COLOR_TEXT),
                          CENTER_BOT_Y, "--");
    s_lbl_br_small = make_label(s_layer_br, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                                CENTER_BOT_Y + 20, "LEVEL");
}

static void build_off_group(lv_obj_t *scr)
{
    s_grp_off = make_layer(scr);
    s_lbl_off = make_label(s_grp_off, &lv_font_montserrat_28, lv_color_hex(UI_COLOR_OFF),
                           -22, "OFF");
    s_lbl_time = make_label(s_grp_off, &lv_font_montserrat_20, lv_color_hex(UI_COLOR_TEXT),
                            22, "--:--");
    s_lbl_date = make_label(s_grp_off, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                            48, "no time sync");
}

static void build_menu_group(lv_obj_t *scr)
{
    /* The menu itself is intentionally empty: this is the foundation the
     * preset editor, settings reset and timezone picker will be built on.
     * Planned items:
     *   - brightness presets
     *   - reset to defaults
     *   - timezone offset (CLOCK_DEFAULT_TZ_OFFSET_S in config.h)
     */
    s_grp_menu = make_layer(scr);
    s_lbl_menu_title = make_label(s_grp_menu, &lv_font_montserrat_16, lv_color_hex(UI_COLOR_DIM),
                                  -56, "MENU");
    s_lbl_menu_items = make_label(s_grp_menu, &lv_font_montserrat_20, lv_color_hex(UI_COLOR_TEXT),
                                  -6, "empty");
    s_lbl_menu_hint = make_label(s_grp_menu, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                                 52, "click to exit");
}

/* ------------------------------------------------------------------ */
/* screen switching                                                    */
/* ------------------------------------------------------------------ */

static void update_clock_labels(void)
{
    char buf[24];
    clock_get_time_str(buf, sizeof(buf));
    lv_label_set_text(s_lbl_time, buf);
    clock_get_date_str(buf, sizeof(buf));
    lv_label_set_text(s_lbl_date, buf);
}

static void show_screen(ui_screen_t screen, bool animate);

static void focus_timeout_cb(lv_timer_t *t)
{
    (void)t;
    if (s_screen == UI_SCREEN_FOCUS_BR || s_screen == UI_SCREEN_FOCUS_CT) {
        show_screen(UI_SCREEN_MAIN, true);
    }
}

static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_screen == UI_SCREEN_OFF) {
        update_clock_labels();
    }
}

static void set_focus_active(bool active)
{
    if (active) {
        lv_timer_reset(s_focus_timer);
        lv_timer_resume(s_focus_timer);
    } else {
        lv_timer_pause(s_focus_timer);
    }
}

static void show_screen(ui_screen_t screen, bool animate)
{
    const uint32_t dur = animate ? UI_ANIM_SCREEN_MS : 0;
    s_screen = screen;

    switch (screen) {
    case UI_SCREEN_MAIN:
        obj_show(s_grp_main, dur);
        obj_hide(s_grp_off, dur);
        obj_hide(s_grp_menu, dur);
        obj_show(s_layer_br, dur);
        obj_show(s_layer_ct, dur);
        set_focus_active(false);
        break;

    case UI_SCREEN_FOCUS_BR:
        /* the temperature scale hides, brightness stays and is read big */
        obj_show(s_grp_main, dur);
        obj_hide(s_grp_off, dur);
        obj_hide(s_grp_menu, dur);
        obj_show(s_layer_br, dur);
        obj_hide(s_layer_ct, dur);
        set_focus_active(true);
        break;

    case UI_SCREEN_FOCUS_CT:
        obj_show(s_grp_main, dur);
        obj_hide(s_grp_off, dur);
        obj_hide(s_grp_menu, dur);
        obj_hide(s_layer_br, dur);
        obj_show(s_layer_ct, dur);
        set_focus_active(true);
        break;

    case UI_SCREEN_OFF:
        update_clock_labels();
        obj_hide(s_grp_main, dur);
        obj_hide(s_grp_menu, dur);
        obj_show(s_grp_off, dur);
        set_focus_active(false);
        break;

    case UI_SCREEN_MENU:
        obj_hide(s_grp_main, dur);
        obj_hide(s_grp_off, dur);
        obj_show(s_grp_menu, dur);
        set_focus_active(false);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* state -> widgets                                                    */
/* ------------------------------------------------------------------ */

static uint32_t mireds_to_kelvin(uint16_t mireds)
{
    return mireds ? (1000000UL / mireds) : 0;
}

static void refresh_locked(void)
{
    light_state_t st;
    light_model_get(&st);

    char buf[24];

    uint8_t br_pct = (uint8_t)((uint32_t)st.brightness * 100UL / LM_LEVEL_MAX);
    uint32_t kelvin = mireds_to_kelvin(st.color_temp_mireds);
    uint8_t ct_pct = (uint8_t)((uint32_t)(st.color_temp_mireds - LM_CT_MIN_MIREDS) * 100UL
                               / (LM_CT_MAX_MIREDS - LM_CT_MIN_MIREDS));

    snprintf(buf, sizeof(buf), "%u%%", (unsigned)br_pct);
    lv_label_set_text(s_lbl_br, buf);
    lv_label_set_text_fmt(s_lbl_br_small, "%u%%", (unsigned)br_pct);

    snprintf(buf, sizeof(buf), "%luK", (unsigned long)kelvin);
    lv_label_set_text(s_lbl_ct, buf);
    lv_label_set_text_fmt(s_lbl_ct_small, "%luK", (unsigned long)kelvin);

    /* Drive the dials from the model, animating towards the new value so a
     * jump coming from Home Assistant looks the same as a local turn. */
    arc_to(s_arc_br, br_pct, UI_ANIM_FAST_MS);
    arc_to(s_arc_ct, ct_pct, UI_ANIM_FAST_MS);

    if (s_screen != UI_SCREEN_OFF) {
        update_clock_labels();
    }
}

/* ------------------------------------------------------------------ */
/* input handling                                                      */
/* ------------------------------------------------------------------ */

static void focus_value(int8_t detents, bool color_temp)
{
    light_model_set_origin(LIGHT_ORIGIN_LOCAL);
    if (color_temp) {
        light_model_adjust_color_temp(detents);
        show_screen(UI_SCREEN_FOCUS_CT, true);
    } else {
        light_model_adjust_brightness(detents);
        show_screen(UI_SCREEN_FOCUS_BR, true);
    }
}

static void apply_event(const input_event_t *ev)
{
    light_model_set_origin(LIGHT_ORIGIN_LOCAL);

    switch (ev->kind) {
    case INPUT_EV_ROTATE:
        if (s_screen == UI_SCREEN_MENU) {
            /* Menu navigation is wired up but has nothing to navigate yet. */
            break;
        }
        if (!light_model_is_on()) {
            light_model_set_power(true);
        }
        focus_value(ev->detents, false);
        break;

    case INPUT_EV_ROTATE_HELD:
        if (s_screen == UI_SCREEN_MENU) {
            break;
        }
        if (!light_model_is_on()) {
            light_model_set_power(true);
        }
        focus_value(ev->detents, true);
        break;

    case INPUT_EV_BTN_UP:
        if (s_screen == UI_SCREEN_MENU) {
            show_screen(light_model_is_on() ? UI_SCREEN_MAIN : UI_SCREEN_OFF, true);
            break;
        }
        light_model_toggle_power();
        break;

    case INPUT_EV_MENU:
        show_screen(UI_SCREEN_MENU, true);
        break;

    case INPUT_EV_FACTORY:
        appzb_factory_reset();
        break;

    case INPUT_EV_BTN_DOWN:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* UI task                                                             */
/* ------------------------------------------------------------------ */

static void ui_task(void *arg)
{
    (void)arg;
    ui_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(s_ui_q, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd.kind == UI_CMD_EVENT) {
                display_lock();
                apply_event(&cmd.ev);
                display_unlock();
            }
        }
        if (s_refresh_pending) {
            s_refresh_pending = false;
            display_lock();
            light_state_t st;
            light_model_get(&st);
            /* The model drives the screen: an off light always shows the
             * OFF screen, whatever the last local focus was. */
            if (s_screen != UI_SCREEN_MENU && !st.on_off) {
                if (s_screen != UI_SCREEN_OFF) {
                    show_screen(UI_SCREEN_OFF, true);
                }
            } else if (s_screen == UI_SCREEN_OFF) {
                show_screen(UI_SCREEN_MAIN, true);
            }
            refresh_locked();
            display_unlock();
        }
    }
}

void ui_init(void)
{
    s_ui_q = xQueueCreate(16, sizeof(ui_cmd_t));
    if (s_ui_q == NULL) {
        ESP_LOGE(TAG, "failed to create the UI queue");
        abort();
    }

    /* lvgl_task is already running at this point and LVGL is not thread safe,
     * so build the object tree under the display lock. */
    display_lock();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_main_group(scr);
    build_off_group(scr);
    build_menu_group(scr);

    /* Start fully transparent so the first transition is a fade-in. */
    obj_hide(s_grp_main, 0);
    obj_hide(s_grp_menu, 0);

    s_focus_timer = lv_timer_create(focus_timeout_cb, UI_FOCUS_TIMEOUT_MS, NULL);
    lv_timer_pause(s_focus_timer);

    s_clock_timer = lv_timer_create(clock_tick_cb, 1000, NULL);

    light_state_t st;
    light_model_get(&st);
    show_screen(st.on_off ? UI_SCREEN_MAIN : UI_SCREEN_OFF, false);
    update_clock_labels();
    refresh_locked();

    display_unlock();

    xTaskCreate(ui_task, "ui", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready");
}

void ui_request_refresh(void)
{
    if (s_ui_q == NULL) {
        return;
    }
    s_refresh_pending = true;
    ui_cmd_t cmd = {.kind = UI_CMD_REFRESH};
    xQueueSend(s_ui_q, &cmd, 0);
}

void ui_handle_input(const input_event_t *ev)
{
    if (s_ui_q == NULL || ev == NULL) {
        return;
    }
    ui_cmd_t cmd = {.kind = UI_CMD_EVENT, .ev = *ev};
    xQueueSend(s_ui_q, &cmd, 0);
}