/**
 * The TV report on the Server tab (web/ui/report.js): what it says about a TV, and above all what
 * it leaves out. It is run over the stats captured from real TVs (tests/fixtures/tv/).
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-tv-report-ui.js ...');

var source = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'report.js'), 'utf8');

function load(fetchReplies) {
  var elements = {};
  function el(id) { return elements[id] || (elements[id] = { id: id, value: '', textContent: '', hidden: true }); }
  var context = {
    console: console, Object: Object, String: String, Promise: Promise, JSON: JSON,
    q: el,
    api: function (p) { return p; },
    t: function (key, english) { return english; },
    copyToClipboard: function () { return Promise.resolve(); },
    fetch: function (url) {
      var reply = fetchReplies[url];
      return reply instanceof Error || reply === undefined
        ? Promise.reject(new Error('offline'))
        : Promise.resolve({ json: function () { return Promise.resolve(reply); } });
    }
  };
  vm.createContext(context);
  vm.runInContext(source, context);
  return { context: context, el: el };
}

function report(d) {
  var page = load({});
  page.context.__d = d;
  return vm.runInContext('tvReportText(__d)', page.context);
}

var caps = { version: '0.120.0', allowControl: true, allowPower: true, fromHomebrewChannel: false };
var home = { ok: true, supported: { ok: true }, nativeHome: { halted: false } };
var fixtureDir = path.join(__dirname, '..', 'fixtures', 'tv');
var fixtures = fs.readdirSync(fixtureDir).filter(function (name) { return /^stats-.*\.json$/.test(name); });
assert.ok(fixtures.length >= 5, 'the real captures are there');

var MAC = /\b[0-9a-f]{2}(:[0-9a-f]{2}){5}\b/i;
var IPV4 = /\b\d{1,3}(\.\d{1,3}){3}\b/;

fixtures.forEach(function (name) {
  var stats = JSON.parse(fs.readFileSync(path.join(fixtureDir, name), 'utf8'));
  var text = report({ stats: stats, caps: caps, home: home });
  // What it says.
  assert.ok(text.indexOf('| Model | ' + stats.device.model + ' |') !== -1, name + ': the model');
  assert.ok(text.indexOf('| webOS | webOS ' + stats.system.webos + ' |') !== -1, name + ': the webOS version as the TV reports it');
  assert.ok(text.indexOf('| Firmware | ' + stats.system.firmware + ' |') !== -1, name + ': the firmware');
  assert.ok(text.indexOf('| SoC | ' + stats.hardware.soc_arch + ' |') !== -1, name + ': the SoC');
  assert.ok(/^Answered: .+$/m.test(text) && /^Not answered: .+$/m.test(text), name + ': both lists');
  // What it leaves out.
  [stats.mac, stats.ssid, stats.address, stats.hostname, stats.device.name].forEach(function (value) {
    if (value && value !== stats.device.model)
      assert.strictEqual(text.indexOf(value), -1, name + ': "' + value + '" is not in the report');
  });
  assert.ok(!MAC.test(text), name + ': no MAC address');
  assert.ok(!IPV4.test(text), name + ': no IPv4 address');
  assert.ok(!/ssid|hostname/i.test(text.replace(/^Left out:.*$/m, '')), name + ': no network name');
});
console.log('  ok - the report of each of ' + fixtures.length + ' real TVs names the TV and holds nothing private');

var answered = {};
fixtures.forEach(function (name) {
  var text = report({ stats: JSON.parse(fs.readFileSync(path.join(fixtureDir, name), 'utf8')), caps: caps, home: home });
  answered[text.match(/^Answered: (.*)$/m)[1]] = true;
});
assert.ok(Object.keys(answered).length > 1, 'the lists follow what each TV answered');
console.log('  ok - which readings were answered differs between the TVs');

// A stats payload that holds more than a report should ever copy.
var hostile = report({
  stats: {
    mac: 'aa:bb:cc:dd:ee:ff', ssid: 'Dad\'s Wi-Fi', address: '192.168.1.9', hostname: 'lg-livingroom',
    wifi: { ssid: 'Dad\'s Wi-Fi', bssid: '11:22:33:44:55:66' }, netTotal: { iface: 'wlan0', mac: 'aa:bb:cc:dd:ee:ff' },
    device: { id: 'secret_id', name: 'Dad\'s TV', model: 'OLED65|B8\nINJECTED' },
    system: { webos: '9.2.0', firmware: '03.00.00' }, hardware: { soc_arch: 'A9', ram: '4GB' },
    tvwebVersion: '0.120.0', powerState: 'Active'
  },
  caps: caps, home: home
});
['aa:bb:cc', 'Dad', '192.168', 'lg-livingroom', 'secret_id', '11:22:33', 'wlan0'].forEach(function (leak) {
  assert.strictEqual(hostile.indexOf(leak), -1, 'the report does not hold ' + leak);
});
assert.ok(hostile.indexOf('| Model | OLED65 B8 INJECTED |') !== -1, 'a pipe or line break in a value cannot split a table row');
assert.ok(/^Answered: .*Network/m.test(hostile), 'a network that answered is counted without being shown');
console.log('  ok - a payload full of private fields still gives a clean report');

var empty = report({});
assert.ok(/\| Model \| — \|/.test(empty) && /^Answered: —$/m.test(empty) && /^Native Tv Home \|/m.test(empty) === false);
assert.ok(empty.indexOf('| Native Tv Home | — |') !== -1, 'Home is not called supported when its status could not be read');
console.log('  ok - with nothing answered the report is dashes, and says nothing it does not know');

var notSupported = report({ stats: {}, caps: caps, home: { ok: true, supported: { ok: false }, nativeHome: {} } });
assert.ok(notSupported.indexOf('| Native Tv Home | not supported on this webOS |') !== -1);
var halted = report({ stats: {}, caps: caps, home: { ok: true, supported: { ok: true }, nativeHome: { halted: true } } });
assert.ok(halted.indexOf('| Native Tv Home | stopped after repeated failures |') !== -1);
var off = report({ stats: {}, caps: { allowControl: false, allowPower: false, fromHomebrewChannel: true }, home: home });
assert.ok(off.indexOf('| Controls | off in the config |') !== -1 && off.indexOf('| Installed from | Homebrew Channel |') !== -1);
console.log('  ok - Home, controls and how it was installed are reported as they are');

var pending = [];
// The button: reads three routes and fills the box, even where some do not answer.
var page = load({ '/api/stats': JSON.parse(fs.readFileSync(path.join(fixtureDir, fixtures[0]), 'utf8')), '/api/caps': caps });
pending.push(vm.runInContext('createTvReport()', page.context).then(function () {
  assert.strictEqual(page.el('report-text').hidden, false);
  assert.strictEqual(page.el('report-copy').hidden, false);
  assert.ok(page.el('report-text').value.indexOf('**Native Tv TV report**') === 0);
  assert.ok(page.el('report-text').value.indexOf('| Native Tv Home | — |') !== -1, 'a route that did not answer shows as unknown');
  console.log('  ok - Create fills the box even when a route does not answer');
}));
pending.push(vm.runInContext('copyTvReport()', page.context).then(function () {
  assert.strictEqual(page.el('report-msg').textContent, 'Copied');
  console.log('  ok - Copy says so');
}));

Promise.all(pending).then(function () {
  console.log('ALL test-tv-report-ui.js assertions passed!\n');
}, function (error) {
  console.error(error);
  process.exit(1);
});
