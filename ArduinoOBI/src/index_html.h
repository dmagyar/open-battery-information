#ifndef OBI_INDEX_HTML_H
#define OBI_INDEX_HTML_H

#include <Arduino.h>

// Self-contained captive-portal page: no external CSS/JS/fonts, since the
// phone has no real internet access while joined to the OBIWiFi AP.
const char INDEX_HTML[] PROGMEM = R"HTMLDOC(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>OBI WiFi</title>
<style>
:root {
  --bg: #f5f6f8;
  --card-bg: #ffffff;
  --text: #1a1d23;
  --muted: #6b7280;
  --border: #e2e4e8;
  --accent: #2563eb;
  --accent-text: #ffffff;
  --danger: #dc2626;
  --ok: #16a34a;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #111318;
    --card-bg: #1a1d23;
    --text: #e8eaed;
    --muted: #9aa0a6;
    --border: #2a2e35;
    --accent: #3b82f6;
    --accent-text: #ffffff;
    --danger: #f87171;
    --ok: #4ade80;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  padding: 0 0 5rem;
}
header { padding: 1.25rem 1rem 1rem; text-align: center; }
header h1 { margin: 0; font-size: 1.3rem; }
header p { margin: .25rem 0 0; color: var(--muted); font-size: .85rem; }
main {
  max-width: 640px;
  margin: 0 auto;
  padding: 0 .75rem;
  display: flex;
  flex-direction: column;
  gap: .75rem;
}
.card {
  background: var(--card-bg);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 1rem;
}
.card h2 { margin: 0 0 .75rem; font-size: 1rem; }
.btn-row { display: flex; flex-wrap: wrap; gap: .5rem; }
button {
  flex: 1 1 auto;
  min-height: 44px;
  padding: .6rem 1rem;
  border-radius: 8px;
  border: 1px solid var(--border);
  background: var(--accent);
  color: var(--accent-text);
  font-size: .95rem;
  font-weight: 600;
  cursor: pointer;
}
button:disabled { opacity: .4; cursor: not-allowed; }
button.secondary { background: transparent; color: var(--text); }
table { width: 100%; border-collapse: collapse; margin-top: .75rem; }
table:empty { display: none; margin: 0; }
th, td { text-align: left; padding: .35rem .25rem; border-bottom: 1px solid var(--border); font-size: .9rem; }
th { color: var(--muted); font-weight: 500; width: 45%; }
td { word-break: break-word; }
pre {
  white-space: pre-wrap;
  word-break: break-all;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: .8rem;
  background: var(--bg);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: .6rem;
  margin: .5rem 0 0;
}
details summary { cursor: pointer; font-weight: 600; }
.hint { margin: .5rem 0 0; color: var(--muted); font-size: .8rem; }
#toast {
  position: fixed;
  left: 50%;
  bottom: 1rem;
  transform: translate(-50%, 150%);
  background: var(--ok);
  color: #06210f;
  padding: .6rem 1rem;
  border-radius: 8px;
  font-size: .9rem;
  max-width: 90vw;
  text-align: center;
  transition: transform .2s ease;
  box-shadow: 0 4px 12px rgba(0,0,0,.25);
  z-index: 10;
}
#toast.show { transform: translate(-50%, 0); }
#toast.err { background: var(--danger); color: #2a0a0a; }
</style>
</head>
<body>
<header>
  <h1>Open Battery Information</h1>
  <p>Makita LXT &middot; firmware <span id="fwVersion">-</span> &middot; via OBIWiFi</p>
