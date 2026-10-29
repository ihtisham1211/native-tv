// The sections: a bottom bar, a rail or a drawer by width, and the tabs inside four of them.

/*
 * Tabs. Each key names one panel; the five sections group them, and a section
 * with several panels has a tab bar of its own. The privacy and MQTT panels
 * load on demand - privacy costs several Luna calls, and two of those start
 * the service they ask - so neither is fetched until its tab is actually shown.
 */
const TABS = {
  control:     'tab-control',
  metrics:     'tab-metrics',
  apps:        'appspane',
  catalogue:   'appspane',
  homescreen:  'appspane',
  home:        'homepane',
  oledcare:    'oledpane',
  screensaver: 'sspane',
  game:        'gamepane',
  privacy:     'privpane',
  mqtt:        'mqttpane',
  server:      'serverpane',
  tools:       'toolspane',
  advanced:    'advpane',
  servicemenu: 'svcpane'
};
// The section each tab sits in, and the part of the Apps panel a tab shows.
const SECTION_OF = {
  control: 'overview', metrics: 'overview',
  apps: 'apps', catalogue: 'apps', homescreen: 'apps', home: 'apps',
  oledcare: 'display', screensaver: 'display', game: 'display',
  privacy: 'privacy',
  mqtt: 'settings', server: 'settings', tools: 'settings', advanced: 'settings', servicemenu: 'settings'
};
const APPS_PANE = { apps: 'installed', catalogue: 'catalogue', homescreen: 'launch' };
// Each tab's page on the docs site, for Help.
const DOCS = 'https://ihtisham1211.github.io/nativeTv/';
const TAB_DOCS = {
  control: 'dashboard/control/', metrics: 'dashboard/metrics/', apps: 'dashboard/apps/', catalogue: 'dashboard/apps/',
  homescreen: 'dashboard/apps/', home: 'dashboard/home/',
  oledcare: 'dashboard/oled-care/', game: 'dashboard/game/', servicemenu: 'dashboard/service-menu/',
  screensaver: 'dashboard/screen-savers/', privacy: 'dashboard/privacy/', advanced: 'dashboard/advanced/',
  mqtt: 'HOME-ASSISTANT/', server: 'dashboard/server/', tools: 'dashboard/tools/'
};
// The tab each section was last on, kept for the visit.
const lastTabIn = {};
let activeTab = null;

/*
 * The Android app names itself in the user agent and passes a bridge with
 * navState, which it asks for the section's name and whether the page's tabs
 * are a drawer. They never are now - a bar or a rail below 1200 px, a
 * permanent drawer above - so it is told false and shows no menu button of
 * its own. An older app has the bridge without navState and is left alone.
 */
const appBridge = /Glasshouse-Android\//.test(navigator.userAgent) && window.GlasshouseApp &&
  typeof window.GlasshouseApp.navState === 'function' ? window.GlasshouseApp : null;

function reportNav() {
  if (!appBridge) return;
  const title = q('page-title');
  appBridge.navState(false, title ? title.textContent : '', false);
}
// null until the first telemetry says either way.
let isOledSet = null;

function sectionButton(section) { return document.querySelector('.nav-item[data-dest="' + section + '"]'); }

// The tabs of a section that the TV offers, in order.
function sectionTabs(section) {
  const bar = q('screen-' + section).querySelector('.tabs');
  return bar ? Array.from(bar.querySelectorAll('.tab:not([hidden])')).map(b => b.dataset.tab) : [];
}

function showSection(section) {
  const tabs = sectionTabs(section);
  const last = lastTabIn[section];
  showTab(last && (!tabs.length || tabs.indexOf(last) !== -1) ? last : (tabs[0] || { overview: 'control', privacy: 'privacy' }[section]));
}

function showTab(name) {
  clearErr();
  // The tab was System for a while, and links and remembered tabs still say so.
  if (name === 'system') name = 'metrics';
  if (!TABS[name]) name = 'control';
  /*
   * The panel is about the OLED panel, so an LCD set has no tab for it - but a
   * deep link or a remembered choice can still ask for it.
   *
   * Only turned away once the set has actually said it has no panel. The tab
   * button starts hidden and is revealed by the first telemetry, which lands
   * after a deep link is handled, so reading the button here would bounce
   * /?tab=oledcare off an OLED set every time.
   */
  if (name === 'oledcare' && isOledSet === false) name = 'control';
  if (name === 'home' && homeAvailable === false) name = 'control';
  const section = SECTION_OF[name];
  activeTab = name;
  lastTabIn[section] = name;

  document.querySelectorAll('.screen').forEach(s => { s.hidden = s.dataset.screen !== section; });
  document.querySelectorAll('.nav-item').forEach(b => {
    if (b.dataset.dest === section) b.setAttribute('aria-current', 'page'); else b.removeAttribute('aria-current');
  });
  const overview = section === 'overview';
  for (const [key, id] of Object.entries(TABS)) {
    const el = q(id);
    if (el) el.hidden = overview ? SECTION_OF[key] !== 'overview' : TABS[name] !== id;
  }
  document.querySelectorAll('.tab').forEach(b => {
    const isSel = b.dataset.tab === name;
    b.setAttribute('aria-selected', String(isSel));
    b.tabIndex = isSel ? 0 : -1;
  });
  document.querySelectorAll('#appspane [data-pane]').forEach(el => { el.hidden = APPS_PANE[name] !== el.dataset.pane; });
  placeTabIndicators();

  const button = sectionButton(section);
  const label = button && button.querySelector('.nav-label');
  if (label) q('page-title').textContent = label.textContent;
  const help = q('nav-help');
  if (help) help.href = DOCS + (TAB_DOCS[name] || 'dashboard/');
  try { localStorage.setItem('tab', name); } catch (e) { /* private window */ }
  reportNav();
  if (overview) window.scrollTo(0, 0);
  if (name === 'metrics' && q('tab-metrics').firstElementChild) q('tab-metrics').firstElementChild.scrollIntoView({ block: 'start' });

  cfgPollStatus(name === 'mqtt');
  if (overview) loadHdmiOnce();
  watchPolling(overview);
  if (APPS_PANE[name]) { loadApps(); loadCatalog(false); pollInstall(); loadRestoredApps(); }
  if (name === 'server') { loadServerStatus(); loadTvApp(); loadSsh(); }
  if (name === 'oledcare') loadOledCare();
  if (name === 'home') loadHome();
  gamePolling(name === 'game');
  if (name === 'advanced') { loadAdvSettings(); loadServerStatus(); loadScreenTime(); }
  if (name === 'servicemenu') loadServiceMenu();
  if (name === 'screensaver') loadScreensavers();
  if (name === 'privacy') loadPrivacy();
  if (name === 'mqtt') loadSettings();
  // Opened at the newest line, wherever it was left scrolled.
  if (name === 'tools') { toolsAutoScroll = true; loadLogs(); scheduleToolsPoll(); loadLogForwarding(); }
  else if (typeof scheduleToolsPoll === 'function') { scheduleToolsPoll(); }
}

