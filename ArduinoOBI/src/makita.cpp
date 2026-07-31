#include "makita.h"
#include "OneWire2.h"
#include "pins.h"
#include <string.h>

// Defined in main.cpp; shared with the legacy USB-serial command path.
extern OneWire makita;

namespace makita_lxt {

static PackKind g_pack_kind = PackKind::UNKNOWN;

PackKind last_pack_kind() {
    return g_pack_kind;
}

static void bus_power(bool on) {
    digitalWrite(ENABLE_PIN, on ? HIGH : LOW);
    if (on) {
        delay(400);
    }
}

static bool all_ff(const uint8_t *buf, size_t len) {
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static uint8_t nibble_swap(uint8_t b) {
    return (uint8_t)(((b & 0x0F) << 4) | ((b & 0xF0) >> 4));
}

static uint16_t u16le(const uint8_t *buf, size_t idx) {
    return (uint16_t)buf[idx] | ((uint16_t)buf[idx + 1] << 8);
}

// Nybble-oriented helpers for the 32-byte "message" frame -- see
// PROTOCOL.md's frame-repair section. Nybble n lives in byte n/2: the low
// nybble if n is even, the high nybble if n is odd.
static uint8_t nyb_get(const uint8_t *d, uint8_t n) {
    return (n % 2 == 0) ? (d[n / 2] & 0x0F) : ((d[n / 2] >> 4) & 0x0F);
}
static void nyb_set(uint8_t *d, uint8_t n, uint8_t v) {
    v &= 0x0F;
    if (n % 2 == 0) {
        d[n / 2] = (uint8_t)((d[n / 2] & 0xF0) | v);
    } else {
        d[n / 2] = (uint8_t)((d[n / 2] & 0x0F) | (v << 4));
    }
}
static uint8_t checksum_calc(const uint8_t *d, uint8_t start_nyb, uint8_t end_nyb) {
    uint8_t sum = 0;
    for (uint8_t i = start_nyb; i <= end_nyb; i++) {
        sum = (uint8_t)(sum + nyb_get(d, i));
    }
    return sum & 0x0F;
}

// The legacy USB path's PC client retries every transaction (2-5 attempts,
// see arduino_obi.py's Interface.request()) before giving up, since a
// single dropped/garbled 1-Wire slot isn't unusual. Wrap each bus
// transaction the same way here instead of giving up after one attempt.
template <typename Fn>
static bool with_retry(uint8_t attempts, Fn fn) {
    for (uint8_t i = 0; i < attempts; i++) {
        if (fn()) {
            return true;
        }
        if (i + 1 < attempts) {
            delay(50);
        }
    }
    return false;
}

// Skip ROM (0xCC) + function code, then read rsp_len bytes directly.
// Unlike the legacy USB-framed cmd_and_read_cc(), this reads exactly what's
// asked for -- no hidden offset quirks. Returns whether the bus reset saw a
// presence pulse (existing callers that only cared about all_ff() on the
// response are free to ignore the return value).
static bool skiprom_cmd(const uint8_t *data, uint8_t data_len, uint8_t *rsp, uint8_t rsp_len) {
    bool present = makita.reset();
    delayMicroseconds(400);
    makita.write(0xCC, 0);
    for (uint8_t i = 0; i < data_len; i++) {
        delayMicroseconds(90);
        makita.write(data[i], 0);
    }
    for (uint8_t i = 0; i < rsp_len; i++) {
        delayMicroseconds(90);
        rsp[i] = makita.read();
    }
    return present;
}

// Read ROM (0x33): capture the 8-byte ROM ID, write the function code, then
// read rsp_len bytes. See PROTOCOL.md §5 for why the legacy USB-framed
// cmd_and_read_33() can't be reused as-is (it discards the trailing 8 bytes
// of the result).
static bool readrom_cmd(const uint8_t *data, uint8_t data_len, uint8_t rom[8], uint8_t *rsp, uint8_t rsp_len) {
    bool present = makita.reset();
    delayMicroseconds(400);
    makita.write(0x33, 0);
    for (uint8_t i = 0; i < 8; i++) {
        delayMicroseconds(90);
        rom[i] = makita.read();
    }
    for (uint8_t i = 0; i < data_len; i++) {
        delayMicroseconds(90);
        makita.write(data[i], 0);
    }
    for (uint8_t i = 0; i < rsp_len; i++) {
        delayMicroseconds(90);
        rsp[i] = makita.read();
    }
    return present;
}

static void send_clear() {
    bus_power(true);
    const uint8_t cmd[2] = {0xF0, 0x00};
    skiprom_cmd(cmd, 2, nullptr, 0);
    bus_power(false);
}

// Test-mode entry/exit goes over Skip ROM (0xCC), not Read ROM (0x33).
// The original OBI protocol used 0x33 here (see PROTOCOL.md), which some
// BMS revisions apparently don't reliably accept -- some Makita LXT
// Battery Monitor/Unlocker project (github.com/synrais) uses 0xCC and
// reports it working across a much wider set of packs.
static bool enter_testmode() {
    uint8_t rsp[1];
    bus_power(true);
    const uint8_t cmd[3] = {0xD9, 0x96, 0xA5};
    bool present = skiprom_cmd(cmd, 3, rsp, 1);
    bus_power(false);
    return present;
}

static void exit_testmode() {
    uint8_t rsp[1];
    bus_power(true);
    const uint8_t cmd[3] = {0xD9, 0xFF, 0xFF};
    skiprom_cmd(cmd, 3, rsp, 1);
    bus_power(false);
}

static bool read_message(uint8_t rom[8], uint8_t msg[32]) {
    bus_power(true);
    const uint8_t cmd[2] = {0xAA, 0x00};
    readrom_cmd(cmd, 2, rom, msg, 32);
    bus_power(false);
    return !(all_ff(rom, 8) && all_ff(msg, 32));
}

static bool read_model_standard(char out[8]) {
    uint8_t rsp[16];
    bus_power(true);
    const uint8_t cmd[2] = {0xDC, 0x0C};
    skiprom_cmd(cmd, 2, rsp, 16);
    bus_power(false);
    if (all_ff(rsp, 16)) {
        return false;
    }
    // Approximates the PC client's `response[2:9].decode('utf-8')`: any
    // non-ASCII byte here means this isn't a valid model string, which is
    // how the PC client's UnicodeDecodeError-driven fallback behaves for
    // the packs that don't support this command.
    for (uint8_t i = 0; i < 7; i++) {
        if (rsp[i] >= 0x80) {
            return false;
        }
        out[i] = (char)rsp[i];
    }
    out[7] = '\0';
    return true;
}

// Mirrors main.cpp's legacy `case 0x31` bus sequence exactly (including the
// swapped read order), so F0513 packs are recognized the same way over
// WiFi as they are over USB.
static bool read_model_f0513(uint8_t out[2]) {
    bus_power(true);
    makita.reset();
    delayMicroseconds(400);
    makita.write(0xCC, 0);
    delayMicroseconds(90);
    makita.write(0x99, 0);
    delay(400);
    makita.reset();
    delayMicroseconds(400);
    makita.write(0x31, 0);
    delayMicroseconds(90);
    uint8_t first = makita.read();
    delayMicroseconds(90);
    uint8_t second = makita.read();
    delayMicroseconds(90);
    bus_power(false);

    if (first == 0xFF && second == 0xFF) {
        return false;
    }
    out[0] = second;
    out[1] = first;
    return true;
}

BatteryInfo read_info() {
    BatteryInfo info;
    memset(&info, 0, sizeof(info));
    info.ok = false;
    info.kind = PackKind::UNKNOWN;

    uint8_t rom[8];
    uint8_t msg[32];
    bool got_message = with_retry(3, [&]() { return read_message(rom, msg); });
    if (!got_message) {
        snprintf(info.error, sizeof(info.error), "No response from battery. Check it is seated correctly.");
        return info;
    }

    memcpy(info.rom, rom, 8);
    memcpy(info.message, msg, 32);

    snprintf(info.mfg_date, sizeof(info.mfg_date), "%02u/%02u/20%02u", rom[2], rom[1], rom[0]);
    info.battery_type = nibble_swap(msg[11]);
    info.capacity_ah = nibble_swap(msg[16]) / 10.0f;
    info.status_code = msg[19];
    info.locked = (msg[20] & 0x0F) != 0;
    uint16_t cc = (uint16_t)((uint16_t)nibble_swap(msg[26]) << 8 | nibble_swap(msg[27]));
    info.charge_count = cc & 0x0FFF;

    // What the charger itself checks -- see PROTOCOL.md's frame-repair
    // section. This can disagree with `locked` above: a pack can have a
    // clean failure code yet still be charger-locked (nybble 34 set from a
    // prior charger lock event, or a corrupt checksum), which DA04 alone
    // can never fix.
    info.lock_cause_cs0 = checksum_calc(msg, 0, 15) != nyb_get(msg, 41);
    info.lock_cause_cs2 = checksum_calc(msg, 32, 40) != nyb_get(msg, 43);
    info.lock_cause_n34 = nyb_get(msg, 34) != 0;
    info.charger_locked = info.lock_cause_cs0 || info.lock_cause_cs2 || info.lock_cause_n34;
    bool new_family = msg[0] == 0xF1;

    char model[8];
    if (with_retry(3, [&]() { return read_model_standard(model); })) {
        memcpy(info.model, model, 8);
        info.model[8] = '\0';
        info.kind = PackKind::STANDARD;
        info.frame_repair_supported = new_family;
        g_pack_kind = PackKind::STANDARD;
        info.ok = true;
        return info;
    }

    uint8_t f0513[2];
    if (with_retry(3, [&]() { return read_model_f0513(f0513); })) {
        snprintf(info.model, sizeof(info.model), "BL%X%X", f0513[0], f0513[1]);
        send_clear(); // mirrors get_f0513_model()'s CLEAR_CMD follow-up
        info.kind = PackKind::F0513;
        info.limited_diagnostics_only = true;
        info.frame_repair_supported = false; // F0513 packs are read-only -- see PROTOCOL.md
        g_pack_kind = PackKind::F0513;
        info.ok = true;
        return info;
    }

    snprintf(info.error, sizeof(info.error), "Battery present but model is not supported.");
    return info;
}

static BatteryData read_data_standard_once() {
    BatteryData d;
    memset(&d, 0, sizeof(d));

    uint8_t rsp[29];
    bus_power(true);
    const uint8_t cmd[4] = {0xD7, 0x00, 0x00, 0xFF};
    skiprom_cmd(cmd, 4, rsp, 29);
    bus_power(false);

    memcpy(d.raw, rsp, 29);
    d.raw_len = 29;

    if (all_ff(rsp, 29)) {
        snprintf(d.error, sizeof(d.error), "No response from battery. Check it is seated correctly.");
        return d;
    }

    d.v_pack = u16le(rsp, 0) / 1000.0f;
    d.v_cell[0] = u16le(rsp, 2) / 1000.0f;
    d.v_cell[1] = u16le(rsp, 4) / 1000.0f;
    d.v_cell[2] = u16le(rsp, 6) / 1000.0f;
    d.v_cell[3] = u16le(rsp, 8) / 1000.0f;
    d.v_cell[4] = u16le(rsp, 10) / 1000.0f;
    // rsp[12:14] is unused, matching the PC client.
    d.t_cell = u16le(rsp, 14) / 100.0f;
    d.t_mosfet = u16le(rsp, 16) / 100.0f;
    d.has_mosfet_temp = true;

    float lo = d.v_cell[0], hi = d.v_cell[0];
    for (uint8_t i = 1; i < 5; i++) {
        if (d.v_cell[i] < lo) lo = d.v_cell[i];
        if (d.v_cell[i] > hi) hi = d.v_cell[i];
    }
    d.v_diff = hi - lo;
    d.ok = true;
    return d;
}

static BatteryData read_data_standard() {
    BatteryData d;
    for (uint8_t i = 0; i < 3; i++) {
        d = read_data_standard_once();
        if (d.ok) {
            return d;
        }
        if (i < 2) {
            delay(50);
        }
    }
    return d;
}

static BatteryData read_data_f0513_once() {
    BatteryData d;
    memset(&d, 0, sizeof(d));

    send_clear();
    send_clear();

    uint8_t raw[10]; // 5 cells x 2 bytes, kept for the raw-bytes debug view
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t rsp[2];
        bus_power(true);
        const uint8_t fn[1] = {(uint8_t)(0x31 + i)};
        skiprom_cmd(fn, 1, rsp, 2);
        bus_power(false);
        raw[i * 2] = rsp[0];
        raw[i * 2 + 1] = rsp[1];
        d.v_cell[i] = u16le(rsp, 0) / 1000.0f;
    }

    uint8_t rspT[2];
    bus_power(true);
    const uint8_t fnT[1] = {0x52};
    skiprom_cmd(fnT, 1, rspT, 2);
    bus_power(false);
    d.t_cell = u16le(rspT, 0) / 100.0f;
    d.has_mosfet_temp = false;

    memcpy(d.raw, raw, 10);
    memcpy(d.raw + 10, rspT, 2);
    d.raw_len = 12;

    if (all_ff(raw, 10) && all_ff(rspT, 2)) {
        snprintf(d.error, sizeof(d.error), "No response from battery. Check it is seated correctly.");
        return d;
    }

    d.v_pack = 0;
    float lo = d.v_cell[0], hi = d.v_cell[0];
    for (uint8_t i = 0; i < 5; i++) {
        d.v_pack += d.v_cell[i];
        if (d.v_cell[i] < lo) lo = d.v_cell[i];
        if (d.v_cell[i] > hi) hi = d.v_cell[i];
    }
    d.v_diff = hi - lo;
    d.ok = true;
    return d;
}

static BatteryData read_data_f0513() {
    BatteryData d;
    for (uint8_t i = 0; i < 3; i++) {
        d = read_data_f0513_once();
        if (d.ok) {
            return d;
        }
        if (i < 2) {
            delay(50);
        }
    }
    return d;
}

BatteryData read_data() {
    if (g_pack_kind == PackKind::F0513) {
        return read_data_f0513();
    }
    return read_data_standard();
}

bool leds(bool on, char *error, size_t error_len) {
    if (!with_retry(3, enter_testmode)) {
        snprintf(error, error_len, "No response from battery while entering test mode.");
        return false;
    }
    uint8_t rom[8];
    uint8_t rsp[9]; // full response, not just the first byte -- see PROTOCOL.md
    bus_power(true);
    const uint8_t cmd[2] = {0xDA, (uint8_t)(on ? 0x31 : 0x34)};
    readrom_cmd(cmd, 2, rom, rsp, 9);
    bus_power(false);
    exit_testmode();
    return true;
}

// Sends DA04 (error-register clear) and reports whether the bus stayed
// present throughout. Does not by itself tell you whether the pack is
// unlocked afterward -- call read_message()/read_info() again to check.
static bool send_da04() {
    if (!with_retry(3, enter_testmode)) {
        return false;
    }
    uint8_t rom[8];
    uint8_t rsp[9];
    bus_power(true);
    const uint8_t cmd[2] = {0xDA, 0x04};
    bool present = readrom_cmd(cmd, 2, rom, rsp, 9);
    bus_power(false);
    exit_testmode();
    return present;
}

// Frame write, arm+write+store -- see PROTOCOL.md's frame-repair section.
// `frame` must already have nybble 34/CS0/CS2 corrected by the caller.
static bool write_frame(const uint8_t frame[32]) {
    if (!with_retry(3, enter_testmode)) {
        return false;
    }

    // Arm: tells the BMS a frame write is coming.
    {
        uint8_t rsp[32];
        bus_power(true);
        const uint8_t cmd[2] = {0xF0, 0x00};
        bool present = skiprom_cmd(cmd, 2, rsp, 32);
        bus_power(false);
        if (!present) {
            exit_testmode();
            return false;
        }
    }
    delay(30);

    // Write: opcode 0x0F, a pad byte, then the 32-byte frame verbatim.
    {
        uint8_t rom[8];
        uint8_t payload[2 + 32];
        payload[0] = 0x0F;
        payload[1] = 0x00;
        memcpy(&payload[2], frame, 32);
        bus_power(true);
        bool present = readrom_cmd(payload, sizeof(payload), rom, nullptr, 0);
        bus_power(false);
        if (!present) {
            exit_testmode();
            return false;
        }
    }
    delay(30);

    // Store: commits the write to flash.
    {
        uint8_t rom[8];
        const uint8_t cmd[2] = {0x55, 0xA5};
        bus_power(true);
        bool present = readrom_cmd(cmd, 2, rom, nullptr, 0);
        bus_power(false);
        if (!present) {
            exit_testmode();
            return false;
        }
    }
    delay(30);

    exit_testmode();
    return true;
}

// One frame-repair attempt: zero nybble 34, recalculate CS0/CS2, write the
// frame back, then poll until the BMS responds again (it goes bus-silent
// for up to a few seconds while committing to flash) and re-read it.
// `msg` is both input and output: replaced with the freshly re-read frame
// on success so the caller can immediately check whether it's unlocked.
static bool repair_frame_once(uint8_t msg[32]) {
    uint8_t frame[32];
    memcpy(frame, msg, 32);
    frame[17] = (uint8_t)(frame[17] & 0xF0); // zero nybble 34, preserve nybble 35
    nyb_set(frame, 41, checksum_calc(frame, 0, 15));  // CS0
    nyb_set(frame, 43, checksum_calc(frame, 32, 40)); // CS2

    if (!write_frame(frame)) {
        return false;
    }

    uint8_t rom[8];
    uint8_t verify[32];
    uint32_t start = millis();
    bool got = false;
    while (millis() - start < 5000) {
        if (read_message(rom, verify)) {
            got = true;
            break;
        }
        delay(200);
    }
    if (!got) {
        return false;
    }
    memcpy(msg, verify, 32);
    return true;
}

static void fill_lock_causes(const uint8_t msg[32], UnlockResult &r) {
    r.lock_cause_cs0 = checksum_calc(msg, 0, 15) != nyb_get(msg, 41);
    r.lock_cause_cs2 = checksum_calc(msg, 32, 40) != nyb_get(msg, 43);
    r.lock_cause_n34 = nyb_get(msg, 34) != 0;
}

// Attempt 1: DA04 error-register clear -- handles ordinary overdischarge/
// overload locks. If the pack is still locked by the charger's own
// criteria (nybble 34, CS0, CS2 -- see PROTOCOL.md), falls back to up to 6
// rounds of frame repair, matching the synrais Makita LXT Battery
// Monitor/Unlocker project's tested behavior. Frame repair is only
// attempted on new-family (byte0=0xF1) standard packs; F0513 and
// unrecognized frame layouts are left untouched.
UnlockResult unlock() {
    UnlockResult result;
    memset(&result, 0, sizeof(result));

    uint8_t rom[8];
    uint8_t msg[32];

    send_da04();
    delay(200); // BMS settle time after DA04, per PROTOCOL.md
    if (!with_retry(5, [&]() { return read_message(rom, msg); })) {
        snprintf(result.error, sizeof(result.error), "No response from battery after DA04.");
        return result;
    }

    fill_lock_causes(msg, result);
    if (!result.lock_cause_cs0 && !result.lock_cause_cs2 && !result.lock_cause_n34) {
        result.ok = true;
        snprintf(result.method, sizeof(result.method), "da04");
        return result;
    }

    if (g_pack_kind != PackKind::STANDARD || msg[0] != 0xF1) {
        result.frame_repair_supported = false;
        snprintf(result.error, sizeof(result.error),
                 "Still locked after DA04. Frame repair isn't supported for this pack.");
        return result;
    }
    result.frame_repair_supported = true;

    for (uint8_t attempt = 1; attempt <= 6; attempt++) {
        if (repair_frame_once(msg)) {
            fill_lock_causes(msg, result);
            result.frame_repair_attempts = attempt;
            if (!result.lock_cause_cs0 && !result.lock_cause_cs2 && !result.lock_cause_n34) {
                result.ok = true;
                snprintf(result.method, sizeof(result.method), "frame-repair");
                return result;
            }
        } else {
            result.frame_repair_attempts = attempt;
        }
    }

    snprintf(result.error, sizeof(result.error),
             "Still locked after DA04 and %u frame-repair attempt(s).", result.frame_repair_attempts);
    return result;
}

} // namespace makita_lxt
