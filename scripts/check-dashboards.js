#!/usr/bin/env node
/*
 * Opens every tab of both dashboards in a headless browser, against the real
 * server and a fake TV, and fails on any script error.
 *
 * Unit tests run the daemon's code, not the pages': a function the page calls
 * that is gone, or an element it reaches for that has moved, shows only when
 * that tab opens in a browser. The daemon runs as on a TV with no Node
 * (TVWEB_NODE_FREE=1), with LG's luna-send answered by the native-fake-luna-send
 * the build makes beside it, and the system snapshot of native-tv/tests/fixtures.
 *
 * Needs Playwright and its Chromium, which CI installs for this job alone:
 *
 *   npm install --no-save playwright && npx playwright install chromium
 *   node scripts/check-dashboards.js     (after python3 native-tv/build.py --service-only;
 *                                         NATIVETV_DAEMON names another build)
 *
 * BROWSER_CHANNEL=chrome uses an installed Chrome instead, with playwright-core
 * alone, as CI does: its runners have Chrome, which saves the download.
 */
'use strict';
const child = require('child_process');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
let chromium;
try { chromium = require('playwright').chromium; } catch (e) { chromium = require('playwright-core').chromium; }

const ROOT = path.join(__dirname, '..');
const ASSETS = path.join(ROOT, 'native-tv', 'assets');
const WEB = path.join(ROOT, 'native-tv', 'web');

// The TV pages, on the design system.
const PAGES = { dashboard: '/assets/google-tv/pages/dashboard/index.html' };
function option(name) { const i = process.argv.indexOf(name); return i === -1 ? null : process.argv[i + 1]; }
async function review(page, name) {
  const dir = option('--review-dir') || process.env.NATIVETV_REVIEW_DIR;
  if (!dir) return;
  fs.mkdirSync(dir, { recursive: true });
  await page.screenshot({ path: path.join(dir, name + '.png') });
}

// The tabs as each page declares them, so a new tab is opened without editing this.
function webTabs() {
  const src = fs.readFileSync(path.join(WEB, 'ui', 'tabs.js'), 'utf8');
  const block = src.slice(src.indexOf('const TABS = {'), src.indexOf('};', src.indexOf('const TABS = {')));
  return [...block.matchAll(/^\s*(\w+):\s*'/gm)].map(m => m[1]);
}
function tvTabs() {
  const src = fs.readFileSync(path.join(ASSETS, 'google-tv', 'pages', 'dashboard', 'dashboard.js'), 'utf8');
  const block = src.slice(src.indexOf('var TABS = ['), src.indexOf('];', src.indexOf('var TABS = [')));
  return [...block.matchAll(/\{\s*key:\s*'(\w+)'/g)].map(m => m[1]);
}

function freePort() {
  return new Promise(resolve => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); });
  });
}

function answers(port) {
  return new Promise(resolve => {
    http.get({ host: '127.0.0.1', port, path: '/api/stats' }, res => { res.resume(); resolve(res.statusCode === 200); })
      .on('error', () => resolve(false));
  });
}

