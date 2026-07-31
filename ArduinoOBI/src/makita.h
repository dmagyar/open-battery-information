#ifndef OBI_MAKITA_H
#define OBI_MAKITA_H

#include <Arduino.h>

// Native (non-USB-framed) implementation of the Makita LXT 1-Wire command
// set described in PROTOCOL.md, for callers running on the MCU itself
// (e.g. the WiFi web portal) rather than relaying bytes from a PC.
namespace makita_lxt {

enum class PackKind : uint8_t { UNKNOWN, STANDARD, F0513 };

struct BatteryInfo {
    bool ok;
    char error[64];

    PackKind kind;
    char model[9];
    bool limited_diagnostics_only; // true for F0513 packs (mirrors the PC client's warning)

    uint8_t rom[8];
    uint8_t message[32];
    char mfg_date[11]; // "DD/MM/20YY"
    float capacity_ah;
    uint8_t battery_type;
    uint16_t charge_count;
    bool locked; // true if the BMS failure-code nybble is non-zero (informational -- NOT what the charger checks)
    uint8_t status_code;

    // What the charger itself actually validates before allowing charge --
    // see PROTOCOL.md. A pack can show `locked=false` above yet still be
    // charger_locked, or vice versa; these are two different fields in the
    // same 32-byte frame.
    bool charger_locked;
    bool lock_cause_cs0;  // nybble 41 checksum (nybbles 0-15) mismatch
    bool lock_cause_cs2;  // nybble 43 checksum (nybbles 32-40) mismatch
    bool lock_cause_n34;  // nybble 34 (charger lock nybble) is non-zero
    bool frame_repair_supported; // new-family (byte0=0xF1) STANDARD pack -- the only layout frame repair is verified against
};

struct BatteryData {
    bool ok;
    char error[64];

    float v_pack;
    float v_cell[5];
    float v_diff;
    float t_cell;
    float t_mosfet;
    bool has_mosfet_temp;

    uint8_t raw[29];
    uint8_t raw_len;
};

// Current best-known pack family, set by a successful read_info() call and
// consumed by read_data()/leds()/unlock() — mirrors the PC client's
// ModuleApplication.command_version instance state.
PackKind last_pack_kind();

BatteryInfo read_info();
BatteryData read_data();
bool leds(bool on, char *error, size_t error_len);

struct UnlockResult {
    bool ok;
    char error[96];
    char method[16]; // "da04" or "frame-repair" once ok; "" if nothing worked
    uint8_t frame_repair_attempts;
    bool frame_repair_supported;
    bool lock_cause_cs0;
    bool lock_cause_cs2;
    bool lock_cause_n34;
};

// Replaces the old DA04-only clear_errors(): tries the standard error-reset
// first, and if the pack is still locked by the charger's own criteria,
// falls back to reading/repairing the 32-byte frame in place (see
// PROTOCOL.md). Read-only ("locked" query) is available via read_info()'s
// lock_cause_* fields without calling this.
UnlockResult unlock();

} // namespace makita_lxt

#endif // OBI_MAKITA_H
