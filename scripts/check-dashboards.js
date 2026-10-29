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
const PAGES = { dashboard: '/assets/google-tv/pages/dashboard/index.html', home: '/assets/google-tv/pages/home/index.html' };
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

async function visitHomeBackground(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home background: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: ['one'], picture: true };
  const counts = {};
  let build = 'one', version = 'one', navigations = 0;
  await page.route('**/api/home/**', route => {
    const key = new URL(route.request().url()).pathname.split('/').pop();
    counts[key] = (counts[key] || 0) + 1;
    let body = { ok: true, build, layoutVersion: version };
    if (key === 'catalogue') body = { ok: true, layout, layoutVersion: 'one', apps: [], devices: [], channels: [],
      order: { app: ['lg-home'], dev: [], ch: [] }, sources: { channels: 0 } };
    if (key === 'now') body = { kind: 'input', port: 1, title: 'HDMI 1' };
    if (key === 'guide') body = { ok: true, guide: {} };
    if (key === 'picture') body = { ok: true, mode: { display: 'Cinema' }, light: 50 };
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    const now = Date.now();
    await page.clock.install({ time: new Date(now) });
    await page.clock.pauseAt(new Date(now + 1000));
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile');
    await waitForCheck(() => (counts.beat || 0) > 0 && (counts.picture || 0) > 0, 'Home background: initial data did not load');
    await page.waitForTimeout(100);
    page.on('framenavigated', frame => { if (frame === page.mainFrame()) navigations++; });
    await setHidden(page, true);
    const before = Object.assign({}, counts);
    build = 'two'; version = 'two';
    await page.clock.fastForward(6 * 60000);
    await waitForCheck(() => counts.beat > before.beat, 'Home background: the safety heartbeat stopped');
    await page.waitForTimeout(100);
    for (const key of ['catalogue', 'now', 'guide', 'picture']) {
      if (counts[key] !== before[key]) problems.push('Home background: hidden ' + key + ' work continued');
    }
    if (navigations) problems.push('Home background: a hidden heartbeat reloaded the page');
    build = version = 'one';
    await setHidden(page, false);
    await waitForCheck(() => counts.catalogue > before.catalogue && counts.now > before.now, 'Home background: return did not refresh immediately');
  } finally { await page.close(); }
}

async function visitHomeRefreshWork(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home refresh work: ' + e.message));
  const layout = { channels: [], hidden: {}, showLive: false, showInputs: false, picture: true };
  let discoverReply, pictureReply, wallReply;
  const reply = (route, body) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  await page.addInitScript(() => {
    const NativeImage = window.Image;
    window.__ghImages = 0;
    window.Image = function () { window.__ghImages++; return new NativeImage(); };
  });
  await page.route('**/api/home/**', route => {
    const key = new URL(route.request().url()).pathname.split('/').pop();
    const wallpapers = { available: true, revision: 'one' };
    if (key === 'catalogue') return reply(route, { ok: true, layout, wallpapers, apps: [], devices: [], channels: [],
      order: { app: ['lg-home'], dev: [], ch: [] }, sources: { channels: 0 } });
    if (key === 'discover' && !discoverReply) { discoverReply = () => reply(route, { ok: true, enabled: true,
      rows: [{ key: 'trending', items: [{ id: 1, title: 'A late title' }] }] }); return; }
    if (key === 'picture' && !pictureReply) { pictureReply = () => reply(route, { ok: true, mode: { display: 'Cinema' } }); return; }
    if (key === 'wallpaper') {
      wallReply = () => route.fulfill({ contentType: 'image/gif', body: Buffer.from('R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==', 'base64') });
      return;
    }
    if (key === 'now') return reply(route, { kind: 'input', port: 1 });
    if (key === 'discover') return reply(route, { ok: true, enabled: true, rows: [{ key: 'trending', items: [{ id: 1, title: 'A late title' }] }] });
    if (key === 'picture') return reply(route, { ok: true, mode: { display: 'Cinema' } });
    return reply(route, { ok: true, wallpapers });
  });
  try {
    await page.clock.install();
    await page.clock.pauseAt(new Date());
    await page.goto(base + PAGES.home, { waitUntil: 'domcontentloaded' });
    await waitForCheck(() => !!discoverReply && !!pictureReply && !!wallReply, 'Home refresh work: requests did not start');
    await page.clock.fastForward(15000);
    const images = await page.evaluate(() => window.__ghImages);
    if (images !== 1) problems.push('Home refresh work: one pending wallpaper created ' + images + ' image loads');
    await wallReply();
    await page.waitForTimeout(50);
    await setHidden(page, true);
    await page.evaluate(() => {
      window.__ghHiddenMutations = 0;
      const observer = new MutationObserver(list => { window.__ghHiddenMutations += list.length; });
      for (const id of ['rows', 'hero', 'plate']) observer.observe(document.getElementById(id), { subtree: true, childList: true, attributes: true });
    });
    await discoverReply();
    await pictureReply();
    await page.waitForTimeout(100);
    const mutations = await page.evaluate(() => window.__ghHiddenMutations);
    if (mutations) problems.push('Home refresh work: late hidden responses made ' + mutations + ' DOM changes');
    await setHidden(page, false);
    await page.waitForFunction(() => document.getElementById('rows').textContent.includes('Trending today') && document.getElementById('plate').textContent === 'Cinema');
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
    for (const [url, selector] of [[PAGES.dashboard, '#rows'], [PAGES.home, '#rows'],
      ['/assets/google-tv/pages/setup/index.html?demo=1&screen=network', '.row']]) {
      await page.goto(base + url);
      await page.waitForSelector(selector, { state: 'attached' });
      const duration = await page.locator(selector).first().evaluate(el => getComputedStyle(el).transitionDuration);
      if (duration !== '0s') problems.push('TV design system: reduced motion ignored by ' + url);
    }
  } finally { await page.close(); }
}

