#ifndef CONFIG_H
#define CONFIG_H

/* ------------------------------------------------------------------ */
/* Product identity (PLATFORM.md §2)                                  */
/* ------------------------------------------------------------------ */
#define PRODUCT_NAME         "SmartLedController"
#define MANUFACTURER         "AntennaNS"
#define MODEL_IDENTIFIER     "SLCTR-C6-CWW"
#define FW_VERSION           "0.1.9"
#define FW_DATE_CODE         "20261124"

/* ------------------------------------------------------------------ */
/* Zigbee OTA upgrade                                                  */
/* ------------------------------------------------------------------ */
/* Keep these in sync with tools/make_ota.py and zigbee2mqtt/ota/index.json.
 * Bump OTA_FILE_VERSION for every release so z2m sees a newer image. */
#define OTA_QUERY_INTERVAL_MS       (6UL * 60UL * 60UL * 1000UL)
#define OTA_FILE_VERSION            0x00010009
#define OTA_MANUFACTURER            0x1A1A
#define OTA_IMAGE_TYPE              0x0002
#define OTA_HARDWARE_VERSION        0x0001
#define OTA_MAX_DATA_SIZE           223
#define OTA_QUERY_INTERVAL_MIN      (OTA_QUERY_INTERVAL_MS / 60000UL)

/* ------------------------------------------------------------------ */
/* I/O pins (ESP32-C6 SuperMini)                                       */
/* ------------------------------------------------------------------ */
/* GC9A01 circular display 240x240 over SPI2.
 * The backlight is not wired to a GPIO on this board, so there is no
 * PIN_LCD_BLK and no backlight control in the driver.
 * NOTE: GPIO0 is not a strapping pin on the ESP32-C6 (download mode is
 * selected by GPIO9), so it is safe to drive LCD RES from it.
 */
#define PIN_LCD_SCLK           6       /* SCL / CLK, hardware SPI      */
#define PIN_LCD_MOSI           7       /* SDA / MOSI, hardware SPI     */
#define PIN_LCD_RES            0       /* RST, output                  */
#define PIN_LCD_DC             1       /* A0 / DC, output              */
#define PIN_LCD_CS             3       /* output                       */

/* EC11 rotary encoder with push button.
 * GPIO4/GPIO5 are the MTMS/MTDI strapping pins; they default to floating
 * with a weak pull-up, which matches the internal pull-ups used here, and
 * they do not take part in boot mode selection on the ESP32-C6.
 */
#define PIN_ENC_CLK            4       /* S1, INPUT_PULLUP, PCNT edge  */
#define PIN_ENC_DT             5       /* S2, INPUT_PULLUP, PCNT level */
#define PIN_ENC_KEY            2       /* SW, INPUT_PULLUP, active low */

/* Dual-colour LED strip driven through IRLB8721 MOSFETs */
#define PIN_LED_WARM           18      /* PWM (LEDC)                   */
#define PIN_LED_COOL           19      /* PWM (LEDC)                   */

/* SPI clock for the display, Hz */
#define PIN_LCD_SPI_CLK_HZ     40000000

/* ------------------------------------------------------------------ */
/* Encoder / gesture timing                                            */
/* ------------------------------------------------------------------ */
/* The PCNT channel counts both A edges and inverts them by B, so one EC11
 * detent produces two raw counts. Divide by that here to emit one logical
 * step per physical click, both in the menu and when adjusting values.
 */
#define ENC_COUNTS_PER_DETENT  2

/* Rotation speed tiers, in detents per second (smoothed over a few
 * samples). Each tier below multiplies the value change per detent, so a
 * slow turn stays precise while a fast turn sweeps the range. Between
 * ENC_RATE_T4 and the pause threshold the multiplier is ENC_ACCEL_MAX.
 */
#define ENC_RATE_T1            8
#define ENC_RATE_T2            20
#define ENC_RATE_T3            45
#define ENC_RATE_T4            90
#define ENC_ACCEL_MAX          16      /* multiplier at ENC_RATE_T4+      */
#define ENC_RATE_RESET_MS      300     /* pause that drops the rate to 0  */

#define BTN_DEBOUNCE_MS          15      /* contact debounce                */
#define BTN_MODE_HOLD_MS         500     /* held this long = toggle mode   */
#define BTN_DOUBLE_CLICK_WINDOW_MS 350    /* two clicks within this = dblclk */
#define BTN_RESET_HOLD_MS        5000    /* held this long = factory reset  */

/* ------------------------------------------------------------------ */
/* Light model                                                         */
/* ------------------------------------------------------------------ */
#define LM_LEVEL_MAX           254     /* ZCL: 0..254                   */
#define LM_LEVEL_MIN           1
/* Value change per detent at accel 1, in percent of the full range. One
 * constant drives both brightness and CCT so a detent always moves the
 * displayed value by the same fraction, whatever is being adjusted. */
#define LM_STEP_PCT            1

/* Colour temperature range. The model stores kelvin; mireds only appear at
 * the ZCL / HA / NVS boundary, where LM_CT_MIN_MIREDS is the cold end and
 * LM_CT_MAX_MIREDS the warm one. */
#define LM_CT_MIN_K            3000    /* warm                          */
#define LM_CT_MAX_K            6500    /* cold                          */
#define LM_CT_MIN_MIREDS       154     /* 6500 K, cold                 */
#define LM_CT_MAX_MIREDS       333     /* 3000 K, warm                 */

#define LM_DEFAULT_TRANSITION_MS   200 /* strip fade on power on/off    */
#define LM_LEVEL_TRANSITION_MS     120 /* strip fade on level change    */
#define LM_IDENTIFY_BLINK_MS      500  /* identify blink half period   */
#define LM_SAVE_DELAY_MS          500  /* debounce for NVS writes      */
#define LM_REPORT_DELAY_MS        400  /* debounce for Zigbee reports  */

/* ------------------------------------------------------------------ */
/* Display / UI                                                        */
/* ------------------------------------------------------------------ */
#define LCD_H_RES              240
#define LCD_V_RES              240
/* One draw buffer stripe, in rows. 60 rows is 28800 bytes, just under the
 * 32768 byte DMA limit of the ESP32-C6, so a stripe goes out in a single
 * transaction and a full frame costs 4 flushes instead of 16. */
#define LCD_DRAW_BUF_LINES     60

/* The GC9A01 init sequence in the IDF component never sends INVON, so the
 * panel boots in INVOFF and shows every colour as its photo-negative: a
 * black background comes out white and light text comes out black. INVON is
 * required for this IPS glass (the component's own test app enables it). */
#define LCD_INVERT_COLOR       1

/* Duration of every UI animation, ms */
#define UI_ANIM_FAST_MS      60      /* value / digit changes         */
#define UI_ANIM_SCREEN_MS    200     /* screen and element fades     */

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */
/* Time is taken from the Zigbee Time cluster (see time/clock.c) and is
 * delivered as UTC. Z2M does not always publish a TimeZone, so a fixed
 * offset is applied as a fallback.
 * TODO: expose this in the settings menu once the menu exists.
 */
#define CLOCK_DEFAULT_TZ_OFFSET_S   (3 * 3600)   /* UTC+3 */
#define CLOCK_RESYNC_PERIOD_MS      (15 * 60 * 1000)
#define CLOCK_LEAP_SECONDS          37   /* ZCL time excludes leap seconds */

#endif /* CONFIG_H */