async function startServer() {
  const daemon = process.env.NATIVETV_DAEMON || path.join(ROOT, 'native-tv', 'build-host-ninja', 'nativetv-daemon');
  const fake = path.join(path.dirname(daemon), 'native-fake-luna-send');
  if (!fs.existsSync(daemon) || !fs.existsSync(fake)) {
    throw new Error('build the daemon first: python3 native-tv/build.py --service-only (or set NATIVETV_DAEMON)');
  }
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'nativetv-dashboards-'));
  // The assets as deploy.sh lays them out on the TV: the TV pages and apps, and
  // the phone dashboard beside them.
  const assets = path.join(root, 'assets');
  fs.cpSync(ASSETS, assets, { recursive: true });
  fs.copyFileSync(path.join(WEB, 'ui.html'), path.join(assets, 'ui.html'));
  fs.cpSync(path.join(WEB, 'ui'), path.join(assets, 'ui'), { recursive: true });
  for (const d of ['data', 'proc/net', 'system/etc', 'system/var/luna/preferences']) fs.mkdirSync(path.join(root, d), { recursive: true });
  fs.writeFileSync(path.join(root, 'system/var/luna/preferences/paneltype_oled'), '');
  fs.writeFileSync(path.join(root, 'system/etc/issue'), 'webOS 4.4.3\n');   // the B8 the fixtures are from
  // The same system snapshot every run, in place of a procfs read.
  const system = JSON.parse(fs.readFileSync(path.join(ROOT, 'native-tv', 'tests', 'fixtures', 'system-metrics.json'), 'utf8')).expected;
  fs.writeFileSync(path.join(root, 'native-system.json'), JSON.stringify(system));
  const port = await freePort();
  const cfg = path.join(root, 'config.json');
  fs.writeFileSync(cfg, JSON.stringify({ port, host: '127.0.0.1', allowControl: true }));
  let out = '';
  const srv = child.spawn(daemon, ['--config', cfg, '--heartbeat', path.join(root, 'tvweb.beat'), '--workers', '12', '--listen',
    '--assets-root', assets, '--proc-root', path.join(root, 'proc'), '--system-root', path.join(root, 'system')], {
    env: Object.assign({}, process.env, { TVWEB_NODE_FREE: '1', TVWEB_LUNA_SEND: fake, TVWEB_DATA_DIR: path.join(root, 'data'),
      TVWEB_STATS_SYSTEM_JSON: path.join(root, 'native-system.json') })
  });
  srv.stdout.on('data', d => { out += d; });
  srv.stderr.on('data', d => { out += d; });
  srv.on('exit', (code, signal) => { out += '\n[daemon exited: code ' + code + ', signal ' + signal + ']'; });
  process.on('exit', () => { try { srv.kill('SIGKILL'); } catch (e) {} fs.rmSync(root, { recursive: true, force: true }); });
  for (let i = 0; i < 60; i++) {
    if (await answers(port)) return { port, output: () => out };
    if (srv.exitCode !== null) break;
    await new Promise(r => setTimeout(r, 500));
  }
  throw new Error('the server did not start:\n' + out.slice(-3000));
}

/*
 * Done once the tab's requests have all answered and none has started for
 * QUIET_MS - a tab's script runs when its data arrives, which is where it
 * breaks - or after MAX_MS, so a slow tab cannot hold the run up.
 */
const QUIET_MS = 500, MAX_MS = 4000;

async function visit(browser, url, label, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  page.on('pageerror', e => problems.push(label + ': ' + e.message));
  // A failed request is reported with its address, which Chrome's console
  // line leaves out. Chrome, unlike the bundled browser, asks for a favicon
  // the server does not have. A 404 for an app's icon is the server saying the
  // app has none, which the page hides; the fake TV's installed apps have no
  // icon files, and the Apps tab asks for them only when its list arrives in
  // time.
  page.on('console', m => {
    if (m.type() === 'error' && !/^Failed to load resource/.test(m.text())) problems.push(label + ': console: ' + m.text());
  });
  page.on('response', r => {
    const expected = /\/favicon\.ico$/.test(r.url()) || (r.status() === 404 && /\/api\/apps\/icon\?/.test(r.url()));
    if (r.status() >= 400 && !expected) problems.push(label + ': ' + r.status() + ' for ' + r.url());
  });
  let open = 0, last = Date.now();
  page.on('request', () => { open++; last = Date.now(); });
  const done = () => { open = Math.max(0, open - 1); last = Date.now(); };
  page.on('requestfinished', done);
  page.on('requestfailed', done);
  await page.goto(url, { waitUntil: 'load' });
  const until = Date.now() + MAX_MS;
  while (Date.now() < until && (open > 0 || Date.now() - last < QUIET_MS)) await page.waitForTimeout(100);
  await page.close();
}

// A few tabs at a time: each is its own page, so they cannot affect each other.
async function visitAll(browser, jobs, problems) {
  let next = 0;
  async function worker() { while (next < jobs.length) { const j = jobs[next++]; await visit(browser, j.url, j.label, problems); } }
  await Promise.all([worker(), worker(), worker(), worker()]);
}

async function waitForCheck(check, message) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (await check()) return;
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  throw new Error(message);
}

async function setHidden(page, hidden) {
  await page.evaluate(value => {
    Object.defineProperty(document, 'hidden', { configurable: true, value });
    document.dispatchEvent(new Event('visibilitychange'));
  }, hidden);
}

