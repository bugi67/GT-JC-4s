'use strict';

const $ = id => document.getElementById(id);

// ── i18n ──────────────────────────────────────────────────────────────────────
const STR = {
  en: {
    nav_dashboard: 'Dashboard', nav_presets: 'Presets',
    nav_settings: 'Settings',   nav_maint: 'Maintenance',
    title_tuner: 'Tuner Controls', lbl_mode: 'Mode', mode_noc: 'No C',
    btn_autotune: 'Start AutoTune', btn_abort: 'Abort',
    tune_states: ['Idle', 'Tuning…', 'Done', 'Aborted'],
    title_presets: 'Saved Presets', th_freq: 'Frequency', th_mode: 'Mode',
    lbl_loading: 'Loading…',     no_presets: 'No presets saved',
    btn_delete_all: 'Delete all', btn_del: 'Delete',
    confirm_del_preset: 'Delete preset for {0} kHz?',
    confirm_del_all:    'Delete all presets?',
    lbl_ssid: 'SSID', lbl_pass: 'Password',
    ph_pass: '(leave empty to keep current)', ph_ssid: 'Network name',
    ph_mqtt: '192.168.1.x', ph_url: 'https://github.com/…/release.json',
    lbl_mqtt_srv: 'Server', lbl_mqtt_port: 'Port', lbl_mqtt_en: 'Enabled',
    lbl_threshold: 'Return Loss threshold [dB]', lbl_tx_level: 'Min Vfwd (TX detect)',
    lbl_coarse_l: 'Coarse step L', lbl_coarse_c: 'Coarse step C',
    lbl_ota_url: 'Manifest URL', lbl_log_level: 'Log level',
    btn_save: 'Save', cfg_ok: 'Saved.', cfg_err: 'Error saving!',
    title_github_ota: 'GitHub OTA', btn_check: 'Check for update',
    btn_install: 'Install', ota_checking: 'Checking GitHub…',
    ota_uptodate: 'Firmware is up to date.',  ota_available: 'Update available',
    ota_states: ['Idle', 'Checking', 'Downloading FW…', 'Downloading FS…', 'Done', 'Error'],
    confirm_install: 'Install update v{0}?',
    title_local_ota: 'Local Upload',
    lbl_fw_file: 'Firmware (firmware.bin)',    btn_upload_fw: 'Upload FW',
    lbl_fs_file: 'Filesystem (littlefs.bin)',  btn_upload_fs: 'Upload FS',
    uploading: 'Uploading…',
    upload_fs_note: 'Uploading filesystem… (reboot follows)',
    fw_ok: 'Firmware uploaded – upload filesystem too.',
    fw_err: 'Firmware upload failed!', fs_err: 'Filesystem upload failed!',
    title_system: 'System', btn_reboot: 'Reboot',
    confirm_reboot: 'Reboot the device?', rebooting: 'Rebooting…',
    sp_header: 'Tuner Stats', lbl_swr: 'SWR', lbl_freq_kHz: 'Freq (kHz)',
    sp_tune_status: 'Tune Status', sp_ota: 'OTA',
    tune_idle: 'Idle', lbl_fw: 'Firmware',
  },
  de: {
    nav_dashboard: 'Dashboard', nav_presets: 'Presets',
    nav_settings: 'Einstellungen', nav_maint: 'Wartung',
    title_tuner: 'Tuner-Steuerung', lbl_mode: 'Modus', mode_noc: 'Kein C',
    btn_autotune: 'AutoTune starten', btn_abort: 'Abbrechen',
    tune_states: ['Bereit', 'Abstimmen…', 'Fertig', 'Abgebrochen'],
    title_presets: 'Gespeicherte Presets', th_freq: 'Frequenz', th_mode: 'Modus',
    lbl_loading: 'Lade…',           no_presets: 'Keine Presets gespeichert',
    btn_delete_all: 'Alle löschen',  btn_del: 'Löschen',
    confirm_del_preset: 'Preset für {0} kHz löschen?',
    confirm_del_all:    'Alle Presets löschen?',
    lbl_ssid: 'SSID', lbl_pass: 'Passwort',
    ph_pass: '(leer = unverändert)', ph_ssid: 'Netzwerk-Name',
    ph_mqtt: '192.168.1.x', ph_url: 'https://github.com/…/release.json',
    lbl_mqtt_srv: 'Server', lbl_mqtt_port: 'Port', lbl_mqtt_en: 'Aktiviert',
    lbl_threshold: 'Rückflussdämpfung Schwellwert [dB]', lbl_tx_level: 'Min. Vfwd (TX-Erkennung)',
    lbl_coarse_l: 'Grobschritt L', lbl_coarse_c: 'Grobschritt C',
    lbl_ota_url: 'Manifest-URL', lbl_log_level: 'Log-Level',
    btn_save: 'Speichern', cfg_ok: 'Gespeichert.', cfg_err: 'Fehler!',
    title_github_ota: 'GitHub OTA', btn_check: 'Update prüfen',
    btn_install: 'Installieren', ota_checking: 'Prüfe GitHub…',
    ota_uptodate: 'Firmware ist aktuell.', ota_available: 'Update verfügbar',
    ota_states: ['Bereit', 'Prüfen', 'FW laden…', 'FS laden…', 'Fertig', 'Fehler'],
    confirm_install: 'Update v{0} installieren?',
    title_local_ota: 'Lokaler Upload',
    lbl_fw_file: 'Firmware (firmware.bin)',     btn_upload_fw: 'FW hochladen',
    lbl_fs_file: 'Dateisystem (littlefs.bin)',  btn_upload_fs: 'FS hochladen',
    uploading: 'Lade hoch…',
    upload_fs_note: 'Lade Dateisystem hoch… (Neustart folgt)',
    fw_ok: 'Firmware hochgeladen – Dateisystem hochladen.',
    fw_err: 'Firmware-Upload fehlgeschlagen!', fs_err: 'Dateisystem-Upload fehlgeschlagen!',
    title_system: 'System', btn_reboot: 'Neustart',
    confirm_reboot: 'Gerät neu starten?', rebooting: 'Starte neu…',
    sp_header: 'Tuner-Status', lbl_swr: 'SWR', lbl_freq_kHz: 'Frequenz (kHz)',
    sp_tune_status: 'Abstimmung', sp_ota: 'OTA',
    tune_idle: 'Bereit', lbl_fw: 'Firmware',
  }
};

