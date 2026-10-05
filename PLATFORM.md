# SmartSpray Platform — Device Specification

**Version associated:** v3.0.8 · **Reference implementation:** `AntennaNaStolbe/SmartSpray` (ESP8266, Arduino, PlatformIO)
**Audience:** anyone (human or agent) building a new device on this platform without prior context.

This document is the single source of truth for how a "platform device" must behave: how it boots, connects, is exposed over MQTT and Home Assistant, what its web interface looks and acts like, how it updates itself, and how it survives a broken network. It is framework-agnostic by design: the canonical implementation is Arduino/ESP8266 (SmartSpray), but every rule below also applies when porting to another framework (e.g. ESP-IDF/C on ESP32).

Normative keywords: **MUST** (hard requirement), **SHOULD** (strong recommendation, deviation needs a reason), **MAY** (optional).

The canonical SmartSpray source is the living reference for all tokens, timings and payloads quoted here. When in doubt — read the actual files listed in the [Reference implementation](#reference-implementation) section.

---

## 1. Architecture overview

A device runs in exactly one of three modes:

```
                settingsLoad() ok?         WiFi connected?
   ┌────────┐  ──────────────────► ┌───────────────┐  ─ (yes) ─► ┌──────────┐
   │ setup() │                     │ AP fallback   │             │ MODE_STA │ working: web + MQTT
   └────────┘                     │ (MODE_AP_STA) │             └──────────┘
        │  no settings                │  no WiFi           ▲   │  periodic check
        ▼                             ▼                     │
   ┌────────┐              scan network every WIFI_RETRY_MS │   once/day: updaterCheck()
   │MODE_SETUP│ ── reconnect────────────── trivial ────────┘
   └────────┘       network appears
```

| Mode | When | WiFi | Services |
|------|------|------|----------|
| `MODE_SETUP` | no valid settings | AP `SmartSpray Setup` | captive-portal config page (`http://192.168.4.1`) |
| `MODE_AP_STA` | settings exist, WiFi not connected | AP + periodic scan | config page (fix credentials) |
| `MODE_STA` | settings exist, WiFi connected | STA | full control page + MQTT + OTA |

Key structural rules:

- **The web interface MUST always answer**, in every mode — this is the top design constraint. Anything that could block the main loop (DNS, TCP connect, MQTT handshake, TLS read) MUST be bounded by a timeout and spaced out. See [Resilience](#9-resilience-and-network-behavior).
- **`setup()` boots into the fastest sensible mode**: try STA; if it fails and settings exist — AP fallback; if there are no settings — pure setup AP.
- **A pending OTA install MUST run at boot, before the web server and MQTT start** (clean heap), and the pending flag MUST be cleared before the install begins so a failed install cannot reboot-loop the device.

---

## 2. Device identity and naming

### 2.1 Technical ID (`device_id`)

- The technical ID is **derived from the WiFi MAC address**, lowercased, all colons removed: `WiFi.macAddress()` → `"84:F3:EB:2A:1C:00"` → `"84f3eb2a1c00"` (12 hex chars).
- It is **stable in hardware**: never changes across OTA, USB reflash, or a settings reset. This is what makes MQTT topics and HA `unique_id`s permanent.
- It is the **MQTT `client_id`** and the basis for every topic and HA entity (`<PRODUCT>_<device_id>_<role>`).
- MUST NOT be user-editable, MUST NOT come from a counter or RNG.

### 2.2 Friendly name

- A user-editable display name stored in settings (`name` in EEPROM). Used ONLY for display: HA device name and web UI title.
- Defaults to `DEVICE_NAME_DEFAULT` (`<ProductName>`). Seeded at load when empty.

### 2.3 Product parameters (per-device defines)

A product is identified by a small set of compile-time defines (see `include/config.h`). Everything else is derived from them — no hardcoded product strings anywhere else:

| Define | Meaning | SmartSpray value |
|--------|---------|------------------|
| `PRODUCT_NAME` | display/model name | `SmartSpray` |
| `PRODUCT_MQTT_ROOT` | MQTT topic root | `antennans/SmartSpray` |
| `FW_VERSION` | local firmware version | `3.0.8` |
| `FW_VERSION_PREFIX` | release tag prefix | `v` |
| `DEVICE_NAME_DEFAULT` | default friendly name | `SmartSpray` |
| `FW_REPO_OWNER` / `FW_REPO_NAME` / `FW_ASSET_NAME` / `FW_GITHUB_API` | GitHub update source | see [§8](#8-ota-and-updates) |

All MQTT topics, HA `unique_id`s/`identifiers`, `model`, page titles, and update sources MUST be built from these defines.

---

## 3. Settings and persistence (EEPROM)

### 3.1 Storage

- Settings live in EEPROM (emulated on ESP8266, 1 KiB reservation, offset 0).
- A **magic number validates the layout**; whenever the struct changes, bump the magic and provide a migration step (see below).
- Text fields are fixed-size `char[]` arrays with `*_MAX` capacities (including trailing `\0`), so a direct struct copy can be stored/loaded with no serialization.

### 3.2 Field set

| Field | Notes |
|-------|-------|
| `wifi_ssid` / `wifi_pass` | network credentials; empty SSID ⇒ config is invalid |
| `mqtt_host` / `mqtt_port` (1883) / `mqtt_user` / `mqtt_pass` | broker; empty host ⇒ config is invalid |
| `web_user` / `web_pass` / `web_auth_enabled` | HTTP Basic credentials for the STA web interface |
| `name` | friendly name (see §2.2) |
| `update_url` | **plain-HTTP** override for firmware download (empty = GitHub-direct); kept for local installs |
| `update_pending` | 0/1; set by "Update" request, consumed at next boot |
| `update_asset_url` | cached GitHub asset URL of the pending update (so boot-install needs no re-check) |
| *device extension* | per-device value, e.g. SmartSpray's `motor_power` (0–255, default 188, web-tunable) |

### 3.3 Validation and normalization

On load, settings MUST be clamped/sanitized:
- port 0 → 1883; power out of `[MIN,MAX]` → default; booleans → 0/1;
- all text fields forcibly null-terminated;
- if web auth is enabled but username missing or password < 4 chars → disable auth;
- empty `name` → seeded with `DEVICE_NAME_DEFAULT`.

### 3.4 Migration

Old layouts (known magic values) are detected on load and migrated in place into the current structure, preserving all still-existing fields; newly added fields take their defaults; the migrated config is written back immediately. A config with an unknown magic is treated as invalid (reset).

### 3.5 Reset

"Reset settings" **zeroes the whole EEPROM region** (not just the struct), then reboots → device comes up in setup mode.

---

## 4. MQTT contract

### 4.1 Base

`mqtt_topic_base = PRODUCT_MQTT_ROOT + "/" + device_id` (e.g. `antennans/SmartSpray/84f3eb2a1c00`).

| Topic | Direction | Retained | Purpose |
|-------|-----------|----------|---------|
| `<base>/trigger` | device ← | no | command channel (see 4.2) |
| `<base>/status` | device → | no | JSON heartbeat: `{"version":"3.0.8","update_available":"true","online":"true"}` |
| `<base>/availability` | device → | yes | LWT: `online` / `offline` |
| `<base>/version` | device → | yes | current `FW_VERSION` (HA sensor) |
| `<base>/update_available` | device → | yes | `true`/`false` (HA binary sensor) |

### 4.2 Commands (`trigger`)

| Command | Effect |
|---------|--------|
| `SPRAY` | trigger the device action (device-specific) |
| `CHECK` | run `updaterCheck()` and republish device info (version / update-available / update-button) |
| `UPDATE` | request install of the found update (`updaterRequestInstall()`) |
| `STATUS` | republish `<base>/status` |

The device SHOULD extend this set with its own commands, but MUST keep `STATUS` (introspection) and SHOULD keep `CHECK`/`UPDATE` (they drive the OTA flow, see §8). Unknown commands are ignored with a log line.

### 4.3 Connection lifecycle

- `client_id` = `device_id`.
- **LWT** is registered at connect: topic `<base>/availability`, qos 1, retained, payload `offline`. On connect the device publishes retained `online`.
- On successful connect, publish the static discovery payloads and then all dynamic status/info payloads, and subscribe to `trigger`.
- `PubSubClient` buffer size: ≥ 1024 (discovery payloads must fit a single publish).

### 4.4 Bounded reconnect (CRITICAL)

A missing broker MUST NOT stall the device — the web page and status endpoint keep working while MQTT is down:

- **Per-attempt cap:** `espClient.setTimeout(MQTT_CONNECT_TIMEOUT_MS)` bounds DNS + TCP connect; `client.setSocketTimeout(MQTT_CONNECT_TIMEOUT_MS/1000)` bounds the CONNACK wait. Default: `MQTT_CONNECT_TIMEOUT_MS = 3000`.
- **Minimum gap between attempts:** `MQTT_RECONNECT_INTERVAL_MS = 20000`. In `mqttLoop()`, only attempt a reconnect if at least one interval has elapsed since the last try — never in a tight loop.
- The loop SHOULD log a one-time warning when MQTT is unavailable and offer a remediation hint (check credentials / reset settings via web).

---

## 5. Home Assistant Discovery

All entities are grouped into **one HA device** via a shared `device` block appended to every discovery payload:

```json
"device": {
  "identifiers": ["SmartSpray_84f3eb2a1c00"],
  "name": "<friendly name>",
  "manufacturer": "AntennaNS",
  "model": "SmartSpray"
}
```

and share the availability JSON fragment:

```json
"availability_topic": "antennans/SmartSpray/84f3eb2a1c00/availability",
"payload_available": "online", "payload_not_available": "offline"
```

Rules:
- `unique_id` = `"<PRODUCT>_<device_id>_<role>"` — MUST be permanent (as stable as `device_id`), so HA never orphans/recreates entities across reflashes/reboots.
- Topic prefix: `homeassistant/<platform>/<PRODUCT>_<device_id>_<role>/config`.
- All discovery payloads are published **retained**.

### 5.1 Always-on entities (SmartSpray reference set)

| Platform | unique_id role | name | Properties |
|----------|----------------|------|------------|
| `button` | `_trigger` | `Spraying` | `command_topic` `<base>/trigger`, `payload_press` `SPRAY`, icon `mdi:spray` |
| `button` | `_check` | `Check updates` | `payload_press` `CHECK`, icon `mdi:cloud-sync` |
| `sensor` | `_version` | `Version` | `state_topic` `<base>/version`, icon `mdi:counter` |
| `binary_sensor` | `_update_available` | `Update available` | `state_topic` `<base>/update_available`, `payload_on`/`payload_off` `true`/`false`, `device_class` `update` |

### 5.2 Conditional entity: the Update button

- Published **only while an update is available**; its name is built **at publish time** from the target version: `Update to vX.Y.Z` (or `Update` if the version is unknown).
- It MUST carry a **stable `unique_id`** (`<PRODUCT>_<id>_update`) so HA keeps the same entity across publications/removals.
- When no update is available, publish an **empty retained message** to its discovery topic to make HA remove it.
- Republished by: `CHECK` command, daily auto-check, `updaterCheck()` results, and after a connect.

### 5.3 Device-specific entities

A device may add entities beyond the reference set (e.g. OLED text lines, number chooses, device-automation triggers such as encoder/IR events). The same helper pattern applies: build on the shared `device` + `availability` fragments, stable `unique_id`s, retained discovery. `device_automation` triggers and `text` entities SHOULD be used rather than generic `button`s for non-action UI.

---

## 6. Web — setup mode (AP)

Served on the access point when no settings exist (and in AP fallback): `GET http://192.168.4.1/`.

### 6.1 Page (one card)

Header: product name + subtitle "Configure WiFi & MQTT". Single card with sections:

1. **Device** — `name` input (prefilled `DEVICE_NAME_DEFAULT`).
2. **WiFi Network** — SSID (required) + password.
3. **MQTT Broker** — host or IP (required), port (default 1883, min 1–max 65535), username, password.
4. **Web Interface Access** — switch labeled exactly *"Username and password for the web interface"* that enables/disables the username + password fields (disabled password is cleared). Min password 4 chars.
5. **Save & Reboot** button + inline status message area.

Footer text (fixed): *"After saving, the device connects to the WiFi network you entered above, and the web interface becomes available at the device's IP address on your local network."*

### 6.2 Behavior and validation

- **Client-side validation** runs on submit: required fields marked with an `.error` class; missing SSID or host blocks submission with an inline error message.
- `POST /api/config` with a JSON body (`name`, `wifi_ssid`, `wifi_pass`, `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_pass`, `web_user`, `web_pass`, `web_auth_enabled`) → server validates (SSID + host required; web-auth completeness), saves, and **reboots** the device instantly. Response `OK`; the page shows "Saved! Rebooting…".
- `update_url` MUST NOT be settable from this page (no firmware-source field); it is always saved as empty — updates always come from GitHub.
- There is no web auth on the setup page (the device has no credentials yet).
- `GET /` is registered; **all other hosts/paths** are answered with a 302 redirect to `http://192.168.4.1/` (captive-portal). A local DNS server (DNSServer, `*` → AP IP) MUST run to complete the captive-portal experience.

---

## 7. Web — control page (working mode, STA)

Served at `http://<device-ip>/` when WiFi is connected. Mobile-first, max width 560px.

### 7.1 Design tokens (MUST match exactly)

```css
:root{
  --bg:#101419;--srf:#161c24;--srf2:#1b222d;--srf3:#232b39;
  --brd:#2a3542;--brd2:#3a4757;
  --tx:#e7ecf3;--tx2:#9fadbd;--tx3:#7d8b9c;
  --acc:#6d8cff;--acc2:#8ba5ff;
  --ok:#43d69a;--warn:#f2b544;--err:#f5646c;
  --r:12px;--r2:10px;--r3:8px;
  --sh:0 12px 34px rgba(0,0,0,.35);
}
```

Shared conventions: dark theme, `-apple-system` font stack, cards (`.card`, `border:1px solid var(--brd)`, radius `--r`, `--sh` shadow), pill buttons (`--srf2` + `--brd2`), full-width buttons min-height 44px (48px under 520px), responsive breakpoint `@media(max-width:520px)`.

### 7.2 Page anatomy (top to bottom)

1. **Header**: `#devName` (friendly name, swapped server-side), subtitle `Firmware v<FW_VERSION>`, and two **status pills** (`#stWifi`, `#stMqtt`): state `ok` / `bad` / `warn`, dot + label `WiFi · Online` / `MQTT · Online` etc.
2. **State bar** `#stateBar` (hidden): persistent rebooting indicator — spinner + message; shown for long reboot flows.
3. **Offline banner** `#offlineBox` (hidden): *"Connection to the device lost — waiting for it to come back…"*, shown after **2 consecutive failed** `/api/status` polls.
4. **Device card** (the one device-specific slot): primary action button (e.g. Spray) + device controls. SmartSpray uses a **slider + steppers** pattern — see 7.3.
5. **Firmware update card**: "Check GitHub for updates" button; `OR` divider; drag-&-drop `.bin` zone (`accept=".bin"`); one **Update** button (`#updBtn`) whose text reflects state:
   - file attached → `Update`, hint "Ready to upload <name>.";
   - GitHub update found → `Update to vX.Y.Z`;
   - otherwise `Update`, **disabled**.
6. **Logs card**: `#logBox` (monospace, dark `#0a0d11`) + Clear button; empty state *"No logs yet"*.
7. **Maintenance card**: "Reset settings" (danger style) → confirm modal → `POST /api/reset-config` → reboot to setup.
8. **Footer**: `IP <ip> · ID <device_id>` (from `#ipInfo`).

### 7.3 The slider + steppers pattern (device control)

Used for a continuously-variable device setting (SmartSpray: spray force):
- `<input type=range>` min `20` max `255` step `1`, value auto-loaded from `/api/status` (`power`).
- Two stepper buttons **right of the slider** (`−` / `+`, `42×42`, `.btn-step`), step 1, disabled at the bounds.
- **Live value label** (`<b>N</b> / 255`).
- **No Save button**: changes fire `POST /api/power` (`{"power":N}`) after a ~1 s debounce; inline `Saving…` → `Saved` status; on failure the value reverts to the last saved one and an error toast shows.

### 7.4 Notifications (toasts)

- A fixed **single-slot** toast container (`#toastWrap`, top-center). **Every new toast instantly removes all previous ones** — the stack never grows.
- Auto-dismiss timings: info/success ~3.6 s, warn ~4 s, error ~4.5 s.
- **In-flight (`opShow`) toasts have NO auto-close**; they persist while an operation runs. `opResult` closes the in-flight toast and shows a **brand-new** result toast with its own full timer (a result can never be cleared by a stale timer).
- Reboot flows switch to the persistent **state bar** (spinner + reason), disable all buttons, poll `/api/status` every 5 s and **auto-reload** when the device is back.

### 7.5 Polling

- `/api/status`: every 5 s (drives pills, offline detection, update-available state, initial slider value, footer).
- `/api/logs`: every 3 s (only re-renders when content changed, keeps scroll when anchored to bottom).

### 7.6 API endpoints (STA)

All MUST be gated by HTTP Basic auth when `web_auth_enabled` (username from settings, default port 80). Every handler with side effects is a POST.

| Route | Method | Purpose |
|-------|--------|---------|
| `/` | GET | control page (HTML with `devName` swapped in) |
| `/api/status` | GET | JSON status: `{"wifi":0|1,"mqtt":0|1,"power":N,"upd":0|1,"uver":"vX.Y.Z","version":"3.0.8","ip":"…","id":"<device_id>"}` |
| `/api/ip` | GET | plain-text local IP |
| `/api/logs` | GET | recent log lines (plain text) |
| `/api/logs/clear` | POST | clear log buffer |
| `/api/spray` | POST | device action (`SPRAY`) → `OK` |
| `/api/power` | POST | set power, body `{"power":N}` → `OK` or 400 |
| `/api/check-update` | POST | `updaterCheck()`; reply `AVAILABLE:<ver>` / `NO UPDATE FOUND…` / 500 |
| `/api/update` | POST | request GitHub-managed install → `Update started...`; 500 if nothing found |
| `/api/reset-config` | POST | erase EEPROM + reboot |
| `/update` | POST | multipart firmware upload (`field: firmware`) — see §8 |

### 7.7 Web embedding (Arduino note)

Pages are stored as **raw string literals** in flash: `R"cfg(...)cfg"` and `R"ota(...)ota"`. **Preprocessor macros do not expand inside `R"..."`**, so the literals are *spliced* around macro names at each point, e.g.:

```cpp
R"ota(<title>…</title><div class="subtitle">Firmware v)ota" FW_VERSION R"ota(</div>…"
```

The HTML source is maintained externally with `@@MACRO@@` cursors and spliced into webserver.cpp by a small build-time tool (`embed.py`); a verification tool (`servecheck.py`) re-serves the page from the `.cpp` and compares it with the HTML source so both files can never drift. Runtime server-side replacements (before `send`): page `<title>` and `#devName` are swapped to `deviceName()`.

---

## 8. OTA and updates

Two independent update paths, both available in STA mode only.

### 8.1 Web upload (`.bin` from the browser)

- **MUST be `multipart/form-data` with field `firmware`** — never raw `application/octet-stream`. ESP8266WebServer buffers a raw POST fully in a String (doesn't fit heap / times out → the handler never runs); multipart streams through the upload callback in ~4 KiB chunks.
- The upload handler runs **before** the completion handler, so **auth MUST be checked before `Update.begin()`**.
- `Update.begin(ESP.getFreeSketchSpace() - 0x1000 & 0xFFFFF000)` — NOT `up.totalSize` (in multipart that's the whole HTTP request size, not the file).
- Feed `Update.write(buf, currentSize)` per chunk; on `UPLOAD_FILE_END` finalize with `Update.end(true)`; on success reply `OK`, delay, reboot. Progress is logged every ~64 KiB.
- The `.bin` file name is validated client-side to end with `.bin`.

### 8.2 GitHub self-update (check and install are SEPARATE)

**Check** (`updaterCheck()`) — no side effect beyond remembering:
- `GET https://api.github.com/repos/<owner>/<name>/releases/latest` over TLS (`WiFiClientSecure.setInsecure()`).
- **TLS recv buffer MUST be reduced to 8 KiB** (`setBufferSizes(8192,512)`) — the default 16 KiB handshake diverges on the ESP8266's heap.
- Parse the JSON with an **ArduinoJson filter** (`tag_name`, `assets[].name`, `assets[].browser_download_url`) reading **streaming from `getStreamPtr()`** — never materialize the whole ~5 KiB body.
- Compare `FW_VERSION` vs `tag_name` numerically (X.Y.Z, optional `v` prefix). Equal/older → `NO_UPDATES`. Newer + asset named `FW_ASSET_NAME` found → remember `availVersion` + `availAssetUrl`, return `AVAILABLE`.
- **Never installs.** Runs on demand (button/`CHECK`) and once a day automatically (`UPDATE_CHECK_PERIOD_MS`), only while WiFi is connected.

**Install** — explicit, reboot-deferred:
- `updaterRequestInstall()`: requires a previously found update; **persists `update_pending=1` and `update_asset_url` to EEPROM**, then the device reboots (from loop, after the HTTP response flushed).
- At boot (WiFi connected, **before** web/MQTT start) `updaterRunPendingOnBoot()` **clears the flag first** (no reboot-loop on failure), then installs on the clean monolithic heap:
  - One `WiFiClientSecure` with the **max receive buffer** (`setBufferSizes(16384,512)`) is used for both hops.
  - **Hop 1:** follow = disabled; `GET browser_download_url` returns 302 → read `getLocation()`. `h.end()` before hop 2.
  - **Hop 2:** reconnect the same client to the CDN URL, `GET`, then **stream manually** — read plaintext via `getStreamPtr()` (`available()`/`read()`) guarded by `h.connected() && total < h.getSize()`, feeding `Update.write()` chunk-wise (512-B buffer), client read timeout 8 s. Never use `writeToStream`/`sendSize`.
  - Retries (3×) rebuild the client so heap is fully released.
- `update_url` (plain `http://`) overrides the GitHub path with `ESP8266httpUpdate` — kept only for local LAN installs.
- On success: reboot into new firmware. On failure: continue to normal STA mode.

### 8.3 Release procedure (maintainers)

1. Bump `FW_VERSION` in `include/config.h` (and the `servecheck.py` MACROS map).
2. Build once (`pio run`).
3. Upload the firmware file **exactly named `FW_ASSET_NAME`** (`SmartSpray.bin`) — the asset name equals the local file name (gh's label-rename does not work) — to a GitHub Release **tagged `v<FW_VERSION>`**.
4. Delete a stale same-name asset first: `gh release delete-asset --yes <tag> SmartSpray.bin` (the API needs a few seconds to reflect the change).

---

## 9. Resilience and network behavior

| Situation | Behavior |
|-----------|----------|
| Config exists, WiFi unavailable at boot | AP fallback: `WIFI_AP_STA`, `SmartSpray Setup` + config page. Every `WIFI_RETRY_MS` (30 s) `WiFi.scanNetworks()` looks for the configured SSID. Found → connect (`WIFI_CONNECT_TIMEOUT_MS` 15 s cap) → install pending update → STA mode. |
| Network found but connect fails (wrong password) | Count failures; after `WIFI_RETRY_MAX_FAILURES` (3) → switch to pure `WIFI_AP`, stay on config page. Counter resets on reboot/reconfig. |
| No settings | pure AP setup, no scanning. |
| Broker unreachable | Bounded reconnect (§4.4): attempt once per `MQTT_RECONNECT_INTERVAL_MS` (20 s), each attempt capped by `MQTT_CONNECT_TIMEOUT_MS` (3 s). Loop and web MUST keep responding. |
| MQTT down for a while | one-time warning log with a hint (check credentials / reset via web). |
| `loop()` | web handling always first; MQTT processing only in STA mode; deferred OTA install runs from loop. |

Constants live in `include/config.h`: `WIFI_CONNECT_TIMEOUT_MS`, `WIFI_RETRY_MS`, `WIFI_RETRY_MAX_FAILURES`, `MQTT_CONNECT_TIMEOUT_MS`, `MQTT_RECONNECT_INTERVAL_MS`.

---

## 10. Logging

- `DEBUG_PRINT / DEBUG_PRINTLN / DEBUG_PRINTF` macros output to **both** Serial and an in-RAM ring buffer visible via web.
- Ring buffer: **20 lines × 80 chars** (`.bss`); sized deliberately small — every byte here pays against the heap that the ≈16.7 KiB TLS+6.2 KiB BearSSL firmware download needs on the ESP8266.
- API: `logPrint/logPrintln/logPrintf`, `logClear()`, `logGet()` (all lines as one string).
- The log window polls every 3 s; a `Clear` button calls `/api/logs/clear`.

---

## 11. Reference implementation

| Requirement/area | Canonical file |
|------------------|----------------|
| defines, timings, version, GitHub source, DEBUG macros | `include/config.h` |
| mode machine, boot/loop, AP fallback scan, STA connect | `src/main.cpp` |
| EEPROM fields, magic migration, `deviceId()`, `deviceName()` | `src/settings.{h,cpp}` |
| ring-buffer logs | `src/debuglog.{h,cpp}` |
| topics, bounded reconnect, discovery payloads, commands | `src/mqtt.{h,cpp}` |
| web pages + handlers, auth, upload, modes, DNS | `src/webserver.{h,cpp}` |
| web-OTA + GitHub check/install, pending-on-boot | `src/updater.{h,cpp}` |
| device layer example (motor/spray/force) | `src/motor.{h,cpp}` |
| HTML sources with `@@MACRO@@` cursors + splice/verify tools | `/tmp/config.html`, `/tmp/ota.html`, `embed.py`, `servecheck.py` |

Key illustrative excerpts (abridged from the canonical implementation):

**Topics + discovery build** (`mqtt.cpp`):
```cpp
String id  = deviceId();
String base = String("antennans/SmartSpray/") + id;          // = PRODUCT_MQTT_ROOT
// ... <base>/trigger, /availability, /status, /version, /update_available
String dev = String("\"device\":{")
           + "\"identifiers\":[\"SmartSpray_" + id + "\"],"
           + "\"name\":\"" + deviceName() + "\","
           + "\"manufacturer\":\"AntennaNS\",\"model\":\"SmartSpray\"}";
// button "Spraying": homeassistant/button/SmartSpray_<id>_trigger/config
payload = "{\"name\":\"Spraying\",\"unique_id\":\"SmartSpray_"+id+"_trigger\","
          "\"command_topic\":\""+base+"/trigger\",\"payload_press\":\"SPRAY\","
          + avail + "\"icon\":\"mdi:spray\"," + dev + "}";
```

**Bounded reconnect** (`mqtt.cpp`):
```cpp
espClient.setTimeout(MQTT_CONNECT_TIMEOUT_MS);            // DNS + TCP cap
client.setSocketTimeout(MQTT_CONNECT_TIMEOUT_MS / 1000);  // CONNACK cap
client.connect(deviceId().c_str(), user, pass, mqtt_topic_availability, 1, true, "offline");
// in mqttLoop(): reconnect at most once per MQTT_RECONNECT_INTERVAL_MS
```

**API status JSON** (`webserver.cpp`):
```cpp
"{\"wifi\":%d,\"mqtt\":%d,\"power\":%d,\"upd\":%d,\"uver\":\"%s\",\"version\":\"" FW_VERSION "\",\"ip\":\"%s\",\"id\":\"%s\"}"
```

**Pending install pre-download flag clear** (`updater.cpp`):
```cpp
gSettings.update_pending = 0;
settingsSave(gSettings);   // BEFORE installing — a failed install can't reboot-loop
```

---

## 12. Framework porting notes

### 12.1 Arduino / ESP8266 (SmartSpray — canonical; SmartOLED target)

Same board (Wemos D1 Mini), same framework, same libraries (`ESP8266WiFi`, `ESP8266WebServer`, `PubSubClient`, `GyverMotor2`/device libs, EEPROM). The platform modules port **almost 1:1**; only the *device layer* (what the device actually does: motor, OLED, encoder, …) is written new. Keep the OTA/ memory cautions: TLS download needs a monolithic heap → any long-lived TLS client or big buffers divided the heap are forbidden except during the boot install.

### 12.2 ESP-IDF / C on ESP32 (SmartRemote target)

Same **behavior**, different building blocks — this is a behavioral port, not a copy:

- WiFi: `esp_wifi` + event loop; reimplement the mode machine (`WIFI_STA`, `WIFI_AP`, fallback scan) as an event-driven state machine.
- HTTP server: `esp_http_server`; map the endpoint table (§7.6) to URIs; Basic auth via an `httpd_req` header check before any handler runs.
- EEPROM → NVS (or a partition); keep magic-versioned struct + migration semantics.
- MQTT: `esp-mqtt` provides non-blocking connect by default; enforce "once per interval, bounded timeouts" via the event callbacks instead of PubSubClient.
- Discovery JSON: hand-build or use `cJSON`; keep the exact topics/payloads (§4–5).
- OTA: `esp_ota` / `esp_http_client`; the *check* uses the GitHub API, the *manual-hop* 302→CDN download translates naturally to two `esp_http_client` requests; consider `Update.begin` → `esp_ota_begin` and DIFF boot policy.
- Keep: stable MAC-derived ID, LWT + retained status, web responsiveness while MQTT is down, once-a-day check + explicit install, pending-on-boot with flag-cleared-first.

---

## 13. Definition of Done (acceptance checklist)

An implementation is "platform-conformant" when an agent can self-verify all of the following:

**Identity & settings**
- [ ] `device_id` = lowercased MAC hex, colons stripped, stable across OTA/reset; never user-set.
- [ ] All topics/`unique_id`s/`identifiers`/model derived from product defines — no stray product strings in code.
- [ ] Settings in EEPROM with magic; old layouts migrate; invalid magic ⇒ reset.
- [ ] `update_url` empty from setup page; `update_pending`/`update_asset_url` respect the boot-install flow.

**MQTT / HA**
- [ ] Topics match §4.1 exactly; subscription on `<base>/trigger`; commands SPRAY/CHECK/UPDATE/STATUS handled.
- [ ] LWT `offline`, retained `online`, retained `version`/`update_available`, JSON `status`.
- [ ] Discovery: all §5.1 entities present with correct topics and **stable** `unique_id`s; Update button published only when available, name `Update to vX.Y.Z`, removed via empty retained; all payloads valid JSON.
- [ ] `/api/status` responds fast (e.g. < 300 ms) **while MQTT is down** — reconnect is bounded.

**Web**
- [ ] Setup page: single card, correct fields/labels/footer, client-side validation, `POST /api/config` saves+reboots.
- [ ] Control page renders tokens §7.1, pills update from `/api/status` every 5 s, offline banner after 2 failed polls, toasts single-slot, state bar + auto-reload on reboot flows.
- [ ] All STA endpoints behind HTTP Basic when enabled; auth checked before `Update.begin()`.
- [ ] servecheck-style round-trip: served HTML equals HTML source with macros expanded (no drift).

**OTA**
- [ ] Multipart `.bin` upload works E2E (> ~100 KiB) and reboots into new FW.
- [ ] GitHub check finds a newer release (tag `v` matching), reports it; UPDATE installs and survives reboot (pending-on-boot).
- [ ] A failed install does not reboot-loop (flag cleared first).

**Resilience**
- [ ] Boot without network: AP fallback page reachable, periodic scan reconnects; wrong password gives up after `WIFI_RETRY_MAX_FAILURES` and stays on AP.
- [ ] Broker down: web still fully usable; reconnect attempts spaced per `MQTT_RECONNECT_INTERVAL_MS`.

---
*This specification is the contract; the live device is the verification. When a rule conflicts with the reference source, fix the source and update this document in the same change.*