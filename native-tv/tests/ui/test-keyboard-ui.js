/**
 * The typing box in the Remote sheet (web/ui/keyboard.js): what it sends, when it clears
 * the box, and what it shows when the TV does not take the text.
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-keyboard-ui.js ...');

function load(replies) {
  var elements = {};
  var fetches = [];
  function el(id) {
    if (!elements[id]) elements[id] = { id: id, value: '', textContent: '', hidden: true };
    return elements[id];
  }
  var context = {
    console: console,
    Promise: Promise,
    JSON: JSON,
    q: el,
    api: function (p) { return p; },
    t: function (key, english) { return english; },
    fetch: function (url, options) {
      fetches.push({ url: url, options: options });
      var reply = replies.length ? replies.shift() : { ok: true };
      if (reply instanceof Error) return Promise.reject(reply);
      return Promise.resolve({ json: function () { return Promise.resolve(reply); } });
    }
  };
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'keyboard.js'), 'utf8'), context);
  return { context: context, el: el, fetches: fetches };
}

function submit(page) {
  var prevented = { done: false };
  page.context.__event = { preventDefault: function () { prevented.done = true; } };
  var finished = vm.runInContext('sendKeyboardText(__event)', page.context);
  return finished.then(function () { return prevented.done; });
}

var pending = [];
function test(name, body) {
  pending.push(Promise.resolve().then(body).then(function () { console.log('  ok - ' + name); }));
}

test('the text is sent to the keyboard route as typed, and the box is cleared once the TV takes it', function () {
  var page = load([{ ok: true }]);
  page.el('kbd-input').value = 'stranger things';
  return submit(page).then(function (prevented) {
    assert.strictEqual(prevented, true, 'the form does not reload the page');
    assert.strictEqual(page.fetches.length, 1);
    assert.strictEqual(page.fetches[0].url, '/api/keyboard');
    assert.strictEqual(page.fetches[0].options.method, 'POST');
    assert.strictEqual(page.fetches[0].options.headers['Content-Type'], 'application/json');
    assert.deepStrictEqual(JSON.parse(page.fetches[0].options.body), { action: 'text', text: 'stranger things' });
    assert.strictEqual(page.el('kbd-input').value, '');
    assert.strictEqual(page.el('kbd-status').hidden, true);
  });
});

test('text with spaces at the ends and accents is sent as it is', function () {
  var page = load([{ ok: true }]);
  page.el('kbd-input').value = ' café 😀 ';
  return submit(page).then(function () {
    assert.strictEqual(JSON.parse(page.fetches[0].options.body).text, ' café 😀 ');
  });
});

test('when the TV does not take it the text stays and the reason is shown', function () {
  var page = load([{ ok: false, error: 'No text field is open' }]);
  page.el('kbd-input').value = 'netflix';
  return submit(page).then(function () {
    assert.strictEqual(page.el('kbd-input').value, 'netflix');
    assert.strictEqual(page.el('kbd-status').textContent, 'No text field is open');
    assert.strictEqual(page.el('kbd-status').hidden, false);
  });
});

test('a refusal without a reason, and a failed request, are both shown', function () {
  var page = load([{ ok: false }, new Error('network down')]);
  page.el('kbd-input').value = 'a';
  return submit(page).then(function () {
    assert.strictEqual(page.el('kbd-status').textContent, 'Action failed');
    return submit(page);
  }).then(function () {
    assert.strictEqual(page.el('kbd-status').textContent, 'network down');
    assert.strictEqual(page.el('kbd-input').value, 'a');
  });
});

test('a message is cleared by the next send that works', function () {
  var page = load([{ ok: false, error: 'No text field is open' }, { ok: true }]);
  page.el('kbd-input').value = 'a';
  return submit(page).then(function () {
    assert.strictEqual(page.el('kbd-status').hidden, false);
    return submit(page);
  }).then(function () {
    assert.strictEqual(page.el('kbd-status').hidden, true);
    assert.strictEqual(page.el('kbd-status').textContent, '');
  });
});

test('an empty box sends nothing, but still keeps the page from reloading', function () {
  var page = load([]);
  return submit(page).then(function (prevented) {
    assert.strictEqual(prevented, true);
    assert.strictEqual(page.fetches.length, 0);
  });
});

test('delete and enter send their own actions', function () {
  var page = load([{ ok: true }, { ok: true }]);
  vm.runInContext("keyboardKey('delete'); keyboardKey('enter')", page.context);
  return new Promise(function (resolve) { setTimeout(resolve, 20); }).then(function () {
    assert.deepStrictEqual(page.fetches.map(function (f) { return JSON.parse(f.options.body); }),
      [{ action: 'delete' }, { action: 'enter' }]);
  });
});

Promise.all(pending).then(function () {
  console.log('ALL test-keyboard-ui.js assertions passed!\n');
}, function (error) {
  console.error(error);
  process.exit(1);
});
