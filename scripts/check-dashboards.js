#!/usr/bin/env node
/*
 * Opens every tab of the web dashboard in a headless browser, against the real
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
try { chromium = require('playwright').chromium; } catch (e) {
  try { chromium = require('playwright-core').chromium; } catch (e2) {
    console.error('Playwright is not installed: npm install --no-save playwright && npx playwright install chromium\n' +
      '(or, with Chrome installed: npm install --no-save playwright-core, and BROWSER_CHANNEL=chrome)');
    process.exit(2);
  }
}

const ROOT = path.join(__dirname, '..');
const ASSETS = path.join(ROOT, 'native-tv', 'assets');
const WEB = path.join(ROOT, 'native-tv', 'web');

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
    await page.emulateMedia({ reducedMotion: 'reduce' });
    const duration = await page.locator('#rows .row').first().evaluate(el => getComputedStyle(el).transitionDuration);
    if (duration !== '0s') problems.push('TV setup: reduced motion did not disable row transitions');
    await page.emulateMedia({ reducedMotion: 'no-preference' });
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

// Source separation must not turn CSS or JavaScript into startup requests.
// The legacy flat URLs must also resolve directly, without another redirect.
async function visitTVAssetAssembly(browser, base, problems) {
  for (const name of ['setup', 'setup-phone']) {
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
  const page = await browser.newPage();
  try {
    for (const retired of ['/assets/dashboard.html', '/assets/google-tv/pages/dashboard.html',
      '/assets/google-tv/pages/dashboard/index.html']) {
      const response = await page.request.get(base + retired, { maxRedirects: 0 });
      if (response.status() !== 404) problems.push('TV assets: retired page still served at ' + retired);
    }
  } finally { await page.close(); }
}

// Each language of the dashboard (ui/i18n.js) opens on a phone without a script
// error or a sideways scroll, translates the Server tab, tells the server which
// language it is on its own requests, and is listed by the Language setting. Arabic
// is laid out right to left, which moves the Remote button to the other corner.
async function visitLanguages(browser, base, problems) {
  const loader = fs.readFileSync(path.join(WEB, 'ui', 'i18n.js'), 'utf8');
  const codes = [...loader.match(/var LANGS = \[([^\]]*)\]/)[1].matchAll(/'([a-z-]+)'/g)].map(m => m[1]);
  for (const code of codes) {
    const label = 'Language ' + code;
    const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
    page.on('pageerror', e => problems.push(label + ': ' + e.message));
    page.on('console', m => {
      if (m.type() === 'error' && !/^Failed to load resource/.test(m.text())) problems.push(label + ': console: ' + m.text());
    });
    let sent = null;
    page.on('request', r => { if (sent === null && /\/api\//.test(r.url())) sent = r.headers()['x-native-tv-lang'] || ''; });
    try {
      await page.goto(base + '/?lang=' + code + '&tab=server', { waitUntil: 'load' });
      await page.waitForFunction(() => document.querySelector('[data-t="server.health.title"]') && document.querySelector('.fab'), null, { timeout: 5000 });
      await page.waitForTimeout(500);
      const seen = await page.evaluate(() => ({
        lang: document.documentElement.lang, dir: document.documentElement.dir,
        title: document.querySelector('[data-t="server.health.title"]').textContent.trim(),
        options: Array.from(document.querySelectorAll('#lang-sel option')).map(o => o.value),
        sideways: document.documentElement.scrollWidth - document.documentElement.clientWidth,
        remoteOnLeft: document.querySelector('.fab').getBoundingClientRect().left < innerWidth / 2
      }));
      if (seen.lang !== code) problems.push(label + ': the page is in "' + seen.lang + '"');
      if (seen.dir !== (code === 'ar' ? 'rtl' : '')) problems.push(label + ': dir is "' + seen.dir + '"');
      if (seen.title === 'Server health') problems.push(label + ': the Server tab is not translated');
      if (seen.remoteOnLeft !== (code === 'ar')) problems.push(label + ': the Remote button is on the ' + (seen.remoteOnLeft ? 'left' : 'right'));
      if (seen.sideways > 0) problems.push(label + ': the page scrolls sideways by ' + seen.sideways + 'px');
      if (sent !== code) problems.push(label + ': the first API request sent language "' + sent + '"');
      const listed = ['auto', 'en'].concat(codes).sort().join();
      if (seen.options.slice().sort().join() !== listed) problems.push(label + ': the Language list offers ' + seen.options.join());
    } finally { await page.close(); }
  }
}

let serverOutput = () => '';
(async () => {
  const server = await startServer();
  serverOutput = server.output;
  const base = 'http://127.0.0.1:' + server.port;
  const browser = await chromium.launch(process.env.BROWSER_CHANNEL ? { channel: process.env.BROWSER_CHANNEL } : {});
  const problems = [];
  const web = webTabs();
  const filter = process.env.DASHBOARD_CHECK_FILTER && new RegExp(process.env.DASHBOARD_CHECK_FILTER, 'i');
  if (!filter) {
    await visitAll(browser, web.map(t => ({ url: base + '/?tab=' + t, label: 'web ' + t })), problems);
  }
  const checks = [visitTVAssetAssembly, visitSetupRendering, visitLanguages];
  let checked = 0;
  for (const check of checks) {
    if (filter && !filter.test(check.name)) continue;
    console.log('Checking ' + check.name);
    await check(browser, base, problems);
    checked++;
  }
  await browser.close();
  for (const p of problems) console.log(p);
  console.log((filter ? 'Selected checks' : web.length + ' web dashboard tabs opened') + ', ' +
              checked + ' regression groups checked, ' + problems.length + ' problem' + (problems.length === 1 ? '' : 's'));
  process.exit(problems.length ? 1 : 0);
})().catch(e => { console.error(e.stack || e.message); const log = path.join(os.tmpdir(), 'nativetv-dashboards-daemon.log'); fs.writeFileSync(log, serverOutput()); console.error('--- daemon output: last lines below, all of it in ' + log + ' ---\n' + serverOutput().slice(-3000)); process.exit(1); });