let lang = localStorage.getItem('lang') || 'en';

function t(key, ...args) {
  let s = (STR[lang][key] ?? STR.en[key]) ?? key;
  args.forEach((a, i) => { s = s.replace('{' + i + '}', a); });
  return s;
}

function applyI18n() {
  document.documentElement.lang = lang;
  document.querySelectorAll('[data-i18n]').forEach(el => {
    el.textContent = t(el.dataset.i18n);
  });
  const ph = {
    'cfg-wifi-ssid': 'ph_ssid', 'cfg-wifi-pass': 'ph_pass',
    'cfg-mqtt-server': 'ph_mqtt', 'cfg-ota-url': 'ph_url',
  };
  Object.entries(ph).forEach(([id, key]) => {
    const el = $(id);
    if (el) el.placeholder = t(key);
  });
  $('btn-lang').textContent = lang === 'en' ? 'DE' : 'EN';
  syncStats();
}

// ── Theme ─────────────────────────────────────────────────────────────────────
let theme = localStorage.getItem('theme') || 'dark';
function applyTheme() {
  document.documentElement.dataset.theme = theme;
  $('btn-theme').textContent = theme === 'dark' ? '☀️' : '🌙';
}

$('btn-theme').addEventListener('click', () => {
  theme = theme === 'dark' ? 'light' : 'dark';
  localStorage.setItem('theme', theme);
  applyTheme();
});
$('btn-lang').addEventListener('click', () => {
  lang = lang === 'en' ? 'de' : 'en';
  localStorage.setItem('lang', lang);
  applyI18n();
});

// ── State ─────────────────────────────────────────────────────────────────────
const state = {
  L: 0, C: 0, mode: 1, freq_kHz: 0,
  swr: 0, returnLoss: 0,
  tuneState: 0, tuneProgress: 0,
  otaState: 0,  otaProgress: 0,
  rssi: 0, fwVersion: '?'
};

