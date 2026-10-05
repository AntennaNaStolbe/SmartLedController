#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "config.h"
#include "display_init.h"

#define LCD_HOST           SPI2_HOST
#define LCD_SPI_DMA_CH     SPI_DMA_CH_AUTO
#define LV_TASK_MS         5
/* One draw buffer stripe. Keeps the DMA allocation small enough for the
 * 512 KB of SRAM on the ESP32-C6. */
#define LCD_STRIPE_PX      (LCD_H_RES * LCD_DRAW_BUF_LINES)

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lv_lock;

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)drv;
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2, area->y2, color_p);
    lv_disp_flush_ready(drv);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    uint32_t last_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t elapsed = now_ms - last_ms;
        last_ms = now_ms;
        lv_tick_inc(elapsed);

        display_lock();
        lv_timer_handler();
        display_unlock();

        vTaskDelay(pdMS_TO_TICKS(LV_TASK_MS));
    }
}

static void lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[LCD_STRIPE_PX];
    static lv_color_t buf2[LCD_STRIPE_PX];
    /* The last argument is the buffer length in pixels, not in rows. */
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_STRIPE_PX);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}

void display_lock(void)
{
    xSemaphoreTake(s_lv_lock, portMAX_DELAY);
}

void display_unlock(void)
{
    xSemaphoreGive(s_lv_lock);
}

void display_init(void)
{
    s_lv_lock = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_STRIPE_PX * sizeof(lv_color_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, LCD_SPI_DMA_CH));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = PIN_LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    ESP_LOGI(TAG, "Install GC9A01 panel driver");
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RES,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
#if LCD_INVERT_COLOR
    /* Flip to 1 if the panel shows inverted colours. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
#endif

    /* No backlight GPIO on this board: BLK is tied on the module. */

    lvgl_init();
    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "Display ready (%dx%d, %d MHz)", LCD_H_RES, LCD_V_RES,
             PIN_LCD_SPI_CLK_HZ / 1000000);
}