async function visitTVPolling(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('TV polling: ' + e.message));
  const stats = JSON.parse(fs.readFileSync(path.join(ROOT, 'native-tv/tests/fixtures/tv/stats-b8-webos4.json'), 'utf8'));
  let reads = 0, hold = false, pending = null;
  await page.route('**/api/stats*', route => {
    reads++;
    const body = Object.assign({}, stats, { pollingProbe: hold ? 'old' : 'fresh' });
    const reply = () => route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
    if (hold) { pending = reply; return; }
    return reply();
  });
  const idle = () => waitForCheck(() => page.evaluate(() => !tabRequest && !!data.system), 'TV polling: refresh did not finish');
  try {
    const now = Date.now();
    await page.clock.install({ time: new Date(now) });
    await page.clock.pauseAt(new Date(now + 1000));
    await page.goto(base + PAGES.dashboard + '?tab=system');
    await idle();
    const start = reads;
    await page.clock.fastForward(5000);
    if (reads !== start) problems.push('TV polling: Metrics still polls every five seconds');
    await page.clock.fastForward(5000);
    await waitForCheck(() => reads > start, 'TV polling: no ten-second refresh');
    await idle();
    if (!await page.evaluate(() => TABS.filter(t => t.key === 'options')[0].poll === 10000)) problems.push('TV polling: System is not ten seconds');

    hold = true;
    await page.evaluate(() => fetchTab(true));
    await waitForCheck(() => !!pending, 'TV polling: no delayed request');
    const delayedReads = reads;
    await page.clock.fastForward(20000);
    if (reads !== delayedReads) problems.push('TV polling: a slow read overlapped another poll');
    await page.evaluate(() => {
      window.pollingPaints = [];
      var previousRender = render;
      render = function () { pollingPaints.push(data[T().key].pollingProbe); previousRender(); };
      fetchTab(true); fetchTab(true);
    });
    hold = false;
    await pending(); pending = null;
    await idle();
    if (reads !== delayedReads + 1) problems.push('TV polling: control refreshes did not coalesce to one follow-up');
    if (await page.evaluate(() => pollingPaints.indexOf('old') !== -1)) problems.push('TV polling: a pre-control response was rendered');

    hold = true;
    await page.evaluate(() => fetchTab(true));
    await waitForCheck(() => !!pending, 'TV polling: no request to cancel');
    await setHidden(page, true);
    const hiddenReads = reads;
    await page.clock.fastForward(60000);
    if (reads !== hiddenReads) problems.push('TV polling: a hidden dashboard kept polling');
    if (!await page.evaluate(() => tabRequest === null && timer === null)) problems.push('TV polling: hiding did not cancel the request and interval');
    await pending(); pending = null;
    hold = false;
    await setHidden(page, false);
    await waitForCheck(() => reads > hiddenReads, 'TV polling: returning did not refresh immediately');
    await idle();
    if (await page.evaluate(() => data.system.pollingProbe !== 'fresh')) problems.push('TV polling: a canceled response replaced fresh data');
    const resumed = reads;
    await page.clock.fastForward(10000);
    await waitForCheck(() => reads > resumed, 'TV polling: returning did not resume polling');
    await idle();
  } finally { await page.close(); }
}

