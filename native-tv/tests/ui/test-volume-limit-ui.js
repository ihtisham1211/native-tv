/**
 * The volume ceiling card on the Advanced tab (web/ui/volumelimit.js): what it shows of the
 * settings, what it sends when a control changes, and what it does when the server refuses.
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-volume-limit-ui.js ...');

var uiDir = path.join(__dirname, '..', '..', 'web', 'ui');

function load(writable, settings) {
  var elements = {};
  var fetches = [];
  var toggles = [];
  var errors = [];
  var replies = [];
  function el(id) {
    if (!elements[id]) elements[id] = { id: id, value: '', innerHTML: '', textContent: '', hidden: false, disabled: false };
    return elements[id];
  }
  var context = {
    console: console,
    Promise: Promise,
    JSON: JSON,
    q: el,
    api: function (p) { return p; },
    esc: function (s) { return String(s); },
    // The first part of the key and English, as the script's own strings fall back to.
    t: function (key, english) { return english; },
    updateToggle: function (id, options) { toggles.push({ id: id, options: options }); },
    showErr: function (message) { errors.push(message); },
    clearErr: function () {},
    fetch: function (url, options) {
      fetches.push({ url: url, options: options });
      var body = replies.length ? replies.shift() : { ok: true, writable: true, settings: settings };
      return Promise.resolve({ json: function () { return Promise.resolve(body); } });
    }
  };
  vm.createContext(context);
  // stOptions is the Screen time script's, which the card reuses.
  vm.runInContext(fs.readFileSync(path.join(uiDir, 'screentime.js'), 'utf8'), context);
  vm.runInContext(fs.readFileSync(path.join(uiDir, 'volumelimit.js'), 'utf8'), context);
  return {
    context: context, el: el, fetches: fetches, toggles: toggles, errors: errors, replies: replies,
    render: function () {
      context.__d = { ok: true, writable: writable, settings: settings };
      vm.runInContext('renderVolumeLimit(__d)', context);
    },
    run: function (code) { return vm.runInContext(code, context); }
  };
}

var defaults = { enabled: false, maxVolume: 100, nightMaxVolume: 100, nightStart: '', nightEnd: '', startupVolume: -1 };
var tuned = { enabled: true, maxVolume: 35, nightMaxVolume: 12, nightStart: '22:00', nightEnd: '07:00', startupVolume: 20 };

function options(select) {
  return (select.innerHTML.match(/value="(-?\d+)"/g) || []).map(function (m) { return Number(m.slice(7, -1)); });
}

var pending = [];
function test(name, body) {
  pending.push(Promise.resolve().then(body).then(function () {
    console.log('  ok - ' + name);
  }));
}

test('the defaults show a switched-off ceiling with no limits chosen', function () {
  var page = load(true, defaults);
  page.render();
  var toggle = page.toggles[0];
  assert.strictEqual(toggle.id, 'vl-enabled');
  assert.strictEqual(toggle.options.on, false);
  assert.strictEqual(toggle.options.cls, 'idle');
  assert.strictEqual(toggle.options.disabled, false);
  assert.strictEqual(page.el('vl-max').value, '100');
  assert.strictEqual(page.el('vl-night-max').value, '100');
  assert.strictEqual(page.el('vl-startup').value, '-1');
  assert.strictEqual(page.el('vl-night-from').value, '');
  assert.strictEqual(page.el('vl-note').hidden, true);
});

test('the choices run from 5 to 100, with silent and start-up leave-alone where they apply', function () {
  var page = load(true, defaults);
  page.render();
  var levels = options(page.el('vl-max'));
  assert.strictEqual(levels[0], 5);
  assert.strictEqual(levels[levels.length - 1], 100);
  assert.strictEqual(levels.length, 20);
  assert.strictEqual(options(page.el('vl-night-max'))[0], 0, 'a night limit can be silent');
  assert.strictEqual(options(page.el('vl-startup'))[0], -1, 'a start-up volume can be left alone');
  assert.ok(options(page.el('vl-startup')).indexOf(0) !== -1);
  assert.ok(page.el('vl-max').innerHTML.indexOf('No limit') !== -1, '100 reads as no limit');
});

test('chosen settings are shown, and a value outside the list is kept', function () {
  var page = load(true, tuned);
  page.render();
  assert.strictEqual(page.toggles[0].options.on, true);
  assert.strictEqual(page.toggles[0].options.cls, 'good');
  assert.strictEqual(page.el('vl-max').value, '35');
  assert.strictEqual(page.el('vl-startup').value, '20');
  assert.strictEqual(page.el('vl-night-from').value, '22:00');
  assert.strictEqual(page.el('vl-night-to').value, '07:00');
  assert.strictEqual(page.el('vl-night-max').value, '12');
  assert.ok(options(page.el('vl-night-max')).indexOf(12) !== -1, 'a 12 set in the config stays in the list');
});

test('with controls off every control is locked and the page says why', function () {
  var page = load(false, tuned);
  page.render();
  assert.strictEqual(page.toggles[0].options.disabled, true);
  ['vl-max', 'vl-night-max', 'vl-startup', 'vl-night-from', 'vl-night-to'].forEach(function (id) {
    assert.strictEqual(page.el(id).disabled, true, id + ' is locked');
  });
  assert.strictEqual(page.el('vl-note').hidden, false);
  assert.ok(page.el('vl-note').textContent.length > 0);
});

test('the switch sends the opposite of what is shown, to the volume limit route', function () {
  var page = load(true, defaults);
  page.render();
  page.replies.push({ ok: true, writable: true, settings: Object.assign({}, defaults, { enabled: true }) });
  page.run('toggleVolumeLimit()');
  assert.strictEqual(page.fetches.length, 1);
  assert.strictEqual(page.fetches[0].url, '/api/volumelimit');
  assert.strictEqual(page.fetches[0].options.method, 'POST');
  assert.strictEqual(page.fetches[0].options.headers['Content-Type'], 'application/json');
  assert.deepStrictEqual(JSON.parse(page.fetches[0].options.body), { enabled: true });
  return new Promise(function (resolve) { setTimeout(resolve, 20); }).then(function () {
    assert.strictEqual(page.toggles[page.toggles.length - 1].options.on, true, 'the reply is shown');
  });
});

test('a change to one control sends only that setting', function () {
  var page = load(true, tuned);
  page.render();
  page.run("saveVolumeLimit({ maxVolume: Number('45') })");
  assert.deepStrictEqual(JSON.parse(page.fetches[0].options.body), { maxVolume: 45 });
});

test('a refusal is shown and the card reloads what the server holds', function () {
  var page = load(true, tuned);
  page.render();
  page.replies.push({ ok: false, error: 'invalid maxVolume' });
  page.run("saveVolumeLimit({ maxVolume: 999 })");
  return new Promise(function (resolve) { setTimeout(resolve, 30); }).then(function () {
    assert.deepStrictEqual(page.errors, ['invalid maxVolume']);
    assert.strictEqual(page.fetches.length, 2);
    assert.strictEqual(page.fetches[1].url, '/api/volumelimit');
    assert.strictEqual(page.fetches[1].options.cache, 'no-store');
  });
});

test('the switch does nothing before the settings have loaded', function () {
  var page = load(true, defaults);
  page.run('toggleVolumeLimit()');
  assert.strictEqual(page.fetches.length, 0);
});

Promise.all(pending).then(function () {
  console.log('ALL test-volume-limit-ui.js assertions passed!\n');
}, function (error) {
  console.error(error);
  process.exit(1);
});
