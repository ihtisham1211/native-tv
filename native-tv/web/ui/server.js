// Shared Server dashboard settings, status and TV app controls.

const REPO = 'https://github.com/ihtisham1211/native-tv';

function setVersion(v) {
  const el = q('version');
  if (!v || el.dataset.v === v) return;
  el.dataset.v = v;
  el.textContent = 'v' + v;
  const commit = v.split('+')[1];
  el.href = commit ? REPO + '/commit/' + commit.split('.')[0] : REPO + '/releases/tag/v' + v;
  el.hidden = false;
}

function renderServerStatus(d) {
  if (!d || !d.ok) return;
  const writable = d.writable !== false;
  if (d.daemon !== undefined) renderServerHealth(d.daemon);
  if (typeof d.tvUpdatesBlocked === 'boolean') {
    updateToggle('tv-updates-block', {
      cls: d.tvUpdatesBlocked ? 'good' : 'idle',
      label: d.tvUpdatesBlocked ? t('server.tvUpdates.blocked', 'Blocked') : t('server.tvUpdates.allowed', 'Allowed'),
      on: d.tvUpdatesBlocked, disabled: !writable
    });
  }
  const dt = q('srv-devtools');
  if (dt && d.devtools !== undefined) {
    const isOff = d.devtools === 'off';
    const noFirewall = d.devtools === 'unsupported';
    dt.hidden = noFirewall;
    const note = q('srv-devtools-nofw');
    if (note) note.hidden = !noFirewall;
    const isOpen = d.devtools === 'open' || d.devtools === 'allowed';
    updateToggle('srv-devtools', {
      cls: isOff ? 'idle' : isOpen ? (d.devtools === 'open' ? 'bad' : 'warn') : 'good',
      label: isOff ? t('server.devtools.off', 'Not running')
        : isOpen ? t('server.devtools.open', 'Open to the network')
        : t('server.devtools.closed', 'Closed to the network'),
      on: isOpen, disabled: !writable || isOff
    });
  }
  if (typeof d.allowTileHiding === 'boolean') updateToggle('exp-tilehiding', {
    cls: d.allowTileHiding ? 'good' : 'idle',
    label: d.allowTileHiding ? t('common.on', 'On') : t('common.off', 'Off'),
    on: d.allowTileHiding, disabled: !writable
  });
  if (typeof d.prometheusEnabled === 'boolean') updateToggle('srv-prometheus', {
    cls: d.prometheusEnabled ? 'good' : 'idle',
    label: d.prometheusEnabled ? t('common.on', 'On') : t('common.off', 'Off'),
    on: d.prometheusEnabled, disabled: !writable
  });
  if (d.watchHistory) {
    updateToggle('srv-watch', {
      cls: d.watchHistory.enabled ? 'good' : 'idle',
      label: d.watchHistory.enabled ? t('common.on', 'On') : t('common.off', 'Off'),
      on: d.watchHistory.enabled, disabled: !writable
    });
    const clear = q('srv-watch-clear');
    clear.hidden = !d.watchHistory.kept;
    clear.disabled = !writable;
  }
  if (d.ntpSettings) renderNtp(d.ntp, d.ntpSettings, writable);
  const portInput = q('port-input');
  if (portInput && d.daemon) {
    portInput.disabled = !writable;
    q('port-save').disabled = !writable;
    if (document.activeElement !== portInput && !portInput.dataset.dirty) portInput.value = d.daemon.port;
  }
  if (d.backupDaily) renderBackupDaily(d.backupDaily, writable);
  const hlRow = q('exp-homelayout-row');
  if (hlRow) {
    hlRow.hidden = !d.homeLayout;
    if (d.homeLayout) updateToggle('exp-homelayout', {
      cls: d.homeLayout.allowed ? 'good' : 'idle',
      label: d.homeLayout.allowed ? t('common.on', 'On') : t('common.off', 'Off'),
      on: d.homeLayout.allowed, disabled: !writable
    });
  }
  const ssRow = q('exp-screensaver-row');
  if (ssRow) {
    ssRow.hidden = !d.isWebos10;
    if (typeof d.allowExperimentalScreensavers === 'boolean') updateToggle('exp-screensaver', {
      cls: d.allowExperimentalScreensavers ? 'good' : 'idle',
      label: d.allowExperimentalScreensavers ? t('common.on', 'On') : t('common.off', 'Off'),
      on: d.allowExperimentalScreensavers, disabled: !writable
    });
  }
}

