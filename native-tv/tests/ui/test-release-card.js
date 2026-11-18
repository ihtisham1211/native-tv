/**
 * test/test-release-card.js - The Server tab's Updates card
 *
 * Runs renderRelease() from ui/server.js against the status shapes the server
 * sends: a newer release that can be installed from here, one that cannot, an
 * update under way and one that failed, none, a failed check and a server with
 * no release check at all; then the install button's confirmation and the
 * page's wait for the server to come back with the new version.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-release-card.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'server.js'), 'utf8');
var els = {};
['release-row', 'release-daily-row', 'release-check', 'release-install', 'release-status', 'release-offer', 'release-daily']
  .forEach(function (id) { els[id] = { hidden: false, disabled: false, textContent: '', innerHTML: '', dataset: {} }; });
var sent = [], asked = [], timers = [], reloads = 0, answer = true;
var ctx = {
  q: function (id) { return els[id]; },
  t: function (key, text, vars) {
    return text.replace(/\{(\w+)\}/g, function (_, name) { return vars[name]; });
  },
  esc: function (text) { return String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;'); },
  cfgAge: function (ms) { return ms == null ? 'not yet' : Math.round(ms / 60000) + 'm ago'; },
  updateToggle: function (id, opts) { Object.assign(els[id], opts); },
  sendCommand: function (action, value) { sent.push([action, value]); return Promise.resolve({ ok: true }); },
  confirm: function (text) { asked.push(text); return answer; },
  loadServerStatus: function () { return Promise.resolve(); },
  setTimeout: function (fn, ms) { timers.push([fn, ms]); },
  location: { reload: function () { reloads += 1; } }
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.renderRelease = renderRelease; this.installRelease = installRelease;', ctx);

var latest = {
  version: '0.119.0', sha256: 'a'.repeat(64),
  releaseUrl: 'https://github.com/ihtisham1211/native-tv/releases/tag/v0.119.0',
  ipkUrl: 'https://github.com/ihtisham1211/native-tv/releases/download/v0.119.0/x_0.119.0_all.ipk'
};
function status(extra) {
  return Object.assign({ current: '0.118.0', fromSource: false, checking: false, checkedAgoS: 120, error: null, latest: null, available: false,
    update: { state: 'idle', version: null, receivedBytes: 0, error: null, unavailable: null } }, extra);
}
function update(extra) {
  return Object.assign({ state: 'idle', version: null, receivedBytes: 0, error: null, unavailable: null }, extra);
}

ctx.renderRelease(null, false, true);
assert.ok(els['release-row'].hidden && els['release-daily-row'].hidden, 'a server with no release check shows no card');

ctx.renderRelease(status(), false, true);
assert.ok(!els['release-row'].hidden && !els['release-daily-row'].hidden, 'the card shows when the server can check');
assert.strictEqual(els['release-status'].textContent, 'Not checked yet. This TV has 0.118.0.');
assert.strictEqual(els['release-check'].textContent, 'Check for updates');
assert.ok(els['release-offer'].hidden && els['release-install'].hidden, 'nothing is offered before a check');
assert.strictEqual(els['release-daily'].label, 'Off');

// A newer release on a server that can install it: a button, not a pointer to another app.
ctx.renderRelease(status({ latest: latest, available: true }), true, true);
assert.strictEqual(els['release-status'].textContent, 'Version 0.119.0 is out. This TV has 0.118.0.');
assert.ok(!els['release-offer'].hidden && !els['release-install'].hidden && !els['release-install'].disabled,
  'a newer release is offered with an Install button');
assert.strictEqual(els['release-install'].textContent, 'Install version 0.119.0');
assert.strictEqual(els['release-install'].dataset.version, '0.119.0');
['releases/tag/v0.119.0', '_0.119.0_all.ipk', 'a'.repeat(64), 'checks it against the checksum of the release'].forEach(function (part) {
  assert.ok(els['release-offer'].innerHTML.indexOf(part) >= 0, 'the offer names ' + part);
});
assert.strictEqual(els['release-daily'].label, 'On');

// One that cannot say why, and is not offered the button.
ctx.renderRelease(status({ latest: latest, available: true, fromSource: true,
  update: update({ unavailable: 'this server was built from a git checkout: pull the release and deploy it again' }) }), false, true);
assert.ok(els['release-install'].hidden, 'a build from a clone is not offered the install');
assert.ok(els['release-offer'].innerHTML.indexOf('git checkout') >= 0, 'it is told to deploy again');
ctx.renderRelease(status({ latest: latest, available: true,
  update: update({ unavailable: 'this server was not installed from the Native Tv app package, so it is updated by deploying it again' }) }), false, true);
assert.ok(els['release-install'].hidden && els['release-offer'].innerHTML.indexOf('deploying it again') >= 0,
  'a deploy.sh install is told how it is updated');

// An update under way: each stage says what it is doing, and nothing else may start.
var stages = [
  [{ state: 'downloading', version: '0.119.0' }, 'Asking GitHub for version 0.119.0…'],
  [{ state: 'downloading', version: '0.119.0', receivedBytes: 12.34 * 1048576 }, 'Downloading version 0.119.0… 12.3 MB'],
  [{ state: 'verifying', version: '0.119.0' }, 'Checking the download against the checksum of the release…'],
  [{ state: 'installing', version: '0.119.0' }, 'Installing the package on the TV…'],
  [{ state: 'restarting', version: '0.119.0' }, 'Restarting the server. This page reloads when it is back.']
];
stages.forEach(function (stage) {
  ctx.renderRelease(status({ latest: latest, available: true, update: update(stage[0]) }), false, true);
  assert.strictEqual(els['release-status'].textContent, stage[1]);
  assert.ok(els['release-install'].disabled && els['release-check'].disabled, 'the buttons wait while ' + stage[0].state);
});

// A failure stays beside the release it was for, and only that one.
ctx.renderRelease(status({ latest: latest, available: true,
  update: update({ state: 'failed', version: '0.119.0', error: 'the downloaded package does not match the release\'s checksum' }) }), false, true);
assert.ok(/The update to version 0\.119\.0 failed: the downloaded package does not match/.test(els['release-status'].textContent),
  'a failed update says why');
assert.ok(!els['release-install'].disabled, 'a failed update can be tried again');
ctx.renderRelease(status({ latest: latest, available: true,
  update: update({ state: 'failed', version: '0.118.5', error: 'old failure' }) }), false, true);
assert.ok(els['release-status'].textContent.indexOf('failed') < 0, 'the failure of an earlier release is not shown against this one');

ctx.renderRelease(status({ latest: Object.assign({}, latest, { version: '0.118.0' }) }), false, true);
assert.strictEqual(els['release-status'].textContent, 'No release newer than 0.118.0, checked 2m ago.');
assert.ok(els['release-offer'].hidden && els['release-install'].hidden, 'nothing is offered when this is the latest');

ctx.renderRelease(status({ error: 'GitHub could not be reached', checkedAgoS: 5 }), false, true);
assert.strictEqual(els['release-status'].textContent, 'Could not check: GitHub could not be reached. This TV has 0.118.0.');

ctx.renderRelease(status({ latest: latest, available: true, error: 'GitHub could not be reached' }), false, true);
assert.ok(/The latest check failed: GitHub could not be reached\.$/.test(els['release-status'].textContent), 'a failed re-check is shown beside the last answer');
assert.ok(!els['release-offer'].hidden, 'the last good answer is still offered');

ctx.renderRelease(status({ checking: true }), false, true);
assert.ok(els['release-check'].disabled && els['release-check'].textContent === 'Checking…', 'the button waits while a check runs');
ctx.renderRelease(status({ latest: latest, available: true }), false, false);
assert.ok(els['release-check'].disabled && els['release-daily'].disabled && els['release-install'].disabled,
  'a read-only dashboard cannot check, switch or install');

// The Install button asks first, and sends nothing when it is refused.
ctx.renderRelease(status({ latest: latest, available: true }), false, true);
answer = false;
ctx.installRelease().then(function () {
  assert.strictEqual(asked.length, 1);
  assert.ok(asked[0].indexOf('Install version 0.119.0?') === 0 && /closes while its package is replaced/.test(asked[0]) &&
    /the old one is put back/.test(asked[0]), 'the question says what an install does and undoes');
  assert.strictEqual(sent.length, 0, 'a refused question installs nothing');
  answer = true;
  return ctx.installRelease();
}).then(function () {
  assert.deepStrictEqual(sent, [['installRelease', '0.119.0']], 'the version asked about is the one installed');
  assert.strictEqual(timers.length, 1, 'the page starts to wait for the update');

  // The server goes away while it restarts, and comes back as the new version: the page reloads.
  var wait = timers.pop()[0];
  ctx.renderRelease(status({ latest: latest, available: true, update: update({ state: 'restarting', version: '0.119.0' }) }), false, true);
  return wait().then(function () {
    assert.strictEqual(timers.length, 1, 'a server that has not answered is waited for again');
    ctx.renderRelease(status({ current: '0.119.0', latest: latest, available: false }), false, true);
    return timers.pop()[0]();
  });
}).then(function () {
  assert.strictEqual(els['release-status'].textContent, 'Updated to version 0.119.0. Reloading…');
  assert.strictEqual(timers.length, 1);
  timers.pop()[0]();
  assert.strictEqual(reloads, 1, 'the page reloads to take the new scripts');

  // An update that failed is not waited on: the old server answers, and says so.
  return ctx.installRelease();
}).then(function () {
  var wait = timers.pop()[0];
  ctx.renderRelease(status({ latest: latest, available: true, update: update({ state: 'failed', version: '0.119.0', error: 'the new server did not start' }) }), false, true);
  return wait().then(function () {
    assert.strictEqual(timers.length, 0, 'the wait ends when the server reports a failure');
    assert.strictEqual(reloads, 1);
    console.log('ALL test-release-card.js assertions passed!\n');
  });
}).catch(function (error) {
  console.error(error);
  process.exit(1);
});