// App icons can be padded transparent marks, opaque squares or ready-made
// banners. Check the real decoded art, including a failed canvas read, rather
// than relying on the 1px images used by navigation/performance checks.
async function visitHomeAppArt(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('Home app art: ' + e.message));
  const art = {
    square: '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256"><rect width="256" height="256" fill="#bd1526"/><path fill="white" d="M68 64h30l60 88V64h30v128h-30l-60-88v88H68z"/></svg>',
    padded: '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256"><path fill="#48cad7" d="M108 88l56 40-56 40z"/></svg>',
    wide: '<svg xmlns="http://www.w3.org/2000/svg" width="320" height="180"><rect width="320" height="180" fill="#182546"/><text x="160" y="110" text-anchor="middle" fill="white" font-size="56" font-family="sans-serif">Cinema</text></svg>'
  };
  // Real low-resolution PNGs expose over-enlargement that the scalable SVG
  // fixtures above hide, especially when most of the source is padding.
  const raster = await page.evaluate(() => {
    const result = {};
    for (const [name, width, height, background, mark] of [
      ['small', 64, 64, '#bd1526', [16, 16, 32, 32]],
      ['small-padded', 128, 128, null, [52, 48, 24, 32]],
      ['small-wide', 96, 54, '#182546', [24, 18, 48, 18]],
      ['small-light', 64, 64, '#f1eee8', [16, 16, 32, 32]],
      ['padded-wide', 512, 128, null, [112, 40, 288, 48]]
    ]) {
      const canvas = document.createElement('canvas');
      canvas.width = width; canvas.height = height;
      const ctx = canvas.getContext('2d');
      if (background) { ctx.fillStyle = background; ctx.fillRect(0, 0, width, height); }
      ctx.fillStyle = name === 'small-light' ? '#202124' : '#ffffff'; ctx.fillRect(...mark);
      result[name] = canvas.toDataURL('image/png').split(',')[1];
    }
    return result;
  });
  const apps = [
    { id: 'square', title: 'Square logo', icon: '/app-art/square.svg', color: '#123456' },
    { id: 'padded', title: 'Padded logo', icon: '/app-art/padded.svg', color: '#287d9c' },
    { id: 'wide', title: 'Cinema', icon: '/app-art/wide.svg' },
    { id: 'fallback', title: 'Media Library', icon: null },
    { id: 'small', title: 'Small source', icon: '/app-art/small.png' },
    { id: 'small-padded', title: 'Small padded source', icon: '/app-art/small-padded.png', color: '#287d9c' },
    { id: 'small-wide', title: 'Small banner', icon: '/app-art/small-wide.png' },
    { id: 'small-light', title: 'Light artwork', icon: '/app-art/small-light.png' },
    { id: 'padded-wide', title: 'Wide logo', icon: '/app-art/padded-wide.png', color: '#287d9c' }
  ];
  await page.route('**/app-art/*', route => {
    const name = new URL(route.request().url()).pathname.split('/').pop().split('.')[0];
    return route.fulfill(raster[name] ? { contentType: 'image/png', body: Buffer.from(raster[name], 'base64') }
      : { contentType: 'image/svg+xml', body: art[name] });
  });
  await page.route('**/api/home/**', route => {
    const key = new URL(route.request().url()).pathname.split('/').pop();
    let body = { ok: true, build: 'one', kind: null };
    if (key === 'catalogue') body = { ok: true, layout: { channels: [], hidden: {}, showInputs: false, showLive: false },
      apps, devices: [], channels: [], sources: { channels: 0 }, order: { app: apps.map(a => a.id).concat(['lg-home']), dev: [], ch: [] } };
    if (key === 'discover') body = { ok: true, enabled: false, rows: [] };
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  async function artworkReady() {
    await page.waitForFunction(() => {
      const images = [...document.querySelectorAll('#rows img')];
      return images.length === 8 && images.every(img => img.naturalWidth && img.style.width);
    });
  }
  async function geometry() {
    return page.evaluate(() => [...document.querySelectorAll('#rows .tile.app')].map(tile => {
      const shot = tile.querySelector('.shot'), img = shot.querySelector('img');
      return { label: tile.getAttribute('aria-label'), background: shot.style.background, fallback: shot.textContent,
        width: img && parseFloat(img.style.width), height: img && parseFloat(img.style.height),
        sourceWidth: img && img.naturalWidth, sourceHeight: img && img.naturalHeight,
        left: img && parseFloat(img.style.left), top: img && parseFloat(img.style.top) };
    }));
  }
  try {
    await page.goto(base + PAGES.home);
    await artworkReady();
    const fitted = await geometry();
    if (fitted[0].background !== 'rgb(189, 21, 38)') problems.push('Home app art: the square background did not extend across the card');
    if (fitted[0].height < 180 || fitted[0].width !== fitted[0].height) problems.push('Home app art: the square logo stayed small or was distorted');
    const padded = fitted[1];
    // The actual play mark occupies x108..164, y88..168 in a padded 256px
    // source. It should fit inside the card at a useful size, with no crop.
    const visible = { left: padded.left + padded.width * 108 / 256, right: padded.left + padded.width * 164 / 256,
      top: padded.top + padded.height * 88 / 256, bottom: padded.top + padded.height * 168 / 256 };
    if (visible.left < 0 || visible.right > 240 || visible.top < 0 || visible.bottom > 135 || visible.bottom - visible.top < 90)
      problems.push('Home app art: transparent padding shrank or clipped the visible logo');
    if (!padded.background.includes('linear-gradient')) problems.push('Home app art: transparent artwork lost its app colour');
    if (fitted[2].width !== 240 || fitted[2].height !== 135) problems.push('Home app art: wide artwork did not fill the card');
    if (fitted[3].fallback !== 'Media Library') problems.push('Home app art: the fallback is not the app name');
    for (const item of fitted.slice(4)) {
      if (item.width > item.sourceWidth * 1.25 || item.height > item.sourceHeight * 1.25)
        problems.push('Home app art: ' + item.label + ' was enlarged beyond its source detail');
      if (Math.abs(item.width / item.height - item.sourceWidth / item.sourceHeight) > .001)
        problems.push('Home app art: ' + item.label + ' was distorted by the quality limit');
    }
    for (const item of fitted.slice(4, 8)) {
      if (item.fallback !== item.label || item.left >= 0 && item.left + item.width / 2 >= 88)
        problems.push('Home app art: ' + item.label + ' did not pair its small logo with its name');
    }
    if (await page.locator('[aria-label="Light artwork"] .app-wordmark').evaluate(el => getComputedStyle(el).color) !== 'rgb(32, 33, 36)')
      problems.push('Home app art: the small logo label has insufficient contrast on light artwork');
    const wideLogo = fitted[8];
    if (wideLogo.width * 288 / 512 < 180 || Math.abs(wideLogo.left + wideLogo.width / 2 - 120) > 1 || wideLogo.fallback)
      problems.push('Home app art: the padded wide logo was not fitted and centred as artwork');
    await review(page, 'home-app-art');

    await page.locator('#tabs .tab', { hasText: 'Apps' }).click();
    await artworkReady();
    if (JSON.stringify(await geometry()) !== JSON.stringify(fitted)) problems.push('Home app art: Apps uses a different artwork treatment from Home');
    if (await page.locator('.lgtile .app-wordmark').textContent() !== 'LG Home') problems.push('Home app art: LG Home is missing its wordmark');
    await review(page, 'apps-app-art');

    // A blocked canvas must preserve the original artwork and must not prevent
    // following icons from being analysed. This also exercises the CSS fit.
    await page.addInitScript(() => {
      const original = CanvasRenderingContext2D.prototype.getImageData;
      let first = true;
      CanvasRenderingContext2D.prototype.getImageData = function (...args) {
        if (first) { first = false; throw new DOMException('Unreadable artwork', 'SecurityError'); }
        return original.apply(this, args);
      };
    });
    await page.reload();
    await artworkReady();
    const fallback = await geometry();
    if (fallback[0].width !== 135 || fallback[0].height !== 135 || fallback[1].height < 250)
      problems.push('Home app art: an unreadable icon broke the contain fallback or following artwork');
  } finally { await page.close(); }
}

async function visitHomeImageBudget(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('Home image budget: ' + e.message));
  const apps = Array.from({ length: 60 }, (_, i) => ({ id: 'app' + i, title: 'App ' + i, icon: '/artwork/' + i + '.gif' }));
  const layout = { channels: [], hidden: {}, showInputs: false, showLive: false, showLgHome: false };
  const requests = [];
  const gif = Buffer.from('R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==', 'base64');
  await page.route('**/artwork/*', route => { requests.push(route); });
  await page.route('**/api/home/**', route => {
    const key = new URL(route.request().url()).pathname.split('/').pop();
    let body = { ok: true, build: 'one', kind: null };
    if (key === 'catalogue') body = { ok: true, layout, apps, devices: [], channels: [], sources: { channels: 0 }, order: { app: apps.map(a => a.id), dev: [], ch: [] } };
    if (key === 'discover') body = { ok: true, enabled: false, rows: [] };
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.clock.install();
    await page.clock.pauseAt(new Date());
    await page.goto(base + PAGES.home, { waitUntil: 'domcontentloaded' });
    await page.waitForSelector('.tile.focus');
    await page.clock.runFor(1);
    await waitForCheck(() => requests.length >= 2, 'Home image budget: first batch did not start');
    if (requests.length !== 2) problems.push('Home image budget: first batch started ' + requests.length + ' images');
    if (!requests[0].request().url().endsWith('/0.gif')) problems.push('Home image budget: focused artwork was not first');
    await page.addScriptTag({ content: fs.readFileSync(path.join(ROOT, 'scripts/profile-home.js'), 'utf8') });
    await page.evaluate(() => window.ghHomeProfile.start());
    await page.clock.runFor(100);
    await waitForCheck(() => requests.length >= 4, 'Home image budget: second batch did not start');
    if (requests.length !== 4) problems.push('Home image budget: outstanding requests exceeded four (' + requests.length + ')');
    await requests[0].fulfill({ contentType: 'image/gif', body: gif });
    await requests[1].abort();
    await page.waitForFunction(() => Array.from(document.querySelectorAll('#rows img')).filter(img => img.complete).length >= 1);
    await page.waitForTimeout(30);
    await page.clock.runFor(20);
    await waitForCheck(() => requests.length >= 6, 'Home image budget: success/failure did not release slots');
    if (requests.length !== 6) problems.push('Home image budget: completion started an unbounded batch');

    // Leaving a tab releases its outstanding slots, while completed art stays
    // cached with the retained nodes. Returning must retry cancelled pictures.
    await page.keyboard.press('ArrowUp');
    await page.keyboard.press('ArrowRight');
    // Entering content commits the pending tab selection immediately.
    await page.keyboard.press('ArrowDown');
    await page.clock.runFor(40);
    await waitForCheck(() => requests.length > 6, 'Home image budget: old tab requests blocked Apps');
    await setHidden(page, true);
    const hiddenCount = requests.length;
    await page.clock.runFor(1000);
    if (requests.length !== hiddenCount) problems.push('Home image budget: pictures started while hidden');
    await setHidden(page, false);
    await page.clock.runFor(40);
    await waitForCheck(() => requests.length > hiddenCount, 'Home image budget: cancelled pictures were not retried on resume');
    const profile = await page.evaluate(() => window.ghHomeProfile.stop());
    if (!profile.frameIntervals.samples || !profile.keyEventToFrameCallback.samples) problems.push('Home image budget: profiling probe did not collect frames and input');
    if (profile.frameIntervals.maxMs > 100) problems.push('Home image budget: profiling probe included hidden time as a frame gap');
  } finally { await page.close(); }
}

async function visitHomeImageRecovery(browser, base, problems) {
  const gif = Buffer.from('R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==', 'base64');
  for (const mode of ['retry', 'title', 'hero', 'backoff', 'focus', 'replace', 'replace-pending']) {
    const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
    page.on('pageerror', e => problems.push('Home image recovery: ' + e.message));
    const reads = {};
    let version = 1, catalogueReads = 0;
    const apps = Array.from({ length: mode === 'focus' ? 20 : 1 }, (_, i) => ({ id: 'app' + i, title: 'App ' + i, icon: mode === 'title' || mode === 'hero' ? null : '/recovery/' + i + '.gif' }));
    const readName = mode === 'title' ? '/recovery/title.gif' : mode === 'hero' ? '/recovery/hero.gif' : '/recovery/0.gif';
    function artwork(route, name) {
      reads[name] = (reads[name] || 0) + 1;
      if (mode === 'focus' || (mode === 'replace-pending' && name === '/recovery/0.gif')) return;
      const failed = mode === 'backoff' || (mode.indexOf('replace') !== 0 && reads[name] === 1);
      return route.fulfill({ status: failed ? 404 : 200, contentType: 'image/gif', body: failed ? Buffer.from('temporarily missing') : gif });
    }
    await page.route('**/recovery/*', route => {
      const name = new URL(route.request().url()).pathname;
      return artwork(route, name);
    });
    await page.route('**/api/home/**', route => {
      const key = new URL(route.request().url()).pathname.split('/').pop();
      let body = { ok: true, build: 'one', layoutVersion: String(version), kind: null };
      if (key === 'catalogue') {
        catalogueReads++;
        body = { ok: true, layoutVersion: String(version), layout: { channels: [], hidden: {}, showLgHome: false }, apps, devices: [], channels: [],
          sources: { channels: 0 }, order: { app: apps.map(a => a.id), dev: [], ch: [] } };
      }
      if (key === 'discover') body = { ok: true, enabled: false, rows: [] };
      if (key === 'discover' && mode === 'title') body = { ok: true, enabled: true, rows: [{ key: 'trending',
        items: [{ id: 1, type: 'movie', title: 'Recovering title', image: '/recovery/title.gif' }] }] };
      if (key === 'discover' && mode === 'hero') body = { ok: true, enabled: true, rows: [{ key: 'trending',
        items: [{ id: 1, type: 'movie', title: 'Recovering hero', backdrop: '/recoverhero.jpg' }] }] };
      if (key === 'image' && mode === 'title') return artwork(route, readName);
      if (key === 'image' && mode === 'hero') return new URL(route.request().url()).searchParams.get('u').includes('w1280')
        ? artwork(route, readName) : route.fulfill({ contentType: 'image/gif', body: gif });
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
    });
    try {
      await page.clock.install();
      await page.clock.pauseAt(new Date());
      await page.goto(base + PAGES.home, { waitUntil: 'domcontentloaded' });
      await page.waitForSelector('.tile.focus');
      if (mode === 'title') {
        await page.waitForSelector('.tile.pick');
        await page.keyboard.press('ArrowDown');
      }
      // Discover arrives independently of the app catalogue; the hero then
      // waits 400ms for navigation to settle before requesting its artwork.
      if (mode === 'hero') await page.waitForSelector('.tile.pick');
      await page.clock.runFor(mode === 'hero' ? 450 : 40);
      await waitForCheck(() => !!reads[readName], 'Home image recovery (' + mode + '): no initial image');
      if (mode.indexOf('replace') === 0) {
        if (mode === 'replace') await page.waitForFunction(() => document.querySelector('.tile.focus img').naturalWidth > 0);
        await page.evaluate(() => { window.__ghSourceTile = document.querySelector('.tile.focus'); });
        const before = catalogueReads;
        apps[0].icon = '/recovery/new.gif'; version++;
        await page.clock.fastForward(5000);
        await waitForCheck(() => catalogueReads > before, 'Home image recovery: replacement catalogue did not load');
        await page.clock.runFor(100);
        await page.waitForFunction(() => Array.from(document.querySelectorAll('.tile.focus img')).some(img =>
          img.getAttribute('src') === '/recovery/new.gif' && img.complete && img.naturalWidth));
        const state = await page.evaluate(() => ({
          same: window.__ghSourceTile === document.querySelector('.tile.focus'), count: document.querySelectorAll('.tile.focus img').length,
          src: document.querySelector('.tile.focus img').getAttribute('src')
        }));
        if (!state.same || state.count !== 1 || state.src !== '/recovery/new.gif') problems.push('Home image recovery: ' + mode + ' kept stale artwork or replaced the tile');
      } else if (mode === 'backoff') {
        await page.waitForFunction(() => !document.querySelector('.tile.focus img'));
        for (let i = 0; i < 8; i++) {
          const before = reads[readName];
          await page.clock.fastForward(31000);
          await waitForCheck(() => reads[readName] > before, 'Home image recovery: retries exceeded the 30-second backoff cap');
          await page.waitForFunction(() => !document.querySelector('.tile.focus img'));
          const after = reads[readName];
          await page.clock.runFor(500);
          if (reads[readName] !== after) problems.push('Home image recovery: repeated failures caused a retry burst');
        }
        await setHidden(page, true);
        const before = reads[readName];
        await page.clock.fastForward(60000);
        await page.waitForTimeout(30);
        if (reads[readName] !== before) problems.push('Home image recovery: hidden retries continued');
      } else if (mode !== 'focus') {
        await page.waitForFunction(() => !document.querySelector('.tile.focus img'));
        await page.clock.runFor(500);
        if (reads[readName] !== 1) problems.push('Home image recovery: failure retried without backoff');
        await page.clock.runFor(600);
        await page.waitForTimeout(50);
        if (reads[readName] !== 2) problems.push('Home image recovery: ' + mode + ' artwork did not retry');
        else if (mode === 'hero') await page.waitForFunction(() => Array.from(document.querySelectorAll('.back.on')).some(back => back.style.backgroundImage.includes('w1280')));
        else await page.waitForFunction(() => {
          const tile = document.querySelector('.tile.focus'), img = tile.querySelector('img');
          return img && img.complete && img.naturalWidth && !tile.querySelector('.mono') && !tile.querySelector('.nopic');
        });
      } else {
        await page.clock.runFor(40);
        await waitForCheck(() => Object.keys(reads).length === 4, 'Home image recovery: four slow requests did not start');
        for (let i = 0; i < 4; i++) await page.keyboard.press('ArrowRight');
        await page.clock.runFor(40);
        await page.waitForTimeout(50);
        if (!reads['/recovery/4.gif']) problems.push('Home image recovery: old requests blocked newly focused artwork');
        await page.clock.fastForward(14000);
        await page.waitForTimeout(50);
        await page.clock.runFor(1100);
        await page.waitForTimeout(50);
        if (!(reads['/recovery/4.gif'] >= 2)) problems.push('Home image recovery: timed-out artwork did not release its slot and retry');
      }
    } finally { await page.close(); }
  }
}

async function visitHomeRefreshScope(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home refresh scope: ' + e.message));
  let version = 1, catalogueReads = 0;
  const apps = Array.from({ length: 600 }, (_, i) => ({ id: 'app' + i, title: 'App ' + i, icon: null }));
  const devices = [{ id: 'hdmi1', name: 'HDMI 1', port: 1 }];
  await page.route('**/api/home/**', route => {
    const key = new URL(route.request().url()).pathname.split('/').pop();
    let body = { ok: true, build: 'one', layoutVersion: String(version), kind: null };
    if (key === 'catalogue') {
      catalogueReads++;
      body = { ok: true, layoutVersion: String(version), layout: { channels: [], hidden: {}, showLgHome: false }, apps, devices, channels: [],
        sources: { channels: 0 }, order: { app: apps.map(a => a.id), dev: ['hdmi1'], ch: [] } };
    }
    if (key === 'discover') body = { ok: true, enabled: false, rows: [] };
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.clock.install();
    await page.clock.pauseAt(new Date());
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.focus');
    await page.keyboard.press('ArrowUp');
    await page.keyboard.press('ArrowRight');
    await page.keyboard.press('ArrowDown');
    await page.keyboard.press('ArrowRight');
    await page.clock.runFor(200);
    await page.evaluate(() => {
      const stringify = JSON.stringify;
      window.__ghFullScans = 0;
      JSON.stringify = function (value) {
        if (Array.isArray(value) && value[0] && value[0].apps) window.__ghFullScans++;
        return stringify.apply(this, arguments);
      };
      window.__ghRowWrites = 0;
      const observer = new MutationObserver(list => { window.__ghRowWrites += list.length; });
      observer.observe(document.getElementById('rows'), { subtree: true, attributes: true, childList: true });
      window.__ghResetWrites = () => { observer.takeRecords(); window.__ghRowWrites = 0; };
    });
    await page.clock.fastForward(60000);
    await page.waitForTimeout(50);
    if (await page.evaluate(() => window.__ghFullScans)) problems.push('Home refresh scope: a source-status refresh serialized the whole catalogue');
    for (const change of ['input', 'offscreen app']) {
      await page.evaluate(() => window.__ghResetWrites());
      if (change === 'input') devices[0].name = 'Console';
      else apps[599].title = 'Changed offscreen app';
      version++;
      const before = catalogueReads;
      await page.clock.runFor(5000);
      await waitForCheck(() => catalogueReads > before, 'Home refresh scope: no catalogue update');
      await page.waitForTimeout(50);
      if (await page.evaluate(() => window.__ghRowWrites)) problems.push('Home refresh scope: ' + change + ' refresh touched unchanged visible Apps rows');
      if (await page.locator('#rows .tile.focus').getAttribute('aria-label') !== 'App 1') problems.push('Home refresh scope: background refresh moved focus');
    }
    apps[1].title = 'Changed visible app'; version++;
    await page.clock.runFor(5000);
    await page.waitForFunction(() => document.querySelector('#rows .tile.focus').getAttribute('aria-label') === 'Changed visible app');
  } finally { await page.close(); }
}

async function visitHomePicture(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home picture: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: [] };
  const hdmi4 = { ok: true, range: 'hdr', mode: { display: 'HDR Filmmaker' }, light: 100, oled: true,
    signal: { width: 3840, height: 2160, hz: 24, chroma: 'YCbCr 4:4:4', bits: 10, hdr: 'hdr10' } };
  const netflix = { ok: true, range: 'dolbyHdr', mode: { display: 'Dolby Vision Cinema Bright' }, light: 100, oled: true,
    signal: { width: 3840, height: 2160, hz: 23.98, hdr: 'dolby_vision', played: true } };
  const asleep = { ok: true, range: null, mode: null, light: null, oled: true, signal: null };
  let source = { kind: 'input', port: 4, title: 'HDMI 4' };
  let releaseCatalogue = null, latePicture = null, holdPicture = false, pictureReads = 0;
  const reply = (route, body) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  await page.route('**/api/home/**', route => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith('/catalogue')) {
      const body = { ok: true, layout, apps: [], devices: [], channels: [], sources: { channels: 0 },
        order: { app: ['lg-home'], dev: [], ch: [] } };
      if (!releaseCatalogue) {
        const snapshot = JSON.parse(JSON.stringify(body));
        releaseCatalogue = () => reply(route, snapshot);
        return;
      }
      return reply(route, body);
    }
    if (url.pathname.endsWith('/now')) return reply(route, source);
    if (url.pathname.endsWith('/picture')) {
      pictureReads++;
      const body = url.searchParams.get('kind') === 'app' ? netflix : url.searchParams.get('port') === '2' ? asleep : hdmi4;
      if (holdPicture) {
        holdPicture = false;
        latePicture = () => reply(route, body);
        return;
      }
      return reply(route, body);
    }
    return reply(route, { ok: true, build: 'one' });
  });
  async function returned() {
    const catalogue = page.waitForResponse(r => new URL(r.url()).pathname === '/api/home/catalogue');
    const now = page.waitForResponse(r => new URL(r.url()).pathname === '/api/home/now');
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await Promise.all([catalogue, now]);
    await page.waitForTimeout(100);
  }
  async function delayPicture() {
    latePicture = null;
    holdPicture = true;
    await page.clock.fastForward(15000);
    const until = Date.now() + 4000;
    while (!latePicture && Date.now() < until) await page.waitForTimeout(20);
    if (!latePicture) throw new Error('Home picture: no refresh to delay');
  }
  const plate = () => page.locator('#plate').textContent();
  try {
    await page.clock.install();
    const firstNow = page.waitForResponse(r => new URL(r.url()).pathname === '/api/home/now');
    await page.goto(base + PAGES.home);
    await firstNow;
    await page.waitForTimeout(100);
    if (pictureReads) problems.push('Home picture: fetched a plate before the layout was known');
    await releaseCatalogue();
    await page.waitForSelector('.tile');
    await page.waitForTimeout(100);
    if (await page.locator('#plate').isVisible()) problems.push('Home picture: a layout without picture details showed a plate');
    layout.picture = true;
    await returned();
    await page.waitForFunction(() => document.getElementById('plate').textContent.indexOf('HDR Filmmaker') !== -1);
    if ((await plate()).indexOf('3840 × 2160 at 24 Hz') === -1) problems.push('Home picture: the plate left out the signal');
    source = { kind: 'input', port: 2, title: 'HDMI 2' };
    await returned();
    await page.waitForFunction(() => !document.getElementById('plate').offsetParent);
    source = { kind: 'input', port: 4, title: 'HDMI 4' };
    await returned();
    await page.waitForFunction(() => document.getElementById('plate').textContent.indexOf('HDR Filmmaker') !== -1);
    await delayPicture();
    source = { kind: 'app', appId: 'netflix', title: 'Netflix' };
    await returned();
    await page.waitForFunction(() => document.getElementById('plate').textContent.indexOf('Dolby Vision Cinema Bright') !== -1);
    await latePicture();
    await page.waitForTimeout(100);
    if ((await plate()).indexOf('Dolby Vision Cinema Bright') === -1) problems.push('Home picture: a late HDMI reply replaced the app picture');
    await delayPicture();
    layout.picture = false;
    await returned();
    if (await page.locator('#plate').isVisible()) problems.push('Home picture: returning with details off kept the plate');
    await latePicture();
    await page.waitForTimeout(100);
    if (await page.locator('#plate').isVisible()) problems.push('Home picture: a late reply restored a disabled plate');
  } finally { await page.close(); }
}

