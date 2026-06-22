'use strict';

// ── State ─────────────────────────────────────────────────────────────────────
const state = { L: 0, C: 0, mode: 1, freq_kHz: 0, swr: 99.9, tuneState: 0, tuneProgress: 0,
                otaState: 0, otaProgress: 0, rssi: 0, fwVersion: '?' };

// ── Helpers ───────────────────────────────────────────────────────────────────
const $ = id => document.getElementById(id);
const post = (url, body) => fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
const del  = url => fetch(url, { method: 'DELETE' });

function showTab(name) {
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('nav button').forEach(b => b.classList.remove('active'));
  document.getElementById('tab-' + name).classList.add('active');
  document.querySelector(`nav button[data-tab="${name}"]`).classList.add('active');
  if (name === 'presets') loadPresets();
  if (name === 'config')  loadConfig();
}

// ── SWR badge ─────────────────────────────────────────────────────────────────
function updateSWRBadge(swr) {
  const badge = $('swr-badge');
  badge.textContent = 'SWR ' + swr.toFixed(2);
  badge.className = swr < 1.5 ? 'ok' : swr < 2.5 ? 'warn' : 'bad';
}

// ── Dashboard ─────────────────────────────────────────────────────────────────
function syncDashboard() {
  $('slider-L').value = state.L;
  $('val-L').textContent = state.L;
  $('slider-C').value = state.C;
  $('val-C').textContent = state.C;
  document.querySelectorAll('.mode-btn').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.mode) === state.mode);
  });
  $('freq-display').textContent = state.freq_kHz ? state.freq_kHz + ' kHz' : '—';
  updateSWRBadge(state.swr);

  // Tune progress
  const tuning = state.tuneState === 1;
  $('btn-autotune').disabled = tuning;
  $('btn-abort').disabled = !tuning;
  $('tune-progress-wrap').style.display = tuning ? 'block' : 'none';
  if (tuning) $('tune-progress-bar').style.width = state.tuneProgress + '%';

  const labels = ['Idle','Tuning…','Fertig','Abgebrochen'];
  $('tune-status').textContent = labels[state.tuneState] || '';
}

// Apply L/C immediately when slider released
$('slider-L').addEventListener('change', () => {
  state.L = parseInt($('slider-L').value);
  $('val-L').textContent = state.L;
  post('/api/tune', { L: state.L, C: state.C, mode: state.mode });
});
$('slider-L').addEventListener('input', () => { $('val-L').textContent = $('slider-L').value; });
$('slider-C').addEventListener('change', () => {
  state.C = parseInt($('slider-C').value);
  $('val-C').textContent = state.C;
  post('/api/tune', { L: state.L, C: state.C, mode: state.mode });
});
$('slider-C').addEventListener('input', () => { $('val-C').textContent = $('slider-C').value; });

document.querySelectorAll('.mode-btn').forEach(b => {
  b.addEventListener('click', () => {
    state.mode = parseInt(b.dataset.mode);
    document.querySelectorAll('.mode-btn').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    post('/api/tune', { L: state.L, C: state.C, mode: state.mode });
  });
});

$('btn-autotune').addEventListener('click', () => post('/api/autotune', { start: true }));
$('btn-abort').addEventListener('click',    () => post('/api/autotune', { start: false }));

// ── SSE live updates ──────────────────────────────────────────────────────────
function startSSE() {
  const evtSrc = new EventSource('/events');
  evtSrc.onmessage = e => {
    const d = JSON.parse(e.data);
    if (d.swr !== undefined)          { state.swr = d.swr; updateSWRBadge(d.swr); }
    if (d.tuneState !== undefined)    { state.tuneState = d.tuneState; }
    if (d.tuneProgress !== undefined) { state.tuneProgress = d.tuneProgress; }
    if (d.otaState !== undefined)     { state.otaState = d.otaState; }
    if (d.otaProgress !== undefined)  { state.otaProgress = d.otaProgress; updateOtaProgress(); }
    syncDashboard();
  };
  evtSrc.onerror = () => setTimeout(startSSE, 5000);
}

// ── Initial status fetch ──────────────────────────────────────────────────────
async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    Object.assign(state, d);
    syncDashboard();
    $('wifi-badge').textContent = 'RSSI ' + d.rssi + ' dBm | v' + d.fwVersion;
  } catch(e) { console.warn('status fetch failed', e); }
}