</header>
<main>
  <section class="card">
    <h2>Battery Info</h2>
    <div class="btn-row">
      <button id="btnReadInfo" data-action="1">Read Battery Info</button>
    </div>
    <table><tbody id="infoTable"></tbody></table>
  </section>

  <section class="card">
    <h2>Battery Data</h2>
    <div class="btn-row">
      <button id="btnReadData" data-action="1" data-requires-info="1" disabled>Read Battery Data</button>
    </div>
    <table><tbody id="dataTable"></tbody></table>
  </section>

  <section class="card">
    <h2>Actions</h2>
    <div class="btn-row">
      <button id="btnLedsOn" data-action="1" data-requires-info="1" disabled class="secondary">LED Test ON</button>
      <button id="btnLedsOff" data-action="1" data-requires-info="1" disabled class="secondary">LED Test OFF</button>
      <button id="btnUnlock" data-action="1" data-requires-info="1" disabled>Unlock Battery</button>
    </div>
    <p id="unlockHint" class="hint">Tries a quick error reset first, then a frame repair if needed &mdash; can take up to ~30s.</p>
    <div class="btn-row" style="margin-top:.5rem">
      <button id="btnResetMsg" disabled class="secondary" title="Not implemented - disabled for safety, see PROTOCOL.md">Reset Battery Message</button>
    </div>
  </section>

  <section class="card">
    <details>
      <summary>Raw bytes (debug)</summary>
      <pre id="rawBytes">(none yet)</pre>
    </details>
  </section>
