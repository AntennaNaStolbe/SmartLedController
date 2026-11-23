#include <math.h>
#include <stdio.h>
#include <string.h>

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
#include "storage.h"
#include "time/clock.h"
#include "zigbee/zb_app.h"

/* The screen background is set once and never changes: pure black with light
 * text on top, in every power state. */
#define UI_COLOR_BG        0x000000
#define UI_COLOR_ARC_BG    0x2a3038
#define UI_COLOR_BR        0x42a5f5
#define UI_COLOR_CT        0xffa726
#define UI_COLOR_TEXT      0xf2f4f7
#define UI_COLOR_DIM       0x79838f
#define UI_COLOR_OFF       0x8d949d

/* One complete ring, split into two halves. Both arcs share the geometry so
 * they read as a single dial. The two halves stop this many degrees short of
 * 12 and 6 o'clock so the round line caps do not collide. */
#define RING_DIAMETER      212
#define RING_THICKNESS     12
#define ARC_GAP_DEG        10
#define ARC_TOP_START      (270 + ARC_GAP_DEG / 2)   /* past 12 o'clock     */
#define ARC_TOP_END        (450 - ARC_GAP_DEG / 2)   /* stops before 6      */
#define ARC_BOT_START      (90 + ARC_GAP_DEG / 2)    /* past 6 o'clock      */
#define ARC_BOT_END        (270 - ARC_GAP_DEG / 2)   /* stops before 12     */

/* Vertical centres of the value / caption pairs. Each pair is value on top,
 * caption underneath, and the two pairs are kept clear of each other in the
 * middle of the dial (the ring starts at radius 100, so anything inside
 * +-90 px vertically is safe). */
#define CENTER_TOP_Y       (-42)   /* colour temperature readout  */
#define CENTER_CAP_Y       (-12)   /* "TEMP"                      */
#define CENTER_BOT_Y       (18)    /* brightness readout          */
#define CENTER_CAP2_Y      (46)    /* "BRIGHT"                    */

#define UI_PI              3.14159265358979323846f
#define CURVED_TIME_RADIUS 88
#define CURVED_DATE_RADIUS 88
#define CURVED_TIME_STEP   9
#define CURVED_DATE_STEP   7
#define CURVED_TIME_GLYPHS 8
#define CURVED_DATE_GLYPHS 16

typedef enum {
    UI_SCREEN_MAIN,        /* both scales visible (spec "state 1")     */
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

/* Which parameter the encoder adjusts: false = brightness (default),
 * true = colour temperature. Toggled by a hold (INPUT_EV_MODE_TOGGLE). */
static bool s_adjust_color_temp;

/* main group */
static lv_obj_t *s_grp_main;
static lv_obj_t *s_layer_ct;
static lv_obj_t *s_layer_br;
static lv_obj_t *s_arc_ct;
static lv_obj_t *s_arc_br;
static lv_obj_t *s_lbl_ct;
static lv_obj_t *s_lbl_br;
static lv_obj_t *s_lbl_cap_ct;
static lv_obj_t *s_lbl_cap_br;
static lv_obj_t *s_time_glyphs[CURVED_TIME_GLYPHS];
static lv_obj_t *s_date_glyphs[CURVED_DATE_GLYPHS];

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

/* Menu entries. Rotate moves the selection, a single click runs the action,
 * a double click leaves the menu. value is optional and shown next to the
 * label (used by settings that cycle through a few states). */
typedef struct {
    const char *label;
    void (*action)(void);
    const char *(*value)(void);
} ui_menu_item_t;

static void update_menu_view(void);

static const char *menu_rotation_value(void)
{
    static char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)display_get_rotation());
    return buf;
}

static void menu_action_rotation(void)
{
    uint16_t deg = (uint16_t)((display_get_rotation() + 90U) % 360U);
    display_set_rotation(deg);
    storage_set_display_rotation(deg);
    update_menu_view();
}

static const ui_menu_item_t s_menu_items[] = {
    { "PAIR ZIGBEE", appzb_start_pairing, NULL },
    { "OTA CHECK",   appzb_check_ota, NULL },
    { "ROTATE",      menu_action_rotation, menu_rotation_value },
};
#define UI_MENU_ITEM_COUNT (sizeof(s_menu_items) / sizeof(s_menu_items[0]))