// The TV dashboard runs on a slow SoC, so a selection move must not rebuild
// the list, an unchanged poll must not touch the DOM, and a changed row must
// replace only itself. The same page then opens the catalog to check that
// icons are created lazily and kept once loaded.
async function visitTVRendering(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('TV rendering: ' + e.message));

  const installed = [];
  for (let i = 0; i < 150; i++) {
    installed.push({ id: 'installed' + i, title: 'Installed app ' + i, version: '1.0.' + i, size: 1024 * (i + 1) });
  }
  const catalog = [];
  // A valid 1x1 GIF: the DOM work is what is under test, not the network.
  const icon = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';
  for (let i = 0; i < 150; i++) {
    catalog.push({ id: 'catalog' + i, title: 'Catalog app ' + i, version: '2.0.' + i, size: 2048 * (i + 1), iconUri: icon });
  }
  const reply = (route, body) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  await page.route('**/api/**', route => {
    const pathname = new URL(route.request().url()).pathname;
    if (pathname === '/api/apps') return reply(route, { ok: true, writable: true, installed });
    if (pathname === '/api/apps/catalog') return reply(route, { ok: true, apps: catalog });
    if (pathname === '/api/apps/install/status') return reply(route, { ok: true, state: 'idle' });
    // Optional tabs stay available, so their probes cannot change the tab bar
    // midway through the checks.
    if (pathname === '/api/oledcare' || pathname === '/api/game' || pathname === '/api/screensaver' || pathname === '/api/home') {
      return reply(route, { ok: true, available: true });
    }
    return route.continue();
  });

  try {
    await page.goto(base + PAGES.dashboard + '?tab=apps');
    await waitForCheck(() => page.evaluate(() => !tabRequest && rows.length === 153), 'TV rendering: apps did not load');
    await page.evaluate(() => {
      const box = document.getElementById('rows');
      window.__ghMutations = [];
      window.__ghObserver = new MutationObserver(list => {
        for (const m of list) {
          window.__ghMutations.push({
            type: m.type, target: m.target,
            top: m.target === box,
            row: m.target.nodeType === 1 && /^row\b/.test(m.target.className || '')
          });
        }
      });
      window.__ghObserver.observe(box, { childList: true, subtree: true, attributes: true });
      window.__ghReset = function () {
        window.__ghObserver.takeRecords();
        window.__ghMutations = [];
        window.__ghNodes = Array.prototype.slice.call(box.children);
      };
      window.__ghState = function () {
        const nodes = document.getElementById('rows').children;
        const child = window.__ghMutations.filter(m => m.type === 'childList');
        return {
          childList: child.length,
          topLevel: child.filter(m => m.top).length,
          rowContent: child.filter(m => m.row).length,
          mutations: window.__ghMutations.length,
          sameNodes: window.__ghNodes.length === nodes.length && window.__ghNodes.every((n, i) => n === nodes[i]),
          selected: document.querySelectorAll('#rows .row.sel').length
        };
      };
      window.__ghReset();
    });

    for (let i = 0; i < 20; i++) await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(120);
    const moved = await page.evaluate(() => window.__ghState());
    if (moved.childList || moved.topLevel || moved.rowContent) {
      problems.push('TV rendering: moving rebuilt ' + moved.childList + ' row-list nodes');
    }
    if (!moved.sameNodes) problems.push('TV rendering: moving replaced row elements');
    if (moved.selected !== 1) problems.push('TV rendering: ' + moved.selected + ' rows carry the focus after moving');

    // A poll with identical data must leave the list completely untouched.
    await page.evaluate(() => {
      const list = document.getElementById('list');
      const getHeight = Object.getOwnPropertyDescriptor(Element.prototype, 'clientHeight').get;
      window.__ghHeightReads = 0;
      Object.defineProperty(list, 'clientHeight', { configurable: true, get() { window.__ghHeightReads++; return getHeight.call(this); } });
      window.__ghReset(); fetchTab(true);
    });
    await waitForCheck(() => page.evaluate(() => !tabRequest && data.apps && data.apps.installed.length === 150), 'TV rendering: unchanged poll did not finish');
    await page.waitForTimeout(120);
    const polled = await page.evaluate(() => window.__ghState());
    if (polled.mutations) problems.push('TV rendering: an unchanged poll made ' + polled.mutations + ' DOM changes');
    if (await page.evaluate(() => window.__ghHeightReads)) problems.push('TV rendering: an unchanged poll measured the list again');

    await page.evaluate(() => { window.__ghReset(); selectRow(sel); tick(); });
    await page.waitForTimeout(20);
    if ((await page.evaluate(() => window.__ghState())).mutations) problems.push('TV rendering: selecting the same row changed its DOM');

    // One changed app replaces that row and only that row.
    installed[0].title = 'Changed app title';
    await page.evaluate(() => { window.__ghReset(); fetchTab(true); });
    await waitForCheck(() => page.evaluate(() => !tabRequest && data.apps && data.apps.installed[0].title === 'Changed app title'), 'TV rendering: changed poll did not finish');
    await page.waitForTimeout(120);
    const patched = await page.evaluate(() => {
      const state = window.__ghState();
      const box = document.getElementById('rows');
      const child = window.__ghMutations.filter(m => m.type === 'childList');
      const targets = [];
      for (const m of child) if (targets.indexOf(m.target) < 0) targets.push(m.target);
      state.changedRows = targets.length;
      state.changedIndex = targets.length === 1 ? Array.prototype.indexOf.call(box.children, targets[0]) : -1;
      state.text = box.children[3] ? box.children[3].textContent : '';
      return state;
    });
    if (!patched.sameNodes) problems.push('TV rendering: a changed row replaced row elements');
    if (patched.topLevel) problems.push('TV rendering: a changed row rebuilt the list');
    if (patched.changedRows !== 1 || patched.changedIndex !== 3) {
      problems.push('TV rendering: a changed row patched ' + patched.changedRows + ' rows (index ' + patched.changedIndex + ')');
    }
    if (patched.text.indexOf('Changed app title') < 0) problems.push('TV rendering: the changed row did not show its new text');

    // The catalog has a hundred and fifty icons. Only those around the focus
    // are created, and a loaded icon stays in its row when the focus moves on.
    await page.evaluate(() => { window.__ghReset(); openSub('catalog'); });
    await waitForCheck(() => page.evaluate(() => sub === 'catalog' && rows.length === 151 &&
      document.querySelectorAll('#rows .row.cat').length === 150), 'TV rendering: catalog did not load');
    await waitForCheck(() => page.evaluate(() => document.querySelectorAll('#rows img.ic').length >= 10), 'TV rendering: no icons near the focus');
    const before = await page.evaluate(() => {
      const nodes = document.getElementById('rows').children;
      window.__ghFirstIcon = nodes[1].querySelector('img.ic');
      return { icons: document.querySelectorAll('#rows img.ic').length,
               far: nodes[150].querySelector('img.ic') === null && nodes[150].querySelector('span.ic.none') !== null };
    });
    if (!before.far) problems.push('TV rendering: an icon far from the focus was loaded');
    await page.evaluate(() => window.__ghReset());
    for (let i = 0; i < 20; i++) await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(120);
    const icons = await page.evaluate(() => {
      const state = window.__ghState();
      const nodes = document.getElementById('rows').children;
      state.icons = document.querySelectorAll('#rows img.ic').length;
      state.keptFirst = nodes[1].querySelector('img.ic') === window.__ghFirstIcon;
      return state;
    });
    if (icons.topLevel || icons.rowContent) problems.push('TV rendering: moving in the catalog rebuilt rows');
    if (!icons.sameNodes) problems.push('TV rendering: moving in the catalog replaced row elements');
    if (icons.icons <= before.icons) problems.push('TV rendering: icons did not load as the focus moved');
    if (!icons.keptFirst) problems.push('TV rendering: a loaded icon was discarded when the focus moved');
  } finally { await page.close(); }
}