async function visitHomeColours(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home colours: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: [], colours: null };
  let version = 1;
  await page.route('**/api/home/**', route => {
    const pathname = new URL(route.request().url()).pathname;
    const body = pathname === '/api/home/catalogue'
      ? { ok: true, layout, layoutVersion: String(version), apps: [], devices: [], channels: [],
          sources: { channels: 0 }, order: { app: ['lg-home'], dev: [], ch: [] } }
      : { ok: true, build: 'one', layoutVersion: String(version) };
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const rgb = hex => 'rgb(' + hex.slice(1).match(/../g).map(v => parseInt(v, 16)).join(', ') + ')';
  async function check(text, accent) {
    const actual = await page.evaluate(() => ({
      text: getComputedStyle(document.getElementById('clockTime')).color,
      focus: getComputedStyle(document.querySelector('.tile.focus .ring')).boxShadow,
      hero: getComputedStyle(document.querySelector('#hero .title')).color,
      boot: getComputedStyle(document.querySelector('#boot path')).stroke
    }));
    if (actual.text !== rgb(text)) problems.push('Home colours: text ' + text + ' rendered as ' + actual.text);
    if (actual.focus.indexOf(rgb(accent)) === -1) problems.push('Home colours: accent ' + accent + ' rendered as ' + actual.focus);
    if (actual.hero !== rgb(text) || actual.boot !== rgb(text)) problems.push('Home colours: shared carousel/boot lost the text override: ' + JSON.stringify(actual));
  }
  try {
    await page.clock.install();
    // Chosen colours can coincide with any original colour in the stylesheet.
    for (const accent of ['#ece7df', '#b3ada3', '#8d877d', '#e9ad55', '#30de61']) {
      layout.colours = { text: '#d4e6f4', accent };
      await page.goto(base + PAGES.home);
      await page.waitForSelector('.tile.focus');
      await check('#d4e6f4', accent);
    }
    // Changes made elsewhere, including reset, arrive through the heartbeat.
    for (const colours of [{ text: '#f4dcdc', accent: '#30de61' }, null]) {
      layout.colours = colours;
      version++;
      await page.clock.fastForward(5000);
      const text = colours ? colours.text : '#ece7df';
      await page.waitForFunction(expected => getComputedStyle(document.getElementById('clockTime')).color === expected, rgb(text));
      await check(text, colours ? colours.accent : text);
    }
  } finally { await page.close(); }
}

async function visitHomePreferences(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home preferences: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: ['news'], appNames: true, clockFormat: '24h' };
  let openOnPowerOn = false, version = 1, opened = null;
  await page.route(/\/api\/home(?:[/?]|$)/, async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = { ok: true, build: 'one', layoutVersion: String(version) };
    if (pathname === '/api/home') body = { ok: true, supported: { ok: true }, enabled: true,
      active: true, writable: true, openOnPowerOn, layout, history: true, hideLgHomeAtStartAvailable: false };
    else if (pathname.endsWith('/catalogue')) body = { ok: true, layout, layoutVersion: String(version),
      apps: [{ id: 'netflix', title: 'Netflix' }, { id: 'youtube', title: 'YouTube' }],
      devices: [{ id: 'hdmi4', name: 'HDMI 4', port: 4 }],
      channels: [{ id: 'news', name: 'News', number: '1' }], sources: { channels: 1 },
      order: { app: ['netflix', 'youtube', 'lg-home'], dev: ['hdmi4'], ch: ['news'] } };
    else if (pathname.endsWith('/startup')) {
      openOnPowerOn = route.request().postDataJSON().openOnPowerOn;
    } else if (pathname.endsWith('/layout')) {
      Object.assign(layout, route.request().postDataJSON());
      version++;
      body.layout = layout;
    } else if (pathname.endsWith('/open')) opened = route.request().postDataJSON();
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + '/?tab=home');
    await page.waitForSelector('#home-app-names:not(:disabled)');
    await page.getByRole('button', { name: 'Show app names', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('home-app-names').getAttribute('aria-pressed') === 'false');
    if (layout.appNames !== false) problems.push('Home preferences: the browser did not save icons-only');
    await page.getByRole('button', { name: 'Power-on screen', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('home-startup').textContent === 'Native Tv Home');
    if (!openOnPowerOn) problems.push('Home preferences: the browser did not save the power-on choice');
    await page.getByRole('button', { name: 'Clock format', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('home-clock').textContent === '12-hour');
    if (layout.clockFormat !== '12h') problems.push('Home preferences: the browser did not save 12-hour time');
    await page.goto(base + PAGES.dashboard + '?tab=home');
    const clockRow = page.locator('.row.doable').filter({ hasText: 'Clock format' });
    await clockRow.waitFor();
    if (!await clockRow.textContent().then(text => text.includes('12-hour'))) problems.push('Home preferences: the TV dashboard missed the saved clock format');
    await clockRow.click();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => rows.some(r => r.act === 'homeClockFormat' && r.clockFormat === '24h'));
    if (layout.clockFormat !== '24h') problems.push('Home preferences: the TV dashboard did not restore 24-hour time');
    const namesRow = page.locator('.row.doable').filter({ hasText: 'Show app names' });
    await namesRow.waitFor();
    await namesRow.click();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => rows.some(r => r.act === 'homeAppNames' && r.on === true));
    if (!layout.appNames) problems.push('Home preferences: the TV dashboard did not restore app names');
    const startupRow = page.locator('.row.doable').filter({ hasText: 'Power-on screen' });
    await startupRow.click();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => rows.some(r => r.act === 'homeStartup' && r.on === false));
    if (openOnPowerOn) problems.push('Home preferences: the TV dashboard did not restore the LG preference');
    layout.appNames = false;
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.app.focus');
    if (await page.locator('.tile.app .name:visible').count()) problems.push('Home preferences: icons-only still shows app names');
    if (await page.getByRole('button', { name: 'Netflix', exact: true }).count() !== 1) {
      problems.push('Home preferences: an icons-only app lost its accessible name');
    }
    await page.keyboard.press('ArrowRight');
    await page.keyboard.press('Enter');
    await page.waitForTimeout(100);
    if (!opened || opened.id !== 'youtube') problems.push('Home preferences: icons-only broke remote app selection');
    if (process.env.NATIVETV_REVIEW_DIR) {
      fs.mkdirSync(process.env.NATIVETV_REVIEW_DIR, { recursive: true });
      await page.screenshot({ path: path.join(process.env.NATIVETV_REVIEW_DIR, 'home-icons-only.png') });
    }
    layout.appNames = true;
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForSelector('.tile.app.focus .name:visible');
  } finally { await page.close(); }
}

async function visitHomeClock(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 }, timezoneId: 'UTC' });
  page.on('pageerror', e => problems.push('Home clock: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: ['news'], clockFormat: '12h' };
  let version = 1;
  await page.route('**/api/home/**', route => {
    const key = new URL(route.request().url()).pathname.split('/').pop();
    let body = { ok: true, build: 'one', layoutVersion: String(version) };
    if (key === 'catalogue') body = { ok: true, layout, layoutVersion: String(version),
      apps: [], devices: [], channels: [{ id: 'news', name: 'News', number: '1' }],
      order: { app: ['lg-home'], dev: [], ch: ['news'] }, sources: { channels: 1 } };
    if (key === 'guide') body = { ok: true, guide: { news: [
      { title: 'Current show', start: Date.parse('2026-10-20T12:00:00Z'), end: Date.parse('2026-10-20T13:30:00Z') },
      { title: 'Next show', start: Date.parse('2026-10-20T13:30:00Z'), end: Date.parse('2026-10-20T14:30:00Z') }
    ] } };
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.clock.install({ time: new Date('2026-10-20T00:05:00Z') });
    await page.goto(base + PAGES.home);
    const cases = [['00:05', '12:05 AM'], ['12:05', '12:05 PM'], ['13:05', '1:05 PM'],
      ['23:59', '11:59 PM'], ['10:58', '10:58 AM']];
    for (const [time, expected] of cases) {
      await page.clock.setSystemTime(new Date('2026-10-20T' + time + ':00Z'));
      await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
      await page.waitForFunction(value => document.getElementById('clockTime').textContent === value, expected);
      const fits = await page.evaluate(() => document.getElementById('clock').getBoundingClientRect().right <= document.getElementById('stage').getBoundingClientRect().right);
      if (!fits) problems.push('Home clock: the clock runs off the screen at ' + time);
    }
    layout.clockFormat = '24h'; version++;
    await page.clock.fastForward(5000);
    await page.waitForFunction(() => document.getElementById('clockTime').textContent === '10:58');
    await page.clock.setSystemTime(new Date('2026-10-20T00:05:00Z'));
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForFunction(() => document.getElementById('clockTime').textContent === '00:05');
    await page.clock.setSystemTime(new Date('2026-10-20T12:05:00Z'));
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForFunction(() => document.getElementById('clockTime').textContent === '12:05');
  } finally { await page.close(); }
}

// The Inputs row and the LG Home tile can each be left out of Native Tv Home.
async function visitHomeVisibility(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home visibility: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: ['news'], appNames: true,
    showInputs: true, showLgHome: true };
  let version = 1;
  await page.route(/\/api\/home(?:[/?]|$)/, async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = { ok: true, build: 'one', layoutVersion: String(version) };
    if (pathname === '/api/home') body = { ok: true, supported: { ok: true }, enabled: true,
      active: true, writable: true, layout, history: true, hideLgHomeAtStartAvailable: false };
    else if (pathname.endsWith('/catalogue')) body = { ok: true, layout, layoutVersion: String(version),
      apps: [{ id: 'netflix', title: 'Netflix' }, { id: 'youtube', title: 'YouTube' }],
      devices: [{ id: 'hdmi4', name: 'HDMI 4', port: 4 }],
      channels: [{ id: 'news', name: 'News', number: '1' }], sources: { channels: 1 },
      order: { app: ['netflix', 'youtube', 'lg-home'], dev: ['hdmi4'], ch: ['news'] } };
    else if (pathname.endsWith('/layout')) {
      Object.assign(layout, route.request().postDataJSON());
      version++;
      body.layout = layout;
    }
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + '/?tab=home');
    await page.waitForSelector('#home-show-inputs:not(:disabled)');
    await page.getByRole('button', { name: 'Show inputs', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('home-show-inputs').getAttribute('aria-pressed') === 'false');
    if (layout.showInputs !== false) problems.push('Home visibility: the browser did not save Show inputs off');
    await page.getByRole('button', { name: 'Show the LG Home tile', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('home-show-lg').getAttribute('aria-pressed') === 'false');
    if (layout.showLgHome !== false) problems.push('Home visibility: the browser did not save the LG Home tile off');
    // Live is off by default; the browser switches it on.
    await page.getByRole('button', { name: 'Show Live', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('home-show-live').getAttribute('aria-pressed') === 'true');
    if (layout.showLive !== true) problems.push('Home visibility: the browser did not save Show Live on');

    await page.goto(base + PAGES.dashboard + '?tab=home');
    const inputsRow = page.locator('.row.doable').filter({ hasText: 'Show inputs' });
    await inputsRow.waitFor();
    await inputsRow.click();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => rows.some(r => r.act === 'homeShowInputs' && r.on === true));
    if (layout.showInputs !== true) problems.push('Home visibility: the TV dashboard did not switch the Inputs row back on');
    const lgRow = page.locator('.row.doable').filter({ hasText: 'Show the LG Home tile' });
    await lgRow.click();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => rows.some(r => r.act === 'homeShowLgHome' && r.on === true));
    if (layout.showLgHome !== true) problems.push('Home visibility: the TV dashboard did not switch the LG Home tile back on');
    // It was switched on in the browser, so the same row switches it off here.
    const liveRow = page.locator('.row.doable').filter({ hasText: 'Show Live' });
    await liveRow.waitFor();
    await liveRow.click();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => rows.some(r => r.act === 'homeShowLive' && r.on === false));
    if (layout.showLive !== false) problems.push('Home visibility: the TV dashboard did not switch Show Live off');

    // The view: both on shows both; each off leaves out only its own.
    const tabs = () => page.locator('#tabs .tab').allTextContents().then(list => list.join(','));
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.app.focus');
    if ((await tabs()).indexOf('Inputs') === -1) problems.push('Home visibility: the Inputs tab is missing while on');
    if (await page.getByRole('button', { name: 'LG Home', exact: true }).count() !== 1) problems.push('Home visibility: the LG Home tile is missing while on');
    layout.showInputs = false;
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForFunction(() => [...document.querySelectorAll('#tabs .tab')].every(t => t.textContent !== 'Inputs'));
    if (await page.getByRole('button', { name: 'LG Home', exact: true }).count() !== 1) problems.push('Home visibility: switching off inputs took the LG Home tile too');
    layout.showInputs = true;
    layout.showLgHome = false;
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForFunction(() => !document.querySelector('[aria-label="LG Home"]'));
    if ((await tabs()).indexOf('Inputs') === -1) problems.push('Home visibility: switching off the LG Home tile took the Inputs tab too');
    if (await page.getByRole('button', { name: 'YouTube', exact: true }).count() !== 1) problems.push('Home visibility: switching off the LG Home tile took an app too');

    // Live stays out of the view while Show Live is off, even with channels
    // tuned and added. Switching it on (a dashboard save, seen by the
    // heartbeat) adds it without moving the tab in view; switching it off
    // while it is in view goes back to Home.
    if ((await tabs()).indexOf('Live') !== -1) problems.push('Home visibility: the Live tab shows while Show Live is off');
    await page.locator('#tabs .tab', { hasText: 'Apps' }).click();
    await page.waitForFunction(() => document.querySelector('.tab.sel') && document.querySelector('.tab.sel').textContent === 'Apps');
    layout.showLive = true;
    version++;
    await page.waitForFunction(() => [...document.querySelectorAll('#tabs .tab')].some(t => t.textContent === 'Live'));
    if (await page.locator('.tab.sel').textContent() !== 'Apps') problems.push('Home visibility: switching Live on moved the view off Apps');
    await page.locator('#tabs .tab', { hasText: 'Live' }).click();
    await page.waitForFunction(() => document.querySelector('.tab.sel') && document.querySelector('.tab.sel').textContent === 'Live');
    if (!await page.locator('#rows .tile.ch').count()) problems.push('Home visibility: the Live tab showed no channels');
    layout.showLive = false;
    version++;
    await page.waitForFunction(() => ![...document.querySelectorAll('#tabs .tab')].some(t => t.textContent === 'Live'));
    if (await page.locator('.tab.sel').textContent() !== 'Home') problems.push('Home visibility: the view did not fall back to Home when Live went away');
  } finally { await page.close(); }
}

