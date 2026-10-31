// The Home tab: Native Tv Home, a home screen of Native Tv's own in place of LG's.

let homeStatus = null;
let homeCat = null;      // what the TV's Home lists: apps, devices, picked channels, order
let homeLayout = null;
let homeChannels = null; // the TV's whole channel list, for picking
let homeArmed = null;    // the control waiting for its second press: 'on', 'off', 'hide' or 'remove:' and a profile's id
let homeArmTimer = null;
let homeSaving = false;
let homeLoadRevision = 0;
let homeWallTimer = null;
let homeWallUploading = false;
// null until the server says; false on a TV Native Tv Home is not offered on.
let homeAvailable = null;

const HOME_ROWS = {
  app: () => t('home.row.apps', 'Apps'),
  dev: () => t('home.row.inputs', 'Inputs'),
  ch: () => t('home.row.channels', 'Channels')
};

async function homeGet(path) {
  const r = await (await fetch(api(path))).json();
  if (!r || r.ok === false) throw new Error((r && r.error) || t('common.unknownError', 'unknown error'));
  return r;
}

async function homePost(path, body) {
  const res = await fetch(api(path), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {})
  });
  const r = await res.json();
  if (!r || !r.ok) throw new Error((r && r.error) || t('common.unknownError', 'unknown error'));
  return r;
}

// The tab, as Game's, only where the server offers Native Tv Home.
function setHomeAvailable(st) {
  homeAvailable = st.available !== false;
  if (q('tab-btn-home')) q('tab-btn-home').hidden = !homeAvailable;
  document.querySelectorAll('[data-nativetv-home-note]').forEach(note => { note.hidden = !st.enabled; });
}

async function loadHomeAvailable() {
  try { setHomeAvailable(await homeGet('/api/home')); } catch (e) {}
}

async function loadHome(afterSave) {
  if (homeSaving && afterSave !== true) return;
  const revision = ++homeLoadRevision;
  try {
    const [st, cat] = await Promise.all([homeGet('/api/home'), homeGet('/api/home/catalogue')]);
    if (revision !== homeLoadRevision) return;
    homeStatus = st;
    homeCat = cat;
    homeLayout = st.layout;
  } catch (e) { showErr(e.message); return; }
  setHomeAvailable(homeStatus);
  if (!homeAvailable && activeTab === 'home') return showTab('control');
  renderHome();
  loadTmdb();
}

/* ---- Films and series (TMDB) ----
   The key is saved on the TV through the server and never read back: the page only learns whether one is set. */
let tmdbKeySet = false;
async function loadTmdb() {
  try { tmdbKeySet = (await (await fetch(api('/api/update'), { cache: 'no-store' })).json()).tmdbKeySet === true; } catch (e) { return; }
  renderTmdb();
}
function renderTmdb() {
  const row = q('home-tmdb-row');
  if (!row) return;
  row.hidden = false;
  q('home-tmdb-state').textContent = tmdbKeySet
    ? t('home.tmdbOn', 'A TMDB key is saved. Films and series are on.')
    : t('home.tmdbOff', 'No TMDB key is saved, so Native Tv Home shows no films or series.');
  q('home-tmdb-remove').hidden = !tmdbKeySet;
}
async function homeTmdbSave() {
  const input = q('home-tmdb-key');
  const key = input.value.trim();
  if (!key) return;
  const r = await sendCommand('setTmdbKey', key);
  if (r && r.ok) { input.value = ''; tmdbKeySet = r.tmdbKeySet === true; renderTmdb(); }
}
async function homeTmdbRemove() {
  const r = await sendCommand('setTmdbKey', '');
  if (r && r.ok) { tmdbKeySet = r.tmdbKeySet === true; renderTmdb(); }
}

function homeWritable() { return !!(!homeSaving && homeStatus && (homeStatus.supported.ok || homeStatus.enabled || homeStatus.restorePending) && homeStatus.writable !== false); }

