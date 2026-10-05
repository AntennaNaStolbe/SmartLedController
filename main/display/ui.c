#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

#include "config.h"
#include "display/display_init.h"
#include "display/ui.h"
#include "light_model.h"

#define UI_COLOR_BG       0x111111
#define UI_COLOR_ARC_BG   0x3a3a3a
#define UI_COLOR_BR       0x42a5f5
#define UI_COLOR_BR_OFF   0x546e7a
#define UI_COLOR_CT       0xffa726
#define UI_COLOR_TEXT     0xeeeeee
#define UI_COLOR_DIM      0x8899aa
#define UI_COLOR_ON       0x66bb6a
#define UI_COLOR_OFF      0x9e9e9e

static const char *TAG = "ui";

static ui_get_state_fn_t s_get_state;

static lv_obj_t *s_arc_br;
static lv_obj_t *s_arc_ct;
static lv_obj_t *s_lbl_br;
static lv_obj_t *s_lbl_ct;
static lv_obj_t *s_lbl_power;
static lv_obj_t *s_lbl_status;

static char s_buf[32];

static lv_obj_t *make_arc(lv_obj_t *parent, int16_t start, int16_t end, lv_color_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, 200, 200);
    lv_arc_set_bg_angles(arc, start, end);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_range(arc, 0, 100);

    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_ARC_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    return arc;
}

static void ui_refresh(void)
{
    light_state_t st;
    s_get_state(&st);

    if (st.on_off) {
        lv_arc_set_value(s_arc_br, st.brightness);
        lv_obj_set_style_arc_color(s_arc_br, lv_color_hex(UI_COLOR_BR), LV_PART_INDICATOR);
        lv_label_set_text(s_lbl_power, "ON");
        lv_obj_set_style_text_color(s_lbl_power, lv_color_hex(UI_COLOR_ON), 0);
    } else {
        lv_arc_set_value(s_arc_br, 0);
        lv_obj_set_style_arc_color(s_arc_br, lv_color_hex(UI_COLOR_BR_OFF), LV_PART_INDICATOR);
        lv_label_set_text(s_lbl_power, "OFF");
        lv_obj_set_style_text_color(s_lbl_power, lv_color_hex(UI_COLOR_OFF), 0);
    }

    lv_arc_set_value(s_arc_ct, (st.color_temp_mireds - LM_CT_MIN_MIREDS) * 100
                                / (LM_CT_MAX_MIREDS - LM_CT_MIN_MIREDS));

    snprintf(s_buf, sizeof(s_buf), "%u%%", (unsigned)((uint32_t)st.brightness * 100 / LM_LEVEL_MAX));
    lv_label_set_text(s_lbl_br, s_buf);

    unsigned kelvin = st.color_temp_mireds ? 1000000u / st.color_temp_mireds : 0;
    snprintf(s_buf, sizeof(s_buf), "%uK", kelvin);
    lv_label_set_text(s_lbl_ct, s_buf);

    snprintf(s_buf, sizeof(s_buf), "%u %u%% %uK",
             st.on_off ? 1 : 0,
             (unsigned)((uint32_t)st.brightness * 100 / LM_LEVEL_MAX),
             kelvin);
    lv_label_set_text(s_lbl_status, s_buf);
}

void ui_init(ui_get_state_fn_t get_state)
{
    s_get_state = get_state;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_scr_load(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SLCTR C6");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* CCT arc: top half of the ring */
    s_arc_ct = make_arc(scr, 90, 270, lv_color_hex(UI_COLOR_CT));
    lv_obj_align(s_arc_ct, LV_ALIGN_CENTER, 0, -30);

    s_lbl_ct = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_ct, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_lbl_ct, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_ct, LV_ALIGN_CENTER, 0, -30);

    /* Brightness arc: bottom half of the ring */
    s_arc_br = make_arc(scr, 90, 270, lv_color_hex(UI_COLOR_BR));
    lv_arc_set_range(s_arc_br, 0, LM_LEVEL_MAX);
    lv_arc_set_bg_angles(s_arc_br, 90, 450);   /* 270..450 == bottom half */
    lv_obj_align(s_arc_br, LV_ALIGN_CENTER, 0, 30);

    s_lbl_br = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_br, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_lbl_br, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_br, LV_ALIGN_CENTER, 0, 30);

    /* Power / status */
    s_lbl_power = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_power, &lv_font_montserrat_16, 0);
    lv_obj_align(s_lbl_power, LV_ALIGN_CENTER, 0, 0);

    s_lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_BOTTOM_MID, 0, -18);

    ui_request_refresh();
    ESP_LOGI(TAG, "UI ready");
}

void ui_request_refresh(void)
{
    display_lock();
    ui_refresh();
    display_unlock();
}