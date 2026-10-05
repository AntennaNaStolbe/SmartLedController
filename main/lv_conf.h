#ifndef LV_CONF_H
#define LV_CONF_H

/* Minimal LVGL 8.3 configuration. Everything not listed here falls back
 * to defaults provided by lv_conf_internal.h.
 * Deliberately kept small and explicit so that the built-in Kconfig of the
 * esp-idf lvgl port cannot silently disable the widgets we use.
 */

/* Color depth: GC9A01 native RGB565 */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Memory */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)

/* Wall-clock ticking is driven by the application (lv_tick_inc). */
#define LV_TICK_CUSTOM 0

/* Logging via lv_log is not needed */
#define LV_USE_LOG 0

/* Widgets used by the UI */
#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1

/* Fonts */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1

/* Theme */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1

/* Asserts/metrics off */
#define LV_USE_ASSERT_NULL 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif /* LV_CONF_H */