function renderHome() {
  const st = homeStatus;
  const state = q('home-state');
  const desc = q('home-state-desc');
  const btn = q('home-switch');
  if (!st || !state) return;
  if (st.restorePending) {
    state.textContent = t('home.restorePending', 'LG Home restoration is incomplete');
    desc.textContent = t('home.restoreRetry', 'Retry restoring LG Home before switching Native Tv Home on again.');
  } else if (!st.supported.ok) {
    state.textContent = t('home.unavailable', 'Not available on this TV');
    desc.textContent = st.supported.reason || '';
  } else if (st.enabled) {
    state.textContent = t('home.isOn', 'Native Tv Home is the TV’s Home');
    desc.textContent = st.lgHomeOpen
      ? t('home.isOnLgOpen', 'LG’s Home is open for now. Native Tv Home comes back when something else is opened, or Home is pressed.')
      : st.active
        ? t('home.isOnDesc', 'It opens in LG’s Home’s place: with the Home and Back buttons, and when the TV wakes on Home.')
        : t('home.isOnPending', 'Switched on, but LG’s Home is still in its place. Switching off and on again tries once more.');
  } else {
    state.textContent = t('home.isOff', 'LG’s Home is the TV’s Home');
    desc.textContent = t('home.isOffDesc', 'Nothing of LG’s is changed: Native Tv Home is laid over LG’s Home while on, and switching off takes it away.');
  }
  const more = q('home-more');
  if (more) more.hidden = !st.supported.ok;
  const startupRow = q('home-startup-row');
  if (startupRow) startupRow.hidden = !st.supported.ok;
  const startup = q('home-startup');
  if (startup) {
    const on = st.openOnPowerOn === true;
    updateToggle(startup, { cls: on ? 'good' : 'idle', label: on ? t('home.startupNativeTv', 'Native Tv Home') : t('home.startupLg', 'Follow LG setting') });
    startup.setAttribute('aria-pressed', String(on));
    startup.disabled = !homeWritable() || !st.enabled;
  }
  const namesRow = q('home-app-names-row');
  if (namesRow) namesRow.hidden = !st.supported.ok;
  const appNames = q('home-app-names');
  if (appNames) {
    const on = !st.layout || st.layout.appNames !== false;
    updateToggle(appNames, { cls: on ? 'good' : 'idle', label: on ? t('common.on', 'On') : t('common.off', 'Off'), on: on });
    appNames.setAttribute('aria-pressed', String(on));
    appNames.disabled = !homeWritable();
  }
  const clockRow = q('home-clock-row'), clock = q('home-clock');
  if (clockRow) clockRow.hidden = !st.supported.ok;
  if (clock) {
    const twelve = !!st.layout && st.layout.clockFormat === '12h';
    updateToggle(clock, { cls: 'idle', label: twelve ? t('home.clock12', '12-hour') : t('home.clock24', '24-hour') });
    clock.disabled = !homeWritable();
  }
  [['home-show-inputs', 'showInputs', true], ['home-show-live', 'showLive', false], ['home-show-lg', 'showLgHome', true]].forEach(([id, key, fallback]) => {
    const row = q(id + '-row'), btn = q(id);
    if (row) row.hidden = !st.supported.ok;
    if (!btn) return;
    const on = st.layout && st.layout[key] !== undefined ? st.layout[key] === true : fallback;
    updateToggle(btn, { cls: on ? 'good' : 'idle', label: on ? t('common.on', 'On') : t('common.off', 'Off'), on: on });
    btn.setAttribute('aria-pressed', String(on));
    btn.disabled = !homeWritable();
  });
  const hideRow = q('home-hide-lg-row');
  // webOS 9 only: newer TVs have no layout file for LG's Home to start on.
  if (hideRow) hideRow.hidden = !st.supported.ok || st.hideLgHomeAtStartAvailable === false;
  const picRow = q('home-picture-row');
  if (picRow) picRow.hidden = !st.supported.ok;
  const pic = q('home-picture');
  if (pic) {
    const on = !!st.layout && st.layout.picture === true;
    updateToggle(pic, { cls: on ? 'good' : 'idle', label: on ? t('common.on', 'On') : t('common.off', 'Off'), on: on });
    pic.disabled = !homeWritable();
  }
  const hide = q('home-hide-lg');
  if (hide) {
    const armed = homeArmed === 'hide';
    updateToggle(hide, {
      cls: armed ? 'warn' : st.hideLgHomeAtStart ? 'good' : 'idle',
      label: armed ? t('home.hideLgConfirm', 'Press again to hide it') : st.hideLgHomeAtStart ? t('common.on', 'On') : t('common.off', 'Off')
    });
    hide.disabled = !homeWritable();
  }
  if (btn) {
    const wantOn = !(st.enabled || st.restorePending);
    const armed = homeArmed === (wantOn ? 'on' : 'off');
    btn.disabled = !homeWritable();
    btn.className = 'pill ' + (armed ? 'warn' : st.enabled ? 'good' : 'idle');
    btn.textContent = armed
      ? (wantOn ? t('home.confirmOn', 'Press again to switch on') : t('home.confirmOff', 'Press again to use LG Home'))
      : (st.restorePending ? t('home.retryRestore', 'Retry restoring LG Home') : wantOn ? t('home.switchOn', 'Use Native Tv Home') : t('home.switchOff', 'Use LG Home'));
  }
  const hist = q('home-history-note');
  if (hist) hist.hidden = st.history;
  renderHomeProfiles();
  renderHomeRows();
  renderHomeChannels();
  renderHomeWalls();
  q('home-reset').disabled = !homeWritable();
}