static uint8_t s_menu_index;

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

static void hide_done_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void fade_to(lv_obj_t *obj, lv_opa_t target, uint32_t dur,
                    lv_anim_ready_cb_t ready)
{
    lv_anim_del(obj, anim_opa_cb);

    if (target > LV_OPA_TRANSP) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    /* Start from wherever the object is. Hardcoding LV_OPA_COVER here made
     * every fade-in a no-op: obj_show() set the object transparent first and
     * the animation then went 255 -> 255, so the new screen appeared on the
     * first frame while the old one was still fading out. */
    lv_anim_set_values(&a, lv_obj_get_style_opa(obj, LV_PART_MAIN), target);
    lv_anim_set_time(&a, dur ? dur : 1);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    if (ready) {
        lv_anim_set_ready_cb(&a, ready);
    }
    lv_anim_start(&a);
}

static void obj_show(lv_obj_t *obj, uint32_t dur)
{
    if (dur == 0) {
        lv_anim_del(obj, anim_opa_cb);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
        return;
    }
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    fade_to(obj, LV_OPA_COVER, dur, NULL);
}

static void obj_hide(lv_obj_t *obj, uint32_t dur)
{
    if (dur == 0) {
        lv_anim_del(obj, anim_opa_cb);
        lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    fade_to(obj, LV_OPA_TRANSP, dur, hide_done_cb);
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
    lv_arc_set_value(arc, 0);

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

static void make_curved_glyphs(lv_obj_t *parent, lv_obj_t **glyphs, size_t count,
                               const lv_font_t *font, lv_color_t color)
{
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, color, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(lbl, 0, 0);
        lv_label_set_text(lbl, " ");
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        glyphs[i] = lbl;
    }
}

static void set_curved_text(lv_obj_t **glyphs, size_t count, const char *text,
                            lv_coord_t radius, int16_t center_deg, int16_t step_deg,
                            bool reverse)
{
    size_t len = strlen(text);
    if (len > count) {
        len = count;
    }

    float mid = len ? (float)(len - 1) / 2.0f : 0.0f;

    for (size_t i = 0; i < count; i++) {
        if (i >= len) {
            lv_obj_add_flag(glyphs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        char ch[2] = {text[i], '\0'};
        lv_label_set_text(glyphs[i], ch);
        lv_obj_clear_flag(glyphs[i], LV_OBJ_FLAG_HIDDEN);

        float theta = (center_deg + (reverse ? -1.0f : 1.0f) * ((float)i - mid) * (float)step_deg) * UI_PI / 180.0f;
        float px = (float)(LCD_H_RES / 2) + radius * cosf(theta);
        float py = (float)(LCD_V_RES / 2) + radius * sinf(theta);

        lv_coord_t w = lv_obj_get_width(glyphs[i]);
        lv_coord_t h = lv_obj_get_height(glyphs[i]);
        lv_obj_set_pos(glyphs[i], (lv_coord_t)lroundf(px - (float)w / 2.0f),
                       (lv_coord_t)lroundf(py - (float)h / 2.0f));
        lv_obj_set_style_transform_pivot_x(glyphs[i], w / 2, 0);
        lv_obj_set_style_transform_pivot_y(glyphs[i], h / 2, 0);

        float tangent;
        if (reverse) {
            tangent = atan2f(-cosf(theta), sinf(theta)) * 180.0f / UI_PI;
        } else {
            tangent = atan2f(cosf(theta), -sinf(theta)) * 180.0f / UI_PI;
        }
        int32_t angle = (int32_t)lroundf(tangent * 10.0f);
        angle %= 3600;
        if (angle < 0) {
            angle += 3600;
        }
        lv_obj_set_style_transform_angle(glyphs[i], (lv_coord_t)angle, 0);
    }
}

static void build_main_group(lv_obj_t *scr)
{
    s_grp_main = make_layer(scr);

    s_layer_ct = make_layer(s_grp_main);
    s_arc_ct = make_arc(s_layer_ct, ARC_TOP_START, ARC_TOP_END);
    lv_obj_set_style_arc_color(s_arc_ct, lv_color_hex(UI_COLOR_CT), LV_PART_INDICATOR);
    s_lbl_ct = make_label(s_layer_ct, &lv_font_montserrat_24, lv_color_hex(UI_COLOR_TEXT),
                          CENTER_TOP_Y, "--");
    s_lbl_cap_ct = make_label(s_layer_ct, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                              CENTER_CAP_Y, "TEMP");

    s_layer_br = make_layer(s_grp_main);
    s_arc_br = make_arc(s_layer_br, ARC_BOT_START, ARC_BOT_END);
    lv_obj_set_style_arc_color(s_arc_br, lv_color_hex(UI_COLOR_BR), LV_PART_INDICATOR);
    s_lbl_br = make_label(s_layer_br, &lv_font_montserrat_24, lv_color_hex(UI_COLOR_TEXT),
                          CENTER_BOT_Y, "--");
    s_lbl_cap_br = make_label(s_layer_br, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                              CENTER_CAP2_Y, "BRIGHT");

    make_curved_glyphs(s_grp_main, s_time_glyphs, CURVED_TIME_GLYPHS,
                       &lv_font_montserrat_16, lv_color_hex(UI_COLOR_TEXT));
    make_curved_glyphs(s_grp_main, s_date_glyphs, CURVED_DATE_GLYPHS,
                       &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM));
}

/* Highlight whichever parameter the encoder currently adjusts: the active
 * side keeps full opacity on its caption, value and arc indicator; the other
 * is dimmed so it is obvious where a rotate will land. */
static void update_mode_indicator(void)
{
    bool ct = s_adjust_color_temp;

    lv_obj_set_style_text_color(s_lbl_cap_ct,
                                lv_color_hex(ct ? UI_COLOR_TEXT : UI_COLOR_DIM), 0);
    lv_obj_set_style_text_color(s_lbl_cap_br,
                                lv_color_hex(ct ? UI_COLOR_DIM : UI_COLOR_TEXT), 0);

    lv_obj_set_style_text_color(s_lbl_ct,
                                lv_color_hex(ct ? UI_COLOR_TEXT : UI_COLOR_DIM), 0);
    lv_obj_set_style_text_color(s_lbl_br,
                                lv_color_hex(ct ? UI_COLOR_DIM : UI_COLOR_TEXT), 0);

    lv_obj_set_style_arc_opa(s_arc_ct, ct ? LV_OPA_COVER : LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_br, ct ? LV_OPA_50 : LV_OPA_COVER, LV_PART_INDICATOR);
}

static void build_off_group(lv_obj_t *scr)
{
    s_grp_off = make_layer(scr);
    s_lbl_off = make_label(s_grp_off, &lv_font_montserrat_16, lv_color_hex(UI_COLOR_OFF),
                           -42, "OFF");
    s_lbl_time = make_label(s_grp_off, &lv_font_montserrat_40, lv_color_hex(UI_COLOR_TEXT),
                            0, "--:--");
    s_lbl_date = make_label(s_grp_off, &lv_font_montserrat_28, lv_color_hex(UI_COLOR_DIM),
                            48, "no time sync");
}

static void update_menu_view(void)
{
    char buf[192];
    size_t n = 0;
    for (size_t i = 0; i < UI_MENU_ITEM_COUNT && n < sizeof(buf); i++) {
        const char *value = s_menu_items[i].value ? s_menu_items[i].value() : NULL;
        if (value != NULL) {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s %s\n",
                                  i == s_menu_index ? "> " : "  ",
                                  s_menu_items[i].label, value);
        } else {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s\n",
                                  i == s_menu_index ? "> " : "  ",
                                  s_menu_items[i].label);
        }
    }
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    }
    lv_label_set_text(s_lbl_menu_items, buf);
}