// The first Metrics request asks for the fast snapshot while the full read is
// still running, then fills in without waiting for the ten-second poll.
async function visitTVFastStats(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('TV fast stats: ' + e.message));
  const full = JSON.parse(fs.readFileSync(path.join(ROOT, 'native-tv/tests/fixtures/tv/stats-b8-webos4.json'), 'utf8'));
  const urls = [];
  await page.route('**/api/stats*', route => {
    urls.push(route.request().url());
    const body = urls.length === 1
      ? { ok: true, loading: true, device: { model: 'Early model' }, system: { webos: '9.2' }, mem: { total: 1024, avail: 512 } }
      : full;
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + PAGES.dashboard + '?tab=system');
    await waitForCheck(() => page.evaluate(() => data.system && !data.system.loading), 'TV fast stats: the full read did not replace the snapshot');
    if (urls.length < 2) problems.push('TV fast stats: the early snapshot was never filled in');
    if (!/[?&]fast=1(?:&|$)/.test(urls[0] || '')) problems.push('TV fast stats: the first read did not ask for the fast snapshot');
    if (/[?&]fast=1(?:&|$)/.test(urls[1] || '')) problems.push('TV fast stats: the fill-in read asked for a fast snapshot again');
    if (!await page.evaluate(() => data.system.device.model === 'OLED65B8SLC')) problems.push('TV fast stats: no full data rendered');
  } finally { await page.close(); }
}



