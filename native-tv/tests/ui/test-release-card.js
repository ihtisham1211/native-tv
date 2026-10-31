/**
 * test/test-release-card.js - The Server tab's Updates card
 *
 * Runs renderRelease() from ui/server.js against the status shapes the server
 * sends: a newer release, none, a failed check, a build from a git checkout
 * and a server with no release check at all.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-release-card.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'server.js'), 'utf8');
var els = {};
['release-row', 'release-daily-row', 'release-check', 'release-hbc', 'release-status', 'release-offer', 'release-daily']
  .forEach(function (id) { els[id] = { hidden: false, disabled: false, textContent: '', innerHTML: '', dataset: {} }; });
var ctx = {
  q: function (id) { return els[id]; },
  t: function (key, text, vars) {
    return text.replace(/\{(\w+)\}/g, function (_, name) { return vars[name]; });
  },
  esc: function (text) { return String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;'); },
  cfgAge: function (ms) { return ms == null ? 'not yet' : Math.round(ms / 60000) + 'm ago'; },
  updateToggle: function (id, opts) { Object.assign(els[id], opts); },
  sendCommand: function () {}
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.renderRelease = renderRelease;', ctx);

var latest = {
  version: '0.119.0', sha256: 'a'.repeat(64),
  releaseUrl: 'https://github.com/ihtisham1211/native-tv/releases/tag/v0.119.0',
  ipkUrl: 'https://github.com/ihtisham1211/native-tv/releases/download/v0.119.0/x_0.119.0_all.ipk'
};
function status(extra) {
  return Object.assign({ current: '0.118.0', fromSource: false, checking: false, checkedAgoS: 120, error: null, latest: null, available: false }, extra);
}

ctx.renderRelease(null, false, true);
assert.ok(els['release-row'].hidden && els['release-daily-row'].hidden, 'a server with no release check shows no card');

ctx.renderRelease(status(), false, true);
assert.ok(!els['release-row'].hidden && !els['release-daily-row'].hidden, 'the card shows when the server can check');
assert.strictEqual(els['release-status'].textContent, 'Not checked yet. This TV has 0.118.0.');
assert.strictEqual(els['release-check'].textContent, 'Check for updates');
assert.ok(els['release-offer'].hidden && els['release-hbc'].hidden, 'nothing is offered before a check');
assert.strictEqual(els['release-daily'].label, 'Off');

ctx.renderRelease(status({ latest: latest, available: true }), true, true);
assert.strictEqual(els['release-status'].textContent, 'Version 0.119.0 is out. This TV has 0.118.0.');
assert.ok(!els['release-offer'].hidden && !els['release-hbc'].hidden, 'a newer release is offered with the Homebrew Channel button');
['releases/tag/v0.119.0', '_0.119.0_all.ipk', 'a'.repeat(64), 'Homebrew Channel'].forEach(function (part) {
  assert.ok(els['release-offer'].innerHTML.indexOf(part) >= 0, 'the offer names ' + part);
});
assert.strictEqual(els['release-daily'].label, 'On');

ctx.renderRelease(status({ latest: latest, available: true, fromSource: true }), false, true);
assert.ok(els['release-hbc'].hidden, 'a build from a clone is not sent to the Homebrew Channel');
assert.ok(els['release-offer'].innerHTML.indexOf('git checkout') >= 0, 'it is told to deploy again');

ctx.renderRelease(status({ latest: Object.assign({}, latest, { version: '0.118.0' }) }), false, true);
assert.strictEqual(els['release-status'].textContent, 'No release newer than 0.118.0, checked 2m ago.');
assert.ok(els['release-offer'].hidden, 'nothing is offered when this is the latest');

ctx.renderRelease(status({ error: 'GitHub could not be reached', checkedAgoS: 5 }), false, true);
assert.strictEqual(els['release-status'].textContent, 'Could not check: GitHub could not be reached. This TV has 0.118.0.');

ctx.renderRelease(status({ latest: latest, available: true, error: 'GitHub could not be reached' }), false, true);
assert.ok(/The latest check failed: GitHub could not be reached\.$/.test(els['release-status'].textContent), 'a failed re-check is shown beside the last answer');
assert.ok(!els['release-offer'].hidden, 'the last good answer is still offered');

ctx.renderRelease(status({ checking: true }), false, true);
assert.ok(els['release-check'].disabled && els['release-check'].textContent === 'Checking…', 'the button waits while a check runs');
ctx.renderRelease(status(), false, false);
assert.ok(els['release-check'].disabled && els['release-daily'].disabled, 'a read-only dashboard cannot check or switch');

console.log('ALL test-release-card.js assertions passed!\n');
