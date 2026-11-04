/**
 * test-pwa.js - The dashboard is installable: the page links a manifest that a browser would
 * accept (name, start URL, standalone display, 192 and 512 px icons that exist at those
 * sizes, a maskable one), and the service worker answers only navigations, so the API and
 * the page are never served from a cache.
 */

var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-pwa.js ...');

var root = path.join(__dirname, '..', '..');
var pwa = path.join(root, 'assets', 'pwa');
var html = fs.readFileSync(path.join(root, 'web', 'ui.html'), 'utf8');
var mainJs = fs.readFileSync(path.join(root, 'web', 'ui', 'main.js'), 'utf8');

function pngSize(file) {
  var bytes = fs.readFileSync(file);
  assert.strictEqual(bytes.toString('latin1', 1, 4), 'PNG', file + ' is a PNG');
  return bytes.readUInt32BE(16) + 'x' + bytes.readUInt32BE(20);
}

(function testManifest() {
  var manifest = JSON.parse(fs.readFileSync(path.join(pwa, 'manifest.webmanifest'), 'utf8'));
  assert(manifest.name && manifest.short_name, 'the app has a name');
  assert.strictEqual(manifest.start_url, '/', 'it opens the dashboard');
  assert(manifest.scope === '/' && manifest.display === 'standalone', 'it opens as an app, scoped to the whole site');
  var theme = /<meta name="theme-color" content="(#\w+)"/.exec(html);
  assert(theme && manifest.theme_color === theme[1], 'the manifest and the page agree on the theme colour');

  var any = {}, maskable = false;
  manifest.icons.forEach(function (icon) {
    assert(/^\/assets\/pwa\/[\w-]+\.png$/.test(icon.src), icon.src + ' is served from assets/pwa');
    assert.strictEqual(pngSize(path.join(root, icon.src.replace('/assets/', 'assets/'))), icon.sizes, icon.src + ' is the size it claims');
    if (icon.purpose === 'maskable') maskable = true; else any[icon.sizes] = true;
  });
  assert(any['192x192'] && any['512x512'] && maskable, 'a 192, a 512 and a maskable icon, which Chrome needs to offer the install');
  console.log('  ok: the manifest is complete and its icons exist');
})();

(function testPageLinks() {
  assert(html.indexOf('<link rel="manifest" href="/assets/pwa/manifest.webmanifest"') !== -1, 'ui.html links the manifest');
  assert(/rel="apple-touch-icon" href="\/assets\/pwa\/icon-maskable-512\.png"/.test(html), 'iOS gets an opaque icon');
  assert(html.indexOf('name="apple-mobile-web-app-capable"') !== -1, 'iOS opens it without browser chrome');
  assert(mainJs.indexOf("navigator.serviceWorker.register('/assets/pwa/sw.js', { scope: '/' })") !== -1, 'main.js registers the worker for the whole site');
  console.log('  ok: the page links the manifest, the icon and the worker');
})();

(function testServiceWorker() {
  var source = fs.readFileSync(path.join(pwa, 'sw.js'), 'utf8');
  var listeners = {};
  var network = { fail: false, calls: [] };
  var sandbox = {
    addEventListener: function (type, fn) { listeners[type] = fn; },
    skipWaiting: function () {}, clients: { claim: function () {} },
    fetch: function (request) { network.calls.push(request); return network.fail ? Promise.reject(new Error('down')) : Promise.resolve('from the TV'); },
    Response: function (body, init) { this.body = body; this.status = init.status; this.type = init.headers['Content-Type']; }
  };
  vm.runInNewContext(source, sandbox);
  assert(listeners.install && listeners.activate && listeners.fetch, 'the worker installs, activates and handles fetches');

  function request(mode) {
    var event = { request: { mode: mode }, answer: undefined, respondWith: function (answer) { this.answer = answer; } };
    listeners.fetch(event);
    return event;
  }
  ['same-origin', 'cors', 'no-cors'].forEach(function (mode) {
    assert.strictEqual(request(mode).answer, undefined, 'a ' + mode + ' request (the API, a script) is left to the browser');
  });
  assert.strictEqual(network.calls.length, 0, 'and the worker does not fetch it');

  var navigation = request('navigate');
  assert(navigation.answer, 'a page navigation is answered');
  return navigation.answer.then(function (answer) {
    assert.strictEqual(answer, 'from the TV', 'it comes from the TV when the TV answers');
    network.fail = true;
    return request('navigate').answer;
  }).then(function (answer) {
    assert(answer.status === 503 && /text\/html/.test(answer.type) && /Can\\u2019t reach the TV|Can’t reach the TV/.test(source), 'an unreachable TV gets the offline page, not the browser error');
    assert(!/caches\./.test(source), 'nothing is cached');
    console.log('  ok: the worker answers navigations only, and never caches');
    console.log('ALL test-pwa.js assertions passed!');
  });
})().catch(function (error) { console.error(error); process.exit(1); });