async function visitSetupRendering(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('Setup rendering: ' + e.message));
  const handoffs = [];
  let holdPoll = false, polls = 0, releasePoll;
  const state = { demo: true, network: false, homeAssistant: {}, handoff: true };
  const reply = (route, body) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  await page.route('**/api/setup?*', route => {
    if (route.request().method() === 'POST') {
      const body = route.request().postDataJSON();
      if (body.action === 'handoff') { handoffs.push(route); return; }
      return reply(route, { ok: true });
    }
    if (holdPoll) { polls++; releasePoll = () => reply(route, state); return; }
    return reply(route, state);
  });
  try {
    await page.clock.install();
    await page.clock.pauseAt(new Date());
    await page.goto(base + '/assets/google-tv/pages/setup/index.html?demo=1&screen=network');
    await page.waitForSelector('#rows .row.focus');
    await page.evaluate(() => {
      window.__ghSetupNodes = Array.from(document.querySelectorAll('#rows .row'));
      window.__ghSetupChildren = 0;
      const observer = new MutationObserver(list => { window.__ghSetupChildren += list.filter(m => m.type === 'childList').length; });
      observer.observe(document.getElementById('rows'), { subtree: true, childList: true });
    });
    for (let i = 0; i < 10; i++) { await page.keyboard.press('ArrowDown'); await page.keyboard.press('ArrowUp'); }
    const moved = await page.evaluate(() => ({ mutations: window.__ghSetupChildren,
      same: window.__ghSetupNodes.every((node, i) => node === document.querySelectorAll('#rows .row')[i]) }));
    if (!moved.same || moved.mutations) problems.push('Setup rendering: 20 focus moves rebuilt rows (' + moved.mutations + ' child mutations)');
    await review(page, 'setup-network');
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('.title').textContent.includes('Home Assistant'));
    await page.keyboard.press('Enter');
    await waitForCheck(() => handoffs.length === 1, 'Setup rendering: first handoff did not start');
    await page.keyboard.press('ArrowLeft');
    await page.keyboard.press('Enter');
    await waitForCheck(() => handoffs.length === 2, 'Setup rendering: second handoff did not start');
    await reply(handoffs[1], { ok: true, url: 'http://192.168.1.2/?c=2222' });
    await page.waitForSelector('#qr');
    await reply(handoffs[0], { ok: true, url: 'http://192.168.1.2/?c=1111' });
    await page.waitForTimeout(50);
    if (await page.locator('#qr').count() !== 1) problems.push('Setup rendering: a stale handoff added a second QR code');
    holdPoll = true;
    await page.clock.fastForward(6000);
    await waitForCheck(() => polls > 0, 'Setup rendering: handoff did not poll');
    if (polls !== 1) problems.push('Setup rendering: a slow handoff overlapped ' + polls + ' polls');
    await page.keyboard.press('ArrowLeft');
    await releasePoll();
    await page.waitForTimeout(50);
    if (await page.locator('#qr').count()) problems.push('Setup rendering: a stale poll changed the screen after Back');
  } finally { await page.close(); }
}

async function visitTVDesignSystem(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('TV design system: ' + e.message));
  try {
    for (const theme of ['dark', 'light']) {
      await page.goto(base + PAGES.dashboard + '?tab=system&theme=' + theme);
      await page.waitForSelector('#rows .row.focus');
      const state = await page.evaluate(() => {
        const row = getComputedStyle(document.querySelector('#rows .row.focus'));
        return { fill: row.backgroundColor, text: row.color, properties: row.transitionProperty, duration: row.transitionDuration };
      });
      const dark = theme === 'dark';
      if (state.fill !== (dark ? 'rgb(227, 227, 227)' : 'rgb(48, 48, 48)') || state.text !== (dark ? 'rgb(19, 19, 20)' : 'rgb(255, 255, 255)')) {
        problems.push('TV design system: ' + theme + ' focus lost its shared colors: ' + JSON.stringify(state));
      }
      if (state.properties !== 'transform' || state.duration !== '0.12s') problems.push('TV design system: list focus animates paint or takes too long: ' + JSON.stringify(state));
      await page.waitForTimeout(220);
      await review(page, 'dashboard-' + theme);
    }
    await page.emulateMedia({ reducedMotion: 'reduce' });
    for (const [url, selector] of [[PAGES.dashboard, '#rows'],
      ['/assets/google-tv/pages/setup/index.html?demo=1&screen=network', '.row']]) {
      await page.goto(base + url);
      await page.waitForSelector(selector, { state: 'attached' });
      const duration = await page.locator(selector).first().evaluate(el => getComputedStyle(el).transitionDuration);
      if (duration !== '0s') problems.push('TV design system: reduced motion ignored by ' + url);
    }
  } finally { await page.close(); }
}

