// Native Tv Home: tabs, the featured carousel, rows, and Back's levels.
async function visitHomeTv(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home TV view: ' + e.message));
  const now = Date.now();
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] },
    channels: ['c101', 'c102'], appNames: true, showInputs: true, showLive: true, showLgHome: true, picture: false, colours: null };
  const opened = [];
  let feed = { ok: true, enabled: false, rows: [] };
  const art = '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#7a4"/></svg>';
  await page.route(/\/api\/home/, async route => {
    const req = route.request(), pathname = new URL(req.url()).pathname;
    let body = { ok: true };
    if (pathname === '/api/home/catalogue') body = { ok: true, layout, layoutVersion: '1', wallpapers: { available: false },
      apps: [{ id: 'netflix', title: 'Netflix', icon: '/api/home/icon?id=netflix' }, { id: 'youtube', title: 'YouTube', icon: null }],
      devices: [{ id: 'hdmi2', name: 'STEREO 70s', port: 2 }, { id: 'hdmi2b', name: 'PlayStation 5', port: 2, via: 'STEREO 70s' }, { id: 'hdmi3', name: 'HDMI 3', port: 3, unused: true }],
      channels: [{ id: 'c101', name: 'BBC ONE HD', number: '101' }, { id: 'c102', name: 'BBC TWO HD', number: '102' }],
      order: { app: ['netflix', 'youtube', 'lg-home'], dev: ['hdmi2', 'hdmi2b', 'hdmi3'], ch: ['c101', 'c102'] }, sources: { apps: true, devices: true, channels: 2 } };
    else if (pathname === '/api/home/now') body = { ok: true, kind: 'input', port: 2, title: 'PlayStation 5', devices: ['hdmi2b'] };
    else if (pathname === '/api/home/guide') body = { ok: true, guide: { c101: [{ start: now - 1800000, end: now + 1800000, title: 'Evening News' }], c102: [] } };
    else if (pathname === '/api/home/recents' && req.method() === 'GET') body = { ok: true, apps: [{ id: 'netflix', title: 'Netflix', image: true }] };
    else if (pathname === '/api/home/icon' || pathname === '/api/home/recents/image' || pathname === '/api/home/image') return route.fulfill({ contentType: 'image/svg+xml', body: art });
    else if (pathname === '/api/home/discover') body = feed;
    else if (pathname === '/api/home/beat') body = { ok: true, build: 'one', layoutVersion: '1' };
    else if (pathname === '/api/home/open') opened.push(req.postDataJSON());
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const key = code => page.evaluate(c => { for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { keyCode: c, bubbles: true })); }, code);
  const hero = () => page.locator('#hero .title').textContent();
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.focus');
    // The boot screen fades with the first render, then leaves the page; a page reloaded onto a kept catalogue never shows it.
    if (await page.locator('#boot.gone').count() !== 1) problems.push('Home TV view: the boot screen stayed after the first render');
    await page.waitForFunction(() => getComputedStyle(document.getElementById('boot')).display === 'none');
    await page.reload();
    await page.waitForSelector('.tile.focus');
    if (await page.evaluate(() => getComputedStyle(document.getElementById('boot')).display) !== 'none') problems.push('Home TV view: the boot screen showed over a kept catalogue');
    const tabs = await page.locator('#tabs .tab').allTextContents();
    if (tabs.join(',') !== 'Home,Live,Apps,Inputs') problems.push('Home TV view: tabs were ' + tabs.join(','));
    if (await hero() !== 'Netflix') problems.push('Home TV view: the apps are not the first row');
    await key(39);
    await page.waitForFunction(() => document.querySelector('#hero .title').textContent === 'YouTube');
    await key(13);
    await page.waitForFunction(() => true);
    await page.waitForTimeout(150);
    if (!opened.length || opened[0].id !== 'youtube') problems.push('Home TV view: OK did not open the focused app: ' + JSON.stringify(opened));
    // Back: the bar first, then recent apps.
    await key(461);
    if (!await page.locator('.tab.focus').count()) problems.push('Home TV view: Back from the first row did not reach the bar');
    await key(39);
    await page.waitForFunction(() => document.querySelector('.tab.sel').textContent === 'Live');
    await key(40);
    await page.waitForFunction(() => document.querySelector('#hero .title').textContent === 'Evening News');
    // The apps grid.
    await key(38); await key(39);
    await page.waitForFunction(() => document.querySelector('.tab.sel').textContent === 'Apps');
    await key(40);
    if (!await page.locator('.tile.grid.focus .name').count()) problems.push('Home TV view: the Apps tab showed no grid');
    // Back from the bar opens recent apps.
    await key(38);
    await key(461);
    await page.waitForSelector('.recents #recents .rc');
    if (process.env.NATIVETV_REVIEW_DIR) {
      fs.mkdirSync(process.env.NATIVETV_REVIEW_DIR, { recursive: true });
      await page.screenshot({ path: path.join(process.env.NATIVETV_REVIEW_DIR, 'home-tv-recents.png') });
    }
    // With a feed, its first title leads the hero and OK on it asks for the title to be opened.
    const picks = [];
    for (let i = 0; i < 12; i++) picks.push({ id: 7 + i, type: 'movie', title: i ? 'Another Film ' + i : 'A Film', year: '2024', rating: 8.1, overview: 'Plot.', poster: '/p.jpg', backdrop: '/b.jpg' });
    feed = { ok: true, enabled: true, rows: [{ key: 'trending', items: picks }] };
    opened.length = 0;
    await page.goto(base + PAGES.home);
    await page.waitForFunction(() => document.querySelector('#hero .title') && document.querySelector('#hero .title').textContent === 'A Film');
    if (await page.locator('.row').first().locator('.sqt').count() < 1) problems.push('Home TV view: the apps are not above the feed');
    await page.waitForFunction(() => document.querySelectorAll('#rows .row:nth-child(2) .shot img').length >= 8);
    const preloaded = await page.locator('#rows .row:nth-child(2) .shot img').count();
    if (preloaded !== 8) problems.push('Home TV view: the next discover row preloaded ' + preloaded + ' images instead of 8');
    await key(40);
    if (!await page.locator('.tile .face.disc').count()) problems.push('Home TV view: the top picks row was not drawn');
    await key(13);
    await page.waitForTimeout(150);
    if (!opened.length || opened[0].kind !== 'title' || opened[0].id !== 7 || opened[0].type !== 'movie') problems.push('Home TV view: OK on a title did not ask to open it: ' + JSON.stringify(opened));
    if (!await page.locator('#dots i.on').count()) problems.push('Home TV view: the carousel showed no page dots for the spotlight');
  } finally { await page.close(); }
}

// The quick settings: the gear in the bar opens the kit's drawer, whose
// changes go to the TV through /api/control and to Home's own layout through
// /api/home/layout, and Back closes it onto the gear.
async function visitHomeQuick(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home quick settings: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] },
    channels: [], appNames: true, showInputs: true, showLive: false, showLgHome: true, picture: false, colours: null };
  const stats = { volume: 37, muted: false, sleepTimer: 'off', oled: {},
    picture: { mode: 'Standard', mode_raw: 'standard', backlight: 50, modes: [{ value: 'standard', label: 'Standard' }, { value: 'vivid', label: 'Vivid' }] },
    sound: { output: 'TV Speaker', output_raw: 'tv_speaker', outputs: [{ id: 'tv_speaker', name: 'TV Speaker' }, { id: 'external_arc', name: 'HDMI ARC' }] } };
  const controls = [], layoutPosts = [], opened = [];
  await page.route(/\/api\/(home|stats|control)/, async route => {
    const req = route.request(), pathname = new URL(req.url()).pathname;
    let body = { ok: true };
    if (pathname === '/api/stats') body = stats;
    else if (pathname === '/api/control') controls.push(req.postDataJSON());
    else if (pathname === '/api/home/layout') { const sent = req.postDataJSON(); layoutPosts.push(sent); Object.assign(layout, sent); body = { ok: true, layout }; }
    else if (pathname === '/api/home/catalogue') body = { ok: true, layout, layoutVersion: String(layoutPosts.length), wallpapers: { available: false },
      apps: [{ id: 'netflix', title: 'Netflix', icon: null }], devices: [{ id: 'hdmi2', name: 'STEREO 70s', port: 2 }, { id: 'hdmi3', name: 'HDMI 3', port: 3, unused: true }],
      channels: [], order: { app: ['netflix'], dev: ['hdmi2', 'hdmi3'], ch: [] }, sources: { apps: true, devices: true, channels: 0 } };
    else if (pathname === '/api/home/now') body = { ok: true, kind: 'input', port: 2, title: 'STEREO 70s', devices: ['hdmi2'] };
    else if (pathname === '/api/home/open') opened.push(req.postDataJSON());
    else if (pathname === '/api/home/beat') body = { ok: true, build: 'one', layoutVersion: String(layoutPosts.length) };
    else if (pathname === '/api/home/discover') body = { ok: true, enabled: false, rows: [] };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const key = code => page.evaluate(c => { for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { keyCode: c, bubbles: true })); }, code);
  const title = () => page.locator('#qsTitle').textContent();
  const trail = name => page.locator('#qsList .gt-item', { hasText: name }).locator('.gt-item-trail').textContent();
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.focus');
    // Up to the bar, then along the tabs (Home, Apps, Inputs) to the gear after them.
    await key(38); await key(39); await key(39); await key(39);
    if (!await page.locator('#gear.focus').count()) problems.push('Home quick settings: the gear did not take the focus after the tabs');
    await key(13);
    await page.waitForSelector('#qs.show');
    if (await title() !== 'Quick settings') problems.push('Home quick settings: the drawer opened on ' + await title());
    await page.waitForFunction(() => /37/.test(document.getElementById('qsList').textContent));
    const rows = await page.locator('#qsList .gt-item-title').allTextContents();
    if (rows.join(',') !== 'Volume,Picture mode,OLED light,Sound output,Input,Sleep timer,Screensaver,Screen off,Profile,Home settings') problems.push('Home quick settings: the list was ' + rows.join(','));
    if (await trail('Input') !== 'STEREO 70s') problems.push('Home quick settings: Input did not show the input the TV is on');

    // Right on the volume steps it, the steps going to the TV together; the rows behind the drawer take no key.
    await key(39); await key(39);
    await waitForCheck(() => controls.filter(c => c.action === 'volumeStep').reduce((n, c) => n + c.value, 0) === 2, 'Home quick settings: two presses did not send two volume steps: ' + JSON.stringify(controls));
    if (await trail('Volume') !== '39') problems.push('Home quick settings: the volume showed ' + await trail('Volume'));
    // OK mutes.
    await key(13);
    await waitForCheck(() => controls.some(c => c.action === 'mute' && c.value === true), 'Home quick settings: OK on the volume did not mute');
    // A choice from a list: Picture mode, its second mode.
    await key(40); await key(13);
    if (await title() !== 'Picture mode') problems.push('Home quick settings: Picture mode opened ' + await title());
    if (await page.locator('#qsList .gt-radio.on').count() !== 1) problems.push('Home quick settings: the picture mode list did not mark the mode the TV is in');
    await key(40); await key(13);
    await waitForCheck(() => controls.some(c => c.action === 'pictureMode' && c.value === 'vivid'), 'Home quick settings: the picture mode was not sent');
    await page.waitForFunction(() => document.getElementById('qsTitle').textContent === 'Quick settings');
    // The light goes in fives, sent once the keys stop.
    await key(40); await key(39); await key(39);
    await waitForCheck(() => controls.some(c => c.action === 'backlight' && c.value === 60), 'Home quick settings: two presses on the light did not send 60: ' + JSON.stringify(controls));
    // An input: its list is Home's own, and choosing one opens it and leaves the drawer.
    await key(40); await key(40); await key(13);
    if (await title() !== 'Input') problems.push('Home quick settings: Input opened ' + await title());
    await key(13);
    await waitForCheck(() => opened.some(o => o.kind === 'dev' && o.id === 'hdmi2'), 'Home quick settings: choosing an input did not open it: ' + JSON.stringify(opened));
    await page.waitForFunction(() => document.getElementById('qs').className === 'gt-scrim');
    if (!await page.locator('#gear.focus').count()) problems.push('Home quick settings: the gear did not have the focus after the drawer closed');

    // Home's own settings: a switch saves the layout and shows on Home.
    await key(13);
    await page.waitForSelector('#qs.show');
    for (let i = 0; i < 9; i++) await key(40);
    await key(13);
    if (await title() !== 'Home settings') problems.push('Home quick settings: Home settings opened ' + await title());
    await key(13);
    await waitForCheck(() => layoutPosts.some(p => p.clockFormat === '24h'), 'Home quick settings: the clock switch did not save the layout: ' + JSON.stringify(layoutPosts));
    await page.waitForFunction(() => document.querySelector('#qsList .gt-switch.on'));
    // Back goes up a level, then closes.
    await key(461);
    if (await title() !== 'Quick settings') problems.push('Home quick settings: Back from Home settings went to ' + await title());
    await key(461);
    await page.waitForFunction(() => document.getElementById('qs').className === 'gt-scrim');

    // The pointer: the gear opens it, an item acts, and a click beside the drawer closes it.
    await page.locator('#gear').click();
    await page.waitForSelector('#qs.show');
    await page.locator('#qsList .gt-item', { hasText: 'Sleep timer' }).click();
    if (await title() !== 'Sleep timer') problems.push('Home quick settings: clicking Sleep timer opened ' + await title());
    await page.mouse.click(100, 300);
    await page.waitForFunction(() => document.getElementById('qs').className === 'gt-scrim');
  } finally { await page.close(); }
}

// Profiles: the avatar in the bar shows once there are two, opens the list of
// them, and a choice switches the layout; the quick settings list the same.
async function visitHomeProfiles(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home profiles: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] },
    channels: [], appNames: true, showInputs: true, showLive: false, showLgHome: true, picture: false, colours: null };
  let profiles = [{ id: 'main', name: 'Main', color: '#a8c7fa', active: true }];
  const posts = [];
  await page.route(/\/api\/(home|stats)/, async route => {
    const req = route.request(), pathname = new URL(req.url()).pathname;
    let body = { ok: true };
    if (pathname === '/api/stats') body = { volume: 20, muted: false, sleepTimer: 'off', picture: { mode: 'Standard', mode_raw: 'standard', modes: [] }, sound: { output: 'TV Speaker', output_raw: 'tv_speaker', outputs: [] } };
    else if (pathname === '/api/home/profile') {
      const sent = req.postDataJSON();
      posts.push(sent);
      if (sent.action === 'add') profiles.push({ id: 'p' + profiles.length, name: 'Profile ' + (profiles.length + 1), color: '#f2b8b5', active: false });
      if (sent.action === 'switch' || (sent.action === 'add' && sent.switch)) {
        const id = sent.action === 'add' ? profiles[profiles.length - 1].id : sent.id;
        profiles = profiles.map(p => Object.assign({}, p, { active: p.id === id }));
      }
      body = { ok: true, profiles };
    }
    else if (pathname === '/api/home/catalogue') body = { ok: true, layout, profiles, layoutVersion: String(posts.length), wallpapers: { available: false },
      apps: [{ id: 'netflix', title: 'Netflix', icon: null }], devices: [], channels: [],
      order: { app: ['netflix'], dev: [], ch: [] }, sources: { apps: true, devices: false, channels: 0 } };
    else if (pathname === '/api/home/beat') body = { ok: true, build: 'one', layoutVersion: String(posts.length) };
    else if (pathname === '/api/home/discover') body = { ok: true, enabled: false, rows: [] };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const key = code => page.evaluate(c => { for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { keyCode: c, bubbles: true })); }, code);
  const title = () => page.locator('#qsTitle').textContent();
  try {
    // With one profile there is no avatar and Left stops at the first tab.
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.focus');
    if (await page.locator('#avatar.on').count()) problems.push('Home profiles: the avatar showed with one profile');
    await key(38); await key(37);
    if (!await page.locator('.tab.focus').count() || await page.locator('#avatar.focus').count()) problems.push('Home profiles: Left from the first tab went to a hidden avatar');

    // With two, the avatar is the active profile's, before the tabs.
    profiles = [{ id: 'main', name: 'Main', color: '#a8c7fa', active: true }, { id: 'p1', name: 'Kids', color: '#f2b8b5', active: false }];
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.focus');
    await page.waitForSelector('#avatar.on');
    if (await page.locator('#avatar').textContent() !== 'M') problems.push('Home profiles: the avatar showed ' + await page.locator('#avatar').textContent());
    await key(38); await key(37);
    if (!await page.locator('#avatar.focus').count()) problems.push('Home profiles: Left from the first tab did not reach the avatar');
    await key(13);
    await page.waitForSelector('#qs.show');
    if (await title() !== 'Who’s watching?') problems.push('Home profiles: the avatar opened ' + await title());
    const names = await page.locator('#qsList .gt-item-title').allTextContents();
    if (names.join(',') !== 'Main,Kids,Add profile,Rename profile') problems.push('Home profiles: the list was ' + names.join(','));
    if (await page.locator('#qsList .gt-item.focus .gt-item-title').textContent() !== 'Main') problems.push('Home profiles: the list did not open on the profile in use');
    // A choice switches the layout and closes the list onto the avatar.
    await key(40); await key(13);
    await waitForCheck(() => posts.some(p => p.action === 'switch' && p.id === 'p1'), 'Home profiles: choosing Kids did not switch: ' + JSON.stringify(posts));
    await page.waitForFunction(() => document.getElementById('qs').className === 'gt-scrim' && document.getElementById('avatar').textContent === 'K');
    if (!await page.locator('#avatar.focus').count()) problems.push('Home profiles: the avatar did not have the focus after the list closed');
    // Back closes a list opened from the avatar at once.
    await key(13);
    await page.waitForSelector('#qs.show');
    await key(461);
    await page.waitForFunction(() => document.getElementById('qs').className === 'gt-scrim');
    // The quick settings have a Profile row that opens the same list and can add one, which is then in use.
    await key(39); await key(39); await key(39); await key(39);
    await key(13);
    await page.waitForSelector('#qs.show');
    for (let i = 0; i < 8; i++) await key(40);
    if (await page.locator('#qsList .gt-item.focus .gt-item-trail').textContent() !== 'Kids') problems.push('Home profiles: the Profile row showed ' + await page.locator('#qsList .gt-item.focus .gt-item-trail').textContent());
    await key(13);
    if (await title() !== 'Who’s watching?') problems.push('Home profiles: the Profile row opened ' + await title());
    await key(40); await key(13);
    await waitForCheck(() => posts.some(p => p.action === 'add' && p.switch === true), 'Home profiles: Add profile did not add one: ' + JSON.stringify(posts));
    await page.waitForFunction(() => document.getElementById('avatar').textContent === 'P');
    // The pointer: the avatar opens the list, and a click on a profile switches to it.
    await page.locator('#avatar').click();
    await page.waitForSelector('#qs.show');
    await page.locator('#qsList .gt-item', { hasText: 'Main' }).click();
    await waitForCheck(() => posts.some(p => p.action === 'switch' && p.id === 'main'), 'Home profiles: a click on Main did not switch: ' + JSON.stringify(posts));
  } finally { await page.close(); }
}

