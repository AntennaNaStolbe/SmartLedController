# SmartLedController — CCT LED controller (ESP32-C6)

A round-display smart controller for a warm/cool white (CCT) LED strip. Built on
the ESP32-C6 SuperMini with ESP-IDF + LVGL + the official `esp-zigbee-lib` v1.6.

The device joins zigbee2mqtt as a **mesh router** and exposes a standard HA
*dimmable color-temperature light*:

- `state` (On/Off)
- `brightness` (0..254)
- `color_temp` in mireds (154..333 ↔ 6500..3000 K)

## Hardware

| Function      | GPIO | Notes                     |
|---------------|------|---------------------------|
| Display SCK   | 6    | SPI2, GC9A01, 240×240     |
| Display MOSI  | 7    |                           |
| Display CS    | 2    |                           |
| Display DC    | 3    |                           |
| Display RES   | 1    |                           |
| Display BLK   | 0    | backlight, 1 = on         |
| Encoder CLK   | 18   | EC11 quadrature           |
| Encoder DT    | 19   |                           |
| Encoder KEY   | 20   | input, pull-up, active low |
| LED warm      | 4    | PWM (LEDC, 11-bit)        |
| LED cool      | 5    | PWM (LEDC, 11-bit)        |

Warm/cool channels are mixed from the requested CCT
(`warm = (mireds-min)/(max-min)`), gamma-corrected (1.5) and driven over a
smooth fade (default transition 200 ms).

## Controls

- **Click** — power on/off
- **Rotate** — brightness (step 8/254)
- **Hold + rotate** — colour temperature (mireds 154..333)
- **Hold 5 s** — factory reset (settings + Zigbee network)
- Zigbee **identify** — blinks the strip for the requested time

## Build & flash

Requires ESP-IDF ≥ 5.0 (with Zigbee support — e.g. `install.sh esp32c6`), plus the
component manager (bundled with IDF). The `espressif/esp-zigbee-lib` dependency
pins the classic `esp_zb_*` v1.6 API.

```sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

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

State changes on the physical controller are **reported** to the coordinator
immediately; hub commands are applied through the standard set-attr path.

### external converter

Install `zigbee2mqtt/smart-led-controller.js` via the `external_converters`
setting, or let zigbee2mqtt pair it as a generic dimmable light and set the
color_temp range to 154–333 in its device configuration.

## Persistence

On/off, brightness, CCT and transition time are saved to NVS (debounced 500 ms,
FNV-1a checksum + version guard). `StartUpOnOff` defaults to "previous state".

## UI

Two circular LVGL arc scales on the 240×240 GC9A01:

- top arc — colour temperature (K)
- bottom arc — brightness (%)
- center — ON/OFF indicator
- bottom line — compact state string `0 45% 3000K`

## Notes / limitations

- Zigbee role is **router** (`ESP_ZB_DEVICE_TYPE_ROUTER`, `max_children = 10`).
  In a small network you may prefer `esp_zb_get...` network steering — this
  implementation follows the esp-zigbee reference (`ESP_ZB_BDB_MODE_NETWORK_STEERING`).
- A brightness level write with `level > 0` while the light is off turns it on
  (`MoveToLevelWithOnOff` semantics); a write of `0` turns it off.
- On `LEAVE` (device removed by coordinator) the device factory-resets and
  reboots to allow clean re-pairing.
- No OTA / MQTT / Home Assistant auto-discovery is implemented (out of scope).