async function visitTvDashboard(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('TV dashboard: ' + e.message));
  page.on('console', m => {
    if (m.type() === 'error' && !/^Failed to load resource/.test(m.text())) {
      problems.push('TV dashboard: console: ' + m.text());
    }
  });

  try {
    await page.goto(base + PAGES.dashboard, { waitUntil: 'load' });
    await page.waitForSelector('#rows .row');
    await page.waitForTimeout(100);

    // Two-column layout geometry check
    const layout = await page.evaluate(() => {
      const main = document.getElementById('main').getBoundingClientRect();
      const list = document.getElementById('list').getBoundingClientRect();
      const hint = document.getElementById('hint').getBoundingClientRect();
      const sel = document.querySelector('.row.sel');
      return {
        main: { w: main.width, h: main.height },
        list: { left: list.left, right: list.right, w: list.width, h: list.height },
        hint: { left: hint.left, right: hint.right, w: hint.width, h: hint.height },
        hasSel: !!sel,
        selLabel: sel ? (sel.querySelector('.lbl') ? sel.querySelector('.lbl').textContent.trim() : '') : ''
      };
    });

    if (layout.main.w <= 0 || layout.main.h <= 0) {
      problems.push('TV dashboard: #main has 0 dimensions (' + layout.main.w + 'x' + layout.main.h + ')');
    }
    if (layout.list.w <= 0 || layout.list.h <= 0) {
      problems.push('TV dashboard: #list column has 0 dimensions (' + layout.list.w + 'x' + layout.list.h + ')');
    }
    if (layout.hint.w <= 0 || layout.hint.h <= 0) {
      problems.push('TV dashboard: #hint column has 0 dimensions (' + layout.hint.w + 'x' + layout.hint.h + ')');
    }
    if (layout.hint.left < layout.list.right - 1) {
      problems.push('TV dashboard: #hint is not to the right of #list (hint left: ' + layout.hint.left + ', list right: ' + layout.list.right + ')');
    }
    if (!layout.hasSel) {
      problems.push('TV dashboard: no row is selected initially');
    }

    // Remote D-pad navigation: ArrowDown / ArrowUp moves selection and updates hint
    const initialTitle = await page.evaluate(() => document.getElementById('hintTitle').textContent.trim());
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(50);
    const secondTitle = await page.evaluate(() => document.getElementById('hintTitle').textContent.trim());
    if (!secondTitle || secondTitle === initialTitle) {
      problems.push('TV dashboard: ArrowDown did not update selected row hint title');
    }

    await page.keyboard.press('ArrowUp');
    await page.waitForTimeout(50);
    const restoredTitle = await page.evaluate(() => document.getElementById('hintTitle').textContent.trim());
    if (restoredTitle !== initialTitle) {
      problems.push('TV dashboard: ArrowUp did not restore initial row selection');
    }

    // PageDown / ChannelDown jumps multiple rows
    await page.keyboard.press('PageDown');
    await page.waitForTimeout(50);
    const pagedTitle = await page.evaluate(() => document.getElementById('hintTitle').textContent.trim());
    if (pagedTitle === initialTitle || pagedTitle === secondTitle) {
      problems.push('TV dashboard: PageDown did not jump selection forward');
    }

    // Repeated ArrowDown triggers translateY scroll and illuminates moreUp
    for (let i = 0; i < 15; i++) {
      await page.keyboard.press('ArrowDown');
      await page.waitForTimeout(20);
    }
    const scrollState = await page.evaluate(() => {
      const rows = document.getElementById('rows');
      const tr = rows.style.transform || rows.style.webkitTransform || '';
      const moreUp = document.getElementById('moreUp').classList.contains('on');
      return { transform: tr, moreUp };
    });
    if (!/translateY\(-/.test(scrollState.transform)) {
      problems.push('TV dashboard: scrolling down list did not apply negative translateY: ' + scrollState.transform);
    }
    if (!scrollState.moreUp) {
      problems.push('TV dashboard: scrolling down list did not illuminate moreUp arrow');
    }

    // Remote navigation: ArrowRight / ArrowLeft switches tabs
    const initialTab = await page.evaluate(() => {
      const on = document.querySelector('.tab.on');
      return on ? on.textContent.trim() : '';
    });
    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(100);
    const nextTab = await page.evaluate(() => {
      const on = document.querySelector('.tab.on');
      return on ? on.textContent.trim() : '';
    });
    if (!nextTab || nextTab === initialTab) {
      problems.push('TV dashboard: ArrowRight did not switch tabs (stayed on ' + initialTab + ')');
    }

    // Verify two columns still intact after tab switch
    const newTabListHeight = await page.evaluate(() => document.getElementById('list').getBoundingClientRect().height);
    if (newTabListHeight <= 0) {
      problems.push('TV dashboard: #list collapsed to 0 height after switching tabs to ' + nextTab);
    }

    await page.keyboard.press('ArrowLeft');
    await page.waitForTimeout(100);
    const backTab = await page.evaluate(() => {
      const on = document.querySelector('.tab.on');
      return on ? on.textContent.trim() : '';
    });
    if (backTab !== initialTab) {
      problems.push('TV dashboard: ArrowLeft did not return to ' + initialTab + ' (on ' + backTab + ')');
    }
  } finally {
    await page.close();
  }
}