// Profiles, each with a Home layout of its own. The rest of the tab changes the one in use.
async function homeProfile(body) {
  if (!homeWritable()) return;
  homeSaving = true;
  homeLoadRevision++;
  renderHome();
  try { await homePost('/api/home/profile', body); } catch (e) { showErr(e.message); }
  await loadHome(true);
  homeSaving = false;
  renderHome();
}

// A profile is removed on the second press, as the dashboard's other things that cannot be undone are.
function homeProfileRemove(id) {
  clearTimeout(homeArmTimer);
  if (homeArmed !== 'remove:' + id) {
    homeArmed = 'remove:' + id;
    homeArmTimer = setTimeout(() => { homeArmed = null; renderHomeProfiles(); }, 4000);
    return renderHomeProfiles();
  }
  homeArmed = null;
  homeProfile({ action: 'remove', id });
}

function renderHomeProfiles() {
  const box = q('home-profiles');
  if (!box || !homeStatus) return;
  const list = homeStatus.profiles || [], colours = homeStatus.profileColours || [];
  const can = homeWritable();
  box.innerHTML = list.map(p => {
    const armed = homeArmed === 'remove:' + p.id;
    return `<div class="home-profile${p.active ? ' picked' : ''}">
      <span class="home-pf-disc" style="background:${esc(p.color)}">${esc(p.name.charAt(0).toUpperCase())}</span>
      <input type="text" maxlength="20" value="${esc(p.name)}" aria-label="${esc(t('home.profiles.name', 'Profile name'))}" ${can ? '' : 'disabled'}
        onchange="homeProfile({ action: 'rename', id: '${esc(p.id)}', name: this.value })">
      <span class="home-pf-colours" role="group" aria-label="${esc(t('home.profiles.colour', 'Colour'))}">${colours.map(c => `<button type="button"
        class="home-pf-col${c === p.color ? ' picked' : ''}" style="background:${esc(c)}" aria-label="${esc(c)}" aria-pressed="${c === p.color}" ${can ? '' : 'disabled'}
        onclick="homeProfile({ action: 'rename', id: '${esc(p.id)}', color: '${esc(c)}' })"></button>`).join('')}</span>
      <button type="button" ${can && !p.active ? '' : 'disabled'} onclick="homeProfile({ action: 'switch', id: '${esc(p.id)}' })">${esc(p.active ? t('home.profiles.inUse', 'In use') : t('home.profiles.use', 'Use'))}</button>
      <button type="button" class="${armed ? 'warn' : ''}" ${can && list.length > 1 ? '' : 'disabled'} onclick="homeProfileRemove('${esc(p.id)}')">${esc(armed ? t('home.profiles.removeAgain', 'Press again to remove') : t('home.profiles.remove', 'Remove'))}</button>
    </div>`;
  }).join('') + `<button type="button" class="home-pf-add" ${can && list.length < 5 ? '' : 'disabled'} onclick="homeProfile({ action: 'add' })">${esc(t('home.profiles.add', 'Add a profile'))}</button>`;
}