const post = (url, body) => fetch(url, {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(body)
});
const del = url => fetch(url, { method: 'DELETE' });

// ── Page navigation ───────────────────────────────────────────────────────────
function showPage(name) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.snav-btn').forEach(b => b.classList.remove('active'));
  $('page-' + name).classList.add('active');
  document.querySelector(`.snav-btn[data-page="${name}"]`).classList.add('active');
  if (name === 'presets')  loadPresets();
  if (name === 'settings') loadConfig();
}

document.querySelectorAll('.snav-btn[data-page]').forEach(b => {
  b.addEventListener('click', () => showPage(b.dataset.page));
});

// ── SWR helpers ───────────────────────────────────────────────────────────────
function swrCls(swr) {
  if (swr <= 0)  return '';
  if (swr < 1.5) return '';
  if (swr < 2.5) return 'warn';
  return 'bad';
}

// ── Stats panel ───────────────────────────────────────────────────────────────
function syncStats() {
  // Hero SWR
  const heroEl = $('sp-swr');
  const cls = swrCls(state.swr);
  heroEl.textContent = state.swr > 0 ? state.swr.toFixed(2) : '—';
  heroEl.className = 'sp-hero-num' + (cls ? ' ' + cls : '');

  // Grid cells
  $('sp-rl').textContent   = state.returnLoss > 0 ? state.returnLoss.toFixed(1) : '—';
  $('sp-freq').textContent = state.freq_kHz || '—';
  $('sp-l').textContent    = state.L;
  $('sp-c').textContent    = state.C;

  // Firmware / RSSI
  $('sp-fw').textContent   = state.fwVersion ? 'v' + state.fwVersion : '—';
  $('sp-rssi').textContent = state.rssi ? state.rssi + ' dBm' : '—';

  // Tune status
  const tuning = state.tuneState === 1;
  const labels = t('tune_states');
  $('sp-tune-lbl').textContent  = Array.isArray(labels) ? (labels[state.tuneState] || '') : '';
  const dot = $('sp-tune-dot');
  dot.className = 'sp-dot' + (tuning ? ' running' : state.tuneState === 2 ? ' done' : '');
  $('sp-tune-wrap').style.display = tuning ? 'block' : 'none';
  if (tuning) $('sp-tune-bar').style.width = state.tuneProgress + '%';

  // OTA status
  const otaLabels = t('ota_states');
  $('sp-ota-lbl').textContent = Array.isArray(otaLabels) ? (otaLabels[state.otaState] || '') : '';
  const otaDot = $('sp-ota-dot');
  otaDot.className = 'sp-dot' +
    (state.otaState === 1 || state.otaState === 2 || state.otaState === 3 ? ' running' :
     state.otaState === 4 ? ' done' :
     state.otaState === 5 ? ' error' : '');
  $('sp-ota-wrap').style.display = (state.otaState > 0 && state.otaState < 4) ? 'block' : 'none';
  if (state.otaProgress) $('sp-ota-bar').style.width = state.otaProgress + '%';
}

// ── Dashboard sync ────────────────────────────────────────────────────────────
function syncDashboard() {
  $('slider-L').value = state.L;  $('num-L').value = state.L;
  $('slider-C').value = state.C;  $('num-C').value = state.C;

  document.querySelectorAll('.seg-btn').forEach(b => {
    b.classList.toggle('active', +b.dataset.mode === state.mode);
  });

  const tuning = state.tuneState === 1;
  $('btn-autotune').disabled         = tuning;
  $('btn-abort').disabled            = !tuning;
  $('tune-prog-wrap').style.display  = tuning ? 'block' : 'none';
  if (tuning) $('tune-prog-bar').style.width = state.tuneProgress + '%';
  const ls = t('tune_states');
  $('tune-status').textContent = Array.isArray(ls) ? (ls[state.tuneState] || '') : '';

  syncStats();
}

