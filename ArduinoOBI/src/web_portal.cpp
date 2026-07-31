#include "web_portal.h"

#ifdef ESP_BUILD

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "makita.h"
#include "index_html.h"
#include "version.h"

static const char *AP_SSID = "OBIWiFi";
static const byte DNS_PORT = 53;
static IPAddress AP_IP(192, 168, 4, 1);

static DNSServer dnsServer;
static WebServer server(80);

static void json_append_escaped(String &out, const char *s) {
    out += '"';
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

static String hex_of(const uint8_t *buf, size_t len) {
    String s;
    s.reserve(len * 3);
    char tmp[4];
    for (size_t i = 0; i < len; i++) {
        snprintf(tmp, sizeof(tmp), i == 0 ? "%02X" : " %02X", buf[i]);
        s += tmp;
    }
    return s;
}

static void sendJson(int code, const String &body) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", body);
}

// Any path that isn't a known API route serves the portal page itself, at
// HTTP 200 -- this is what makes iOS/Android/Windows connectivity checks
// (which all probe different well-known URLs) decide a captive portal is
// present and pop it open automatically.
static void handleRoot() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", INDEX_HTML);
}

static void handleApiVersion() {
    String out = "{";
    out += "\"major\":" + String(ARDUINO_OBI_VERSION_MAJOR);
    out += ",\"minor\":" + String(ARDUINO_OBI_VERSION_MINOR);
    out += ",\"patch\":" + String(ARDUINO_OBI_VERSION_PATCH);
    out += "}";
    sendJson(200, out);
}

static void handleApiReadInfo() {
    makita_lxt::BatteryInfo info = makita_lxt::read_info();

    String out = "{";
    out += "\"ok\":" + String(info.ok ? "true" : "false");
    if (!info.ok) {
        out += ",\"error\":";
        json_append_escaped(out, info.error);
        out += "}";
        sendJson(200, out);
        return;
    }

    out += ",\"kind\":";
    json_append_escaped(out, info.kind == makita_lxt::PackKind::F0513 ? "f0513" : "standard");
    out += ",\"limited\":" + String(info.limited_diagnostics_only ? "true" : "false");
    out += ",\"model\":";
    json_append_escaped(out, info.model);
    out += ",\"romId\":";
    json_append_escaped(out, hex_of(info.rom, 8).c_str());
    out += ",\"message\":";
    json_append_escaped(out, hex_of(info.message, 32).c_str());
    out += ",\"mfgDate\":";
    json_append_escaped(out, info.mfg_date);
    out += ",\"capacityAh\":" + String(info.capacity_ah, 1);
    out += ",\"batteryType\":" + String(info.battery_type);
    out += ",\"chargeCount\":" + String(info.charge_count);
    out += ",\"locked\":" + String(info.locked ? "true" : "false");

    char statusHex[3];
    snprintf(statusHex, sizeof(statusHex), "%02X", info.status_code);
    out += ",\"statusCode\":";
    json_append_escaped(out, statusHex);

    // What the charger itself validates -- see PROTOCOL.md. Can disagree
    // with `locked` above.
    out += ",\"chargerLocked\":" + String(info.charger_locked ? "true" : "false");
    out += ",\"lockCauseCs0\":" + String(info.lock_cause_cs0 ? "true" : "false");
    out += ",\"lockCauseCs2\":" + String(info.lock_cause_cs2 ? "true" : "false");
    out += ",\"lockCauseN34\":" + String(info.lock_cause_n34 ? "true" : "false");
    out += ",\"frameRepairSupported\":" + String(info.frame_repair_supported ? "true" : "false");
    out += "}";
    sendJson(200, out);
}

static void handleApiReadData() {
    makita_lxt::BatteryData d = makita_lxt::read_data();

    String out = "{";
    out += "\"ok\":" + String(d.ok ? "true" : "false");
    if (!d.ok) {
        out += ",\"error\":";
        json_append_escaped(out, d.error);
        out += ",\"raw\":";
        json_append_escaped(out, hex_of(d.raw, d.raw_len).c_str());
        out += "}";
        sendJson(200, out);
        return;
    }

    out += ",\"packVoltage\":" + String(d.v_pack, 3);
    out += ",\"cellVoltages\":[";
    for (int i = 0; i < 5; i++) {
        if (i) out += ",";
        out += String(d.v_cell[i], 3);
    }
    out += "]";
    out += ",\"cellDiff\":" + String(d.v_diff, 3);
    out += ",\"tempCell\":" + String(d.t_cell, 2);
    if (d.has_mosfet_temp) {
        out += ",\"tempMosfet\":" + String(d.t_mosfet, 2);
    } else {
        out += ",\"tempMosfet\":null";
    }
    out += ",\"raw\":";
    json_append_escaped(out, hex_of(d.raw, d.raw_len).c_str());
    out += "}";
    sendJson(200, out);
}

