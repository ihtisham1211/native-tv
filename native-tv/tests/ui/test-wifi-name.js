/**
 * test/test-wifi-name.js - The Network row shows a Wi-Fi name as it was typed
 *
 * Runs tick() from ui/readouts.js against one /api/stats answer. row() writes the Network row's
 * sub-line with textContent, so a name escaped first showed "Home & Garden" as
 * "Home &amp; Garden". The scripts the rest of tick() calls are stood in for.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-wifi-name.js ...');

var ui = path.join(__dirname, '..', '..', 'web', 'ui');
var els = {};
function element() {
  var classList = { add: function () {}, remove: function () {}, toggle: function () {}, contains: function () { return false; } };
  return {
    hidden: false, textContent: '', innerHTML: '', dataset: {}, firstChild: null,
    style: { setProperty: function () {}, display: '' }, classList: classList,
    parentElement: { classList: classList },
    querySelector: element, querySelectorAll: function () { return []; },
    setAttribute: function () {}, removeAttribute: function () {}
  };
}

var stats;
var clock = 0;
var failure = '';
var ctx = {
  token: '',
  document: { title: '' },
  q: function (id) { return els[id] || (els[id] = element()); },
  t: function (key, text) { return text; },
  fetch: function () { return Promise.resolve({ ok: true, json: function () { return Promise.resolve(stats); } }); },
  // tick() returns quietly from an error within 6 s of its last good read; a clock that jumps
  // 10 s a call lets the error show.
  Date: { now: function () { return (clock += 10000); } },
  showErr: function (message) { failure = message; },
  // Defined by other scripts of the page.
  errTimer: null, advLgsDevices: false,
  getLease: function () {}, advPill: function () {}, arOffRow: function () {}, powerTimerRows: function () {}, renderHdmiDiag: function () {}
};
vm.createContext(ctx);
['format.js', 'readouts.js'].forEach(function (name) {
  vm.runInContext(fs.readFileSync(path.join(ui, name), 'utf8'), ctx, { filename: name });
});

function netLine(ssid) {
  stats = {
    ssid: ssid, wifi: { level: -50 }, net: { rx: 2048, tx: 0 },
    mem: { total: 1024, avail: 512 }, swap: { total: 0, free: 0 }, load: 5, cores: []
  };
  els = {};
  failure = '';
  return vm.runInContext('tick()', ctx).then(function () {
    // What follows the Network row needs more of the page than is stood in for here.
    assert.ok(els['sub-net'], 'tick() drew the Network row (it stopped with: ' + failure + ')');
    return els['sub-net'].textContent;
  });
}

netLine('Home & Garden <5G> "Guest"').then(function (line) {
  assert.strictEqual(line, 'Home & Garden <5G> "Guest"  ·  -50 dBm', 'the name is shown as typed, not as HTML');
  return netLine('Home');
}).then(function (line) {
  assert.strictEqual(line, 'Home  ·  -50 dBm');
  return netLine(undefined);
}).then(function (line) {
  assert.strictEqual(line, '-50 dBm', 'a set with no name shows the signal alone');
  console.log('ALL test-wifi-name.js assertions passed!\n');
}).catch(function (error) {
  console.error(error);
  process.exit(1);
});