async function visitTvNonOled(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('TV non-OLED: ' + e.message));
  await page.route('**/api/oledcare', route => {
    route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: true, isOled: false, available: false }) });
  });
  try {
    await page.goto(base + PAGES.dashboard, { waitUntil: 'load' });
    await page.waitForSelector('#rows .row');
    // Optional tabs are probed once the first data is on screen, or after 2.5 s.
    const omitted = await page.waitForFunction(() => ![...document.querySelectorAll('.tab')]
      .some(t => t.textContent.trim().toLowerCase() === 'oled care'), null, { timeout: 5000 }).then(() => true, () => false);
    if (!omitted) {
      problems.push('TV non-OLED: OLED Care tab was not omitted on an LCD TV');
    }
  } finally {
    await page.close();
  }
}

// Source separation must not turn CSS or JavaScript into startup requests.
// The legacy flat URLs must also resolve directly, without another redirect.
async function visitTVAssetAssembly(browser, base, problems) {
  for (const name of ['dashboard', 'setup', 'setup-phone']) {
    const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
    const extra = [];
    page.on('pageerror', e => problems.push('TV assets ' + name + ': ' + e.message));
    page.on('request', req => {
      if (['stylesheet', 'script'].includes(req.resourceType())) extra.push(new URL(req.url()).pathname);
    });
    try {
      const canonical = '/assets/google-tv/pages/' + name + '/index.html';
      const response = await page.goto(base + canonical + (name === 'setup' ? '?demo=1' : ''));
      const html = await response.text();
      if (response.status() !== 200) problems.push('TV assets: ' + canonical + ' returned ' + response.status());
      if (/<link\b[^>]*rel="stylesheet"|<script\b[^>]*\bsrc=/.test(html)) problems.push('TV assets: ' + name + ' left CSS/JS to fetch');
      await page.waitForTimeout(100);
      if (extra.length) problems.push('TV assets: ' + name + ' made extra CSS/JS requests: ' + extra.join(', '));
      const legacy = '/assets/google-tv/pages/' + name + '.html';
      const old = await page.request.get(base + legacy, { maxRedirects: 0 });
      if (old.status() !== 200) problems.push('TV assets: flat alias ' + legacy + ' requires a redirect or is missing');
    } finally { await page.close(); }
  }
}

// A TV app installed from an older release opens the old addresses; each lands on its page.
async function visitMovedPages(browser, base, problems) {
  const page = await browser.newPage();
  try {
    for (const [old, now] of [['/assets/dashboard.html?tab=apps', PAGES.dashboard + '?tab=apps'], ['/assets/setup.html?demo=1', '/assets/google-tv/pages/setup/index.html?demo=1']]) {
      await page.goto(base + old);
      if (page.url() !== base + now) problems.push('Moved pages: ' + old + ' opened ' + page.url());
    }
  } finally { await page.close(); }
}


let serverOutput = () => '';
(async () => {
  const server = await startServer();
  serverOutput = server.output;
  const base = 'http://127.0.0.1:' + server.port;
  const browser = await chromium.launch(process.env.BROWSER_CHANNEL ? { channel: process.env.BROWSER_CHANNEL } : {});
  const problems = [];
  const web = webTabs(), tv = tvTabs();
  const filter = process.env.DASHBOARD_CHECK_FILTER && new RegExp(process.env.DASHBOARD_CHECK_FILTER, 'i');
  if (!filter) {
    await visitAll(browser, web.map(t => ({ url: base + '/?tab=' + t, label: 'web ' + t })), problems);
    await visitAll(browser, tv.map(t => ({ url: base + PAGES.dashboard + '?tab=' + t, label: 'TV ' + t })), problems);
  }
  const checks = [visitTvDashboard, visitTvNonOled, visitMovedPages, visitTVAssetAssembly, visitTVPolling, visitTVRendering, visitTVFastStats,
    visitSetupRendering, visitTVDesignSystem];
  let checked = 0;
  for (const check of checks) {
    if (filter && !filter.test(check.name)) continue;
    console.log('Checking ' + check.name);
    await check(browser, base, problems);
    checked++;
  }
  await browser.close();
  for (const p of problems) console.log(p);
  console.log((filter ? 'Selected checks' : (web.length + tv.length) + ' tabs opened (' + web.length + ' web, ' + tv.length + ' TV app)') + ', ' +
              checked + ' regression groups checked, ' + problems.length + ' problem' + (problems.length === 1 ? '' : 's'));
  process.exit(problems.length ? 1 : 0);
})().catch(e => { console.error(e.stack || e.message); const log = path.join(os.tmpdir(), 'nativetv-dashboards-daemon.log'); fs.writeFileSync(log, serverOutput()); console.error('--- daemon output: last lines below, all of it in ' + log + ' ---\n' + serverOutput().slice(-3000)); process.exit(1); });