static void handleApiLeds(bool on) {
    char err[64] = {0};
    bool ok = makita_lxt::leds(on, err, sizeof(err));
    String out = "{\"ok\":" + String(ok ? "true" : "false");
    if (!ok) {
        out += ",\"error\":";
        json_append_escaped(out, err);
    }
    out += "}";
    sendJson(200, out);
}
static void handleApiLedsOn() { handleApiLeds(true); }
static void handleApiLedsOff() { handleApiLeds(false); }

// Tries DA04 first, then falls back to frame repair if the pack is still
// locked by the charger's own criteria -- see PROTOCOL.md. Can take up to
// ~30s worst case if several frame-repair rounds are needed; the frontend
// shows a "this can take a while" notice while it's in flight.
static void handleApiUnlock() {
    makita_lxt::UnlockResult r = makita_lxt::unlock();

    String out = "{\"ok\":" + String(r.ok ? "true" : "false");
    if (r.ok) {
        out += ",\"method\":";
        json_append_escaped(out, r.method);
    } else {
        out += ",\"error\":";
        json_append_escaped(out, r.error);
    }
    out += ",\"frameRepairSupported\":" + String(r.frame_repair_supported ? "true" : "false");
    out += ",\"frameRepairAttempts\":" + String(r.frame_repair_attempts);
    out += ",\"lockCauseCs0\":" + String(r.lock_cause_cs0 ? "true" : "false");
    out += ",\"lockCauseCs2\":" + String(r.lock_cause_cs2 ? "true" : "false");
    out += ",\"lockCauseN34\":" + String(r.lock_cause_n34 ? "true" : "false");
    out += "}";
    sendJson(200, out);
}

static void handleApiResetMessage() {
    // Deliberately not wired up: the underlying CLEAN_FRAME_CMD has a known
    // length-mismatch bug (see PROTOCOL.md #5) and writes straight to the
    // battery's stored diagnostic frame. Left disabled until that's fixed
    // and verified against real hardware, same as the PC client's stub.
    sendJson(501, "{\"ok\":false,\"error\":\"Not implemented (disabled for safety, see PROTOCOL.md)\"}");
}

void web_portal_begin() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    // Modem sleep periodically parks the radio between beacon intervals,
    // which can jitter the time-critical OneWire bit-banging in makita.cpp
    // enough to drop a read/write. AP mode has no battery-life reason to
    // keep it enabled here.
    WiFi.setSleep(false);

    dnsServer.start(DNS_PORT, "*", AP_IP);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/generate_204", HTTP_GET, handleRoot);               // Android
    server.on("/gen_204", HTTP_GET, handleRoot);                    // Android
    server.on("/hotspot-detect.html", HTTP_GET, handleRoot);        // Apple
    server.on("/library/test/success.html", HTTP_GET, handleRoot);  // Apple (older)
    server.on("/ncsi.txt", HTTP_GET, handleRoot);                   // Windows
    server.on("/connecttest.txt", HTTP_GET, handleRoot);            // Windows

    server.on("/api/version", HTTP_GET, handleApiVersion);
    server.on("/api/read-info", HTTP_GET, handleApiReadInfo);
    server.on("/api/read-data", HTTP_GET, handleApiReadData);
    server.on("/api/leds-on", HTTP_POST, handleApiLedsOn);
    server.on("/api/leds-off", HTTP_POST, handleApiLedsOff);
    server.on("/api/unlock", HTTP_POST, handleApiUnlock);
    server.on("/api/reset-message", HTTP_POST, handleApiResetMessage);

    server.onNotFound(handleRoot);

    server.begin();
}

void web_portal_loop() {
    dnsServer.processNextRequest();
    server.handleClient();
}

#endif // ESP_BUILD