// Slider ↔ number-input binding
function bindSlider(id) {
  const slider = $('slider-' + id);
  const num    = $('num-' + id);
  const send   = v => { state[id] = v; slider.value = v; num.value = v;
    post('/api/tune', { L: state.L, C: state.C, mode: state.mode }); };
  slider.addEventListener('input',  () => { num.value = slider.value; });
  slider.addEventListener('change', () => send(+slider.value));
  num.addEventListener('change', () => {
    const v = Math.max(+num.min, Math.min(+num.max, parseInt(num.value) || 0));
    send(v);
  });
}
bindSlider('L');
bindSlider('C');

document.querySelectorAll('.seg-btn').forEach(b => {
  b.addEventListener('click', () => {
    state.mode = +b.dataset.mode;
    document.querySelectorAll('.seg-btn').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    post('/api/tune', { L: state.L, C: state.C, mode: state.mode });
  });
});

$('btn-autotune').addEventListener('click', () => post('/api/autotune', { start: true }));
$('btn-abort').addEventListener('click',    () => post('/api/autotune', { start: false }));

// ── SSE ───────────────────────────────────────────────────────────────────────
function startSSE() {
  const src = new EventSource('/events');
  src.onmessage = e => { Object.assign(state, JSON.parse(e.data)); syncDashboard(); };
  src.onerror   = () => setTimeout(startSSE, 5000);
}

// ── Status poll ───────────────────────────────────────────────────────────────
async function fetchStatus() {
  try {
    const d = await fetch('/api/status').then(r => r.json());
    Object.assign(state, d);
    syncDashboard();
  } catch(e) { /* SSE keeps UI fresh */ }
}

// ── Presets ───────────────────────────────────────────────────────────────────
const MODE_NAMES = { 1: 'C@TRX', 2: 'C@ANT', 3: '—' };

async function loadPresets() {
  try {
    const rows = await fetch('/api/presets').then(r => r.json());
    const tbody = $('presets-tbody');
    tbody.innerHTML = '';
    if (!rows.length) {
      tbody.innerHTML = `<tr><td colspan="5" class="empty-cell">${t('no_presets')}</td></tr>`;
      return;
    }
    rows.forEach(p => {
      const tr = document.createElement('tr');
      tr.innerHTML =
        `<td>${p.freq_kHz} kHz</td><td>${p.L}</td><td>${p.C}</td>` +
        `<td>${MODE_NAMES[p.mode] ?? p.mode}</td>` +
        `<td><button class="btn btn-danger" style="padding:3px 10px;font-size:.74rem"` +
        ` onclick="deletePreset(${p.freq_kHz})">${t('btn_del')}</button></td>`;
      tbody.appendChild(tr);
    });
  } catch(e) {
    $('presets-tbody').innerHTML = `<tr><td colspan="5" class="empty-cell">Error</td></tr>`;
  }
}

window.deletePreset = async freq => {
  if (!confirm(t('confirm_del_preset', freq))) return;
  await del('/api/presets/' + freq);
  loadPresets();
};

$('btn-delete-all').addEventListener('click', async () => {
  if (!confirm(t('confirm_del_all'))) return;
  await del('/api/presets');
  loadPresets();
});

// ── Config ────────────────────────────────────────────────────────────────────
async function loadConfig() {
  try {
    const c = await fetch('/api/config').then(r => r.json());
    $('cfg-wifi-ssid').value   = c.wifi_ssid        ?? '';
    $('cfg-mqtt-server').value = c.mqtt_server       ?? '';
    $('cfg-mqtt-port').value   = c.mqtt_port         ?? 1883;
    $('cfg-mqtt-en').checked   = c.mqtt_enabled      !== false;
    $('cfg-tune-thr').value    = c.tune_threshold    ?? 18;
    $('cfg-tune-tx').value     = c.tune_tx_level     ?? 10;
    $('cfg-coarse-l').value    = c.coarse_step_l     ?? 64;
    $('cfg-coarse-c').value    = c.coarse_step_c     ?? 16;
    $('cfg-ota-url').value     = c.ota_manifest_url  ?? '';
    $('cfg-log-level').value   = c.log_level         ?? 2;
  } catch(e) { /* ignore */ }
}

