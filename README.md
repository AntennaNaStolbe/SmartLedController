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

- **Rotate** — adjust the active parameter (brightness by default); one
  physical detent is one step
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

`partitions.csv` reserves an NVS partition, an OTA data partition, two 1.25 MB
OTA app slots and Zigbee stack storage/frame partitions (as required by the
esp-zigbee radio) on 4 MB flash. There is no `factory` slot: the bootloader
boots `ota_0`, and OTA writes the next image into `ota_1` (then alternates).

When moving an existing device from the old factory-only layout, erase flash
(or at least `otadata` at `0x10000`) and flash the initial OTA image at
`0x20000`. The Zigbee partitions moved, so the device must be paired again.

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
There is no timezone setting in the menu yet.

### external converter

Copy `zigbee2mqtt/smart-led-controller.mjs` to the Zigbee2MQTT add-on data
directory `external_converters/` (on HAOS:
`/addon_configs/45df7312_zigbee2mqtt/external_converters/`), then restart the
add-on. Z2M 2.11+ also requires `advanced.enable_external_js: true`. The
converter sets `ota: true`, which enables the update entity in Z2M/Home
Assistant.

### OTA setup in zigbee2mqtt

For a fresh installation, do this once:

1. In the Zigbee2MQTT add-on, open **Configuration** and add this to
   `configuration.yaml` (on HAOS the file is
   `/addon_configs/45df7312_zigbee2mqtt/configuration.yaml`):

   ```yaml
   ota:
     zigbee_ota_override_index_location: https://github.com/AntennaNaStolbe/SmartLedController/releases/latest/download/index.json
   ```

2. Save and restart only the Zigbee2MQTT add-on. Home Assistant does not need
   a restart.
3. Pair the lamp. Open the device in Z2M → **OTA** → **Check for updates**.
   If a newer version is found, press **Update**. The same works from the
   Home Assistant `update` entity.
4. Updates started from the HA entity are slow: Z2M defaults to 250 ms between
   blocks and 50-byte chunks, so a 1.2 MB image can take over an hour. Use
   `tools/fast_ota.py` below to send 100-byte chunks with a 1 ms delay.
5. After the device reboots, Z2M re-interviews it automatically. If the HA
   version still looks stale, use Z2M → **Re-interview**.

`Check for updates` only asks whether an image is available; it does not
download anything. The download starts only from **Update**, the HA `update`
entity, or `tools/fast_ota.py`. The device menu has no OTA item because the
device cannot start the coordinator-side transfer by itself.

OTA settings in `config.h`: `OTA_FILE_VERSION` (bump for every release),
`OTA_MANUFACTURER=0x1A1A`, `OTA_IMAGE_TYPE=0x0002`, `OTA_HARDWARE_VERSION=0x0001`
and `OTA_MAX_DATA_SIZE=223`. They must match `zigbee2mqtt/ota/index.json`.
Z2M itself caps chunks at 100 bytes, which is fine for this firmware.

Building and publishing a release:

```bash
pio run -e smartled-c6
python3 tools/make_ota.py --base-url https://github.com/AntennaNaStolbe/SmartLedController/releases/latest/download

git tag v1.0.1
git push origin v1.0.1
gh release create v1.0.1 \
  zigbee2mqtt/ota/AntennaNS-SmartLedController.zigbee \
  --title "v1.0.1" \
  --notes "Zigbee OTA firmware 0x01000001, manufacturer AntennaNS"
gh release upload v1.0.1 zigbee2mqtt/ota/index.json
```

Upload the `.zigbee` file first, then `index.json`. The index must be published
in a non-draft release, and `fileVersion` must be higher than the version
running on the device. After OTA, use `Re-interview` in Z2M to refresh metadata.

For faster one-off updates, `tools/fast_ota.py` publishes the Z2M bridge request
with 100-byte blocks and a 1 ms delay (set `HA_URL` and `HA_TOKEN` first). It
targets the default Z2M friendly name; pass `--device` if yours differs:

```bash
python3 tools/fast_ota.py --version v1.0.1 --yes
python3 tools/fast_ota.py --version v1.0.1 --device 0x1234567890abcdef --dry-run
```

The manufacturer/image/hardware values in `config.h` and `index.json` must
match; the image type is deliberately different from SmartDoorBell's `0x0001`.

## Persistence

On/off, brightness, CCT and transition time are saved to NVS (debounced 500 ms,
FNV-1a checksum + version guard). `StartUpOnOff` defaults to "previous state".

Display orientation is stored in a separate NVS key (`rot` in the same `appcfg`
namespace) so it survives reboot/OTA without changing the settings blob layout.
It is applied during `display_init()` before the first frame, and factory reset
clears it back to 0°.

## UI

One dial split into two halves on the 240×240 GC9A01:

- top arc — colour temperature, value in K
- bottom arc — brightness, value in %
- the side matching the active adjust mode is highlighted, the other is dimmed

Screens:

- **main** — both scales, both readouts, active-mode highlight
- **off** — large clock, full date and a minute arc around the bezel
- **menu** — entered / left with a double click; the title shows the firmware
  version. Rotate to choose `PAIR ZIGBEE`, `ROTATE` or `EXIT`, click to run,
  hold 5 s for a factory reset. `ROTATE` cycles the display through 0°, 90°,
  180° and 270° and shows the current angle next to the item.

Dial values and screen changes are animated (60–200 ms).

## Notes / limitations

- Zigbee role is **router** (`ESP_ZB_DEVICE_TYPE_ROUTER`, `max_children = 10`).
  In a small network you may prefer `esp_zb_get...` network steering — this
  implementation follows the esp-zigbee reference (`ESP_ZB_BDB_MODE_NETWORK_STEERING`).
- A brightness level write with `level > 0` while the light is off turns it on
  (`MoveToLevelWithOnOff` semantics); a write of `0` turns it off.
- On `LEAVE` (device removed by coordinator) the device factory-resets and
  reboots to allow clean re-pairing.
- Zigbee OTA is implemented; MQTT and Home Assistant auto-discovery are not.
- The timezone is fixed at UTC+3 until the menu provides a way to change it.
- The display can be mounted in any of four orientations; use the `ROTATE`
  menu item. The orientation is a panel MADCTL change only, so the 240×240
  resolution and draw buffers are unaffected.
- OTA requires an explicit Z2M/HA update command; the device's periodic query
  only checks for availability.