function homeItem(kind, id) {
  if (kind === 'app' && id === 'lg-home') return { title: t('home.lgHome', 'LG Home'), lg: true };
  const list = kind === 'app' ? homeCat.apps : kind === 'dev' ? homeCat.devices : homeCat.channels;
  const it = list.find(x => x.id === id);
  if (!it) return null;
  if (kind === 'dev' && it.unused) return { title: it.name, sub: t('home.dev.unused', 'Nothing connected') };
  if (kind === 'dev') return { title: it.name, sub: it.via ? t('home.dev.through', 'HDMI {port}, through {via}', { port: it.port, via: it.via }) : t('home.dev.port', 'HDMI {port}', { port: it.port }) };
  if (kind === 'ch') return { title: it.number + ' ' + it.name };
  return { title: it.title, icon: it.icon };
}

function renderHomeRows() {
  const box = q('home-rows');
  if (!box || !homeCat) return;
  const usable = homeWritable();
  // Redrawing after each change keeps the rows that were open, and what was typed in the channel search.
  const open = box.children.length ? [...box.querySelectorAll('details[open]')].map(d => d.dataset.kind) : [homeLayout.rows[0]];
  const filter = q('home-ch-filter') ? q('home-ch-filter').value : '';
  box.innerHTML = homeLayout.rows.map(kind => {
    const order = homeCat.order[kind] || [];
    // A Last opened row is in the order the server sends; moving a tile means choosing Your order first.
    const recent = homeSort(kind) === 'recent';
    const movable = usable && !recent;
    const tiles = order.map((id, i) => {
      const it = homeItem(kind, id);
      if (!it) return '';
      const hidden = kind !== 'ch' && homeHiddenIds(kind).includes(id);
      // The apps have a second list: the Apps tab's own hidden apps.
      const tabHidden = kind === 'app' && homeHiddenIds('appsTab').includes(id);
      const icon = it.icon ? `<img class="rk-ico" src="${esc(api(it.icon))}" alt="">` : '';
      return `<div class="prot-row home-tile${hidden && (kind !== 'app' || tabHidden) ? ' home-hidden' : ''}">
        <div class="prot-main"><div class="prot-title">${icon}<span translate="no">${esc(it.title)}</span></div>
          ${it.sub ? `<div class="prot-desc">${esc(it.sub)}</div>` : ''}</div>
        <div class="prot-status">
          <button type="button" class="pill idle home-move" ${movable && i > 0 ? '' : 'disabled'} onclick="homeMoveTile('${kind}', ${i}, -1)" aria-label="${esc(t('home.earlier', 'Earlier'))}"><svg class="sym" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 19V5M5.5 11.5 12 5l6.5 6.5"/></svg></button>
          <button type="button" class="pill idle home-move" ${movable && i < order.length - 1 ? '' : 'disabled'} onclick="homeMoveTile('${kind}', ${i}, 1)" aria-label="${esc(t('home.later', 'Later'))}"><svg class="sym" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5v14M5.5 12.5 12 19l6.5-6.5"/></svg></button>
          ${it.lg ? `<span class="pill idle">${esc(t('home.alwaysShown', 'Always shown'))}</span>` :
            kind === 'ch' ? `<button type="button" class="pill idle" ${usable ? '' : 'disabled'} onclick="homeToggleChannel('${jsq(id)}')">${esc(t('home.ch.remove', 'Remove'))}</button>` :
            kind === 'app' ? renderToggle({ cls: hidden ? 'idle' : 'good', sw: !hidden, label: hidden ? t('home.hiddenOnHome', 'Hidden on Home') : t('home.shownOnHome', 'Shown on Home'),
              disabled: !usable, onclick: `homeToggleTile('app', '${jsq(id)}')` }) + renderToggle({ cls: tabHidden ? 'idle' : 'good', sw: !tabHidden, label: tabHidden ? t('home.hiddenOnApps', 'Hidden on the Apps tab') : t('home.shownOnApps', 'Shown on the Apps tab'),
              disabled: !usable, onclick: `homeToggleTile('app', '${jsq(id)}', 'appsTab')` }) :
            renderToggle({ cls: hidden ? 'idle' : 'good', sw: !hidden, label: hidden ? t('home.hidden', 'Hidden') : t('home.shown', 'Shown'),
              disabled: !usable, onclick: `homeToggleTile('${kind}', '${jsq(id)}')` })}
        </div></div>`;
    }).join('');
    return `<details class="home-row" data-kind="${kind}"${open.includes(kind) ? ' open' : ''}>
      <summary><span class="prot-title">${esc(HOME_ROWS[kind]())}</span></summary>
      <div class="prot-table">${homeSortRow(kind, usable)}${kind === 'app' && homeSort('app') === 'kind' ? homeKindsRows(usable) : ''}${tiles || `<div class="sub">${esc(kind === 'ch' ? t('home.ch.empty', 'No channels yet. Add them below.') : t('home.rowEmpty', 'Nothing to show in this row yet.'))}</div>`}</div>
      ${kind === 'ch' ? homeChannelPicker() : ''}
    </details>`;
  }).join('');
  if (q('home-ch-filter')) q('home-ch-filter').value = filter;
}