async function visitHomeContent(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('Home content: ' + e.message));
  const opened = [], searches = [], renamed = [];
  let enabled = true, failDetails = false, failRename = false, staleSearch = null, lateDetails = null, holdDetails = false, netflixRoute = null;
  const title = id => ({ id, type: 'movie', title: id === 7 ? 'The quiet coast' : 'Another horizon ' + id, year: '2026', rating: 8.1,
    overview: 'A journey along the coast, where each morning brings a different horizon.', poster: '/abcdefghijklmnop.jpg', backdrop: '/ponmlkjihgfedcba.jpg' });
  const profiles = [{ id: 'main', name: 'Main', color: '#a8c7fa', active: true }, { id: 'p1', name: 'Kids', color: '#f2b8b5', active: false }];
  const layout = { channels: [], hidden: {}, appNames: true, showLive: false, showInputs: false };
  const art = '<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="900"><rect width="1600" height="900" fill="#264d51"/></svg>';
  const answer = (route, body) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  await page.route(/\/api\/(home|stats)/, async route => {
    const req = route.request(), url = new URL(req.url()), pathname = url.pathname;
    let body = { ok: true };
    if (pathname === '/api/home/catalogue') body = { ok: true, layout, profiles, layoutVersion: String(renamed.length),
      apps: [{ id: 'netflix', title: 'Netflix', icon: null }], devices: [], channels: [], sources: { channels: 0 }, order: { app: ['netflix'], dev: [], ch: [] } };
    if (pathname === '/api/home/discover') body = { ok: true, enabled, titlesEnabled: enabled, rows: enabled ? [{ key: 'trending', items: [title(7), title(8)] }, { key: 'toprated', items: [title(8)] }] : [] };
    if (pathname === '/api/home/beat') body = { ok: true, build: 'one', layoutVersion: String(renamed.length) };
    if (pathname === '/api/home/image') return route.fulfill({ contentType: 'image/svg+xml', body: art });
    if (pathname === '/api/home/details') {
      if (holdDetails) { lateDetails = route; return; }
      body = failDetails ? { ok: false, enabled: true } : { ok: true, enabled: true, netflixEnabled: true, item: Object.assign(title(Number(url.searchParams.get('id'))), {
        runtime: 121, genres: ['Drama', 'Adventure'], cast: [{ name: '<Actor>', character: 'A role' }], trailer: 'dQw4w9WgXcQ', similar: [title(20), title(21)] }) };
    }
    if (pathname === '/api/home/netflix') { netflixRoute = route; return; }
    if (pathname === '/api/home/open') opened.push(req.postDataJSON());
    if (pathname === '/api/home/search') {
      const q = url.searchParams.get('q'); searches.push(q);
      if (q === 'ab') { staleSearch = route; return; }
      body = { ok: true, enabled: true, items: Array.from({ length: 9 }, (_, i) => title(30 + i)) };
    }
    if (pathname === '/api/home/profile') {
      const sent = req.postDataJSON(); renamed.push(sent);
      if (failRename) body = { ok: false, error: 'Save failed' };
      else { profiles[0].name = sent.name; body = { ok: true }; }
    }
    return answer(route, body);
  });
  const key = code => page.evaluate(c => { for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { keyCode: c, bubbles: true })); }, code);
  const clear = () => page.locator('#editClear').click();
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('#searchTool.on');
    await page.waitForSelector('.tile.focus');
    await page.waitForFunction(() => getComputedStyle(document.getElementById('boot')).display === 'none');
    await key(40); await key(403);
    await waitForCheck(() => opened.length === 1, 'Home content: Red did not open a title directly');
    if (opened[0].kind !== 'title' || opened[0].id !== 7) problems.push('Home content: direct open changed its target');
    opened.length = 0;
    await key(13);
    await page.waitForSelector('#titleView .gt-btn.focus');
    await page.waitForFunction(() => document.getElementById('dtFacts').textContent.includes('121 min'));
    await waitForCheck(() => !!netflixRoute, 'Home content: optional Netflix lookup did not start');
    await key(39);
    await answer(netflixRoute, { ok: true, enabled: true, netflixId: '80057281' });
    await page.waitForSelector('#dtActions [data-da="3"]');
    if (await page.locator('#dtActions .focus').textContent() !== 'Trailer') problems.push('Home content: a late Netflix mapping changed the focused action');
    await key(37); await key(37);
    if (opened.length) problems.push('Home content: OK launched before the details action');
    if (await page.locator('#dtCast').textContent() !== 'Cast: <Actor>') problems.push('Home content: cast was not escaped or displayed');
    if (await page.locator('#dtCast actor').count()) problems.push('Home content: metadata became markup');
    await review(page, 'home-details');
    await key(13);
    await waitForCheck(() => opened.length === 1, 'Home content: Open did not launch');
    await key(39); await key(13);
    await waitForCheck(() => opened.some(o => o.kind === 'netflix'), 'Home content: Netflix action missing');
    await key(39); await key(13);
    await waitForCheck(() => opened.some(o => o.kind === 'youtube' && o.id === 'dQw4w9WgXcQ'), 'Home content: trailer did not launch YouTube');
    await key(40); await key(13);
    await page.waitForFunction(() => document.getElementById('dtTitle').textContent === 'Another horizon 20');
    await key(461);
    if (await page.locator('#dtTitle').textContent() !== 'The quiet coast') problems.push('Home content: nested Back lost the first title');
    await key(38); await key(39); await key(13);
    await page.waitForSelector('#overviewView.show');
    await key(461); await key(461);
    await page.waitForFunction(() => !document.body.classList.contains('content-open'));
    if (await page.locator('.tile.focus').getAttribute('data-c') !== '0') problems.push('Home content: details Back lost the row selection');

    failDetails = true; await key(13);
    await page.waitForFunction(() => document.getElementById('dtStatus').textContent.includes('unavailable'));
    failDetails = false;
    await page.locator('#dtActions button', { hasText: 'Retry' }).click();
    await page.waitForFunction(() => document.getElementById('dtStatus').textContent === '');
    await key(461);
    holdDetails = true; await key(13);
    await waitForCheck(() => !!lateDetails, 'Home content: delayed details did not start');
    await key(461); holdDetails = false;
    await answer(lateDetails, { ok: true, enabled: true, item: title(999) });
    if (await page.locator('#contentView.show').count()) problems.push('Home content: a late details reply reopened the view');

    await page.locator('#searchTool').click();
    await page.waitForSelector('#editKeyboard .gt-key.focus');
    await page.locator('#editKeyboard [aria-label="q"]').click();
    await page.waitForTimeout(450);
    if (searches.length) problems.push('Home content: a one-character query reached the server');
    await clear(); await page.keyboard.type('ab');
    await waitForCheck(() => !!staleSearch, 'Home content: search did not debounce');
    await clear(); await page.keyboard.type('coast');
    await page.waitForSelector('#editResults .gt-card');
    await answer(staleSearch, { ok: true, enabled: true, items: [title(999)] });
    await page.waitForTimeout(100);
    if (await page.locator('#editResults .gt-card-title').first().textContent() !== 'Another horizon 30') problems.push('Home content: stale search replaced the current results');
    if (searches.join(',') !== 'ab,coast') problems.push('Home content: debounce sent intermediate queries: ' + searches.join(','));
    await review(page, 'home-search');
    await page.locator('#editDone').click();
    for (let i = 0; i < 4; i++) await key(40);
    if (await page.locator('#editResults .gt-card.focus').getAttribute('data-result') !== '8') problems.push('Home content: results did not page with the remote');
    await key(13);
    await page.waitForFunction(() => document.getElementById('dtTitle').textContent === 'Another horizon 38');
    await key(461);
    if (await page.locator('#editQuery').inputValue() !== 'coast') problems.push('Home content: details Back lost the query');
    await key(461); await key(461);
    await page.locator('#avatar').click();
    await page.locator('#qsList .gt-item', { hasText: 'Rename profile' }).click();
    await page.waitForSelector('#editorView.rename');
    await clear(); await page.keyboard.type('Canceled'); await key(461);
    if (renamed.length) problems.push('Home content: cancel renamed a profile');
    await page.locator('#qsList .gt-item', { hasText: 'Rename profile' }).click();
    await clear(); await page.keyboard.type('Evening'); failRename = true;
    await page.locator('#editDone').click();
    await page.waitForFunction(() => document.getElementById('editStatus').textContent === 'Save failed');
    failRename = false; await page.locator('#editDone').click();
    await page.waitForSelector('#qs.show');
    if (profiles[0].name !== 'Evening' || renamed[1].id !== 'main') problems.push('Home content: retry renamed the wrong profile');
    await key(461);
    enabled = false;
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForFunction(() => !document.getElementById('searchTool').classList.contains('on'));
  } finally { await page.close(); }
}

// The Magic Remote's pointer: hovering focuses and clicking acts. A tab opens
// on a click, the carousel card acts on what it shows, its dots move the
// spotlight on, and a card a hover slid out from under the pointer still opens
// on the click that follows it.
async function visitHomePointer(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home pointer: ' + e.message));
  const now = Date.now();
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] },
    channels: ['c101', 'c102'], appNames: true, showInputs: true, showLive: true, showLgHome: true, picture: false, colours: null };
  const opened = [];
  const picks = [];
  for (let i = 0; i < 12; i++) picks.push({ id: 7 + i, type: 'movie', title: i ? 'Another Film ' + i : 'A Film', year: '2024', rating: 8.1, overview: 'Plot.', poster: '/p.jpg', backdrop: '/b.jpg' });
  const feed = { ok: true, enabled: true, rows: [{ key: 'trending', items: picks }] };
  const art = '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#7a4"/></svg>';
  await page.route(/\/api\/home/, async route => {
    const req = route.request(), pathname = new URL(req.url()).pathname;
    let body = { ok: true };
    if (pathname === '/api/home/catalogue') body = { ok: true, layout, layoutVersion: '1', wallpapers: { available: false },
      apps: [{ id: 'netflix', title: 'Netflix', icon: '/api/home/icon?id=netflix' }, { id: 'youtube', title: 'YouTube', icon: null }],
      devices: [{ id: 'hdmi2', name: 'STEREO 70s', port: 2 }, { id: 'hdmi2b', name: 'PlayStation 5', port: 2, via: 'STEREO 70s' }],
      channels: [{ id: 'c101', name: 'BBC ONE HD', number: '101' }, { id: 'c102', name: 'BBC TWO HD', number: '102' }],
      order: { app: ['netflix', 'youtube', 'lg-home'], dev: ['hdmi2', 'hdmi2b'], ch: ['c101', 'c102'] }, sources: { apps: true, devices: true, channels: 2 } };
    else if (pathname === '/api/home/now') body = { ok: true, kind: 'input', port: 2, title: 'PlayStation 5', devices: ['hdmi2b'] };
    else if (pathname === '/api/home/guide') body = { ok: true, guide: { c101: [{ start: now - 1800000, end: now + 1800000, title: 'Evening News' }], c102: [] } };
    else if (pathname === '/api/home/recents' && req.method() === 'GET') body = { ok: true, apps: [{ id: 'netflix', title: 'Netflix', image: true }] };
    else if (pathname === '/api/home/icon' || pathname === '/api/home/recents/image' || pathname === '/api/home/image') return route.fulfill({ contentType: 'image/svg+xml', body: art });
    else if (pathname === '/api/home/discover') body = feed;
    else if (pathname === '/api/home/beat') body = { ok: true, build: 'one', layoutVersion: '1' };
    else if (pathname === '/api/home/open') opened.push(req.postDataJSON());
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const key = code => page.evaluate(c => { for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { keyCode: c, bubbles: true })); }, code);
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.app.focus');

    // A card opens on one click.
    opened.length = 0;
    await page.locator('.tile.app[aria-label="Netflix"]').click();
    await page.waitForTimeout(200);
    if (!opened.length || opened[0].kind !== 'app' || opened[0].id !== 'netflix') problems.push('Home pointer: clicking a card did not open it: ' + JSON.stringify(opened));

    // Clicking a tab opens it.
    await page.locator('#tabs .tab', { hasText: 'Live' }).click();
    await page.waitForFunction(() => document.querySelector('.tab.sel') && document.querySelector('.tab.sel').textContent === 'Live');
    await page.waitForFunction(() => document.querySelector('#hero .title') && document.querySelector('#hero .title').textContent === 'Evening News');
    await page.locator('#tabs .tab', { hasText: 'Apps' }).click();
    await page.waitForSelector('#stage.apps .tile.grid');
    await page.locator('#tabs .tab', { hasText: 'Home' }).click();
    await page.waitForFunction(() => document.querySelector('.tab.sel') && document.querySelector('.tab.sel').textContent === 'Home');
    await page.waitForFunction(() => document.querySelector('#hero .title') && document.querySelector('#hero .title').textContent === 'A Film');

    // The carousel card acts on what it shows.
    opened.length = 0;
    await page.locator('#feature').click({ position: { x: 200, y: 200 } });
    await page.waitForTimeout(250);
    if (!opened.length || opened[0].kind !== 'title' || opened[0].id !== 7) problems.push('Home pointer: clicking the carousel did not open its title: ' + JSON.stringify(opened));

    // Its dots move the spotlight on.
    await page.locator('#dots i').nth(2).click();
    await page.waitForFunction(() => document.querySelector('#hero .title') && document.querySelector('#hero .title').textContent === 'Another Film 2');
    const dotMotion = await page.evaluate(() => Array.from(document.querySelectorAll('#dots i')).map(dot => ({
      width: dot.offsetWidth, properties: getComputedStyle(dot, '::after').transitionProperty
    })));
    if (dotMotion.some(dot => dot.width !== 32 || /width|all/.test(dot.properties))) problems.push('Home pointer: carousel dots animate layout instead of a fixed-slot transform');
    await page.waitForTimeout(180);
    await review(page, 'home-dots');

    // Only the first row shows under the carousel, as in the kit: Down brings the second up
    // (the first Down leaves the bar if the pointer left focus there).
    await page.keyboard.press('ArrowDown');
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(450);

    // A card a hover slid out from under the pointer still opens on the click:
    // the rows must not slide out from under the pointer's row, and the click
    // on the row's padding acts on the card the hover focused.
    const edge = await page.evaluate(() => {
      const wrap = document.querySelector('#rows .row:nth-child(2) .wrap').getBoundingClientRect();
      const tiles = [].slice.call(document.querySelectorAll('#rows .row:nth-child(2) .tile.pick'));
      for (let i = 0; i < tiles.length; i++) {
        const r = tiles[i].getBoundingClientRect();
        if (r.right > wrap.right && r.left < wrap.right - 4) {
          return { id: 7 + i, x: (Math.max(r.left, wrap.left) + wrap.right) / 2, y: (r.top + r.bottom) / 2 };
        }
      }
      return null;
    });
    if (!edge) problems.push('Home pointer: no partly visible card to test the slide click');
    else {
      opened.length = 0;
      await page.mouse.move(edge.x, edge.y);
      await page.waitForTimeout(450);
      await page.mouse.click(edge.x, edge.y);
      await page.waitForTimeout(250);
      if (!opened.length || opened[0].id !== edge.id) problems.push('Home pointer: the card a hover slid from under the pointer did not open: ' + JSON.stringify(opened));
    }

    // The recent apps' cards open from the pointer too.
    for (let i = 0; i < 3 && await page.locator('.recents #recents .rc').count() === 0; i++) await key(461);
    await page.waitForSelector('.recents #recents .rc');
    await page.locator('.recents #recents .rc').first().click();
    await page.waitForFunction(() => !/\brecents\b/.test(document.getElementById('stage').className));
  } finally { await page.close(); }
}

