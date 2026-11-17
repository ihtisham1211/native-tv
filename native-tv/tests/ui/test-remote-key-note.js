/**
 * test/test-remote-key-note.js - A remote-key change the TV took in part
 *
 * Runs chooseRemoteKey() from ui/apps.js against the answers /api/remotekeys gives: a plain
 * success, a failure, and a success with a note, which the TV sends where it will not stop
 * Alexa from launching and leaves the button as LG has it.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-remote-key-note.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'apps.js'), 'utf8');
var alerts = [], loads = 0, posted = [], answer = { ok: true };
var ctx = {
  document: {
    addEventListener: function () {},
    querySelectorAll: function () { return []; }
  },
  window: { addEventListener: function () {} },
  alert: function (text) { alerts.push(text); },
  api: function (route) { return route; },
  t: function (key, english, vars) {
    return english.replace(/\{(\w+)\}/g, function (all, name) { return vars[name]; });
  },
  q: function () { return null; },
  fetch: function (url, init) {
    posted.push(JSON.parse(init.body));
    return Promise.resolve({ json: function () { return Promise.resolve(answer); } });
  }
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.chooseRemoteKey = chooseRemoteKey; this.openKey = function (key) { rkOpen = key; };', ctx);

// apps.js declares its own loadApps; the draw is counted instead of run.
ctx.loadApps = function () { loads++; return Promise.resolve(); };

var button = { dataset: { value: 'nothing' }, classList: { add: function () {} } };

function choose(next) {
  ctx.openKey('alexa');
  return ctx.chooseRemoteKey(button).then(next);
}

choose(function () {
  assert.deepStrictEqual(posted[0], { key: 'alexa', action: 'nothing' });
  assert.deepStrictEqual(alerts, [], 'a plain success says nothing');
  assert.strictEqual(loads, 1, 'and redraws the keys');
  answer = { ok: false, error: 'The TV did not take the change' };
  return choose(function () {
    assert.strictEqual(alerts.length, 1);
    assert.ok(/Could not change it: The TV did not take the change/.test(alerts[0]), alerts[0]);
    answer = { ok: true, note: 'This TV will not stop Alexa from launching, so the Alexa button stays as LG has it' };
    return choose(function () {
      assert.strictEqual(alerts.length, 2, 'a note is shown once');
      assert.strictEqual(alerts[1], answer.note, 'as the server said it');
      assert.strictEqual(loads, 3, 'and the keys are redrawn all the same');
      console.log('test-remote-key-note.js passed');
    });
  });
}).catch(function (error) {
  console.error(error);
  process.exit(1);
});