// A row's order: custom, recent, or (the apps only) kind.
function homeSort(kind) {
  const value = homeLayout.sort && homeLayout.sort[kind];
  return value === 'recent' || (value === 'kind' && kind === 'app') ? value : 'custom';
}

function homeSortRow(kind, usable) {
  const sort = homeSort(kind);
  return `<div class="prot-row">
    <div class="prot-main"><div class="prot-title"><label for="home-sort-${kind}">${esc(t('home.sort', 'Order'))}</label></div>
      <div class="prot-desc">${esc(sort === 'recent'
        ? t('home.sortRecentDesc', 'The tile opened last comes first, whether it was opened from Native Tv Home or not. Tiles not yet opened follow in this row’s own order.')
        : sort === 'kind'
          ? t('home.sortKindDesc', 'Rows by kind on the Apps screen, in the order under Kinds. Tiles keep their order within a kind.')
          : t('home.sortCustomDesc', 'As arranged here.'))}</div></div>
    <div class="prot-status"><select id="home-sort-${kind}" class="pill-sel" ${usable ? '' : 'disabled'} onchange="homeSetSort('${kind}', this.value)">
      <option value="custom"${sort === 'custom' ? ' selected' : ''}>${esc(t('home.sortCustom', 'Your order'))}</option>
      <option value="recent"${sort === 'recent' ? ' selected' : ''}>${esc(t('home.sortRecent', 'Last opened'))}</option>
      ${kind === 'app' ? `<option value="kind"${sort === 'kind' ? ' selected' : ''}>${esc(t('home.sortKind', 'By kind'))}</option>` : ''}
    </select></div></div>`;
}

// The kinds in their order, each with Earlier and Later, while the apps are sorted by kind.
function homeKindsRows(usable) {
  const kinds = homeCat.kinds || [];
  return `<div class="prot-row"><div class="prot-main"><div class="prot-title">${esc(t('home.kinds', 'Kinds'))}</div>
      <div class="prot-desc">${esc(t('home.kindsDesc', 'The rows of the Apps screen, top to bottom. On the Apps screen itself, holding OK on a row moves it.'))}</div></div></div>` +
    kinds.map((kind, i) => `<div class="prot-row home-tile">
      <div class="prot-main"><div class="prot-title"><span translate="no">${esc(kind.label)}</span></div></div>
      <div class="prot-status">
        <button type="button" class="pill idle home-move" ${usable && i > 0 ? '' : 'disabled'} onclick="homeMoveKind(${i}, -1)" aria-label="${esc(t('home.earlier', 'Earlier'))}"><svg class="sym" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 19V5M5.5 11.5 12 5l6.5 6.5"/></svg></button>
        <button type="button" class="pill idle home-move" ${usable && i < kinds.length - 1 ? '' : 'disabled'} onclick="homeMoveKind(${i}, 1)" aria-label="${esc(t('home.later', 'Later'))}"><svg class="sym" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5v14M5.5 12.5 12 19l6.5-6.5"/></svg></button>
      </div></div>`).join('');
}