// Moving on Native Tv Home patches only the tiles whose focus changed and
// the rows that gained or lost the selection; a key press never rebuilds the
// rows or touches every tile in them.
async function visitHomeRendering(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home rendering: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: [], appNames: true, showInputs: true, showLgHome: true, picture: false };
  const apps = [];
  // Enough rows to leave most of the Apps grid off screen, with an icon on
  // every app so the focus-windowed loader has something to load.
  const icon = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';
  for (let i = 0; i < 600; i++) apps.push({ id: 'app' + i, title: 'Application ' + i, icon });
  const devices = [{ id: 'hdmi1', name: 'HDMI 1', port: 1, unused: true }];
  const catalogue = { ok: true, layout, layoutVersion: '1', wallpapers: { available: false }, apps, devices, channels: [],
    order: { app: apps.map(a => a.id).concat(['lg-home']), dev: ['hdmi1'], ch: [] },
    sources: { apps: true, devices: true, channels: 0 } };
  const openedApps = [];
  await page.route('**/api/home/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = { ok: true };
    if (pathname.endsWith('/catalogue')) body = catalogue;
    else if (pathname.endsWith('/now')) body = { ok: true, kind: 'input', port: 1, title: 'HDMI 1' };
    else if (pathname.endsWith('/guide')) body = { ok: true, guide: {} };
    else if (pathname.endsWith('/discover')) body = { ok: true, enabled: false, rows: [] };
    else if (pathname.endsWith('/beat')) body = { ok: true, build: 'one', layoutVersion: '1' };
    else if (pathname.endsWith('/picture')) body = { ok: true, mode: null };
    else if (pathname.endsWith('/recents')) body = { ok: true, apps: [] };
    else if (pathname.endsWith('/open')) openedApps.push(route.request().postDataJSON());
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('#rows .tile.focus');
    // The focused/visible art starts first; the nearby buffer fills after
    // navigation has been idle and the preceding image batches finish.
    await page.waitForFunction(() => document.querySelectorAll('#rows .tile.sqt .shot img').length === 9);
    const homeIcons = await page.locator('#rows .tile.sqt .shot img').count();
    if (homeIcons !== 9) problems.push('Home rendering: the Home row loaded ' + homeIcons + ' icons instead of the focused tile plus 8 ahead');
    if (homeIcons >= apps.length) problems.push('Home rendering: the Home row loaded every app icon at once');
    if (await page.locator('#rows .tile').count() > 17) problems.push('Home rendering: the Home row mounted tiles outside its window');
    await review(page, 'home-window');
    await page.waitForFunction(() => {
      const tile = document.querySelector('#rows .tile.sqt'), img = tile && tile.querySelector('.shot img');
      return !!(img && img.complete && img.naturalWidth > 0 && !tile.querySelector('.mono'));
    });
    await page.evaluate(() => {
      const box = document.getElementById('rows');
      window.__ghChanged = [];
      window.__ghChild = 0;
      const observer = new MutationObserver(list => {
        for (const m of list) {
          // Images are added and their monogram taken out around the focus.
          // Those are not row or tile rebuilds; only the list is.
          if (m.type === 'childList' && !(m.target.classList && m.target.classList.contains('shot'))) {
            window.__ghChild += m.addedNodes.length + m.removedNodes.length;
          } else if (m.type === 'attributes' && m.attributeName === 'class') window.__ghChanged.push(m.target);
        }
      });
      observer.observe(box, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
      window.__ghReset = function () {
        observer.takeRecords();
        window.__ghChanged = [];
        window.__ghChild = 0;
        window.__ghRows = Array.prototype.slice.call(box.children);
        window.__ghTiles = Array.prototype.slice.call(box.querySelectorAll('.tile'));
      };
      window.__ghState = function () {
        const rows = Array.prototype.slice.call(box.children);
        const tiles = Array.prototype.slice.call(box.querySelectorAll('.tile'));
        const changed = window.__ghChanged.filter((n, i) => window.__ghChanged.indexOf(n) === i);
        return {
          child: window.__ghChild,
          rowsSame: window.__ghRows.length === rows.length && window.__ghRows.every((n, i) => n === rows[i]),
          tilesSame: window.__ghTiles.length === tiles.length && window.__ghTiles.every((n, i) => n === tiles[i]),
          tiles: changed.filter(n => n.classList && n.classList.contains('tile')).length,
          rows: changed.filter(n => n.classList && n.classList.contains('row')).length,
          focus: box.querySelectorAll('.tile.focus').length
        };
      };
    });
    async function moved(name, expectedRows) {
      await page.waitForTimeout(80);
      const state = await page.evaluate(() => window.__ghState());
      if (state.child) problems.push('Home rendering: ' + name + ' rebuilt ' + state.child + ' nodes');
      if (!state.rowsSame || !state.tilesSame) problems.push('Home rendering: ' + name + ' replaced row or tile elements');
      if (state.focus !== 1) problems.push('Home rendering: ' + name + ' left ' + state.focus + ' focused tiles');
      if (state.tiles !== 2) problems.push('Home rendering: ' + name + ' touched ' + state.tiles + ' tiles');
      if (state.rows !== expectedRows) problems.push('Home rendering: ' + name + ' touched ' + state.rows + ' rows');
    }
    await page.evaluate(() => window.__ghReset());
    await page.keyboard.press('ArrowRight');
    await moved('moving along a row', 0);
    const homeIconsAfter = await page.locator('#rows .tile.sqt .shot img').count();
    if (!await page.locator('#rows .tile.focus .shot img').count()) problems.push('Home rendering: the focused Home tile has no image');
    await page.evaluate(() => window.__ghReset());
    await page.keyboard.press('ArrowLeft');
    await moved('moving back along a row', 0);
    if (await page.locator('#rows .tile.sqt .shot img').count() !== homeIconsAfter) problems.push('Home rendering: moving back dropped icons already loaded');
    for (let i = 0; i < 60; i++) await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(200);
    const horizontal = await page.evaluate(() => ({ tiles: document.querySelectorAll('#rows .tile').length,
      images: document.querySelectorAll('#rows img').length, col: document.querySelector('#rows .tile.focus').getAttribute('data-c') }));
    const horizontalRect = await page.locator('#rows .tile.focus').boundingBox();
    if (horizontal.tiles > 17 || horizontal.images > 17 || horizontal.col !== '60') problems.push('Home rendering: horizontal window grew or lost focus: ' + JSON.stringify(horizontal));
    if (!horizontalRect || horizontalRect.x < 0 || horizontalRect.x + horizontalRect.width > 1280) problems.push('Home rendering: horizontal virtual focus scrolled off screen: ' + JSON.stringify(horizontalRect));
    await review(page, 'home-horizontal-window');
    for (let i = 0; i < 60; i++) await page.keyboard.press('ArrowLeft');
    await page.waitForTimeout(200);
    if (await page.locator('#rows .tile.focus').getAttribute('data-c') !== '0') problems.push('Home rendering: returning across evicted tiles lost the selection');
    // Up to the bar, across to the Apps grid, down into its first row, then on
    // to the second: the old row loses .cur and the new one gains it.
    await page.keyboard.press('ArrowUp');
    await page.keyboard.press('ArrowRight');
    await page.waitForFunction(() => {
      const selected = document.querySelector('.tab.sel');
      return selected && selected.textContent === 'Apps';
    });
    await page.keyboard.press('ArrowDown');
    await page.waitForSelector('#rows .tile.grid.focus');
    await page.waitForFunction(() => document.querySelectorAll('#rows .tile.grid .shot img').length > 0);
    const gridIcons = await page.locator('#rows .tile.grid .shot img').count();
    if (gridIcons < 1 || gridIcons >= apps.length) problems.push('Home rendering: the Apps grid loaded ' + gridIcons + ' of ' + apps.length + ' icons at once');
    await page.evaluate(() => window.__ghReset());
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(80);
    // Expanding the buffer may mount a row, but rows already in the window
    // keep their tiles. The focus itself still changes only two tiles.
    const gridState = await page.evaluate(() => window.__ghState());
    if (gridState.focus !== 1 || gridState.tiles !== 2) problems.push('Home rendering: changing grid rows did not patch just the old and new focus');
    const gridIconsAfter = await page.locator('#rows .tile.grid .shot img').count();
    if (gridIconsAfter <= gridIcons) problems.push('Home rendering: moving down the Apps grid loaded no buffered icons (' + gridIcons + ' -> ' + gridIconsAfter + ')');
    // Fill the six-row window, then measure container allocations while it
    // crosses many row boundaries. Recycled rows retain delegated input.
    await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(20);
    await page.evaluate(() => {
      const create = document.createElement;
      window.__ghContainersCreated = 0;
      document.createElement = function (tag) {
        if (tag === 'section' || tag === 'div') window.__ghContainersCreated++;
        return create.apply(this, arguments);
      };
    });
    for (let i = 0; i < 69; i++) await page.keyboard.press('ArrowDown');
    await page.waitForTimeout(200);
    const containersCreated = await page.evaluate(() => window.__ghContainersCreated);
    if (containersCreated) problems.push('Home rendering: scrolling allocated ' + containersCreated + ' new grid row/tile containers');
    const bounded = await page.evaluate(() => ({ rows: document.querySelectorAll('#rows .row').length,
      tiles: document.querySelectorAll('#rows .tile').length, images: document.querySelectorAll('#rows img').length,
      focus: document.querySelector('#rows .tile.focus').getAttribute('data-r') }));
    if (bounded.rows > 6 || bounded.tiles > 36 || bounded.images > 36) problems.push('Home rendering: grid DOM/images grew with browsing: ' + JSON.stringify(bounded));
    if (+bounded.focus !== 71) problems.push('Home rendering: virtualized grid lost remote navigation: ' + JSON.stringify(bounded));
    await page.locator('#rows .tile.focus').click();
    await waitForCheck(() => openedApps.length > 0, 'Home rendering: recycled tile click did not open an app');
    if (openedApps[openedApps.length - 1].id !== 'app426') problems.push('Home rendering: recycled tile opened stale app content');
    // The focused tile must be physically on screen, not merely in the DOM.
    const rect = await page.locator('#rows .tile.focus').boundingBox();
    if (!rect || rect.y < 0 || rect.y + rect.height > 720) problems.push('Home rendering: virtualized focus scrolled off screen: ' + JSON.stringify(rect));
    await review(page, 'apps-window');
    layout.appNames = false;
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForSelector('#stage.apps.nonames');
    await page.waitForTimeout(200);
    const compact = await page.evaluate(() => {
      const row = document.querySelector('#rows .row.cur'), focus = row.querySelector('.tile.focus');
      return { height: row.offsetHeight, tiles: document.querySelectorAll('#rows .tile').length,
        col: focus && focus.getAttribute('data-c'), row: row.getAttribute('data-row') };
    });
    if (compact.height !== 180 || compact.tiles > 48 || compact.row !== '71' || compact.col !== '0') problems.push('Home rendering: compact grid lost its geometry, bounds or focus: ' + JSON.stringify(compact));
    await review(page, 'apps-compact-window');
  } finally { await page.close(); }
}

// Focus never waits for hero work, and neither resume nor tab changes throw
// away the already rendered screen. A held catalogue also exercises startup
// from the bounded session snapshot.
async function visitHomeSpeed(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home speed: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] },
    channels: [], appNames: true, showLive: false, showInputs: false, showLgHome: true, picture: false };
  const apps = Array.from({ length: 150 }, (_, i) => ({ id: 'app' + i, title: 'Application ' + i, icon: null }));
  const catalogue = { ok: true, layout, layoutVersion: '1', wallpapers: { available: false }, apps, devices: [], channels: [],
    order: { app: apps.map(a => a.id).concat(['lg-home']), dev: [], ch: [] }, sources: { apps: true, devices: true, channels: null } };
  const picks = Array.from({ length: 30 }, (_, i) => ({ id: i + 1, type: 'movie', title: 'Film ' + i, backdrop: '/back' + i + '.jpg', poster: '/poster' + i + '.jpg' }));
  let hold = false, release = null, catalogueReads = 0;
  const opened = [], backdrops = [];
  const art = '<svg xmlns="http://www.w3.org/2000/svg" width="160" height="90"><rect width="160" height="90" fill="#375943"/></svg>';
  await page.route('**/api/home/**', async route => {
    const url = new URL(route.request().url()), pathname = url.pathname;
    let body = { ok: true };
    if (pathname.endsWith('/catalogue')) {
      catalogueReads++;
      if (url.searchParams.get('view') !== '1') problems.push('Home speed: view requested the settings catalogue');
      if (hold) await new Promise(resolve => { release = resolve; });
      body = catalogue;
    } else if (pathname.endsWith('/discover')) body = { ok: true, enabled: true, rows: [{ key: 'trending', items: picks }] };
    else if (pathname.endsWith('/beat')) body = { ok: true, build: 'one', layoutVersion: '1' };
    else if (pathname.endsWith('/now')) body = { ok: true, kind: null };
    else if (pathname.endsWith('/open')) opened.push(route.request().postDataJSON());
    else if (pathname.endsWith('/image')) {
      if ((url.searchParams.get('u') || '').includes('/w1280/')) backdrops.push(url.searchParams.get('u'));
      return route.fulfill({ contentType: 'image/svg+xml', body: art });
    }
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + PAGES.home);
    await page.waitForFunction(() => document.querySelector('#hero .title') && document.querySelector('#hero .title').textContent === 'Film 0');
    await page.waitForFunction(() => document.querySelector('.back.on'));
    await page.keyboard.press('ArrowDown');
    const immediate = await page.evaluate(() => {
      for (let i = 0; i < 3; i++) document.dispatchEvent(new KeyboardEvent('keydown', { keyCode: 39, bubbles: true }));
      const focus = document.querySelector('#rows .tile.focus');
      // A click during the settling delay must act on the hero still shown.
      document.getElementById('feature').click();
      return { row: focus.getAttribute('data-r'), col: focus.getAttribute('data-c'), hero: document.querySelector('#hero .title').textContent };
    });
    if (immediate.row !== '1' || immediate.col !== '3' || immediate.hero !== 'Film 0') problems.push('Home speed: hero work blocked or followed rapid focus moves: ' + JSON.stringify(immediate));
    await page.waitForFunction(() => document.querySelector('#hero .title').textContent === 'Film 3');
    await page.waitForFunction(() => document.querySelector('.back.on').style.backgroundImage.includes('back3'));
    if (backdrops.some(u => /\/back[12]\.jpg$/.test(u))) problems.push('Home speed: intermediate hero artwork was requested during rapid navigation');
    if (!opened.length || opened[0].id !== 1) problems.push('Home speed: clicking the deferred hero opened a title other than the one shown');

    await page.evaluate(() => { window.__speedFocus = document.querySelector('#rows .tile.focus'); window.__speedApp = document.querySelector('[aria-label="Application 1"]'); });
    const before = catalogueReads;
    hold = true;
    await setHidden(page, true);
    await setHidden(page, false);
    await waitForCheck(() => release !== null, 'Home speed: returning did not start a background refresh');
    if (!await page.evaluate(() => document.querySelector('#rows .tile.focus') === window.__speedFocus)) problems.push('Home speed: returning rebuilt the screen or reset focus before data arrived');
    hold = false; release(); release = null;
    await waitForCheck(() => catalogueReads > before, 'Home speed: return made no catalogue read');
    await page.waitForTimeout(200);
    if (!await page.evaluate(() => document.querySelector('#rows .tile.focus') === window.__speedFocus)) problems.push('Home speed: unchanged return data replaced the selected tile');

    apps[0].title = 'Renamed application';
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForSelector('[aria-label="Renamed application"]');
    if (!await page.evaluate(() => document.querySelector('[aria-label="Application 1"]') === window.__speedApp && document.querySelector('#rows .tile.focus') === window.__speedFocus)) problems.push('Home speed: changing one app replaced unchanged tiles');
    await page.locator('#tabs .tab', { hasText: 'Apps', exact: true }).click();
    await page.waitForSelector('#stage.apps .tile');
    await page.locator('#tabs .tab', { hasText: 'Home', exact: true }).click();
    if (!await page.evaluate(() => document.querySelector('[aria-label="Application 1"]') === window.__speedApp)) problems.push('Home speed: switching tabs rebuilt the cached Home tiles');

    hold = true;
    await page.reload();
    await waitForCheck(() => release !== null, 'Home speed: reload did not request the catalogue');
    await page.waitForSelector('[aria-label="Renamed application"]');
    hold = false; release(); release = null;
    await page.waitForTimeout(200);
    if (!await page.locator('#clock').isVisible()) problems.push('Home speed: cached startup left the clock hidden');
    await review(page, 'home-speed');
  } finally { if (release) release(); await page.close(); }
}

