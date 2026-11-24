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
#include "zigbee/zb_ota.h"

/* The screen background is set once and never changes: pure black with light
 * text on top, in every power state. */
#define UI_COLOR_BG        0x000000
#define UI_COLOR_ARC_BG    0x2a3038
#define UI_COLOR_BR        0x42a5f5
#define UI_COLOR_CT        0xffa726
#define UI_COLOR_TEXT      0xf2f4f7
#define UI_COLOR_DIM       0x79838f

/* One complete ring, split into two halves. Both arcs share the geometry so
 * they read as a single dial. The two halves stop this many degrees short of
 * 12 and 6 o'clock so the round line caps do not collide. */
#define RING_DIAMETER      238
#define RING_THICKNESS     12
#define ARC_GAP_DEG        70
#define ARC_TOP_START      (270 + ARC_GAP_DEG / 2)   /* past 12 o'clock     */
#define ARC_TOP_END        (450 - ARC_GAP_DEG / 2)   /* stops before 6      */
#define ARC_BOT_START      (90 + ARC_GAP_DEG / 2)    /* past 6 o'clock      */
#define ARC_BOT_END        (270 - ARC_GAP_DEG / 2)   /* stops before 12     */

/* Vertical centres of the value / caption pairs. Each pair is value on top,
 * caption underneath, and the two pairs are kept clear of each other in the
 * middle of the dial (the ring's inner edge is near 107 px, so anything
 * inside +-90 px vertically is safe). */
#define CENTER_TOP_Y       (-42)   /* colour temperature readout  */
#define CENTER_CAP_Y       (-12)   /* "TEMP"                      */
#define CENTER_BOT_Y       (18)    /* brightness readout          */
#define CENTER_CAP2_Y      (46)    /* "BRIGHT"                    */

#define CLOCK_TIME_Y       (-104)
#define CLOCK_DATE_Y       (104)

#define STATUS_Y_OFF       (-104)
#define STATUS_Y_ON        (-78)

#define OFF_TIME_Y         (-25)
#define OFF_DATE_Y         (35)
#define OFF_RING_DIAMETER  (236)
#define OFF_RING_WIDTH     (4)
#define OFF_RING_GAP       (8)

#define MENU_ITEM_Y0       (-24)
#define MENU_ITEM_STEP     (32)
#define MENU_ITEM_W        (176)
#define MENU_ITEM_H        (28)

