#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "config.h"
#include "display_init.h"
#include "storage.h"

#define LCD_HOST           SPI2_HOST
#define LCD_SPI_DMA_CH     SPI_DMA_CH_AUTO
#define LV_TASK_MS         2
/* One draw buffer stripe. Keeps the DMA allocation small enough for the
 * 512 KB of SRAM on the ESP32-C6. */
#define LCD_STRIPE_PX      (LCD_H_RES * LCD_DRAW_BUF_LINES)

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lv_lock;
static uint8_t s_rotation;

/* esp_lcd_panel_io_spi hands colour data to the DMA engine and returns while
 * the transfer is still running, so the source buffer must not be touched
 * until on_color_trans_done fires. LVGL reuses the draw buffer the moment
 * flush_ready is called, hence this flag has to be waited on.
 * Starts true: nothing is in flight before the first flush, and the selftest
 * that used to clear it is compiled out of normal firmware. */
static volatile bool s_color_done = true;

static bool color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    s_color_done = true;
    return false;
}

/* Block until the panel has consumed the previous buffer. The callback runs
 * in the SPI ISR, so this is a plain flag poll: a 100 us step tracks a
 * transfer (a 60-line stripe is ~5.8 ms at 40 MHz) without paying a FreeRTOS
 * tick, which is what vTaskDelay(1) costs - at least 1 ms per stripe. */
static void lcd_wait_idle(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms * 1000U && !s_color_done; waited += 100) {
        esp_rom_delay_us(100);
    }
}

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void)drv;
    esp_err_t err;

    /* Wait for the transfer this flush is about to replace - i.e. the
     * previous stripe - then hand our buffer to the DMA and release LVGL
     * straight away. Waiting after the transfer instead would serialise
     * rendering and DMA: LVGL could not start the next stripe until the
     * current one was already on the panel. */
    lcd_wait_idle(100);
    if (!s_color_done) {
        /* A timeout means the previous buffer is still being read, so the
         * flag would be cleared while a transfer is in flight. */
        ESP_LOGE(TAG, "flush wait timed out");
    }
    s_color_done = false;

    /* esp_lcd_panel_draw_bitmap takes exclusive end coordinates: the driver
     * writes x_end - 1 into CASET and sizes the transfer as
     * (x_end - x_start) * (y_end - y_start). LVGL areas are inclusive, so
     * passing x2/y2 straight through shifts every flush by one pixel. */
    err = esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                    area->x2 + 1, area->y2 + 1, color_p);
    if (err != ESP_OK) {
        /* A dropped flush leaves stale pixels on screen, which looks like a
         * skewed or striped image rather than an obvious failure. */
        ESP_LOGE(TAG, "flush %d,%d..%d,%d failed: %s", area->x1, area->y1,
                 area->x2, area->y2, esp_err_to_name(err));
        s_color_done = true;
    }
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
    disp_drv.sw_rotate = 1;
    lv_disp_drv_register(&disp_drv);
}

uint8_t display_get_rotation(void)
{
    return s_rotation;
}

uint8_t display_rotate_90(void)
{
    s_rotation = (uint8_t)((s_rotation + 1U) & 3U);
    storage_display_rotation_save(s_rotation);
    lv_disp_set_rotation(NULL, (lv_disp_rot_t)s_rotation);
    return s_rotation;
}

void display_lock(void)
{
    xSemaphoreTake(s_lv_lock, portMAX_DELAY);
}

void display_unlock(void)
{
    xSemaphoreGive(s_lv_lock);
}

/* TEMPORARY diagnostic: solid colour fill bypassing LVGL. Keep at 0 for
 * normal firmware — lcd_selftest() never returns. */
#define LCD_SELFTEST 0

static void lcd_selftest(void)
{
#if LCD_SELFTEST
    static lv_color_t stripe[LCD_H_RES * LCD_DRAW_BUF_LINES];
    int fails = 0;
    for (;;) {
        static int phase = 0;
        phase ^= 1;
        uint32_t rgb = phase ? 0x000000 : 0xFFFFFF;
        for (uint32_t i = 0; i < LCD_H_RES; i++) {
            stripe[i] = lv_color_hex(rgb);
        }
        for (uint32_t y = 0; y < LCD_V_RES; y += LCD_DRAW_BUF_LINES) {
            s_color_done = false;
            esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES,
                                                      y + LCD_DRAW_BUF_LINES, stripe);
            if (err != ESP_OK) {
                fails++;
            } else {
                lcd_wait_idle(200);
            }
        }
        ESP_LOGW(TAG, "selftest SOLID %s, band failures=%d",
                 phase ? "BLACK" : "WHITE", fails);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
#endif
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
        .max_transfer_sz = LCD_STRIPE_PX * sizeof(lv_color_t) + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, LCD_SPI_DMA_CH));

    ESP_LOGI(TAG, "Install panel IO");
    static esp_lcd_panel_io_handle_t io_handle;
    io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = PIN_LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        /* Needs to be > 1: esp_lcd splits a 240x15 stripe into several DMA
         * transactions and with a single slot it has to wait for each chunk
         * before queueing the next one. */
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));
    esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = color_trans_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL));

    ESP_LOGI(TAG, "Install GC9A01 panel driver");
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RES,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* Must match LV_COLOR_DEPTH; the driver rejects 0. */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_cfg, &s_panel));
    /* Log the reset line so we can tell whether the panel is really being
     * reset: the pin idles high, so a high level after reset() means the
     * GPIO is not actually wired to the panel's RES input. */
    ESP_LOGI(TAG, "RES pin before reset: level=%d", gpio_get_level(PIN_LCD_RES));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_LOGI(TAG, "RES pin after reset: level=%d", gpio_get_level(PIN_LCD_RES));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* The glass on this round module is wired mirrored: with MADCTL = 0x00
     * every glyph comes out flipped left-to-right (the dial itself is
     * symmetric, so only the text gives it away). */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
#if LCD_INVERT_COLOR
    /* Must be sent before DISPON: the component's init sequence leaves the
     * panel in INVOFF, which shows every colour as its photo-negative. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* No backlight GPIO on this board: BLK is tied on the module. */

    /* NOTE: do not "read back" MADCTL/COLMOD with esp_lcd_panel_io_rx_param().
     * It sends the command byte and then clocks the requested number of bits
     * with D/C high, which the GC9A01 latches as the *parameter* of that
     * command. So a 1-byte read of 0x3A actually overwrites COLMOD and a read
     * of 0x36 overwrites MADCTL: the panel switches away from the 16-bit
     * pixel format and/or flips to swapped axes, which shows up on screen as
     * noise or as blocks in the wrong order. The module has no MISO wired
     * anyway, so the returned value is always 0xFF and proves nothing. */

    lcd_selftest();

    lvgl_init();
    s_rotation = storage_display_rotation_load();
    lv_disp_set_rotation(NULL, (lv_disp_rot_t)s_rotation);
    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "Display ready (%dx%d, %d MHz)", LCD_H_RES, LCD_V_RES,
             PIN_LCD_SPI_CLK_HZ / 1000000);
}