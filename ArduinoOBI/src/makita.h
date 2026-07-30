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
    bool locked;
    uint8_t status_code;
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
// consumed by read_data()/leds()/clear_errors() — mirrors the PC client's
// ModuleApplication.command_version instance state.
PackKind last_pack_kind();

BatteryInfo read_info();
BatteryData read_data();
bool leds(bool on, char *error, size_t error_len);
bool clear_errors(char *error, size_t error_len);

} // namespace makita_lxt

#endif // OBI_MAKITA_H