typedef enum {
    UI_SCREEN_MAIN,        /* both scales visible (spec "state 1")     */
    UI_SCREEN_OFF,         /* large clock                              */
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

/* off group */
static lv_obj_t *s_grp_off;
static lv_obj_t *s_lbl_hours;
static lv_obj_t *s_lbl_colon;
static lv_obj_t *s_lbl_minutes;
static lv_obj_t *s_lbl_off_date;
static lv_obj_t *s_arc_seconds;
static bool s_colon_visible = true;

/* clock pairs */
typedef struct {
    lv_obj_t *time;
    lv_obj_t *date;
} clock_pair_t;

static clock_pair_t s_clock_main;
static clock_pair_t s_clock_menu;

/* menu group */
static lv_obj_t *s_grp_menu;
static lv_obj_t *s_lbl_menu_title;
static lv_obj_t *s_menu_item_box[4];
static lv_obj_t *s_menu_item_lbl[4];
static lv_obj_t *s_lbl_status;

static TickType_t s_status_until;
static bool s_status_transient;
static bool s_ota_check_pending;
static TickType_t s_ota_check_until;

/* Menu entries. Rotate moves the selection, a single click runs the action,
 * a double click leaves the menu. value is optional and shown next to the
 * label (used by settings that cycle through a few states). */
typedef struct {
    const char *label;
    void (*action)(void);
    const char *(*value)(void);
} ui_menu_item_t;

static void update_menu_view(void);
static void show_screen(ui_screen_t screen, bool animate);
static void status_set_pairing(void);
static void status_set_ota_check(void);

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

static void menu_action_pair(void)
{
    status_set_pairing();
    appzb_start_pairing();
}

static void menu_action_ota_check(void)
{
    status_set_ota_check();
    appzb_check_ota();
}

static void menu_action_exit(void)
{
    show_screen(light_model_is_on() ? UI_SCREEN_MAIN : UI_SCREEN_OFF, true);
}

static const ui_menu_item_t s_menu_items[] = {
    { "PAIR ZIGBEE", menu_action_pair, NULL },
    { "OTA CHECK",   menu_action_ota_check, NULL },
    { "ROTATE",      menu_action_rotation, menu_rotation_value },
    { "EXIT",        menu_action_exit, NULL },
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

static clock_pair_t make_clock_pair(lv_obj_t *parent)
{
    clock_pair_t pair;
    pair.time = make_label(parent, &lv_font_montserrat_24, lv_color_hex(UI_COLOR_TEXT),
                           CLOCK_TIME_Y, "--:--");
    pair.date = make_label(parent, &lv_font_montserrat_24, lv_color_hex(UI_COLOR_TEXT),
                           CLOCK_DATE_Y, "--.--");
    return pair;
}

static void set_clock_pair(clock_pair_t *pair, const char *time, const char *date)
{
    lv_label_set_text(pair->time, time);
    lv_label_set_text(pair->date, date);
}

static void build_main_group(lv_obj_t *scr)
{
    s_grp_main = make_layer(scr);

    s_layer_ct = make_layer(s_grp_main);
    s_arc_ct = make_arc(s_layer_ct, ARC_TOP_START, ARC_TOP_END);
    lv_obj_set_style_arc_color(s_arc_ct, lv_color_hex(UI_COLOR_CT), LV_PART_INDICATOR);
    s_lbl_ct = make_label(s_layer_ct, &lv_font_montserrat_28, lv_color_hex(UI_COLOR_TEXT),
                          CENTER_TOP_Y, "--");
    s_lbl_cap_ct = make_label(s_layer_ct, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                              CENTER_CAP_Y, "TEMP");

    s_layer_br = make_layer(s_grp_main);
    s_arc_br = make_arc(s_layer_br, ARC_BOT_START, ARC_BOT_END);
    lv_obj_set_style_arc_color(s_arc_br, lv_color_hex(UI_COLOR_BR), LV_PART_INDICATOR);
    s_lbl_br = make_label(s_layer_br, &lv_font_montserrat_28, lv_color_hex(UI_COLOR_TEXT),
                          CENTER_BOT_Y, "--");
    s_lbl_cap_br = make_label(s_layer_br, &lv_font_montserrat_14, lv_color_hex(UI_COLOR_DIM),
                              CENTER_CAP2_Y, "BRIGHT");

    s_clock_main = make_clock_pair(s_grp_main);
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

static void layout_off_time(void)
{
    if (s_lbl_hours == NULL || s_lbl_colon == NULL || s_lbl_minutes == NULL) {
        return;
    }

    const lv_font_t *font = &lv_font_montserrat_48;
    const char *hours = lv_label_get_text(s_lbl_hours);
    const char *minutes = lv_label_get_text(s_lbl_minutes);
    lv_coord_t hours_w = lv_txt_get_width(hours, (uint32_t)strlen(hours), font, 0, LV_TEXT_FLAG_NONE);
    lv_coord_t colon_w = lv_txt_get_width(":", 1, font, 0, LV_TEXT_FLAG_NONE);
    lv_coord_t minutes_w = lv_txt_get_width(minutes, (uint32_t)strlen(minutes), font, 0, LV_TEXT_FLAG_NONE);
    lv_coord_t total = hours_w + colon_w + minutes_w + 2 * OFF_RING_GAP;

    lv_coord_t hours_x = -total / 2 + hours_w / 2;
    lv_coord_t colon_x = -total / 2 + hours_w + OFF_RING_GAP + colon_w / 2;
    lv_coord_t minutes_x = -total / 2 + hours_w + OFF_RING_GAP + colon_w + OFF_RING_GAP + minutes_w / 2;

    lv_obj_align(s_lbl_hours, LV_ALIGN_CENTER, hours_x, OFF_TIME_Y);
    lv_obj_align(s_lbl_colon, LV_ALIGN_CENTER, colon_x, OFF_TIME_Y);
    lv_obj_align(s_lbl_minutes, LV_ALIGN_CENTER, minutes_x, OFF_TIME_Y);
}

static void build_off_group(lv_obj_t *scr)
{
    s_grp_off = make_layer(scr);

    s_arc_seconds = lv_arc_create(s_grp_off);
    lv_obj_set_size(s_arc_seconds, OFF_RING_DIAMETER, OFF_RING_DIAMETER);
    lv_obj_align(s_arc_seconds, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(s_arc_seconds, 0, 360);
    lv_arc_set_rotation(s_arc_seconds, 270);
    lv_arc_set_range(s_arc_seconds, 0, 1000);
    lv_obj_set_style_arc_width(s_arc_seconds, OFF_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_seconds, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_seconds, OFF_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_seconds, lv_color_hex(UI_COLOR_TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_seconds, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_seconds, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc_seconds, false, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc_seconds, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_seconds, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_arc_seconds, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_angles(s_arc_seconds, 0, 0);

    s_lbl_hours = make_label(s_grp_off, &lv_font_montserrat_48, lv_color_hex(UI_COLOR_TEXT),
                             OFF_TIME_Y, "--");
    s_lbl_colon = make_label(s_grp_off, &lv_font_montserrat_48, lv_color_hex(UI_COLOR_TEXT),
                             OFF_TIME_Y, ":");
    s_lbl_minutes = make_label(s_grp_off, &lv_font_montserrat_48, lv_color_hex(UI_COLOR_TEXT),
                               OFF_TIME_Y, "--");
    s_lbl_off_date = make_label(s_grp_off, &lv_font_montserrat_28, lv_color_hex(UI_COLOR_DIM),
                                OFF_DATE_Y, "--.--.----");
    layout_off_time();
}

static void update_menu_view(void)
{
    for (size_t i = 0; i < UI_MENU_ITEM_COUNT; i++) {
        lv_obj_t *box = s_menu_item_box[i];
        lv_obj_t *lbl = s_menu_item_lbl[i];
        if (box == NULL || lbl == NULL) {
            continue;
        }
        bool selected = (i == s_menu_index);
        lv_obj_set_style_bg_color(box, lv_color_hex(selected ? UI_COLOR_BR : UI_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(box, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(selected ? UI_COLOR_BR : UI_COLOR_DIM), 0);
        lv_obj_set_style_border_opa(box, selected ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(selected ? UI_COLOR_TEXT : UI_COLOR_DIM), 0);

        char buf[48];
        const char *value = s_menu_items[i].value ? s_menu_items[i].value() : NULL;
        if (value != NULL) {
            snprintf(buf, sizeof(buf), "%s %s", s_menu_items[i].label, value);
            lv_label_set_text(lbl, buf);
        } else {
            lv_label_set_text(lbl, s_menu_items[i].label);
        }
    }
}

static void build_menu_group(lv_obj_t *scr)
{
    s_grp_menu = make_layer(scr);
    s_lbl_menu_title = make_label(s_grp_menu, &lv_font_montserrat_16, lv_color_hex(UI_COLOR_DIM),
                                  -56, "MENU");

    for (size_t i = 0; i < UI_MENU_ITEM_COUNT; i++) {
        lv_obj_t *box = lv_obj_create(s_grp_menu);
        lv_obj_set_size(box, MENU_ITEM_W, MENU_ITEM_H);
        lv_obj_align(box, LV_ALIGN_CENTER, 0, MENU_ITEM_Y0 + (lv_coord_t)i * MENU_ITEM_STEP);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_DIM), 0);
        lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(box);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        s_menu_item_box[i] = box;
        s_menu_item_lbl[i] = lbl;
    }

    s_clock_menu = make_clock_pair(s_grp_menu);
    update_menu_view();
}

static void status_show(const char *text)
{
    lv_label_set_text(s_lbl_status, text);
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
}

static void status_hide(void)
{
    lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
}

static void status_set_transient(const char *text, TickType_t ms)
{
    s_status_transient = true;
    s_ota_check_pending = false;
    s_status_until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    status_show(text);
}

static void status_set_pairing(void)
{
    status_set_transient("PAIRING...", 2500);
}

static void status_set_ota_check(void)
{
    s_status_transient = false;
    s_ota_check_pending = true;
    s_ota_check_until = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
    status_show("OTA CHECK...");
}

static void status_reposition(ui_screen_t screen)
{
    if (s_lbl_status == NULL) {
        return;
    }
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0,
                 screen == UI_SCREEN_OFF ? STATUS_Y_OFF : STATUS_Y_ON);
}

static void status_tick_cb(lv_timer_t *t)
{
    (void)t;

    uint8_t pct;
    if (zb_ota_get_progress(&pct)) {
        char buf[24];
        snprintf(buf, sizeof(buf), "OTA %u%%", (unsigned)pct);
        status_show(buf);
        return;
    }

    zb_ota_query_status_t result;
    if (zb_ota_take_query_result(&result)) {
        s_ota_check_pending = false;
        s_status_transient = true;
        s_status_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        const char *text = "NO ANSWER";
        if (result == ZB_OTA_QUERY_AVAILABLE) {
            text = "UPDATE FOUND";
        } else if (result == ZB_OTA_QUERY_NOT_AVAILABLE) {
            text = "NO UPDATE";
        } else if (result == ZB_OTA_QUERY_NO_SERVER) {
            text = "NO OTA SERVER";
        } else if (result == ZB_OTA_QUERY_NO_NETWORK) {
            text = "NOT CONNECTED";
        }
        status_show(text);
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_ota_check_pending) {
        if ((int32_t)(s_ota_check_until - now) <= 0) {
            s_ota_check_pending = false;
            s_status_transient = true;
            s_status_until = now + pdMS_TO_TICKS(3000);
            status_show("NO ANSWER");
        }
        return;
    }

    if (s_status_transient && (int32_t)(s_status_until - now) <= 0) {
        s_status_transient = false;
        status_hide();
    }
}

static void off_seconds_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_screen != UI_SCREEN_OFF || s_arc_seconds == NULL || s_lbl_colon == NULL) {
        return;
    }

    clock_parts_t parts;
    clock_get_parts(&parts);

    int minutes;
    int seconds;
    uint32_t phase_ms;
    if (parts.minutes >= 0) {
        minutes = parts.minutes;
        seconds = parts.seconds;
        phase_ms = (uint32_t)seconds * 1000U + parts.ms;
    } else {
        uint32_t mono_ms = lv_tick_get();
        minutes = (int)((mono_ms / 60000U) % 60U);
        seconds = (int)((mono_ms / 1000U) % 60U);
        phase_ms = mono_ms % 1000U;
    }
    if (phase_ms >= 60000U) {
        phase_ms = 59999U;
    }

    uint16_t angle = (uint16_t)((phase_ms * 360UL) / 60000UL);
    if ((minutes % 2) == 0) {
        lv_arc_set_angles(s_arc_seconds, 0, angle);
    } else {
        lv_arc_set_angles(s_arc_seconds, angle, 360);
    }

    bool colon_on = ((seconds % 2) == 0);
    if (colon_on != s_colon_visible) {
        if (colon_on) {
            lv_obj_clear_flag(s_lbl_colon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_colon, LV_OBJ_FLAG_HIDDEN);
        }
        s_colon_visible = colon_on;
    }
}

/* ------------------------------------------------------------------ */
/* screen switching                                                    */
/* ------------------------------------------------------------------ */

static void update_clock_labels(void)
{
    char time_buf[24];
    char date_buf[24];
    char full_date_buf[32];
    clock_get_time_str(time_buf, sizeof(time_buf));
    clock_get_short_date_str(date_buf, sizeof(date_buf));
    if (clock_is_synced()) {
        clock_get_date_str(full_date_buf, sizeof(full_date_buf));
    } else {
        snprintf(full_date_buf, sizeof(full_date_buf), "--.--.----");
    }

    set_clock_pair(&s_clock_main, time_buf, date_buf);
    set_clock_pair(&s_clock_menu, time_buf, date_buf);

    char hours[8] = "--";
    char minutes[8] = "--";
    const char *colon = strchr(time_buf, ':');
    if (colon != NULL) {
        snprintf(hours, sizeof(hours), "%.*s", (int)(colon - time_buf), time_buf);
        snprintf(minutes, sizeof(minutes), "%s", colon + 1);
    }
    lv_label_set_text(s_lbl_hours, hours);
    lv_label_set_text(s_lbl_minutes, minutes);
    layout_off_time();
    lv_label_set_text(s_lbl_off_date, full_date_buf);
}

static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    update_clock_labels();
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
    update_clock_labels();
    status_reposition(screen);

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

    update_clock_labels();
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
    s_lbl_status = make_label(scr, &lv_font_montserrat_16, lv_color_hex(UI_COLOR_BR),
                              STATUS_Y_ON, "");
    lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    update_mode_indicator();

    /* Start fully transparent so the first transition is a fade-in. */
    obj_hide(s_grp_main, 0);
    obj_hide(s_grp_menu, 0);

    s_clock_timer = lv_timer_create(clock_tick_cb, 1000, NULL);
    lv_timer_create(status_tick_cb, 250, NULL);
    lv_timer_create(off_seconds_tick_cb, 25, NULL);

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