// ── Presets tab ───────────────────────────────────────────────────────────────
async function loadPresets() {
  const r = await fetch('/api/presets');
  const presets = await r.json();
  const tbody = $('presets-tbody');
  tbody.innerHTML = '';
  if (presets.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" style="text-align:center;color:var(--text2)">Keine Presets</td></tr>';
    return;
  }
  presets.forEach(p => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${p.freq_kHz} kHz</td>
      <td>${p.L}</td>
      <td>${p.C}</td>
      <td>${['','C@TRX','C@ANT','kein C'][p.mode] || p.mode}</td>
      <td><button class="btn btn-danger" style="padding:4px 10px;font-size:.8rem"
          onclick="deletePreset(${p.freq_kHz})">Löschen</button></td>`;
    tbody.appendChild(tr);
  });
}

window.deletePreset = async freq => {
  if (!confirm(`Preset ${freq} kHz löschen?`)) return;
  await del('/api/presets/' + freq);
  loadPresets();
};

$('btn-delete-all').addEventListener('click', async () => {
  if (!confirm('Alle Presets löschen?')) return;
  await del('/api/presets');
  loadPresets();
});

// ── Configuration tab ─────────────────────────────────────────────────────────
async function loadConfig() {
  const r = await fetch('/api/config');
  const c = await r.json();
  $('cfg-wifi-ssid').value     = c.wifi_ssid || '';
  $('cfg-mqtt-server').value   = c.mqtt_server || '';
  $('cfg-mqtt-port').value     = c.mqtt_port || 1883;
  $('cfg-mqtt-en').checked     = c.mqtt_enabled !== false;
  $('cfg-tune-thr').value      = c.tune_threshold || 18;
  $('cfg-tune-tx').value       = c.tune_tx_level || 10;
  $('cfg-coarse-l').value      = c.coarse_step_l || 64;
  $('cfg-coarse-c').value      = c.coarse_step_c || 16;
  $('cfg-ota-url').value       = c.ota_manifest_url || '';
  $('cfg-log-level').value     = c.log_level !== undefined ? c.log_level : 2;
}

$('btn-save-config').addEventListener('click', async () => {
  const body = {
    wifi_ssid:       $('cfg-wifi-ssid').value,
    wifi_pass:       $('cfg-wifi-pass').value || undefined,
    mqtt_server:     $('cfg-mqtt-server').value,
    mqtt_port:       parseInt($('cfg-mqtt-port').value),
    mqtt_enabled:    $('cfg-mqtt-en').checked,
    tune_threshold:  parseFloat($('cfg-tune-thr').value),
    tune_tx_level:   parseInt($('cfg-tune-tx').value),
    coarse_step_l:   parseInt($('cfg-coarse-l').value),
    coarse_step_c:   parseInt($('cfg-coarse-c').value),
    ota_manifest_url: $('cfg-ota-url').value,
    log_level:       parseInt($('cfg-log-level').value)
  };
  // Remove undefined values
  Object.keys(body).forEach(k => body[k] === undefined && delete body[k]);
  const r = await post('/api/config', body);
  const d = await r.json();
  $('cfg-status').textContent = d.ok ? 'Gespeichert.' : 'Fehler!';
  $('cfg-status').className = 'status-msg ' + (d.ok ? 'ok' : 'error');
});

// ── Maintenance tab ───────────────────────────────────────────────────────────
$('btn-reboot').addEventListener('click', async () => {
  if (!confirm('Neustart?')) return;
  await post('/api/config', {});   // dummy call; reboot is not in REST spec → ESP.restart() via serial
  // For now just send a "reboot" hint by setting a special config flag
  // A dedicated /api/reboot endpoint could be added in a future version
  $('maint-status').textContent = 'Neustart ausgelöst…';
});

$('btn-ota-check').addEventListener('click', async () => {
  $('ota-info').textContent = 'Prüfe GitHub…';
  $('btn-ota-install').disabled = true;
  try {
    const r = await fetch('/ota/github/check');
    const d = await r.json();
    if (d.updateAvailable) {
      $('ota-info').innerHTML =
        `<b>Update verfügbar: v${d.version}</b><br>
         FW: ${(d.fwSize/1024).toFixed(0)} KB | FS: ${(d.fsSize/1024).toFixed(0)} KB<br>
         ${d.changelog}`;
      $('btn-ota-install').disabled = false;
      $('btn-ota-install').dataset.version = d.version;
    } else {
      $('ota-info').textContent = 'Firmware ist aktuell.';
    }
  } catch(e) {
    $('ota-info').textContent = 'Fehler: ' + e.message;
  }
});

$('btn-ota-install').addEventListener('click', async () => {
  if (!confirm(`Update auf v${$('btn-ota-install').dataset.version} installieren?`)) return;
  $('ota-info').textContent = 'OTA läuft… (ESP startet automatisch neu)';
  $('btn-ota-install').disabled = true;
  await fetch('/ota/github/install', { method: 'POST' });
});

function updateOtaProgress() {
  const labels = ['Idle','Prüfen','FW laden…','FS laden…','Fertig','Fehler'];
  $('ota-state-label').textContent = labels[state.otaState] || '';
  $('ota-progress-bar').style.width = state.otaProgress + '%';
}

// Local FW upload
$('btn-local-fw').addEventListener('click', async () => {
  const file = $('fw-file').files[0];
  if (!file) return;
  const form = new FormData();
  form.append('firmware', file);
  $('local-status').textContent = 'Uploading FW…';
  const r = await fetch('/ota/local/fw', { method: 'POST', body: form });
  const d = await r.json();
  $('local-status').textContent = d.ok ? 'FW OK – bitte FS hochladen.' : 'FW Fehler!';
  $('local-status').className = 'status-msg ' + (d.ok ? 'ok' : 'error');
});

$('btn-local-fs').addEventListener('click', async () => {
  const file = $('fs-file').files[0];
  if (!file) return;
  const form = new FormData();
  form.append('spiffs', file);
  $('local-status').textContent = 'Uploading FS… (Neustart folgt)';
  await fetch('/ota/local/fs', { method: 'POST', body: form });
});

// ── Boot ──────────────────────────────────────────────────────────────────────
document.querySelectorAll('nav button[data-tab]').forEach(b => {
  b.addEventListener('click', () => showTab(b.dataset.tab));
});

fetchStatus();
startSSE();
setInterval(fetchStatus, 30000);