function homeMoveKind(i, d) {
  if (!homeWritable()) return;
  const order = (homeCat.kinds || []).map(kind => kind.key);
  const j = i + d;
  if (j < 0 || j >= order.length) return;
  [order[i], order[j]] = [order[j], order[i]];
  homeSaveLayout({ kinds: order });
}

function homeSetSort(kind, value) {
  if (value === homeSort(kind)) return;
  homeSaveLayout({ sort: { [kind]: value } });
}

async function homeSaveLayout(patch) {
  if (!homeWritable()) return;
  homeSaving = true;
  homeLoadRevision++;
  renderHome();
  try {
    const r = await homePost('/api/home/layout', patch);
    homeLayout = r.layout;
    homeStatus.layout = r.layout;
  } catch (e) { showErr(e.message); }
  await loadHome(true);
  homeSaving = false;
  renderHome();
}

function homeMoveTile(kind, i, d) {
  if (!homeWritable()) return;
  const order = homeCat.order[kind].slice();
  const j = i + d;
  if (j < 0 || j >= order.length) return;
  [order[i], order[j]] = [order[j], order[i]];
  // Only this row: the others keep following their own default order.
  homeSaveLayout({ order: { [kind]: order } });
}

// The ids hidden from a list: a row's own ('app' is the apps row on Native Tv Home), or 'appsTab', the
// Apps tab's own apps (a layout from before it had a list hides there what Home hides).
function homeHiddenIds(list) {
  const hidden = homeLayout.hidden;
  return hidden[list] || (list === 'appsTab' ? hidden.app : []) || [];
}

// `list`: the hidden list to change, the row's own unless given.
function homeToggleTile(kind, id, list) {
  const name = list || kind;
  const ids = homeHiddenIds(name).slice();
  const i = ids.indexOf(id);
  if (i >= 0) ids.splice(i, 1); else ids.push(id);
  homeSaveLayout({ hidden: { [name]: ids } });
}

function homeResetLayout() { homeSaveLayout({ reset: true }); }

// The master switch: one press arms it, a second within five seconds acts.
async function homeSwitch() {
  if (!homeWritable()) return;
  const want = homeStatus.enabled || homeStatus.restorePending ? 'off' : 'on';
  if (homeArmed !== want) {
    homeArmed = want;
    clearTimeout(homeArmTimer);
    homeArmTimer = setTimeout(() => { homeArmed = null; renderHome(); }, 5000);
    return renderHome();
  }
  homeArmed = null;
  clearTimeout(homeArmTimer);
  const btn = q('home-switch');
  if (btn) { btn.disabled = true; btn.textContent = t('apps.service.updating', 'Updating…'); }
  try {
    await homePost(want === 'on' ? '/api/home/enable' : '/api/home/disable');
  } catch (e) { showErr(e.message); }
  loadHome();
}

// Hiding LG's Home is what can leave a TV without one, so switching it on takes a second press.
async function homeHideLg() {
  if (!homeWritable()) return;
  const want = !homeStatus.hideLgHomeAtStart;
  if (want && homeArmed !== 'hide') {
    homeArmed = 'hide';
    clearTimeout(homeArmTimer);
    homeArmTimer = setTimeout(() => { homeArmed = null; renderHome(); }, 5000);
    return renderHome();
  }
  homeArmed = null;
  clearTimeout(homeArmTimer);
  homeSaving = true;
  renderHome();
  try {
    await homePost('/api/home/start-layout', { hideLgHomeAtStart: want });
  } catch (e) { showErr(e.message); }
  homeSaving = false;
  loadHome();
}

function homePicture() {
  homeSaveLayout({ picture: homeLayout.picture !== true });
}

