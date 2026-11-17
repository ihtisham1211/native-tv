/**
 * test/test-ntp-row.js - The Server tab's network time status line
 *
 * Runs renderNtp() from ui/server.js against the status shapes the server sends, including a
 * clock the TV did not take the time into (notTakenS), shown as how far out it stayed.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-ntp-row.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'server.js'), 'utf8');
var classes = {};
var els = {
  'ntp-toggle': {},
  'ntp-form': { hidden: true },
  'ntp-status': {
    hidden: true, textContent: '',
    classList: {
      add: function (name) { classes[name] = true; },
      remove: function (name) { delete classes[name]; }
    }
  },
  'ntp-server': { value: '', dataset: {} },
  'ntp-save': {}
};
var ctx = {
  q: function (id) { return els[id]; },
  document: { activeElement: null },
  updateToggle: function () {},
  hostPort: function (host, port) { return host + ':' + port; },
  cfgAge: function (ms) { return Math.round(ms / 1000) + ' s ago'; },
  t: function (key, english, vars) {
    return english.replace(/\{(\w+)\}/g, function (all, name) { return vars[name]; });
  }
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.renderNtp = renderNtp;', ctx);

var cfg = { enabled: true, server: 'time.lan', port: 123 };
function status(n) {
  classes = {};
  ctx.renderNtp(n, cfg, true);
  return els['ntp-status'].textContent;
}

assert.ok(/Set the clock 5 s ago; last checked 2 s ago/.test(
  status({ error: null, notTakenS: null, setAgoS: 5, answeredAgoS: 2 })), 'a set clock is shown as set');
assert.ok(!classes['row-err'], 'and is not an error');

var text = status({ error: 'the time service did not answer', notTakenS: null, setAgoS: null, answeredAgoS: 2 });
assert.ok(/has not answered/.test(text) && classes['row-err'], 'a failure shows its reason');

text = status({ error: 'the TV accepted the time but its clock stayed +6940.000 s out', notTakenS: 6940, setAgoS: null, answeredAgoS: 2 });
assert.ok(/not taking the time from the server, so its clock is still 1 h 56 min out\. Native Tv tries again every hour/.test(text), text);
assert.ok(classes['row-err'], 'a clock that stayed out is an error');
assert.ok(!/has not answered/.test(text), 'and is not shown as a server that did not answer');

assert.ok(/still 40 s out/.test(status({ error: 'x', notTakenS: -40, setAgoS: null, answeredAgoS: 1 })),
  'seconds, and a clock ahead is shown without a sign');
assert.ok(/still 12 min out/.test(status({ error: 'x', notTakenS: 720, setAgoS: null, answeredAgoS: 1 })),
  'minutes');
assert.ok(/still 2 h 0 min out/.test(status({ error: 'x', notTakenS: 7200, setAgoS: null, answeredAgoS: 1 })),
  'whole hours');

text = status({ error: null, setAgoS: 5, answeredAgoS: 2 });
assert.ok(/Set the clock/.test(text), 'a server from before the field existed shows as usual');

console.log('test-ntp-row.js passed');