static void build_menu_group(lv_obj_t *scr)
{
    s_grp_menu = make_layer(scr);
    s_lbl_menu_title = make_label(s_grp_menu, &lv_font_montserrat_16, lv_color_hex(UI_COLOR_DIM),
                                  -56, "MENU");
    s_lbl_menu_items = make_label(s_grp_menu, &lv_font_montserrat_20, lv_color_hex(UI_COLOR_TEXT),
                                  -6, "");
    s_lbl_menu_hint = make_label(s_grp_menu, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                                 52, "rotate / click, double = exit");
    update_menu_view();
}

/* ------------------------------------------------------------------ */
/* screen switching                                                    */
/* ------------------------------------------------------------------ */

static void update_clock_labels(void)
{
    char buf[24];
    clock_get_time_str(buf, sizeof(buf));
    lv_label_set_text(s_lbl_time, buf);
    set_curved_text(s_time_glyphs, CURVED_TIME_GLYPHS, buf,
                    CURVED_TIME_RADIUS, 270, CURVED_TIME_STEP, false);
    clock_get_date_str(buf, sizeof(buf));
    lv_label_set_text(s_lbl_date, buf);
    set_curved_text(s_date_glyphs, CURVED_DATE_GLYPHS, buf,
                    CURVED_DATE_RADIUS, 90, CURVED_DATE_STEP, true);
}