function homeAppNames() {
  homeSaveLayout({ appNames: homeLayout.appNames === false });
}

function homeClockFormat() {
  homeSaveLayout({ clockFormat: homeLayout.clockFormat === '12h' ? '24h' : '12h' });
}

function homeShowInputs() {
  homeSaveLayout({ showInputs: homeLayout.showInputs === false });
}

function homeShowLive() {
  homeSaveLayout({ showLive: homeLayout.showLive !== true });
}

function homeShowLgHome() {
  homeSaveLayout({ showLgHome: homeLayout.showLgHome === false });
}

async function homeStartup() {
  if (!homeWritable() || !homeStatus.enabled) return;
  const want = !homeStatus.openOnPowerOn;
  homeSaving = true;
  renderHome();
  try {
    await homePost('/api/home/startup', { openOnPowerOn: want });
  } catch (e) { showErr(e.message); }
  homeSaving = false;
  loadHome();
}

// Added channels are the Channels row; this finds more to add. Built once, then only its list redraws.
function homeChannelPicker() {
  return `<div class="home-ch-add">
    <div class="prot-title">${esc(t('home.ch.addTitle', 'Add channels'))}</div>
    <input type="search" id="home-ch-filter" class="home-ch-filter" placeholder="${esc(t('home.ch.find', 'Find a channel'))}" oninput="renderHomeChannels()">
    <div class="prot-table home-ch-list" id="home-ch-list"></div>
  </div>`;
}

async function homeLoadChannels() {
  const box = q('home-ch-list');
  if (box) box.innerHTML = `<div class="sub">${esc(t('home.ch.loading', 'Reading the TV’s channels…'))}</div>`;
  try {
    const r = await homeGet('/api/home/channels');
    homeChannels = r.channels;
  } catch (e) { showErr(e.message); homeChannels = null; }
  renderHomeChannels();
}

function renderHomeChannels() {
  const box = q('home-ch-list');
  const filterBox = q('home-ch-filter');
  if (!box || !homeLayout) return;
  const picked = homeLayout.channels;
  if (filterBox) filterBox.hidden = !homeChannels || !homeChannels.length;
  if (!homeChannels) {
    box.innerHTML = `<button type="button" class="pill idle" onclick="homeLoadChannels()">${esc(t('home.ch.show', 'Show the TV’s channels'))}</button>`;
    return;
  }
  if (!homeChannels.length) {
    box.innerHTML = `<div class="sub">${esc(t('home.ch.none', 'This TV has no channels tuned, so the Channels row stays empty.'))}</div>`;
    return;
  }
  const filter = (filterBox && filterBox.value || '').toLowerCase();
  const usable = homeWritable();
  const rows = homeChannels
    .filter(c => !picked.includes(c.id))
    .filter(c => !filter || (c.number + ' ' + c.name).toLowerCase().includes(filter))
    .map(c => `<div class="prot-row"><div class="prot-main"><div class="prot-title"><span class="num">${esc(c.number)}</span> ${esc(c.name)}</div>
        ${c.radio ? `<div class="prot-desc">${esc(t('home.ch.radio', 'Radio'))}</div>` : ''}</div>
        <div class="prot-status"><button type="button" class="pill idle" ${usable ? '' : 'disabled'} onclick="homeToggleChannel('${jsq(c.id)}')">${esc(t('home.ch.add', 'Add'))}</button></div></div>`);
  box.innerHTML = rows.join('') || `<div class="sub">${esc(filter ? t('home.ch.noMatch', 'No channel matches that.') : t('home.ch.allAdded', 'Every channel is on Home.'))}</div>`;
}

function homeToggleChannel(id) {
  const list = homeLayout.channels.slice();
  const i = list.indexOf(id);
  if (i >= 0) list.splice(i, 1); else list.push(id);
  homeSaveLayout({ channels: list });
}

