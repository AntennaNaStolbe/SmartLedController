# SmartLedController — CCT LED controller (ESP32-C6)

A round-display smart controller for a warm/cool white (CCT) LED strip. Built on
the ESP32-C6 SuperMini with PlatformIO + ESP-IDF 5.5 + LVGL 8.4 + the official
`esp-zigbee-lib` v1.6.

The device joins zigbee2mqtt as a **mesh router** and exposes a standard HA
*dimmable color-temperature light*:

- `state` (On/Off)
- `brightness` (0..254)
- `color_temp` in mireds (154..333 ↔ 6500..3000 K)

## Hardware

| Function      | GPIO | Notes                     |
|---------------|------|---------------------------|
| Display SCK   | 6    | SPI2, GC9A01, 240×240      |
| Display MOSI  | 7    |                            |
| Display CS    | 3    |                            |
| Display DC    | 1    |                            |
| Display RES   | 0    |                            |
| Encoder CLK   | 4    | EC11 quadrature            |
| Encoder DT    | 5    |                            |
| Encoder KEY   | 2    | input, pull-up, active low  |
| LED warm      | 18   | PWM (LEDC, 11-bit)         |
| LED cool      | 19   | PWM (LEDC, 11-bit)         |

There is no backlight control pin: the display backlight is hard-wired on.

Warm/cool channels are mixed from the requested CCT
(`warm = (mireds-min)/(max-min)`), gamma-corrected (1.5) and driven over a
smooth fade (default transition 200 ms).

## Controls

- **Rotate** — adjust the active parameter (brightness by default)
- **Click** (< 500 ms) — power on/off
- **Double click** — open / close the settings menu
- **Menu → ROTATE** — cycle the display orientation: 0° → 90° → 180° → 270°
- **Hold** (≥ 0.5 s) — toggle the active parameter: brightness ↔ CCT
- **Hold 5 s** — factory reset (settings + Zigbee network)
- Zigbee **identify** — blinks the strip for the requested time

The active parameter is highlighted on the dial (caption, value and arc of the
selected side at full opacity, the other dimmed). Turning or adjusting while
off switches the light on first, then applies the change, matching
`MoveToLevelWithOnOff` semantics.

## Build & flash

Built with PlatformIO, which supplies ESP-IDF 5.5 itself — no separate IDF
install is needed. The `espressif/esp-zigbee-lib` dependency pins the classic
`esp_zb_*` v1.6 API.

```sh
pio run -e smartled-c6
pio run -e smartled-c6 -t upload
pio device monitor
```

LVGL is configured through Kconfig rather than a hand-written `lv_conf.h`: the
`lvgl__lvgl` port sets `CONFIG_LV_CONF_SKIP=y` by default. Font sizes, colour
depth and the memory pool live in `sdkconfig.defaults`.

For a fresh checkout, delete `sdkconfig.smartled-c6` first so the defaults are
re-applied.

### Partition table

`partitions.csv` reserves an NVS partition plus Zigbee stack storage/frame
partitions (as required by the esp-zigbee radio) and a 2 MB factory app slot
(flash is 4 MB).

## Zigbee pairing

1. Put the coordinator (zigbee2mqtt) into permit-join mode.
2. Power the controller on. In factory state it starts network steering
   automatically (all 16 channels scanned).
3. The device appears in zigbee2mqtt as `AntennaNS SLCTR-C6-CWW`.
4. After a power reboot it rejoins the stored network without re-pairing.

State changes on the physical controller are reported to the coordinator
(immediately for On/Off, debounced by 400 ms while rotating); hub commands are
applied through the standard set-attr path and are not echoed back onto the
mesh. Writes arriving from the network are tagged `REMOTE`, local encoder
changes `LOCAL`, and only `LOCAL` changes are reported.

The device also acts as a **Time cluster client** and shows the clock on the
off screen. It reads UTC from the coordinator on join and every 15 min. The
timezone offset is the compiled-in fallback of UTC+3 (`CLOCK_DEFAULT_TZ_OFFSET_S`
in `config.h`); a coordinator-supplied timezone, if present, takes precedence.
Until the menu exists, the offset cannot be changed on the device.

### external converter

Install `zigbee2mqtt/smart-led-controller.js` via the `external_converters`
setting, or let zigbee2mqtt pair it as a generic dimmable light and set the
color_temp range to 154–333 in its device configuration.

## Persistence

On/off, brightness, CCT and transition time are saved to NVS (debounced 500 ms,
FNV-1a checksum + version guard). `StartUpOnOff` defaults to "previous state".

The display orientation is stored separately in NVS (key `rot` in the same
`appcfg` namespace) so the settings blob layout stays unchanged. It is applied
during `display_init()` before the first frame, and a factory reset clears it
back to 0°.

## UI

One dial split into two halves on the 240×240 GC9A01:

- top arc — colour temperature, value in K
- bottom arc — brightness, value in %
- the side matching the active adjust mode is highlighted, the other is dimmed

Screens:

- **main** — both scales, both readouts, active-mode highlight
- **off** — `OFF` plus the current time and date
- **menu** — entered / left with a double click; rotate moves the selection
  and a click runs the item. Current items: `PAIR ZIGBEE` and `ROTATE`
  (cycles the display orientation 0/90/180/270 and saves it).

Dial values and screen changes are animated (60–200 ms).

## Notes / limitations

- Zigbee role is **router** (`ESP_ZB_DEVICE_TYPE_ROUTER`, `max_children = 10`).
  In a small network you may prefer `esp_zb_get...` network steering — this
  implementation follows the esp-zigbee reference (`ESP_ZB_BDB_MODE_NETWORK_STEERING`).
- A brightness level write with `level > 0` while the light is off turns it on
  (`MoveToLevelWithOnOff` semantics); a write of `0` turns it off.
- On `LEAVE` (device removed by coordinator) the device factory-resets and
  reboots to allow clean re-pairing.
- No OTA / MQTT / Home Assistant auto-discovery is implemented (out of scope).
- The timezone is fixed at UTC+3 until the menu provides a way to change it.
- The display can be mounted in any of four orientations; use the `ROTATE`
  menu item. The orientation is a panel MADCTL change only, so the 240×240
  resolution and draw buffers are unaffected.