$('btn-save-cfg').addEventListener('click', async () => {
  const st   = $('cfg-status');
  const body = {
    wifi_ssid:        $('cfg-wifi-ssid').value,
    mqtt_server:      $('cfg-mqtt-server').value,
    mqtt_port:        +$('cfg-mqtt-port').value,
    mqtt_enabled:     $('cfg-mqtt-en').checked,
    tune_threshold:   +$('cfg-tune-thr').value,
    tune_tx_level:    +$('cfg-tune-tx').value,
    coarse_step_l:    +$('cfg-coarse-l').value,
    coarse_step_c:    +$('cfg-coarse-c').value,
    ota_manifest_url: $('cfg-ota-url').value,
    log_level:        +$('cfg-log-level').value,
  };
  const pass = $('cfg-wifi-pass').value;
  if (pass) body.wifi_pass = pass;
  try {
    const d = await post('/api/config', body).then(r => r.json());
    st.textContent = t(d.ok ? 'cfg_ok' : 'cfg_err');
    st.className   = 'status-line ' + (d.ok ? 'ok' : 'error');
  } catch(e) {
    st.textContent = t('cfg_err');
    st.className   = 'status-line error';
  }
  $('cfg-wifi-pass').value = '';
});

// ── OTA: GitHub ───────────────────────────────────────────────────────────────
let pendingOtaVer = null;

$('btn-ota-check').addEventListener('click', async () => {
  $('ota-info').textContent     = t('ota_checking');
  $('btn-ota-install').disabled = true;
  pendingOtaVer = null;
  try {
    const d = await fetch('/ota/github/check').then(r => r.json());
    if (d.updateAvailable) {
      pendingOtaVer = d.version;
      $('ota-info').innerHTML =
        `<strong>${t('ota_available')}: v${d.version}</strong><br>` +
        `FW: ${(d.fwSize/1024).toFixed(0)} KB &nbsp;|&nbsp; FS: ${(d.fsSize/1024).toFixed(0)} KB` +
        (d.changelog ? `<br><em>${d.changelog}</em>` : '');
      $('btn-ota-install').disabled = false;
    } else {
      $('ota-info').textContent = t('ota_uptodate');
    }
  } catch(e) { $('ota-info').textContent = 'Error: ' + e.message; }
});

$('btn-ota-install').addEventListener('click', async () => {
  if (!confirm(t('confirm_install', pendingOtaVer || '?'))) return;
  $('btn-ota-install').disabled = true;
  await fetch('/ota/github/install', { method: 'POST' });
});

// ── OTA: Local upload ─────────────────────────────────────────────────────────
async function uploadBin(fileId, url, isFS) {
  const file = $(fileId).files[0];
  const st   = $('local-status');
  if (!file) return;
  st.textContent = isFS ? t('upload_fs_note') : t('uploading');
  st.className   = 'status-line';
  const form = new FormData();
  form.append('file', file);
  try {
    const d = await fetch(url, { method: 'POST', body: form }).then(r => r.json());
    if (!isFS) {
      st.textContent = d.ok ? t('fw_ok') : t('fw_err');
      st.className   = 'status-line ' + (d.ok ? 'ok' : 'error');
    }
  } catch(e) {
    st.textContent = isFS ? t('fs_err') : t('fw_err');
    st.className   = 'status-line error';
  }
}
$('btn-local-fw').addEventListener('click', () => uploadBin('fw-file', '/ota/local/fw', false));
$('btn-local-fs').addEventListener('click', () => uploadBin('fs-file', '/ota/local/fs', true));

// ── Reboot ────────────────────────────────────────────────────────────────────
$('btn-reboot').addEventListener('click', async () => {
  if (!confirm(t('confirm_reboot'))) return;
  $('maint-status').textContent = t('rebooting');
  $('maint-status').className   = 'status-line warn';
  await fetch('/api/reboot', { method: 'POST' }).catch(() => {});
});

// ── Boot ──────────────────────────────────────────────────────────────────────
applyTheme();
applyI18n();
fetchStatus();
startSSE();
setInterval(fetchStatus, 30000);