async function loadServerStatus() {
  try {
    const [statusResponse, backups] = await Promise.all([
      fetch(api('/api/update'), { cache: 'no-store' }),
      fetch(api('/api/backup/saved/list'), { cache: 'no-store' })
        .then(response => response.ok ? response.json() : null)
        .catch(() => null)
    ]);
    const d = await statusResponse.json();
    if (d && d.backupDaily && backups && backups.ok && Array.isArray(backups.saved))
      d.backupDaily.saved = backups.saved;
    if (d && d.ok) renderServerStatus(d);
  } catch (e) { /* server status is refreshed on the next poll */ }
}

async function toggleTvUpdates() {
  const b = q('tv-updates-block');
  b.disabled = true;
  const r = await sendCommand('blockTvUpdates', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

function renderServerHealth(dm) {
  const row = q('server-health-row');
  if (!row) return;
  if (!dm) {
    row.hidden = true;
    return;
  }
  row.hidden = false;
  const bMem = q('server-badge-mem');
  const bUp = q('server-badge-uptime');

  if (bMem) {
    if (dm.rss) {
      const rssMb = Math.round(dm.rss / 1048576);
      bMem.textContent = t('server.health.memBadge', '{n} MB RAM', { n: rssMb });
      bMem.title = t('server.health.memTooltip', 'Memory (RSS)');
      bMem.hidden = false;
    } else {
      bMem.hidden = true;
    }
  }

  if (bUp) {
    if (dm.uptime != null) {
      const s = dm.uptime;
      const dd = Math.floor(s / 86400), hh = Math.floor((s % 86400) / 3600), mm = Math.floor((s % 3600) / 60);
      const upStr = dd ? t('metrics.uptime.days', '{d}d {h}h {m}m', { d: dd, h: hh, m: mm })
        : (hh ? t('metrics.uptime.hours', '{h}h {m}m', { h: hh, m: mm })
        : t('server.health.uptimeMins', '{m}m', { m: mm || 1 }));
      bUp.textContent = t('server.health.up', 'Up {time}', { time: upStr });
      bUp.title = t('server.health.uptimeTooltip', 'Process uptime');
      bUp.hidden = false;
    } else {
      bUp.hidden = true;
    }
  }
}


async function toggleExpTileHiding() {
  const b = q('exp-tilehiding');
  b.disabled = true;
  const r = await sendCommand('setTileHidingAllowed', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

// Network time (#633). A switch, and the server's box only while it is on:
// the server is kept while off, so switching back on needs no retyping.
let ntpEditing = false;

function renderNtp(n, cfg, writable) {
  const toggle = q('ntp-toggle'), form = q('ntp-form'), status = q('ntp-status'), input = q('ntp-server');
  if (!toggle) return;
  const on = cfg.enabled;
  if (on) ntpEditing = false;
  updateToggle('ntp-toggle', {
    cls: on ? 'good' : 'idle',
    label: on ? t('common.on', 'On') : t('common.off', 'Off'),
    on: on || ntpEditing,
    disabled: !writable
  });
  form.hidden = !(on || ntpEditing);
  q('ntp-save').disabled = !writable;
  input.disabled = !writable;
  if (document.activeElement !== input && !input.dataset.dirty) {
    input.value = cfg.server ? (cfg.port && cfg.port !== 123 ? hostPort(cfg.server, cfg.port) : cfg.server) : '';
  }
  status.classList.remove('row-err');
  if (ntpEditing) {
    status.hidden = false;
    status.textContent = t('server.ntp.enter', 'Enter the time server and save to switch this on.');
    return;
  }
  if (!on || !n) { status.hidden = true; return; }
  status.hidden = false;
  if (n.error) {
    status.classList.add('row-err');
    status.textContent = t('server.ntp.failing', 'The time server has not answered, so the TV keeps the time it has. {error}', { error: n.error });
  } else {
    status.textContent = n.setAgoS !== null
      ? t('server.ntp.setAgo', 'Set the clock {set}; last checked {checked}.',
          { set: cfgAge(n.setAgoS * 1000), checked: cfgAge(n.answeredAgoS * 1000) })
      : n.answeredAgoS !== null
        ? t('server.ntp.rightAgo', 'Checked {checked}: the clock was already right.', { checked: cfgAge(n.answeredAgoS * 1000) })
        : t('server.ntp.asking', 'Asking the time server\u2026');
  }
}

async function toggleNtp() {
  const b = q('ntp-toggle'), input = q('ntp-server');
  // Switching on with no server saved opens the box for one first.
  if (!b.dataset.on && !input.value.trim()) {
    ntpEditing = true;
    loadServerStatus();
    q('ntp-form').hidden = false;
    input.focus();
    return;
  }
  // Switching off a box opened for a first server just closes it.
  if (ntpEditing) {
    ntpEditing = false;
    delete input.dataset.dirty;
    loadServerStatus();
    return;
  }
  b.disabled = true;
  const r = await sendCommand('setNtpEnabled', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

async function saveNtpServer() {
  const input = q('ntp-server'), btn = q('ntp-save');
  if (!input.value.trim()) return input.focus();
  if (btn) { btn.disabled = true; btn.classList.add('busy'); }
  try {
    const r = await sendCommand('setNtpServer', input.value.trim());
    delete input.dataset.dirty;
    // A refusal, such as a mistyped address, is shown by sendCommand.
    if (r && r.ok) renderServerStatus(r); else loadServerStatus();
  } finally {
    if (btn) { btn.disabled = false; btn.classList.remove('busy'); }
  }
}

async function saveDashboardPort() {
  const input = q('port-input'), btn = q('port-save'), status = q('port-status');
  const port = input.value.trim();
  if (!port) return input.focus();
  status.hidden = true;
  status.classList.add('row-err');
  btn.disabled = true; btn.classList.add('busy');
  try {
    const r = await sendCommand('setDashboardPort', port);
    delete input.dataset.dirty;
    if (r && r.ok && r.restarting) {
      // The server comes back on the new port; this page's address is then stale.
      status.classList.remove('row-err');
      status.textContent = t('server.port.moving', 'Saved. The server is restarting on port {port}; this page opens there when it is back.', { port: r.newPort });
      status.hidden = false;
      const next = location.protocol + '//' + location.hostname + ':' + r.newPort + location.pathname + location.search + location.hash;
      // A browser that forbids the probe would otherwise wait here for good, so
      // after about fifteen seconds the page goes to the new address regardless.
      let tries = 0;
      const tryNext = () => {
        if (++tries > 8) { location.href = next; return; }
        fetch(location.protocol + '//' + location.hostname + ':' + r.newPort + '/api/caps', { mode: 'no-cors' })
          .then(() => { location.href = next; })
          .catch(() => { setTimeout(tryNext, 1500); });
      };
      setTimeout(tryNext, 2500);
    } else if (r && r.ok) {
      loadServerStatus();
    }
    // A refusal, such as a port already in use, is shown by sendCommand.
  } finally {
    btn.disabled = false; btn.classList.remove('busy');
  }
}

// Advanced stays open in this browser once opened.
(function rememberServerAdvanced() {
  const adv = q('srv-adv');
  if (!adv) return;
  try { if (localStorage.getItem('srvAdvOpen') === '1') adv.open = true; } catch (e) {}
  adv.addEventListener('toggle', () => {
    try { localStorage.setItem('srvAdvOpen', adv.open ? '1' : '0'); } catch (e) {}
  });
})();

async function togglePrometheus() {
  const b = q('srv-prometheus');
  b.disabled = true;
  const r = await sendCommand('setPrometheusEnabled', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

async function toggleWatchTime() {
  const b = q('srv-watch');
  b.disabled = true;
  const r = await sendCommand('setHistoryEnabled', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

// Stopping keeps what is there; this deletes it.
async function clearWatchTime() {
  if (!confirm(t('server.watch.clearConfirm', 'Delete all watch time kept on the TV?'))) return;
  const r = await sendCommand('clearHistory');
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

// ---- settings backup ----
function backupMessage(text) {
  const m = q('backup-msg');
  m.textContent = text;
  m.hidden = !text;
}

// Hands a backup to the browser as a file named for its TV and day.
function downloadBackupFile(b) {
  const model = String((b.from && b.from.model) || 'tv').replace(/[^A-Za-z0-9_-]+/g, '-');
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(b, null, 2)], { type: 'application/json' }));
  a.download = `nativetv-backup-${model}-${b.created.slice(0, 10)}.json`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function saveBackup() {
  try {
    const r = await fetch(api('/api/backup'), { cache: 'no-store' });
    const b = await r.json();
    if (!r.ok || b.format !== 'nativetv-backup') throw new Error(b.error || r.status);
    downloadBackupFile(b);
    backupMessage(t('server.backup.saved', 'Saved {count} LG settings and Native Tv’s own.', { count: Object.keys(b.lg || {}).length }));
  } catch (e) {
    backupMessage(t('server.backup.saveFailed', 'Could not make a backup — {error}', { error: e.message }));
  }
}

async function restoreBackup(input) {
  const file = input.files && input.files[0];
  input.value = '';
  if (!file) return;
  let b;
  try { b = JSON.parse(await file.text()); } catch (e) { b = null; }
  restoreBackupData(b);
}

async function restoreBackupData(b) {
  if (!b || b.format !== 'nativetv-backup') {
    return backupMessage(t('server.backup.notBackup', 'That file is not a Native Tv backup'));
  }
  const from = (b.from && b.from.model) || t('server.backup.unknownTv', 'an unknown TV');
  const when = b.created ? new Date(b.created).toLocaleString() : '?';
  if (!confirm(t('server.backup.confirm', 'Restore the settings saved from {tv} on {date}? This changes LG’s settings on this TV.', { tv: from, date: when }))) return;
  backupMessage(t('server.backup.restoring', 'Restoring…'));
  try {
    const r = await fetch(api('/api/backup'), {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b)
    });
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || r.status);
    const lg = d.lg || {};
    let text = t('server.backup.restored', 'Changed {changed} LG settings, {same} were already so, {skipped} skipped as this TV does not have them or they are not available now.', {
      changed: (lg.applied || []).length, same: lg.unchanged || 0, skipped: (lg.skipped || []).length
    });
    if ((lg.failed || []).length) text += ' ' + t('server.backup.failedSome', '{count} could not be set.', { count: lg.failed.length });
    const apps = d.apps || {};
    if ((apps.toInstall || []).length) text += ' ' + t('server.backup.appsToInstall', '{count} apps from the backup can be installed from the Apps tab.', { count: apps.toInstall.length });
    if ((apps.manual || []).length) text += ' ' + t('server.backup.appsManual', 'Not in the Homebrew Channel catalog, to install by hand: {names}.', { names: apps.manual.map(a => a.title).join(', ') });
    if (d.restart) text += ' ' + t('server.backup.restarting', 'Native Tv restarts to apply its own settings.');
    backupMessage(text);
  } catch (e) {
    backupMessage(t('server.backup.restoreFailed', 'Could not restore — {error}', { error: e.message }));
  }
}

// ---- daily backups, kept on the TV ----
function renderBackupDaily(daily, writable) {
  updateToggle('backup-daily', {
    cls: daily.enabled ? 'good' : 'idle',
    label: daily.enabled ? t('common.on', 'On') : t('common.off', 'Off'),
    on: daily.enabled,
    disabled: !writable
  });
  const list = q('backup-saved');
  if (!Array.isArray(daily.saved)) return;
  list.hidden = !daily.saved.length;
  list.innerHTML = daily.saved.map(s => {
    const day = new Date(s.date + 'T12:00:00Z').toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric' });
    return `<div class="bk"><span class="bk-date">${esc(day)}</span>` +
      `<span class="bk-size">${esc(t('server.backup.kb', '{kb} KB', { kb: Math.max(1, Math.round(s.size / 1024)) }))}</span>` +
      `<button type="button" class="link-btn" data-name="${esc(s.name)}" data-do="download">${esc(t('server.backup.download', 'Download'))}</button>` +
      `<button type="button" class="link-btn" data-name="${esc(s.name)}" data-do="restore"${writable ? '' : ' disabled'}>${esc(t('server.backup.restoreSaved', 'Restore'))}</button></div>`;
  }).join('');
  for (const b of list.querySelectorAll('button')) {
    b.onclick = () => (b.dataset.do === 'download' ? downloadSavedBackup : restoreSavedBackup)(b.dataset.name);
  }
}

async function toggleBackupDaily() {
  const b = q('backup-daily');
  b.disabled = true;
  await sendCommand('setBackupDaily', !b.dataset.on);
  loadServerStatus();
}

async function fetchSavedBackup(name) {
  const r = await fetch(api('/api/backup/saved?name=' + encodeURIComponent(name)), { cache: 'no-store' });
  const b = await r.json();
  if (!r.ok || b.format !== 'nativetv-backup') throw new Error(b.error || r.status);
  return b;
}

async function downloadSavedBackup(name) {
  try {
    downloadBackupFile(await fetchSavedBackup(name));
  } catch (e) {
    backupMessage(t('server.backup.readFailed', 'Could not read that backup — {error}', { error: e.message }));
  }
}

async function restoreSavedBackup(name) {
  let b;
  try { b = await fetchSavedBackup(name); } catch (e) {
    return backupMessage(t('server.backup.readFailed', 'Could not read that backup — {error}', { error: e.message }));
  }
  restoreBackupData(b);
}

async function toggleExpScreensaver() {
  const b = q('exp-screensaver');
  b.disabled = true;
  const r = await sendCommand('setWebos10ScreensaversAllowed', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

async function toggleNetworkDebugger() {
  const b = q('srv-devtools');
  b.disabled = true;
  const r = await sendCommand('setNetworkDebuggerAllowed', !b.dataset.on);
  if (r && r.ok) renderServerStatus(r); else loadServerStatus();
}

// ---- the app on the TV's home screen ----
/*
 * Offered here because an in-place update installs no app: it copies files and
 * runs nothing, so without this the only way to get the tile is to re-run
 * deploy.sh. Hidden where the TV will not take an unsigned app.
 */
let tvAppBusy = false;

function renderTvApp(d) {
  const row = q('tvapp-row');
  if (!row) return;
  row.hidden = !(d && d.supported);
  updateToggle('tvapp-btn', {
    cls: d.installed ? 'good' : 'idle',
    label: d.installed ? t('server.tvApp.added', 'Added') : t('server.tvApp.notAdded', 'Not added'),
    on: d.installed,
    disabled: !d.writable || tvAppBusy
  });
  q('tvapp-desc').textContent = d.installed
    ? t('server.tvApp.desc.added', 'Opens this dashboard on the TV itself, driven by the remote. Remove it to take the tile off the home screen.')
    : t('server.tvApp.desc', 'Adds a tile that opens this dashboard on the TV itself, driven by the remote.');
}

async function loadTvApp() {
  try {
    const d = await (await fetch(api('/api/tvapp'), { cache: 'no-store' })).json();
    renderTvApp(d);
  } catch (e) { /* leave the row hidden */ }
}

async function toggleTvApp() {
  if (tvAppBusy) return;
  const btn = q('tvapp-btn');
  const adding = !btn.dataset.on;
  if (!adding && !confirm(t('server.tvApp.remove.confirm', "Take the dashboard off the TV's home screen?"))) return;
  tvAppBusy = true;
  btn.disabled = true;
  btn.textContent = adding ? t('server.tvApp.adding', 'Adding\u2026') : t('server.tvApp.removing', 'Removing\u2026');
  const r = await sendCommand(adding ? 'tvAppInstall' : 'tvAppRemove');
  tvAppBusy = false;
  if (!r || !r.ok) q('server-status-msg').textContent = (r && r.error) || t('server.tvApp.failed', 'Could not change it.');
  loadTvApp();
}


// ---------------------------------------------------------------------------
// The admin PIN: the card in this tab, and the prompt any risky action raises
// (core.js asks for it when the server answers pin:true).

function adminMessage(text, bad) {
  const m = q('admin-msg');
  m.textContent = text;
  m.hidden = !text;
  m.classList.toggle('bad', !!bad);
}

function renderAdminPin(d) {
  const state = q('admin-state');
  state.textContent = !d.set
    ? t('server.admin.off', 'No admin PIN is set, so anyone who can open this page can do these.')
    : d.defaultPin
      ? t('server.admin.default', 'The admin PIN is the default, 0000. Choose your own.')
      : t('server.admin.on', 'An admin PIN is set.');
  state.classList.toggle('warn', !d.set || !!d.defaultPin);
  q('admin-save').textContent = d.set ? t('server.admin.change', 'Change PIN') : t('server.admin.set', 'Set PIN');
  q('admin-remove').hidden = !d.set;
}

async function loadAdminPin() {
  try {
    const r = await fetch(api('/api/admin'), { cache: 'no-store' });
    renderAdminPin(await r.json());
  } catch (e) {
    adminMessage(t('server.admin.failed', 'Could not read the admin PIN setting — {error}', { error: e.message }), true);
  }
}

async function adminPinRequest(body) {
  const r = await fetch(api('/api/admin/pin'), {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
  });
  const d = await r.json().catch(() => ({}));
  if (!d.ok) throw new Error(d.pin ? t('server.admin.needed', 'The admin PIN was not given.') : d.error || r.status);
  return d;
}

async function saveAdminPin() {
  const input = q('admin-new');
  if (!/^[0-9]{4}$/.test(input.value)) {
    adminMessage(t('server.admin.fourDigits', 'The PIN is four digits.'), true);
    return;
  }
  try {
    const d = await adminPinRequest({ pin: input.value });
    setAdminSession(d.session); // whoever sets the PIN has it open
    input.value = '';
    adminMessage(t('server.admin.saved', 'Admin PIN saved.'));
    loadAdminPin();
  } catch (e) { adminMessage(e.message, true); }
}

async function removeAdminPin() {
  if (!confirm(t('server.admin.removeConfirm', 'Remove the admin PIN? Installing apps, SSH keys and the other risky actions will no longer ask for it.'))) return;
  try {
    await adminPinRequest({ remove: true });
    setAdminSession('');
    adminMessage('');
    loadAdminPin();
  } catch (e) { adminMessage(e.message, true); }
}

// What a browser does with the token the passcode gave it: keep it, as a link would, and reload.
// Where storage is blocked it stays in the address, as core.js does for a link.
function adoptToken(newToken) {
  if (!newToken) { location.reload(); return; }
  try { localStorage.setItem('tvweb_k', newToken); location.reload(); } catch (e) {
    location.search = '?k=' + encodeURIComponent(newToken);
  }
}

/*
 * A PIN prompt over the page; resolves true once the PIN was accepted. One at a
 * time: every request waiting for a PIN shares it. With login, it is the screen
 * a browser gets when the TV does not know its token (core.js raises it on a
 * 401): it cannot be dismissed, and what it gets back is the token. Without,
 * it opens an admin session for the actions that ask for one, and can be
 * cancelled.
 */
let pinPrompt = null;
function askPin(login) {
  if (pinPrompt) return pinPrompt;
  pinPrompt = new Promise(resolve => {
    const dialog = document.createElement('dialog');
    dialog.className = 'admin-pin';
    dialog.setAttribute('aria-labelledby', 'admin-pin-title');
    const title = login ? t('admin.login.title', 'Enter passcode') : t('admin.prompt.title', 'Admin PIN');
    const body = login
      ? t('admin.login.body', 'Enter the four-digit passcode set on the TV, or open the dashboard from the link or QR code on the TV.')
      : t('admin.prompt.body', 'Enter the four-digit admin PIN to go on.');
    dialog.innerHTML =
      `<form><h2 id="admin-pin-title">${esc(title)}</h2><p>${esc(body)}</p>` +
      `<input type="password" inputmode="numeric" pattern="[0-9]{4}" maxlength="4" autocomplete="off" ` +
      `aria-label="${esc(t('admin.prompt.label', 'Admin PIN'))}">` +
      `<div class="sub bad" role="alert" hidden></div>` +
      `<div class="btns">` +
      (login ? '' : `<button type="button" class="link-btn" data-cancel>${esc(t('admin.prompt.cancel', 'Cancel'))}</button>`) +
      `<button type="submit">${esc(t('admin.prompt.unlock', 'Unlock'))}</button></div></form>`;
    document.body.appendChild(dialog);
    const input = dialog.querySelector('input');
    const error = dialog.querySelector('[role=alert]');
    let accepted = false;
    dialog.addEventListener('close', () => { dialog.remove(); resolve(accepted); });
    dialog.addEventListener('cancel', e => { if (login) e.preventDefault(); });
    const cancel = dialog.querySelector('[data-cancel]');
    if (cancel) cancel.onclick = () => dialog.close();
    dialog.querySelector('form').onsubmit = async e => {
      e.preventDefault();
      try {
        const r = await fetch(api(login ? '/api/admin/login' : '/api/admin/unlock'), {
          method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ pin: input.value })
        });
        const d = await r.json();
        if (d.ok) {
          setAdminSession(d.session);
          accepted = true;
          dialog.close();
          if (login) adoptToken(d.token);
          return;
        }
        error.textContent = r.status === 429
          ? t('admin.prompt.locked', 'Too many wrong PINs. Try again in {seconds} s.', { seconds: d.retryAfter })
          : d.defaultPin
            ? t('admin.login.default', 'The passcode is still the default, 0000, so it cannot be used to sign in. Change it on the TV first, or use the dashboard link on the TV.')
            : d.error === 'wrong PIN'
              ? t('admin.prompt.wrong', 'That is not the PIN.')
              : d.error === 'no admin PIN is set'
                ? t('admin.login.unset', 'No passcode is set on the TV. Open the dashboard from the link or QR code on the TV.')
                : d.error;
      } catch (err) { error.textContent = err.message; }
      error.hidden = false;
      input.value = '';
      input.focus();
    };
    dialog.showModal();
    input.focus();
  }).finally(() => { pinPrompt = null; });
  return pinPrompt;
}
const askAdminPin = () => askPin(false);