</main>
<div id="toast"></div>
<script>
(function () {
  var hasInfo = false;

  function $(id) { return document.getElementById(id); }

  function toast(msg, isErr) {
    var el = $('toast');
    el.textContent = msg;
    el.classList.toggle('err', !!isErr);
    el.classList.add('show');
    clearTimeout(toast._t);
    toast._t = setTimeout(function () { el.classList.remove('show'); }, 4000);
  }

  function setBusy(busy) {
    var buttons = document.querySelectorAll('button[data-action]');
    for (var i = 0; i < buttons.length; i++) {
      var btn = buttons[i];
      if (busy) {
        btn.disabled = true;
      } else {
        btn.disabled = btn.dataset.requiresInfo === '1' && !hasInfo;
      }
    }
  }

  function api(path, method) {
    return fetch(path, { method: method || 'GET' }).then(function (res) {
      return res.json().catch(function () {
        throw new Error('Bad response from device');
      }).then(function (data) {
        if (!res.ok || data.ok === false) {
          throw new Error(data.error || ('HTTP ' + res.status));
        }
        return data;
      });
    });
  }

  function row(tbody, label, value) {
    var tr = document.createElement('tr');
    var th = document.createElement('th');
    th.textContent = label;
    var td = document.createElement('td');
    td.textContent = value;
    tr.appendChild(th);
    tr.appendChild(td);
    tbody.appendChild(tr);
  }

  function setRaw(hex) {
    $('rawBytes').textContent = (hex && hex.length) ? hex : '(none yet)';
  }

  // The failure-code nybble ("State" below) and the fields the charger
  // itself checks (nybble 34 + two checksums) are different parts of the
  // same frame and can disagree -- see PROTOCOL.md. Both are shown.
  function lockSummary(cs0, cs2, n34) {
    if (!cs0 && !cs2 && !n34) return 'UNLOCKED';
    var causes = [];
    if (cs0) causes.push('CS0');
    if (cs2) causes.push('CS2');
    if (n34) causes.push('lock nybble');
    return 'LOCKED (' + causes.join(', ') + ')';
  }

  function readInfo() {
    setBusy(true);
    api('/api/read-info').then(function (d) {
      var tbody = $('infoTable');
      tbody.innerHTML = '';
      row(tbody, 'Model', d.model);
      row(tbody, 'Pack type', d.kind === 'f0513' ? 'F0513 (limited diagnostics)' : 'Standard');
      row(tbody, 'ROM ID', d.romId);
      row(tbody, 'Charge count*', d.chargeCount);
      row(tbody, 'State (failure code)', d.locked ? 'LOCKED' : 'UNLOCKED');
      row(tbody, 'Charger lock', lockSummary(d.lockCauseCs0, d.lockCauseCs2, d.lockCauseN34));
      row(tbody, 'Status code', d.statusCode);
      row(tbody, 'Manufacturing date', d.mfgDate);
      row(tbody, 'Capacity', d.capacityAh + ' Ah');
      row(tbody, 'Battery type', d.batteryType);
      row(tbody, 'Battery message', d.message);
      setRaw(d.romId + '  ' + d.message);
      hasInfo = true;
      toast(d.limited ? 'Read OK - limited pack, diagnostics only' : 'Battery info read OK', false);
    }).catch(function (e) {
      toast(e.message, true);
    }).finally(function () {
      setBusy(false);
    });
  }

  function readData() {
    setBusy(true);
    api('/api/read-data').then(function (d) {
      var tbody = $('dataTable');
      tbody.innerHTML = '';
      row(tbody, 'Pack Voltage', d.packVoltage.toFixed(2) + ' V');
      d.cellVoltages.forEach(function (v, i) {
        row(tbody, 'Cell ' + (i + 1) + ' Voltage', v.toFixed(2) + ' V');
      });
      row(tbody, 'Cell Voltage Difference', d.cellDiff.toFixed(2) + ' V');
      row(tbody, 'Temperature Sensor 1', d.tempCell.toFixed(1) + ' °C');
      row(tbody, 'Temperature Sensor 2', d.tempMosfet === null ? 'N/A' : d.tempMosfet.toFixed(1) + ' °C');
      setRaw(d.raw);
      toast('Battery data read OK', false);
    }).catch(function (e) {
      toast(e.message, true);
    }).finally(function () {
      setBusy(false);
    });
  }

  function doAction(path, okMsg) {
    setBusy(true);
    api(path, 'POST').then(function () {
      toast(okMsg, false);
    }).catch(function (e) {
      toast(e.message, true);
    }).finally(function () {
      setBusy(false);
    });
  }

  // Bypasses the shared api() helper (which rejects on ok:false) because
  // the lock-cause fields in a failed unlock response are still wanted
  // here, not just an error string.
  function runUnlock() {
    setBusy(true);
    toast('Unlocking... this can take up to ~30s', false);
    fetch('/api/unlock', { method: 'POST' }).then(function (res) {
      return res.json();
    }).then(function (r) {
      var tbody = $('infoTable');
      var causeRow = null;
      for (var i = 0; i < tbody.rows.length; i++) {
        if (tbody.rows[i].cells[0].textContent === 'Charger lock') { causeRow = tbody.rows[i]; break; }
      }
      var summary = lockSummary(r.lockCauseCs0, r.lockCauseCs2, r.lockCauseN34);
      if (causeRow) causeRow.cells[1].textContent = summary;

      if (r.ok) {
        toast(r.method === 'frame-repair'
          ? 'Unlocked via frame repair (' + r.frameRepairAttempts + ' attempt(s))'
          : 'Unlocked via error reset', false);
      } else {
        toast(r.error || 'Unlock failed', true);
      }
    }).catch(function () {
      toast('Bad response from device', true);
    }).finally(function () {
      setBusy(false);
    });
  }

  function loadVersion() {
    api('/api/version').then(function (v) {
      $('fwVersion').textContent = v.major + '.' + v.minor + '.' + v.patch;
    }).catch(function () { /* non-fatal */ });
  }

  window.addEventListener('DOMContentLoaded', function () {
    $('btnReadInfo').addEventListener('click', readInfo);
    $('btnReadData').addEventListener('click', readData);
    $('btnLedsOn').addEventListener('click', function () { doAction('/api/leds-on', 'LEDs on'); });
    $('btnLedsOff').addEventListener('click', function () { doAction('/api/leds-off', 'LEDs off'); });
    $('btnUnlock').addEventListener('click', runUnlock);
    setBusy(false);
    loadVersion();
  });
})();
</script>
</body>
</html>
)HTMLDOC";

#endif // OBI_INDEX_HTML_H
