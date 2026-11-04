/**
 * The Home Assistant configuration card on the MQTT tab (web/ui/mqtt.js): it fills in the
 * saved topic prefix and device id and the TV's MAC address, and its button copies the text.
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-ha-config-ui.js ...');

var source = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'mqtt.js'), 'utf8');
var start = source.indexOf('function haConfigText');
var end = source.indexOf('async function loadSettings');
assert.ok(start >= 0 && end > start, 'mqtt.js still defines the generator before loadSettings');

function load(options) {
  var elements = {};
  var copied = [];
  function el(id) { return elements[id] || (elements[id] = { id: id, value: '', textContent: '', selected: false, select: function () { this.selected = true; } }); }
  var context = {
    console: console,
    Promise: Promise,
    JSON: JSON,
    q: el,
    api: function (p) { return p; },
    t: function (key, english) { return english; },
    fetch: function () {
      return options.stats instanceof Error ? Promise.reject(options.stats)
        : Promise.resolve({ json: function () { return Promise.resolve(options.stats); } });
    },
    copyToClipboard: function (text) {
      copied.push(text);
      return options.copy === false ? Promise.reject(new Error('refused')) : Promise.resolve();
    }
  };
  vm.createContext(context);
  vm.runInContext(source.slice(start, end), context);
  return { context: context, el: el, copied: copied };
}

var settings = { device: { id: 'den_tv', name: 'Den TV' }, mqtt: { topicPrefix: 'home/tv' } };
var pending = [];
function test(name, body) {
  pending.push(Promise.resolve().then(body).then(function () { console.log('  ok - ' + name); }));
}

test('the card shows the saved values and the TV\'s own MAC address and address', function () {
  var page = load({ stats: { mac: '00:11:22:aa:bb:cc', address: '192.168.1.50' } });
  page.context.__s = settings;
  return vm.runInContext('renderHaConfig(__s)', page.context).then(function () {
    var text = page.el('ha-config').value;
    assert.ok(text.indexOf('# Native Tv: Den TV (192.168.1.50)') === 0, text.split('\n')[0]);
    assert.ok(text.indexOf('mac: "00:11:22:AA:BB:CC"') !== -1);
    assert.ok(text.indexOf('"home/tv/command/volumeStep"') !== -1);
    assert.ok(text.indexOf('number.den_tv_volume') !== -1 && text.indexOf('lg_tv') === -1);
  });
});

test('without stats the configuration still renders, with a placeholder MAC address and a note', function () {
  var page = load({ stats: new Error('offline') });
  page.context.__s = settings;
  return vm.runInContext('renderHaConfig(__s)', page.context).then(function () {
    var text = page.el('ha-config').value;
    assert.ok(text.indexOf('mac: "AA:BB:CC:DD:EE:FF"') !== -1);
    assert.ok(text.indexOf('MAC address was not found') !== -1);
  });
});

test('a device with no name is named for an LG webOS TV', function () {
  var page = load({ stats: {} });
  page.context.__s = { device: { id: 'lg_tv', name: '' }, mqtt: { topicPrefix: 'lgtv' } };
  return vm.runInContext('renderHaConfig(__s)', page.context).then(function () {
    assert.ok(page.el('ha-config').value.indexOf('friendly_name: "LG webOS TV"') !== -1);
  });
});

test('Copy puts the shown text on the clipboard and says so', function () {
  var page = load({ stats: {} });
  page.el('ha-config').value = 'template:\n';
  return vm.runInContext('copyHaConfig()', page.context).then(function () {
    assert.deepStrictEqual(page.copied, ['template:\n']);
    assert.strictEqual(page.el('ha-copy-msg').textContent, 'Copied');
    assert.strictEqual(page.el('ha-config').selected, false);
  });
});

test('when the browser refuses, the text is selected for a manual copy', function () {
  var page = load({ stats: {}, copy: false });
  page.el('ha-config').value = 'template:\n';
  return vm.runInContext('copyHaConfig()', page.context).then(function () {
    assert.strictEqual(page.el('ha-config').selected, true);
    assert.strictEqual(page.el('ha-copy-msg').textContent, 'Select the text and copy it');
  });
});

Promise.all(pending).then(function () {
  console.log('ALL test-ha-config-ui.js assertions passed!\n');
}, function (error) {
  console.error(error);
  process.exit(1);
});