// The guide refreshes every minute. It is only drawn on Live, so its minute
// tick must not rebuild Home or Apps, while Live still follows the change.
async function visitHomeGuideRefresh(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('pageerror', e => problems.push('Home guide refresh: ' + e.message));
  const now = Date.now(), guide = { title: 'Evening News', end: Date.now() + 1800000 };
  let guideReads = 0, version = 1;
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] },
    channels: ['c101'], appNames: true, showInputs: false, showLive: true, showLgHome: false, picture: false, colours: null };
  const catalogue = { ok: true, layout, layoutVersion: String(version), wallpapers: { available: false },
    apps: [{ id: 'netflix', title: 'Netflix', icon: null }], devices: [],
    channels: [{ id: 'c101', name: 'BBC ONE HD', number: '101' }],
    order: { app: ['netflix'], dev: [], ch: ['c101'] }, sources: { apps: true, devices: false, channels: 1 } };
  await page.route('**/api/home/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = { ok: true };
    if (pathname === '/api/home/catalogue') body = Object.assign({}, catalogue, { layoutVersion: String(version) });
    else if (pathname === '/api/home/now') body = { ok: true, kind: 'app', appId: 'netflix', title: 'Netflix' };
    else if (pathname === '/api/home/guide') {
      guideReads++;
      body = { ok: true, guide: { c101: [{ start: now - 1800000, end: guide.end, title: guide.title }] } };
    } else if (pathname === '/api/home/discover') body = { ok: true, enabled: false, rows: [] };
    else if (pathname === '/api/home/beat') body = { ok: true, build: 'one', layoutVersion: String(version) };
    else if (pathname === '/api/home/picture') body = { ok: true, mode: null };
    else if (pathname === '/api/home/recents') body = { ok: true, apps: [] };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.clock.install({ time: new Date(now) });
    await page.clock.pauseAt(new Date(now + 1000));
    await page.goto(base + PAGES.home);
    await page.waitForSelector('#rows .tile.focus');
    await waitForCheck(() => guideReads > 0, 'Home guide refresh: the page never read the guide');
    await page.evaluate(() => {
      const box = document.getElementById('rows');
      window.__ghMutations = 0;
      const observer = new MutationObserver(list => {
        for (const m of list) {
          if (m.type === 'childList' && !(m.target.classList && m.target.classList.contains('shot'))) {
            window.__ghMutations += m.addedNodes.length + m.removedNodes.length;
          }
        }
      });
      observer.observe(box, { childList: true, subtree: true });
      window.__ghResetMutations = function () { observer.takeRecords(); window.__ghMutations = 0; };
    });
    // A guide-only refresh on Home.
    guide.title = 'Evening News 2';
    await page.evaluate(() => window.__ghResetMutations());
    let reads = guideReads;
    await page.clock.fastForward(60000);
    await waitForCheck(() => guideReads > reads, 'Home guide refresh: Home did not refresh its guide');
    await page.waitForTimeout(100);
    if (await page.evaluate(() => window.__ghMutations)) problems.push('Home guide refresh: a guide update rebuilt the Home rows');

    // The same change on Apps.
    await page.keyboard.press('ArrowUp');
    await page.keyboard.press('ArrowRight');
    await page.keyboard.press('ArrowRight');
    await page.clock.runFor(450);
    await page.waitForFunction(() => {
      const selected = document.querySelector('.tab.sel');
      return selected && selected.textContent === 'Apps';
    });
    await page.evaluate(() => window.__ghResetMutations());
    guide.title = 'Evening News 3';
    reads = guideReads;
    await page.clock.fastForward(60000);
    await waitForCheck(() => guideReads > reads, 'Home guide refresh: Apps did not refresh its guide');
    await page.waitForTimeout(100);
    if (await page.evaluate(() => window.__ghMutations)) problems.push('Home guide refresh: a guide update rebuilt the Apps grid');

    // Live draws the guide, so the same change must show there.
    await page.keyboard.press('ArrowUp');
    await page.keyboard.press('ArrowLeft');
    await page.clock.runFor(450);
    await page.waitForFunction(() => {
      const selected = document.querySelector('.tab.sel');
      return selected && selected.textContent === 'Live';
    });
    await page.evaluate(() => window.__ghResetMutations());
    guide.title = 'Evening News 4';
    reads = guideReads;
    await page.clock.fastForward(60000);
    await waitForCheck(() => guideReads > reads, 'Home guide refresh: Live did not refresh its guide');
    await page.waitForTimeout(100);
    if (!await page.evaluate(() => window.__ghMutations)) problems.push('Home guide refresh: Live ignored the guide update');
    await page.clock.runFor(450);
    if (!await page.locator('#hero .title').filter({ hasText: 'Evening News 4' }).count()) problems.push('Home guide refresh: Live still showed the old programme');

    // Schedule corrections can keep the same title within the same minute.
    const beforeCorrection = await page.locator('#hero .line').textContent();
    guide.end += 600000;
    reads = guideReads;
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await waitForCheck(() => guideReads > reads, 'Home guide refresh: corrected schedule was not read');
    await page.waitForTimeout(50);
    await page.clock.runFor(450);
    if (await page.locator('#hero .line').textContent() === beforeCorrection) problems.push('Home guide refresh: a same-title schedule correction was ignored');

    // With Live off the guide draws nothing else, so it is not read at all.
    layout.showLive = false;
    version++;
    await page.clock.fastForward(5000);
    await waitForCheck(() => page.locator('#tabs .tab', { hasText: 'Live' }).count().then(n => n === 0),
      'Home guide refresh: the Live tab stayed after Show Live was switched off');
    reads = guideReads;
    guide.title = 'Evening News 5';
    await page.clock.fastForward(60000);
    await page.waitForTimeout(100);
    if (guideReads !== reads) problems.push('Home guide refresh: the guide was read while Live was off');
  } finally { await page.close(); }
}

// Each row's Order: shown by Native Tv Home, set on the Home tab and the TV app's Home section.
async function visitHomeOrder(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('Home order: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    sort: { app: 'custom', dev: 'custom', ch: 'custom' }, hidden: { app: [], dev: [], ch: [] }, channels: [] };
  const saves = [];
  await page.route(/\/api\/home(?:[/?]|$)/, async route => {
    const pathname = new URL(route.request().url()).pathname;
    // The server sends a Last opened row in that order: here, YouTube was opened last.
    const order = { app: layout.sort.app === 'recent' ? ['two', 'one', 'lg-home'] : ['one', 'two', 'lg-home'], dev: [], ch: [] };
    const catalogue = { ok: true, apps: [{ id: 'one', title: 'One' }, { id: 'two', title: 'Two' }], devices: [], channels: [],
      sources: { channels: 0 }, order, layout };
    let body = { ok: true };
    if (pathname === '/api/home') body = Object.assign({ ok: true, supported: { ok: true }, enabled: true, active: true,
      writable: true, layout, history: true }, new URL(route.request().url()).searchParams.get('with') ? { catalogue, channels: [] } : {});
    else if (pathname === '/api/home/catalogue') body = catalogue;
    else if (pathname === '/api/home/layout') {
      const patch = route.request().postDataJSON();
      saves.push(patch);
      if (patch.sort) Object.assign(layout.sort, patch.sort);
      body = { ok: true, layout: JSON.parse(JSON.stringify(layout)) };
    }
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const appTiles = () => page.evaluate(() => [...document.querySelectorAll('#rows .row:first-child .tile')].map(n => n.getAttribute('aria-label')).join(','));
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('.tile.app');
    if (await appTiles() !== 'One,Two,LG Home') problems.push('Home order: the view did not show the Apps row in its order');
    layout.sort.app = 'recent';
    await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
    await page.waitForFunction(() => document.querySelector('#rows .row:first-child .tile').getAttribute('aria-label') === 'Two');
    await page.goto(base + '/?tab=home');
    await page.waitForSelector('#home-sort-app:not(:disabled)');
    if (!await page.locator('details.home-row[data-kind="app"] .home-tile button[aria-label="Later"]').first().isDisabled()) {
      problems.push('Home order: tiles in a Last opened row can be moved on the Home tab');
    }
    const saved = page.waitForResponse(r => new URL(r.url()).pathname === '/api/home/layout');
    await page.locator('#home-sort-app').selectOption('custom');
    await saved;
    if (saves[saves.length - 1].sort.app !== 'custom') problems.push('Home order: the Home tab did not save Your order');

    await page.goto(base + PAGES.dashboard + '?tab=home');
    await page.waitForFunction(() => document.body.innerText.indexOf('Your order') !== -1);
    // One for Apps and one for Channels: the fixture has no inputs, and an empty row has no section.
    const orders = await page.evaluate(() => (document.body.innerText.match(/Your order|Last opened/g) || []).length);
    if (orders !== 2) problems.push('Home order: the TV app\'s Home section has no Order for each row');
  } finally { await page.close(); }
}

// Saves are acknowledged before another edit can replace a whole list.
async function visitHomeEditor(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home editor: ' + e.message));
  let layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: ['a'] };
  const channels = ['a', 'b', 'c'].map(id => ({ id, name: id.toUpperCase(), number: id }));
  const saves = [];
  let release = null, refuse = false, pendingRestore = false, portChanged = false;
  await page.route(/\/api\/home(?:[/?]|$)/, async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body;
    if (pathname === '/api/home') body = { ok: true, supported: { ok: !portChanged }, installed: true,
      enabled: !pendingRestore, restorePending: pendingRestore, active: true, writable: true, layout, history: true };
    else if (pathname.endsWith('/catalogue')) body = { ok: true, apps: [{ id: 'one', title: 'One' }, { id: 'two', title: 'Two' }], devices: [],
      channels: channels.filter(c => layout.channels.includes(c.id)),
      order: { app: layout.order.app.length ? layout.order.app : ['one', 'two', 'lg-home'], dev: [], ch: layout.channels }, layout };
    else if (pathname.endsWith('/channels')) body = { ok: true, channels };
    else if (pathname.endsWith('/layout')) {
      const patch = route.request().postDataJSON();
      saves.push(patch);
      if (refuse) body = { ok: false, error: 'save refused' };
      else {
        if (patch.channels) layout.channels = patch.channels;
        if (patch.order) Object.assign(layout.order, patch.order);
        body = { ok: true, layout: JSON.parse(JSON.stringify(layout)) };
        if (saves.length === 1) await new Promise(resolve => { release = resolve; });
      }
    } else body = { ok: true };
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + '/?tab=home');
    const appRows = page.locator('details.home-row[data-kind="app"] .home-tile');
    const ready = () => page.waitForFunction(() => {
      const button = document.querySelector('details.home-row[data-kind="app"] .home-tile button[aria-label="Later"]');
      return button && !button.disabled;
    });
    await ready();
    await page.locator('details.home-row[data-kind="ch"] summary').click();
    await page.getByRole('button', { name: 'Show the TV’s channels', exact: true }).click();
    const add = id => page.locator('#home-ch-list .prot-row').filter({ hasText: id + ' ' + id.toUpperCase() }).getByRole('button', { name: 'Add', exact: true });
    await add('b').click();
    await page.waitForTimeout(100);
    if (!await add('c').isDisabled()) problems.push('Home editor: controls remain enabled during a pending save');
    await add('c').click({ force: true });
    await page.waitForTimeout(100);
    if (saves.length !== 1) problems.push('Home editor: a pending save allowed a second stale channel list');
    release();
    await ready();
    if (await add('c').count()) { await add('c').click(); await ready(); }
    if (JSON.stringify(layout.channels) !== JSON.stringify(['a', 'b', 'c'])) problems.push('Home editor: an acknowledged channel was lost');
    refuse = true;
    const refused = page.waitForResponse(r => new URL(r.url()).pathname === '/api/home/layout');
    await appRows.first().getByRole('button', { name: 'Later', exact: true }).click();
    await refused; await ready();
    if (await appRows.first().locator('[translate="no"]').textContent() !== 'One') problems.push('Home editor: refused move remained visible');
    pendingRestore = true;
    await page.reload();
    await page.waitForSelector('#home-switch:not(:disabled)');
    if (!await page.locator('#home-switch').textContent().then(t => /Retry/.test(t))) problems.push('Home editor: failed restoration has no retry action');
    pendingRestore = false; portChanged = true;
    await page.reload();
    await page.waitForTimeout(200);
    if (await page.locator('#home-switch').isDisabled()) problems.push('Home editor: changing the port blocked switching Home off');
  } finally { await page.close(); }
}

// The browser dashboard's Home tab manages profiles: add, rename, recolour,
// switch to one and remove one on a second press; the last cannot be removed.
async function visitHomeProfileEditor(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home profile editor: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] }, channels: [] };
  const colours = ['#a8c7fa', '#f2b8b5', '#6dd58c', '#ffb74d', '#7fcfff', '#d0bcff'];
  let profiles = [{ id: 'main', name: 'Main', color: colours[0], active: true }];
  const posts = [];
  await page.route(/\/api\/home(?:[/?]|$)/, async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body = { ok: true };
    if (pathname === '/api/home') body = { ok: true, supported: { ok: true }, installed: true, enabled: true, restorePending: false, active: true,
      writable: true, layout, history: true, profiles, profile: (profiles.filter(p => p.active)[0] || {}).id, profileColours: colours };
    else if (pathname.endsWith('/catalogue')) body = { ok: true, apps: [{ id: 'one', title: 'One' }], devices: [], channels: [],
      order: { app: ['one', 'lg-home'], dev: [], ch: [] }, layout };
    else if (pathname.endsWith('/profile')) {
      const sent = route.request().postDataJSON();
      posts.push(sent);
      if (sent.action === 'add') profiles.push({ id: 'p' + profiles.length, name: 'Profile ' + (profiles.length + 1), color: colours[profiles.length], active: false });
      const target = profiles.filter(p => p.id === sent.id)[0];
      if (sent.action === 'rename' && target) { if (sent.name) target.name = sent.name; if (sent.color) target.color = sent.color; }
      if (sent.action === 'switch') profiles = profiles.map(p => Object.assign({}, p, { active: p.id === sent.id }));
      if (sent.action === 'remove') { profiles = profiles.filter(p => p.id !== sent.id); if (!profiles.some(p => p.active)) profiles[0].active = true; }
    }
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  const rows = () => page.locator('#home-profiles .home-profile');
  try {
    await page.goto(base + '/?tab=home');
    await page.waitForSelector('#home-profiles .home-profile');
    if (!await rows().nth(0).getByRole('button', { name: 'Remove', exact: true }).isDisabled()) problems.push('Home profile editor: the only profile could be removed');
    await page.getByRole('button', { name: 'Add a profile', exact: true }).click();
    await page.waitForFunction(() => document.querySelectorAll('#home-profiles .home-profile').length === 2);
    await rows().nth(1).locator('input').fill('Kids');
    await rows().nth(1).locator('input').press('Enter');
    await waitForCheck(() => posts.some(p => p.action === 'rename' && p.id === 'p1' && p.name === 'Kids'), 'Home profile editor: a new name was not sent: ' + JSON.stringify(posts));
    await rows().nth(1).locator('button.home-pf-col').nth(2).click();
    await waitForCheck(() => posts.some(p => p.action === 'rename' && p.id === 'p1' && p.color === '#6dd58c'), 'Home profile editor: a new colour was not sent: ' + JSON.stringify(posts));
    await page.waitForFunction(() => document.querySelectorAll('#home-profiles .home-profile input')[1].value === 'Kids');
    await rows().nth(1).getByRole('button', { name: 'Use', exact: true }).click();
    await waitForCheck(() => posts.some(p => p.action === 'switch' && p.id === 'p1'), 'Home profile editor: Use did not switch: ' + JSON.stringify(posts));
    await page.waitForFunction(() => document.querySelectorAll('#home-profiles .home-profile.picked').length === 1 && document.querySelector('#home-profiles .home-profile.picked input').value === 'Kids');
    // The first press arms Remove, the second removes.
    await rows().nth(0).getByRole('button', { name: 'Remove', exact: true }).click();
    if (posts.some(p => p.action === 'remove')) problems.push('Home profile editor: one press removed a profile');
    await rows().nth(0).getByRole('button', { name: 'Press again to remove', exact: true }).click();
    await waitForCheck(() => posts.some(p => p.action === 'remove' && p.id === 'main'), 'Home profile editor: the second press did not remove: ' + JSON.stringify(posts));
    await page.waitForFunction(() => document.querySelectorAll('#home-profiles .home-profile').length === 1);
  } finally { await page.close(); }
}