static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_screen != UI_SCREEN_MENU) {
        update_clock_labels();
    }
}

static uint8_t brightness_pct(const light_state_t *st)
{
    return (uint8_t)((uint32_t)st->brightness * 100UL / LM_LEVEL_MAX);
}

/* Warmer colour (larger mired value) fills less of the arc; colder fills more. */
static uint8_t color_temp_pct(const light_state_t *st)
{
    int k = (int)st->color_temp_kelvin;
    if (k < LM_CT_MIN_K) {
        k = LM_CT_MIN_K;
    } else if (k > LM_CT_MAX_K) {
        k = LM_CT_MAX_K;
    }
    return (uint8_t)(((uint32_t)(k - LM_CT_MIN_K) * 100UL) / (LM_CT_MAX_K - LM_CT_MIN_K));
}

/* Animate both arcs to the current model values. dur decides whether this
 * is a plain value update (short) or the sweep that runs with a screen
 * change (as long as the fade-in). */
static void arc_sync(uint32_t dur)
{
    light_state_t st;
    light_model_get(&st);
    arc_to(s_arc_br, brightness_pct(&st), dur);
    arc_to(s_arc_ct, color_temp_pct(&st), dur);
}

/* ------------------------------------------------------------------ */
/* screen switching                                                    */
/* ------------------------------------------------------------------ */

/* The arcs are parked at 0 while the main screen is hidden, so that the next
 * appearance has something to sweep from. Held means "refresh_locked() must
 * not touch them": without it the model push that follows a power change
 * would drive them back to the value while nobody can see it. */
static bool s_arc_hold;

/* The group on its way out and the one waiting to come in. The two halves of
 * a transition never overlap - the incoming group starts fading only when the
 * outgoing one is fully gone, which is what keeps the two texts apart. */
static lv_obj_t *s_cur_group;
static lv_obj_t *s_pending_show;

static lv_obj_t *group_of(ui_screen_t screen)
{
    switch (screen) {
    case UI_SCREEN_MAIN: return s_grp_main;
    case UI_SCREEN_OFF:  return s_grp_off;
    default:             return s_grp_menu;
    }
}

static void show_next_cb(lv_anim_t *a)
{
    lv_obj_t *hid = (lv_obj_t *)a->var;
    lv_obj_t *next = s_pending_show;

    lv_obj_add_flag(hid, LV_OBJ_FLAG_HIDDEN);
    s_pending_show = NULL;
    if (next == NULL) {
        return;   /* an instant switch cancelled the transition */
    }
    s_cur_group = next;

    if (hid == s_grp_main) {
        /* Nobody is looking any more: reset for the next appearance. */
        lv_arc_set_value(s_arc_br, 0);
        lv_arc_set_value(s_arc_ct, 0);
        s_arc_hold = true;
    }
    if (next == s_grp_main) {
        /* Sweep the scales over exactly the fade-in, so they finish running
         * in the same frame the screen finishes appearing. */
        s_arc_hold = false;
        arc_sync(UI_ANIM_SCREEN_MS);
    }

    obj_show(next, UI_ANIM_SCREEN_MS);
}

