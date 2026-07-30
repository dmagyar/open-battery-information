# OBI Communication Protocol

This document describes the protocol spoken between the `OpenBatteryInformation`
desktop client (Python/Tkinter, `interfaces/arduino_obi.py`) and the
`ArduinoOBI` firmware (`ArduinoOBI/src/main.cpp`), and how that protocol
carries Makita LXT battery commands (`modules/makita_lxt.py`) over a
1-Wire-style bus to the battery pack.

It was reverse engineered by reading the source of both sides; it is not
based on an official specification. Byte offsets and field names below are
descriptive labels chosen for this document, not names that appear in the
code.

There are two layers:

1. **Transport layer** — a fixed framing format sent over the USB CDC
   serial link between the host PC and the Arduino/ESP32.
2. **Application layer** — the Makita LXT "function codes" that are carried
   inside that framing and shifted out over the single-wire bus to the
   battery pack.

On WiFi-capable (`ESP_BUILD`) targets there is also a third, independent
way to reach the same battery commands: a self-hosted captive-portal web
UI, so the device can be used from a phone with no PC involved. It reuses
the application-layer knowledge from §3 but talks to the bus directly
instead of going through the USB framing — see [§6](#6-wifi-captive-portal--http-api-esp-builds-only).

---

## 1. Hardware / physical layer

The firmware bit-bangs a Dallas/Maxim-style 1-Wire bus using the bundled
`OneWire` library (vendored as `OneWire2` to avoid clashing with any
system-installed copy). Two GPIOs are used:

| Signal | AVR (Uno/Nano) | ESP32-C3 SuperMini | Lolin S2 Mini |
|---|---|---|---|
| One-Wire data | D6 | GPIO1 | GPIO7 |
| Enable / RTS  | D8 | GPIO0 | GPIO5 |

These are compiled in via `ONEWIRE_PIN`/`ENABLE_PIN` (`ArduinoOBI/src/main.cpp:11-17`),
which on ESP builds resolve to the `ESP_OW_PIN`/`ESP_EN_PIN` build flags set
per-environment in `platformio.ini`.

Per the reference schematic (`docs/images/arduino-obi.png`), `ENABLE_PIN`
and `ONEWIRE_PIN` are two independent lines wired straight to the battery
pack's connector — `Enable` and `OneWire` — each with its own 4.7 kΩ
pull-up to the MCU's logic-supply rail (5V on the Uno, 3.3V on ESP boards —
see the ESP wiring warning in `ArduinoOBI/README.md`). The **enable pin** is
driven high before every command that touches the bus and low again
afterwards (`digitalWrite(ENABLE_PIN, HIGH); delay(400);` ...
`digitalWrite(ENABLE_PIN, LOW);` in `read_usb()`). This directly asserts the
battery pack's own `Enable` input, and the ~400 ms delay gives the pack's
internal BMS/controller time to power up and start driving the `OneWire`
line before bytes are exchanged.

The serial port runs at **9600 baud, 8N1** (`Serial.begin(9600)`), with no
flow control other than the application-level framing described below.

---

## 2. Transport layer (Host ⇄ Arduino, over USB serial)

### 2.1 Request frame (Host → Arduino)

```
byte 0     : START     always 0x01
byte 1     : LEN        number of DATA bytes that follow (0-255)
byte 2     : RSP_LEN     number of response bytes the host wants back
byte 3     : CMD        firmware command / dispatch selector
byte 4..N  : DATA       LEN bytes, only present if LEN > 0
```