async function visitHomeUploads(browser, base, problems) {
  const context = await browser.newContext();
  const page = await context.newPage(), observer = await context.newPage();
  for (const p of [page, observer]) p.on('pageerror', e => problems.push('Home uploads: ' + e.message));
  const layout = { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] },
    hidden: { app: [], dev: [], ch: [] }, channels: [] };
  let uploading = false, uploaded = false, release, image;
  const wallpapers = () => ({ available: uploaded, uploading,
    slots: { morning: uploaded }, revision: uploaded ? '1' : '0' });
  await context.route(/\/api\/home(?:[/?]|$)/, async route => {
    const pathname = new URL(route.request().url()).pathname;
    let body;
    if (pathname === '/api/home/wallpaper') return route.fulfill({ contentType: 'image/jpeg', body: image });
    if (pathname.endsWith('/upload')) {
      image = route.request().postDataBuffer();
      if (!image || image.readUInt16BE(0) !== 0xffd8 || route.request().headers()['content-type'] !== 'image/jpeg') {
        problems.push('Home uploads: PNG was not prepared as a JPEG');
      }
      uploading = true;
      await new Promise(resolve => { release = resolve; });
      uploading = false; uploaded = true;
      body = { ok: true, wallpapers: wallpapers() };
    } else if (pathname === '/api/home/wallpapers') {
      if (route.request().postDataJSON().removeUploads) uploaded = false;
      body = { ok: true, wallpapers: wallpapers() };
    } else if (pathname === '/api/home') {
      body = { ok: true, supported: { ok: true }, installed: true, enabled: false, writable: true, layout, wallpapers: wallpapers() };
    } else {
      body = { ok: true, layout, apps: [], devices: [], channels: [], order: { app: [], dev: [], ch: [] }, sources: { channels: 0 } };
    }
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
  });
  try {
    await page.goto(base + '/?tab=home');
    await page.waitForFunction(() => typeof homeStatus !== 'undefined' && homeStatus && homeCat);
    await page.waitForSelector('#home-wall-slot:not(:disabled)');
    await page.locator('#home-wall-file').setInputFiles(path.join(ASSETS, 'dashboard-app/assets/icon130.png'));
    const sent = page.waitForRequest(r => new URL(r.url()).pathname.endsWith('/wallpapers/upload'));
    await page.locator('#home-wall-upload').click(); await sent;
    await observer.goto(base + '/?tab=home');
    await observer.getByRole('button', { name: 'Uploading…', exact: true }).waitFor();
    if (!await observer.locator('#home-wall-slot').isDisabled()) problems.push('Home uploads: another tab did not show the pending upload');
    release();
    await observer.waitForSelector('#home-wall-slot:not(:disabled)');
    await page.waitForSelector('#home-wall-slot:not(:disabled)');
    await page.waitForFunction(() => {
      const image = document.querySelector('#home-walls img');
      return image && image.complete && image.naturalWidth;
    });
    await page.locator('#home-wall-clear').click();
    await page.waitForFunction(() => !document.querySelector('#home-walls img'));
  } finally { await context.close(); }
}

// Where Native Tv Home is not offered its tab is left out; where it is on, the Apps tab says what LG's settings reach.
async function visitHomeAvailability(browser, base, problems) {
  for (const st of [{ available: false, enabled: false }, { available: true, enabled: true }]) {
    const page = await browser.newPage();
    page.on('pageerror', e => problems.push('Home availability: ' + e.message));
    await page.route(/\/api\/home(?:[/?]|$)/, route => route.fulfill({ contentType: 'application/json', body: JSON.stringify(Object.assign({
      ok: true, supported: { ok: st.available }, installed: true, restorePending: false, writable: true,
      layout: { rows: ['app', 'dev', 'ch'], order: { app: [], dev: [], ch: [] }, hidden: { app: [], dev: [], ch: [] }, channels: [] },
      apps: [], devices: [], channels: [], order: { app: [], dev: [], ch: [] }, sources: { channels: 0 }, wallpapers: { slots: {} }
    }, st)) }));
    try {
      await page.goto(base + '/?tab=home');
      await page.waitForFunction(() => homeAvailable !== null);
      await page.waitForTimeout(100);
      const hidden = await page.locator('#tab-btn-home').isHidden();
      if (hidden === st.available) problems.push('Home availability: tab ' + (hidden ? 'hidden' : 'shown') + ' with available ' + st.available);
      if (!st.available && await page.evaluate(() => activeTab) !== 'control') problems.push('Home availability: an unavailable Home tab stayed open');
      const notes = await page.locator('[data-nativetv-home-note]:not([hidden])').count();
      if (notes !== (st.enabled ? 2 : 0)) problems.push('Home availability: ' + notes + ' Apps tab notes with Native Tv Home ' + (st.enabled ? 'on' : 'off'));
    } finally { await page.close(); }
  }
}

async function visitHomeReadiness(browser, base, problems) {
  const page = await browser.newPage();
  page.on('pageerror', e => problems.push('Home readiness: ' + e.message));
  let failed = true, release = null, beats = 0, escapes = 0;
  await page.route('**/api/home/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    if (pathname.endsWith('/beat')) beats++;
    if (pathname.endsWith('/open')) escapes++;
    if (pathname.endsWith('/catalogue')) {
      if (failed) return route.fulfill({ status: 503, body: '{}' });
      await new Promise(resolve => { release = resolve; });
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: true, apps: [], devices: [], channels: [],
        order: { app: ['lg-home'], dev: [], ch: [] }, sources: { channels: 0 },
        layout: { rows: ['app', 'dev', 'ch'], hidden: { app: [], dev: [], ch: [] }, order: { app: [], dev: [], ch: [] },
          channels: [] } }) });
    }
    return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: true, build: 'one', kind: null }) });
  });
  try {
    await page.clock.install();
    await page.goto(base + PAGES.home);
    await page.waitForTimeout(100);
    await page.keyboard.press('ArrowDown');
    if (!escapes) problems.push('Home readiness: failed first catalogue did not ask for LG Home');
    if (beats) problems.push('Home readiness: unusable view sent a healthy heartbeat');
    failed = false;
    await page.reload();
    await page.waitForTimeout(100);
    await page.clock.fastForward(6500);
    if (beats) problems.push('Home readiness: heartbeat preceded initial rendering');
    release();
    await page.waitForSelector('.tile');
    await page.waitForTimeout(100);
    if (beats !== 1) problems.push('Home readiness: delayed first render did not immediately send a heartbeat');
  } finally { await page.close(); }
}

async function visitHomeRecents(browser, base, problems) {
  const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  page.on('pageerror', e => problems.push('Home recents: ' + e.message));
  let apps = [{ id: 'first', title: 'First' }, { id: 'second', title: 'Second' }];
  let posts = 0, fail = false, hold = false, release = null;
  const reply = (route, body, status = 200) => route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
  await page.route('**/api/home/**', async route => {
    const pathname = new URL(route.request().url()).pathname;
    if (pathname === '/api/home/catalogue') return reply(route, {
      ok: true, apps: [], devices: [], channels: [], sources: {}, order: { app: ['lg-home'], dev: [], ch: [] },
      layout: { rows: ['app', 'dev', 'ch'], hidden: { app: [], dev: [], ch: [] }, channels: [] }
    });
    if (pathname === '/api/home/recents') {
      if (route.request().method() === 'POST') {
        posts++;
        if (fail) return reply(route, { ok: false }, 400);
        const body = route.request().postDataJSON();
        apps = apps.filter(a => a.id !== body.id);
      } else if (hold) await new Promise(resolve => { release = resolve; });
      return reply(route, { ok: true, apps });
    }
    return reply(route, { ok: true, kind: null, build: 'one' });
  });
  async function back() {
    await page.evaluate(() => {
      for (const type of ['keydown', 'keyup']) document.dispatchEvent(new KeyboardEvent(type, { keyCode: 461 }));
    });
  }
  try {
    await page.goto(base + PAGES.home);
    await page.waitForSelector('#rows .tile');
    // Back from the first row reaches the bar, and from the bar the panel.
    await back();
    await back();
    await page.waitForSelector('#stage.recents');
    await page.keyboard.press('ArrowDown');
    await page.keyboard.down('Enter');
    await page.keyboard.down('Enter');
    await page.waitForTimeout(150);
    if (posts) problems.push('Home recents: held OK acted before release');
    await page.keyboard.up('Enter');
    await page.waitForFunction(() => document.querySelectorAll('#recentsTrack .rc').length === 1);
    if (posts !== 1) problems.push('Home recents: held OK closed more than one card');
    fail = true;
    await page.keyboard.press('Enter');
    await page.waitForSelector('#toast.show');
    const visibleError = await page.evaluate(() => {
      const toast = document.getElementById('toast'), r = toast.getBoundingClientRect();
      return toast.contains(document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2));
    });
    if (!visibleError) problems.push('Home recents: the panel covered its close error');
    await back();
    apps = Array.from({ length: 20 }, (_, i) => ({ id: 'recent' + i, title: 'Recent app ' + i }));
    await back();
    await page.waitForSelector('#stage.recents');
    for (let i = 0; i < 19; i++) await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(200);
    async function recentFits() {
      return page.evaluate(() => {
        const card = document.querySelector('#recentsTrack .rc.focus .card').getBoundingClientRect();
        return card.left >= 74 && card.right <= 1846;
      });
    }
    if (!await recentFits()) problems.push('Home recents: the last focused card was clipped');
    for (let i = 0; i < 19; i++) await page.keyboard.press('ArrowLeft');
    await page.waitForTimeout(200);
    if (!await recentFits()) problems.push('Home recents: the first focused card was clipped after returning');
    await back();
    for (const cancel of ['back', 'relaunch']) {
      hold = true; release = null;
      await back();
      const deadline = Date.now() + 3000;
      while (!release && Date.now() < deadline) await page.waitForTimeout(20);
      if (!release) throw new Error('Home recents: no panel request to delay');
      if (cancel === 'back') await back();
      else await page.evaluate(() => document.dispatchEvent(new Event('webOSRelaunch')));
      const answered = page.waitForResponse(r => new URL(r.url()).pathname === '/api/home/recents');
      hold = false; release();
      await answered;
      await page.waitForTimeout(100);
      if (await page.locator('#stage').evaluate(el => el.classList.contains('recents'))) {
        problems.push('Home recents: a late reply reopened the panel after ' + cancel);
      }
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
  for (const name of ['dashboard', 'home', 'setup', 'setup-phone']) {
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
      if (name === 'home' && !html.includes('<style id="look">')) problems.push('TV assets: Home lost its theme stylesheet');
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

// Optional paired operation benchmark against a saved Home page. Timings
// concern JavaScript under desktop throttling, not TV frame presentation.
async function benchmarkHome(browser, base, baseline) {
  const results = [];
  for (const [name, source] of [['before', fs.readFileSync(baseline, 'utf8')], ['after', null]]) {
    const runs = [];
    for (let run = 0; run < 3; run++) {
      const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
      if (source) await page.route('**' + PAGES.home, route => route.fulfill({ contentType: 'text/html', body: source }));
      const apps = Array.from({ length: 150 }, (_, i) => ({ id: 'app' + i, title: 'Application ' + i, icon: null }));
      const layout = { channels: [], hidden: {}, appNames: true, showInputs: false, showLive: false };
      await page.route('**/api/home/**', route => {
        const key = new URL(route.request().url()).pathname.split('/').pop();
        let body = { ok: true, build: 'one', layoutVersion: '1' };
        if (key === 'catalogue') body = { ok: true, layout, apps, devices: [], channels: [], sources: { channels: 0 },
          order: { app: apps.map(a => a.id).concat(['lg-home']), dev: [], ch: [] } };
        if (key === 'discover') body = { ok: true, enabled: false, rows: [] };
        return route.fulfill({ contentType: 'application/json', body: JSON.stringify(body) });
      });
      try {
        await page.goto(base + PAGES.home);
        await page.waitForSelector('.tile.focus');
        await page.waitForTimeout(200);
        const cdp = await page.context().newCDPSession(page);
        await cdp.send('Emulation.setCPUThrottlingRate', { rate: 20 });
        runs.push(await page.evaluate(async () => {
          function key(code) { document.dispatchEvent(new KeyboardEvent('keydown', { keyCode: code, bubbles: true })); }
          key(38);
          const start = performance.now();
          key(39);
          const gridMs = performance.now() - start;
          const gridTiles = document.querySelectorAll('#rows .tile').length;
          key(40);
          const times = [];
          for (let i = 0; i < 20; i++) { const at = performance.now(); key(40); times.push(performance.now() - at); }
          times.sort((a, b) => a - b);
          const burstMedian = times[10], burstMax = times[19];
          // Also measure separate remote events with time to refill the
          // overscan between them. The burst above deliberately gives none.
          key(461);
          await new Promise(resolve => setTimeout(resolve, 250));
          times.length = 0;
          for (let i = 0; i < 20; i++) {
            const at = performance.now(); key(40); times.push(performance.now() - at);
            await new Promise(resolve => setTimeout(resolve, 80));
          }
          times.sort((a, b) => a - b);
          return { gridMs: Math.round(gridMs * 10) / 10, gridTiles,
            burstMedianMs: Math.round(burstMedian * 10) / 10, burstMaxMs: Math.round(burstMax * 10) / 10,
            pacedMedianMs: Math.round(times[10] * 10) / 10, pacedMaxMs: Math.round(times[19] * 10) / 10,
            retainedTiles: document.querySelectorAll('#rows .tile').length };
        }));
      } finally { await page.close(); }
    }
    results.push({ name, runs });
  }
  console.log('Home benchmark (Chrome, 20x CPU throttling): ' + JSON.stringify(results));
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
  const checks = [visitTvDashboard, visitTvNonOled, visitMovedPages, visitTVAssetAssembly, visitTVPolling, visitTVRendering, visitTVFastStats, visitHomeBackground, visitHomeRefreshWork, visitHomeAppArt, visitHomeImageBudget, visitHomeImageRecovery, visitHomeRefreshScope, visitSetupRendering, visitTVDesignSystem,
    visitHomePicture, visitHomeColours, visitHomePreferences, visitHomeClock, visitHomeVisibility, visitHomeTv, visitHomeQuick, visitHomeProfiles,
    visitHomeContent, visitHomePointer, visitHomeRendering, visitHomeSpeed, visitHomeGuideRefresh, visitHomeEditor, visitHomeOrder, visitHomeProfileEditor, visitHomeUploads,
    visitHomeReadiness, visitHomeAvailability, visitHomeRecents];
  let checked = 0;
  for (const check of checks) {
    if (filter && !filter.test(check.name)) continue;
    console.log('Checking ' + check.name);
    await check(browser, base, problems);
    checked++;
  }
  if (option('--baseline-home')) await benchmarkHome(browser, base, option('--baseline-home'));
  await browser.close();
  for (const p of problems) console.log(p);
  console.log((filter ? 'Selected checks' : (web.length + tv.length) + ' tabs opened (' + web.length + ' web, ' + tv.length + ' TV app)') + ', ' +
              checked + ' regression groups checked, ' + problems.length + ' problem' + (problems.length === 1 ? '' : 's'));
  process.exit(problems.length ? 1 : 0);
})().catch(e => { console.error(e.stack || e.message); const log = path.join(os.tmpdir(), 'nativetv-dashboards-daemon.log'); fs.writeFileSync(log, serverOutput()); console.error('--- daemon output: last lines below, all of it in ' + log + ' ---\n' + serverOutput().slice(-3000)); process.exit(1); });