static void show_screen(ui_screen_t screen, bool animate)
{
    lv_obj_t *newg = group_of(screen);
    lv_obj_t *oldg = s_cur_group;

    s_screen = screen;
    if (screen == UI_SCREEN_OFF) {
        update_clock_labels();
    }

    if (!animate || oldg == NULL || oldg == newg) {
        s_pending_show = NULL;
        /* Show the target and drop every other group straight away: with
         * nothing to chain onto there is no callback to hide them later. */
        obj_show(newg, 0);
        if (s_grp_main != newg) {
            obj_hide(s_grp_main, 0);
        }
        if (s_grp_off != newg) {
            obj_hide(s_grp_off, 0);
        }
        if (s_grp_menu != newg) {
            obj_hide(s_grp_menu, 0);
        }
        s_cur_group = newg;
        s_arc_hold = (newg != s_grp_main);
        if (newg == s_grp_main) {
            arc_sync(0);
        }
        return;
    }

    if (oldg == s_grp_main) {
        s_arc_hold = true;
    }
    s_pending_show = newg;
    fade_to(oldg, LV_OPA_TRANSP, UI_ANIM_SCREEN_MS, show_next_cb);
}

static void refresh_locked(void)
{
    light_state_t st;
    light_model_get(&st);

    char buf[24];

    snprintf(buf, sizeof(buf), "%u%%", (unsigned)brightness_pct(&st));
    lv_label_set_text(s_lbl_br, buf);

    snprintf(buf, sizeof(buf), "%luK", (unsigned long)st.color_temp_kelvin);
    lv_label_set_text(s_lbl_ct, buf);

    /* Drive the dials from the model, animating towards the new value so a
     * jump coming from Home Assistant looks the same as a local turn. */
    if (!s_arc_hold) {
        arc_to(s_arc_br, brightness_pct(&st), UI_ANIM_FAST_MS);
        arc_to(s_arc_ct, color_temp_pct(&st), UI_ANIM_FAST_MS);
    }

    if (s_screen == UI_SCREEN_MAIN) {
        update_clock_labels();
    }
}

/* ------------------------------------------------------------------ */
/* input handling                                                      */
/* ------------------------------------------------------------------ */

static void apply_event(const input_event_t *ev)
{
    light_model_set_origin(LIGHT_ORIGIN_LOCAL);

    switch (ev->kind) {
    case INPUT_EV_ROTATE:
        if (s_screen == UI_SCREEN_MENU) {
            int count = (int)UI_MENU_ITEM_COUNT;
            int idx = ((int)s_menu_index + ev->detents) % count;
            s_menu_index = (uint8_t)((idx + count) % count);
            update_menu_view();
            break;
        }
        if (!light_model_is_on()) {
            light_model_set_power(true);
        }
        if (s_adjust_color_temp) {
            light_model_adjust_color_temp(ev->detents, ev->accel);
        } else {
            light_model_adjust_brightness(ev->detents, ev->accel);
        }
        break;

    case INPUT_EV_MODE_TOGGLE:
        if (s_screen == UI_SCREEN_MENU) {
            break;
        }
        s_adjust_color_temp = !s_adjust_color_temp;
        update_mode_indicator();
        break;

    case INPUT_EV_DOUBLE_CLICK:
        if (s_screen == UI_SCREEN_MENU) {
            show_screen(light_model_is_on() ? UI_SCREEN_MAIN : UI_SCREEN_OFF, true);
        } else {
            show_screen(UI_SCREEN_MENU, true);
        }
        break;

    case INPUT_EV_BTN_UP:
        if (s_screen == UI_SCREEN_MENU) {
            if (s_menu_index < UI_MENU_ITEM_COUNT && s_menu_items[s_menu_index].action) {
                s_menu_items[s_menu_index].action();
                update_menu_view();
            }
            break;
        }
        light_model_toggle_power();
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
             * OFF screen, whatever was on it before. */
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
    update_mode_indicator();

    /* Start fully transparent so the first transition is a fade-in. */
    obj_hide(s_grp_main, 0);
    obj_hide(s_grp_menu, 0);

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