// The four wallpapers, by the hours each is shown.
function renderHomeWalls() {
  const box = q('home-walls');
  if (!box) return;
  const walls = homeStatus.wallpapers || {};
  const busy = !homeWritable() || homeWallUploading || walls.uploading;
  q('home-wall-slot').disabled = busy;
  q('home-wall-file').disabled = busy;
  q('home-wall-upload').disabled = busy || !q('home-wall-file').files.length;
  q('home-wall-upload').textContent = homeWallUploading || walls.uploading ? t('home.wall.uploading', 'Uploading…') : t('home.wall.upload', 'Upload image');
  q('home-wall-clear').disabled = busy || !walls.available;
  clearTimeout(homeWallTimer);
  if (walls.uploading) homeWallTimer = setTimeout(() => { if (activeTab === 'home') loadHome(); }, 2000);
  if (!walls.available) { box.innerHTML = ''; delete box.dataset.revision; return; }
  if (box.dataset.revision === walls.revision) return;
  box.dataset.revision = walls.revision;
  const slots = [
    ['morning', t('home.wall.morning', 'Morning, 06:00 to 11:00')],
    ['afternoon', t('home.wall.afternoon', 'Afternoon, 11:00 to 17:00')],
    ['evening', t('home.wall.evening', 'Evening, 17:00 to 21:00')],
    ['night', t('home.wall.night', 'Night, 21:00 to 06:00')]
  ];
  box.innerHTML = slots.map(([key, label]) => `<figure class="home-wall">
    ${walls.slots && !walls.slots[key] ? `<div class="home-wall-empty">${esc(t('home.wall.black', 'Black'))}</div>`
      : `<img src="${esc(api('/api/home/wallpaper?slot=' + key + '&v=' + encodeURIComponent(walls.revision || '')))}" alt="" loading="lazy">`}
    <figcaption class="sub">${esc(label)}</figcaption></figure>`).join('');
}

async function homeUploadWall() {
  const file = q('home-wall-file').files[0];
  if (!file || !homeWritable() || homeWallUploading) return;
  const slot = q('home-wall-slot').value;
  const note = q('home-wall-upload-note');
  homeWallUploading = true;
  note.textContent = '';
  renderHomeWalls();
  let url;
  try {
    if (!/^image\/(jpeg|png|webp)$/.test(file.type) || file.size > 25 * 1024 * 1024) {
      throw new Error(t('home.wall.chooseImage', 'Choose a JPEG, PNG or WebP image up to 25 MB.'));
    }
    url = URL.createObjectURL(file);
    const img = new Image();
    await new Promise((resolve, reject) => {
      img.onload = resolve;
      img.onerror = () => reject(new Error(t('home.wall.unreadable', 'The image could not be opened.')));
      img.src = url;
    });
    // Native Tv Home is drawn at 1920 x 1080; the server refuses anything larger.
    const scale = Math.min(1, 1920 / img.naturalWidth, 1080 / img.naturalHeight);
    const canvas = document.createElement('canvas');
    canvas.width = Math.max(1, Math.round(img.naturalWidth * scale));
    canvas.height = Math.max(1, Math.round(img.naturalHeight * scale));
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
    const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', 0.88));
    if (!blob || blob.size > 5 * 1024 * 1024) throw new Error(t('home.wall.tooLarge', 'The prepared image is too large. Choose a smaller image.'));
    const res = await fetch(api('/api/home/wallpapers/upload?slot=' + slot), {
      method: 'POST', headers: { 'Content-Type': 'image/jpeg' }, body: blob
    });
    const r = await res.json();
    if (!res.ok || !r.ok) throw new Error(r.error || t('home.wall.uploadFailed', 'The image could not be uploaded.'));
    q('home-wall-file').value = '';
    note.textContent = t('home.wall.uploadDone', 'Image uploaded. Home updates within a few seconds.');
  } catch (e) { note.textContent = e.message; }
  finally {
    if (url) URL.revokeObjectURL(url);
    homeWallUploading = false;
    await loadHome();
    renderHomeWalls();
  }
}

async function homeClearUploads() {
  if (!homeWritable() || homeWallUploading) return;
  q('home-wall-upload-note').textContent = '';
  try { await homePost('/api/home/wallpapers', { removeUploads: true }); }
  catch (e) { q('home-wall-upload-note').textContent = e.message; }
  await loadHome();
}