Total frame length = `4 + LEN` bytes. The firmware (`read_usb()` in
`main.cpp`) only starts parsing once at least 4 bytes are available, checks
`start == 0x01`, then reads `LEN`, `RSP_LEN`, `CMD`, and finally blocks
(`while (Serial.available() < 1);`) until `LEN` more bytes have arrived for
`DATA`. There is no length prefix independent of `LEN`/`RSP_LEN` — if a
sender writes a different number of `DATA` bytes than `LEN` declares, the
stream desynchronizes (see [§5 Known quirks](#5-known-quirks-observed-in-the-firmware)).

### 2.2 Response frame (Arduino → Host)

```
byte 0     : CMD        echoed command byte
byte 1     : RSP_LEN     echoed response length
byte 2..N  : DATA       RSP_LEN bytes
```

Total response length = `RSP_LEN + 2` bytes, sent in one shot via
`send_usb()`. The host (`Interface.request()` in `arduino_obi.py`) always
computes `expected_length = request[2] + 2` (i.e. the *requested* `RSP_LEN`
plus 2 header bytes) and reads exactly that many bytes back — it trusts the
request it just sent, it does not re-parse the echoed header.

If the host's `RSP_LEN` is `0`, `request()` still performs the 2-byte read
(to stay in sync with the stream) but returns `None` immediately without
validating anything — this is the "fire and forget" path used for commands
whose result isn't needed (`STORE_CMD`, `CLEAR_CMD`, `CLEAN_FRAME_CMD`, ...).

### 2.3 CMD dispatch table

`CMD` selects one of the following behaviors in the firmware's `switch` (`main.cpp:118-166`):

| CMD  | Meaning | Bus behavior |
|---|---|---|
| `0x01` | Get firmware version | No bus activity at all. Returns `[MAJOR, MINOR, PATCH]` from `ARDUINO_OBI_VERSION_{MAJOR,MINOR,PATCH}` (currently 0.2.1). |
| `0x31` | F0513 "read model" (hard-coded sequence) | Reset → Skip ROM (`0xCC`) → write `0x99` (enter test/service mode) → 400 ms delay → Reset → write `0x31` → read 2 bytes. The two bytes are stored **swapped** (first byte read → `rsp[3]`, second → `rsp[2]`). |
| `0x32` | F0513 "read version" (hard-coded sequence) | Identical sequence to `0x31` but writes `0x32` instead of `0x31` as the bus command. |
| `0x33` | Generic "Read ROM + function code" | See [§2.4](#24-cmd-0x33--read-rom-prefixed-commands). |
| `0xCC` | Generic "Skip ROM + function code" | See [§2.5](#25-cmd-0xcc--skip-rom-commands). |
| *anything else* | Unknown | `rsp_len` is forced to `0`; response is just `[CMD, 0x00]`. |

Note that `CMD` values `0x33` and `0xCC` are not arbitrary — they are reused
directly as the standard Dallas/Maxim 1-Wire ROM commands **Read ROM**
(`0x33`) and **Skip ROM** (`0xCC`), because the firmware's dispatcher was
built so the USB-level command byte doubles as the on-the-wire ROM command
it triggers.

### 2.4 CMD `0x33` — Read-ROM-prefixed commands

Implemented by `cmd_and_read_33()` (`main.cpp:22-42`):

1. Bus reset, write `0x33` (1-Wire **Read ROM**).
2. Read 8 bytes — this is the battery's 64-bit ROM/serial ID.
3. Write the `DATA` bytes from the request (the Makita function code).
4. Read `RSP_LEN` more bytes from the bus.

The 8 ROM bytes and the post-write bytes are written into the *same*
response buffer, offset by the 2-byte USB header, i.e. response bytes
`[2..9]` are always the ROM ID and response bytes `[10..]` are the function
result — **regardless of what `RSP_LEN` the request declared**, because the
final `send_usb(rsp, rsp_len + 2)` call sends only `RSP_LEN` bytes of
payload in total, including the 8 ROM bytes. See
[§5](#5-known-quirks-observed-in-the-firmware) for the practical effect of
this.

### 2.5 CMD `0xCC` — Skip-ROM commands

Implemented by `cmd_and_read_cc()` (`main.cpp:44-59`):

1. Bus reset, write `0xCC` (1-Wire **Skip ROM** — address whatever single
   device is on the bus without needing its ROM ID).
2. Write the `DATA` bytes from the request (the Makita function code).
3. Read `RSP_LEN` bytes from the bus and return them directly (no ROM
   prefix, no offset quirk).

This is the "clean" path — response bytes `[2..RSP_LEN+1]` are exactly the
`RSP_LEN` bytes read from the bus after the command was written.

(There is also a third, unused helper, `cmd_and_read()`, which resets and
writes the command bytes with no ROM-select prefix at all. It is not called
from anywhere in `read_usb()` — it appears to be scaffolding for a future
Match-ROM-style addressed command that was never wired up.)

---

## 3. Application layer — Makita LXT function codes

All battery-specific commands live in `OpenBatteryInformation/modules/makita_lxt.py`
as pre-built request frames. Each is a `[START, LEN, RSP_LEN, CMD, ...DATA]`
list per §2.1. `DATA` is the Makita "function code" (typically 2 bytes) that
is written to the bus after the ROM-select prefix.

### 3.1 Standard command family (most LXT packs)

| Constant | Frame | Function code | Purpose |
|---|---|---|---|
| `MODEL_CMD` | `01 02 10 CC DC 0C` | `DC 0C` | Read model string (7 ASCII bytes back, e.g. `BL1850`) |
| `READ_DATA_REQUEST` | `01 04 1D CC D7 00 00 FF` | `D7 00 00 FF` | Read live pack telemetry (voltages/temps) |
| `TESTMODE_CMD` | `01 03 09 33 D9 96 A5` | `D9 96 A5` | Enter service/test mode (unlock privileged functions) |
| `LEDS_ON_CMD` | `01 02 09 33 DA 31` | `DA 31` | Turn on all fuel-gauge LEDs (requires test mode) |
| `LEDS_OFF_CMD` | `01 02 09 33 DA 34` | `DA 34` | Turn off LEDs / exit LED test (requires test mode) |
| `RESET_ERROR_CMD` | `01 02 09 33 DA 04` | `DA 04` | Clear the fault/error latch (requires test mode) |
| `ROMID_CHARGER_CMD` | `01 02 28 33 F0 00` | `F0 00` | Read-ROM variant of the charger-message read |
| `CHARGER_CMD` | `01 02 20 CC F0 00` | `F0 00` | Skip-ROM variant of the charger-message read |
| `READ_MSG_CMD` | `01 02 28 33 AA 00` | `AA 00` | Read the stored ROM ID + diagnostic message frame |
| `CLEAR_CMD` | `01 02 00 CC F0 00` | `F0 00` | Fire-and-forget variant of the charger read (used to reset internal read pointer) |
| `STORE_CMD` | `01 02 00 33 55 A5` | `55 A5` | Commit/store a pending write (requires test mode) |
| `CLEAN_FRAME_CMD` | `01 22 00 33 33 0F ...` | `33 0F ...` (36 bytes) | Write a full "blank" diagnostic message frame back to the battery — used by the (currently disabled) "Reset battery message" feature |

`TESTMODE_CMD` acts as a session unlock: after it is sent, subsequent
frames such as `LEDS_ON_CMD`/`RESET_ERROR_CMD`/`STORE_CMD` are accepted as
privileged even though each USB request is independent and each begins with
its own fresh 1-Wire bus reset. The unlock state is therefore held inside
the *battery's* controller, not the bus, and appears to survive a bus
reset — it is only entered explicitly via `0x99`/`TESTMODE_CMD` and is not
re-armed by the firmware automatically.

#### `on_read_static_click()` — decoding `READ_MSG_CMD`

`READ_MSG_CMD` requests `RSP_LEN = 0x28` (40). Per §2.4/§5 this yields 8
ROM-ID bytes followed by 32 message bytes in the response, decoded as
(`modules/makita_lxt.py:199-219`):

| Response bytes | Field |
|---|---|
| `[2:10]`  | ROM ID (8 bytes, hex) |
| `[10:42]` | Raw message frame (32 bytes, hex) |
| `[4], [3], [2]` | Manufacturing date, formatted `DD/MM/20YY` — note these are the 3rd, 2nd and 1st bytes **of the ROM ID** (not the message frame); Makita appears to encode the pack's build date directly into what would otherwise be the 1-Wire serial-number field |
| `[21]` (nibble-swapped) | Battery type |
| `[26]` (nibble-swapped) `/ 10` | Nominal capacity, in Ah |
| `[29]` | Status/error code (hex) |
| `[30]` low nibble | Lock state: non-zero ⇒ `LOCKED`, else `UNLOCKED` |
| `[36],[37]` (nibble-swapped, reversed, big-endian) `& 0x0FFF` | Charge cycle count |

("Nibble-swap" = swap the high/low 4-bit halves of a byte —
`nibble_swap()` in `makita_lxt.py:186-190` — Makita appears to store some
fields with swapped nibbles in the raw frame.)

#### `on_read_data_click()` — decoding `READ_DATA_REQUEST`

All values are little-endian 16-bit integers, scaled as shown
(`modules/makita_lxt.py:271-282`):

| Response bytes | Field | Scale |
|---|---|---|
| `[2:4]`   | Pack voltage | ÷1000 (mV → V) |
| `[4:6]`   | Cell 1 voltage | ÷1000 |
| `[6:8]`   | Cell 2 voltage | ÷1000 |
| `[8:10]`  | Cell 3 voltage | ÷1000 |
| `[10:12]` | Cell 4 voltage | ÷1000 |
| `[12:14]` | Cell 5 voltage | ÷1000 |
| `[16:18]` | Cell/pack temperature | ÷100 (centi-°C → °C) |
| `[18:20]` | MOSFET temperature | ÷100 |

(Bytes `[14:16]` are skipped/unused by the client.) Cell voltage difference
is computed client-side as `max(cells) - min(cells)`.

### 3.2 F0513 command family (limited-support variant)

A second, older/simpler pack variant ("F0513") uses different, shorter
frames that bypass the ROM-prefix logic entirely via the firmware's
hard-coded `0x31`/`0x32` cases and dedicated Skip-ROM reads:

| Constant | Frame | Purpose |
|---|---|---|
| `F0513_MODEL_CMD`   | `01 00 02 31` | Read model ID (firmware CMD `0x31` hard-coded sequence) |
| `F0513_VERSION_CMD` | `01 00 02 32` | Read version (firmware CMD `0x32` hard-coded sequence) |
| `F0513_TESTMODE_CMD`| `01 01 00 CC 99` | Enter test mode (note: normally folded into the `0x31` firmware path already, see below) |
| `F0513_VCELL_1..5_CMD` | `01 01 02 CC 3{1..5}` | Read individual cell voltage |
| `F0513_TEMP_CMD`    | `01 01 02 CC 52` | Read temperature |

`get_f0513_model()` reads the two (swapped) bytes from CMD `0x31` and
formats them as `f"BL{response[2]:X}{response[3]:X}"`. The client's own
`F0513_TESTMODE_CMD` is commented out with a `TODO` note about timing
issues — because the firmware's `case 0x31`/`0x32` handlers already send the
`0x99` test-mode-unlock byte themselves before issuing `0x31`/`0x32`, a
separate unlock round-trip from the host is redundant for those two reads.
Detection of an F0513-family pack falls back to this path only after the
standard `MODEL_CMD` (§3.1) fails to decode as a valid model string
(`on_read_static_click()`, `modules/makita_lxt.py:192-244`).

For cell voltages/temperature, the F0513 path issues `CLEAR_CMD` twice
before reading each cell, then reads `F0513_VCELL_1..5_CMD` and
`F0513_TEMP_CMD` individually (`modules/makita_lxt.py:252-269`) — one
Skip-ROM round trip per value, versus the single combined
`READ_DATA_REQUEST` used for standard packs.

---

## 4. End-to-end example

Reading the model of a standard pack (`MODEL_CMD`):

```
Host → Arduino  : 01 02 10 CC DC 0C
                   START=01 LEN=02 RSP_LEN=16(0x10) CMD=0xCC(SkipROM) DATA=[DC 0C]

Arduino:
  ENABLE_PIN → HIGH, wait 400ms
  bus.reset(); bus.write(0xCC)         // Skip ROM
  bus.write(0xDC); bus.write(0x0C)     // Makita "get model" function code
  read 16 bytes from bus               // e.g. "BL1850 " + padding
  ENABLE_PIN → LOW

Arduino → Host  : CC 10 42 4C 31 38 35 30 20 ... (16 data bytes)
                   CMD=0xCC RSP_LEN=16 DATA="BL1850 " + 9 more bytes

Host: model = response[2:9].decode('utf-8')  → "BL1850 " (7 bytes; the
      remaining 9 of the 16 requested bytes are read but unused by the client)
```

---

## 5. Known quirks (observed in the firmware)

These are behaviors confirmed by reading `main.cpp`, not confirmed bugs —
they're documented here because they matter if you implement a second host
client or add a new battery command.

- **CMD `0x33` over-reads and silently discards 8 bytes.**
  `cmd_and_read_33()` reads `8 (ROM) + RSP_LEN` bytes total from the bus,
  but `send_usb()` only ever forwards `RSP_LEN` bytes to the host. Since
  those `RSP_LEN` bytes are taken from the *front* of the buffer (the 8 ROM
  bytes plus the first `RSP_LEN − 8` function-result bytes), the **last 8
  bytes** of the function result that were actually read off the bus are
  computed but never transmitted to the host. In practice this means: for
  any `0x33` command, the usable function-result length delivered to the
  host is `RSP_LEN − 8`, not `RSP_LEN`. All current callers in
  `makita_lxt.py` already account for this (e.g. `READ_MSG_CMD` requests
  `RSP_LEN=40` to get 32 usable message bytes).

- **`CLEAN_FRAME_CMD`'s declared `LEN` doesn't match its payload.**
  The constant is 39 bytes long (`01 22 00 33` + 35 data bytes), but the
  `LEN` field (byte 1) is `0x22` = 34, one less than the actual 35 data
  bytes present. If this command were ever sent, the firmware would read
  only 34 of the 35 data bytes into its buffer, leaving one stray byte
  (`0x83`) in the serial receive buffer to be misinterpreted as the `START`
  byte of the next frame, desynchronizing the stream. This currently can't
  happen in the shipped client: `on_reset_message_click()` returns early
  with a "Not Implemented" warning before ever sending `CLEAN_FRAME_CMD`
  (`modules/makita_lxt.py:351-372`). Worth fixing before that feature is
  completed.

- **CMD `0x01` (version query) still asserts `ENABLE_PIN` and delays
  400 ms**, even though it never touches the 1-Wire bus. Harmless, just an
  unnecessary latency/power-gating cost on every version check.

- **No checksum/CRC validation at the transport layer.** The `OneWire`
  library exposes `crc8`/`crc16` helpers but nothing in `main.cpp` calls
  them on Makita traffic — data integrity for the application layer relies
  entirely on the Python client's sanity check that the response isn't all
  `0xFF` (`arduino_obi.py:111-112`), which only catches the
  "nothing answered" case, not bit errors.

- **No addressed (Match-ROM) command path.** Every request is either
  Skip-ROM (`0xCC`, works with exactly one device on the bus) or Read-ROM
  (`0x33`, also implicitly single-device). Multi-battery/multi-drop bus
  topologies are not supported by this protocol as implemented.

---

## 6. WiFi captive portal / HTTP API (ESP builds only)

On `ESP_BUILD` targets (`esp32-c3-devkitm-1`, `lolin_s2_mini`), the firmware
runs the USB-serial transport above *and* a standalone WiFi UI, so the
device is usable from a phone with no PC involved. This is implemented in
`web_portal.h/.cpp`, backed by a native (non-USB-framed) reimplementation of
the Makita command set in `makita.h/.cpp` — the web handlers talk to the
`OneWire` bus directly rather than looping requests back through the §2
framing, which also means they don't inherit the `CMD 0x33` 8-byte discard
quirk from §5: each handler reads exactly the bytes it needs.

### 6.1 Access point and captive portal

- SSID: `OBIWiFi`, open (no password), IP `192.168.4.1`.
- A `DNSServer` resolves every hostname to `192.168.4.1`, and every HTTP
  path that isn't a known API route — including the well-known
  connectivity-check URLs used by iOS (`/hotspot-detect.html`), Android
  (`/generate_204`), and Windows (`/ncsi.txt`, `/connecttest.txt`) — serves
  the portal page directly at `200 OK`. This is what makes phones
  auto-prompt to open the portal after joining the network, the same
  mechanism used by commodity WiFi captive portals.
- The page itself is a single self-contained HTML/CSS/JS document stored in
  flash (`index_html.h`, `PROGMEM`) — no filesystem upload step, no
  external CDN assets (the phone has no real internet access on this AP).

### 6.2 JSON API

All endpoints return `application/json`. Reads are `GET`, actions that send
a command to the battery are `POST`. There is no authentication — anything
on the `OBIWiFi` network can call these.

| Method | Path | Mirrors (PC client) | Notes |
|---|---|---|---|
| GET | `/api/version` | — | `{"major":0,"minor":2,"patch":1}` |
| GET | `/api/read-info` | `on_read_static_click()` | Reads ROM/message frame, then tries the standard model read, falling back to the F0513 sequence. See below. |
| GET | `/api/read-data` | `on_read_data_click()` | Uses whichever pack kind the last successful `read-info` call detected. |
| POST | `/api/leds-on` | `on_all_leds_on_click()` | Enters test mode, then LED-on. |
| POST | `/api/leds-off` | `on_all_leds_off_click()` | Enters test mode, then LED-off. |
| POST | `/api/clear-errors` | `on_reset_errors_click()` | Enters test mode, then clears the error latch. |
| POST | `/api/reset-message` | `on_reset_message_click()` | Always returns `501`. Intentionally not wired up — see below. |

`GET /api/read-info` response (success):

```json
{
  "ok": true,
  "kind": "standard",
  "limited": false,
  "model": "BL1850 ",
  "romId": "AA BB CC DD EE FF 00 11",
  "message": "AA BB ... (32 bytes hex)",
  "mfgDate": "05/03/2021",
  "capacityAh": 5.0,
  "batteryType": 3,
  "chargeCount": 128,
  "locked": false,
  "statusCode": "00"
}
```

`kind` is `"f0513"` for the limited-diagnostics pack family (§3.2); `limited`
is `true` in that case, matching the PC client's `messagebox.showwarning`.
Any endpoint can instead return `{"ok": false, "error": "..."}` — the same
"no response" / "all `0xFF`" detection from §2.2 applies, since the native
helpers still check for an unresponsive bus before parsing.

`GET /api/read-data` response (success):

```json
{
  "ok": true,
  "packVoltage": 20.123,
  "cellVoltages": [4.02, 4.01, 4.03, 4.02, 4.02],
  "cellDiff": 0.02,
  "tempCell": 25.3,
  "tempMosfet": 24.8,
  "raw": "AA BB CC ... (hex of the bytes actually read off the bus)"
}
```

`tempMosfet` is `null` for F0513 packs, which don't expose a second
temperature sensor via this command (§3.2).

### 6.3 Why "Reset Battery Message" is disabled here too

The PC client's `on_reset_message_click()` never actually sends
`CLEAN_FRAME_CMD` — it shows a "Not Implemented" dialog and returns early.
The web UI does the same (button permanently disabled, endpoint returns
`501`), and for good reason beyond parity: §5 documents that
`CLEAN_FRAME_CMD`'s declared `LEN` byte doesn't match its actual payload
length, which would desync the USB-framed parser if it were ever sent that
way — and more importantly, this command writes directly to the battery's
stored diagnostic frame. It should stay disabled until that's fixed and
verified against real hardware, not silently enabled just because the
native command layer doesn't have the same framing bug.