// The indicator under the selected tab, for each bar showing.
function placeTabIndicators() {
  document.querySelectorAll('.tabs').forEach(bar => {
    const current = bar.querySelector('.tab[aria-selected="true"]');
    const indicator = bar.querySelector('.tab-ind');
    if (!current || !indicator || !current.offsetWidth) return;
    indicator.style.transform = 'translateX(' + current.offsetLeft + 'px) scaleX(' + current.offsetWidth / 100 + ')';
    current.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  });
}

document.querySelectorAll('.nav-item').forEach(b => b.addEventListener('click', () => {
  showSection(b.dataset.dest);
  q('panels').focus({ preventScroll: true });
}));
document.querySelectorAll('.tabs').forEach(bar => {
  bar.addEventListener('click', e => {
    const tab = e.target.closest('.tab');
    if (tab) showTab(tab.dataset.tab);
  });
  bar.addEventListener('keydown', e => {
    if (['ArrowLeft', 'ArrowRight', 'Home', 'End'].indexOf(e.key) === -1) return;
    const tabs = Array.from(bar.querySelectorAll('.tab:not([hidden])'));
    const idx = tabs.indexOf(document.activeElement);
    if (idx === -1) return;
    let nextIdx = idx;
    if (e.key === 'ArrowLeft') nextIdx = (idx - 1 + tabs.length) % tabs.length;
    else if (e.key === 'ArrowRight') nextIdx = (idx + 1) % tabs.length;
    else if (e.key === 'Home') nextIdx = 0;
    else if (e.key === 'End') nextIdx = tabs.length - 1;
    e.preventDefault();
    tabs[nextIdx].focus();
    showTab(tabs[nextIdx].dataset.tab);
  });
});
// Reveals the Game tab on TVs with LG's Game Optimizer, and Home where nativeTv Home is offered.
loadGame();
loadHomeAvailable();

// Keys 1 to 5 jump to a section, as on a keyboard-driven app.
document.addEventListener('keydown', e => {
  if (e.ctrlKey || e.altKey || e.metaKey || e.defaultPrevented) return;
  const el = e.target;
  if (el && (el.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(el.tagName))) return;
  if (document.querySelector('dialog[open], .modal-backdrop:not([hidden])')) return;
  const items = document.querySelectorAll('.nav-item');
  const i = Number(e.key) - 1;
  if (i >= 0 && i < items.length) showSection(items[i].dataset.dest);
});

// The remote opens from the button that floats over every section.
(function wireRemote() {
  const sheet = q('sheet-remote');
  q('fab-remote').addEventListener('click', () => sheet.showModal());
  sheet.querySelectorAll('[data-close]').forEach(b => b.addEventListener('click', () => sheet.close()));
  sheet.addEventListener('click', e => { if (e.target === sheet) sheet.close(); });
  // Dragged down by its handle, a sheet closes past 120 px or on a quick flick.
  const handle = sheet.querySelector('.sheet-handle');
  let startY = 0, startT = 0, offset = 0, dragging = false;
  handle.addEventListener('pointerdown', e => {
    dragging = true; startY = e.clientY; startT = performance.now();
    sheet.classList.add('dragging'); handle.setPointerCapture(e.pointerId);
  });
  handle.addEventListener('pointermove', e => {
    if (!dragging) return;
    offset = Math.max(0, e.clientY - startY);
    sheet.style.transform = 'translateY(' + offset + 'px)';
  });
  function release() {
    if (!dragging) return;
    dragging = false;
    sheet.classList.remove('dragging');
    const velocity = offset / Math.max(1, performance.now() - startT);
    sheet.style.transform = '';
    if (offset > 120 || (velocity > 0.11 && offset > 24)) sheet.close();
    offset = 0;
  }
  handle.addEventListener('pointerup', release);
  handle.addEventListener('pointercancel', release);
})();

document.addEventListener('DOMContentLoaded', () => { reportNav(); placeTabIndicators(); });
// A tab the TV turns out to offer moves the ones after it, and the indicator with them.
document.querySelectorAll('.tabs').forEach(bar =>
  new MutationObserver(placeTabIndicators).observe(bar, { attributes: true, attributeFilter: ['hidden'], subtree: true }));
window.addEventListener('resize', () => { placeTabIndicators(); reportNav(); });

// The bar takes a surface once the page has scrolled under it.
new IntersectionObserver(([entry]) => q('appbar').classList.toggle('scrolled', !entry.isIntersecting)).observe(q('sentinel'));
