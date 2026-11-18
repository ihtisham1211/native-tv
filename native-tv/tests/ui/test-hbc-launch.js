/**
 * test-hbc-launch.js - The Homebrew Channel launcher opens the native app and
 * reports failures from the app manager and the setup bridge.
 *
 * Runs the page's own script (hbc/app/index.html) against stand-ins for the document, the
 * address, XMLHttpRequest and PalmServiceBridge.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-hbc-launch.js ...');

var html = fs.readFileSync(path.join(__dirname, '..', '..', '..', 'hbc', 'app', 'index.html'), 'utf8');
var begin = html.indexOf('<script>') + '<script>'.length;
var script = html.slice(begin, html.lastIndexOf('</script>')).replace('@VERSION@', '0.124.0');
assert.ok(begin > '<script>'.length && script.indexOf('PalmServiceBridge') !== -1, 'the launch script is in the page');

var INSTALLED = '{"ok":true,"action":"installed","version":"0.124.0","webOff":false}';

// answers: what each /api/caps ask gets, in order (null: nothing answers); the last one repeats.
function launch(answers, execReply, nativeReplies, done) {
  var asked = 0, installs = 0, attempts = 0, closed = false;
  var element = function () { return { textContent: '', className: '', addEventListener: function () {} }; };
  var elements = { status: element(), retry: element(), spinner: element() };
  var sandbox = {
    document: { getElementById: function (id) { return elements[id]; }, addEventListener: function () {} },
    location: { pathname: '/media/app/index.html' },
    window: { close: function () { closed = true; } },
    Date: Date,
    JSON: JSON,
    decodeURIComponent: decodeURIComponent,
    parseInt: parseInt,
    String: String,
    Math: Math,
    clearTimeout: function () {},
    setTimeout: function (fn, ms) { if (ms <= 1000) setImmediate(fn); return 0; },
    PalmServiceBridge: function () {
      this.call = function (uri, params) {
        var bridge = this;
        var reply;
        if (uri === 'luna://com.webos.applicationManager/launch') {
          assert.deepStrictEqual(JSON.parse(params), { id: 'com.tvweb.dashboard' });
          reply = { returnValue: nativeReplies[attempts++] };
        } else {
          assert.strictEqual(uri, 'luna://org.webosbrew.hbchannel.service/exec');
          installs++;
          reply = execReply;
        }
        setImmediate(function () {
          bridge.onservicecallback(JSON.stringify(reply));
        });
      };
    },
    XMLHttpRequest: function () {
      var xhr = this;
      this.open = function () {};
      this.send = function () {
        var answer = answers[Math.min(asked++, answers.length - 1)];
        setImmediate(function () {
          if (answer === null) return xhr.onerror();
          xhr.readyState = 4;
          xhr.status = 200;
          xhr.responseText = JSON.stringify(answer);
          xhr.onreadystatechange();
        });
      };
    }
  };
  vm.runInNewContext(script, sandbox);
  var waited = 0;
  (function poll() {
    if (closed || elements.retry.className === 'on' || ++waited > 200) {
      return done(closed, installs, attempts, elements.status.textContent, elements.retry.className);
    }
    setTimeout(poll, 5);
  })();
}
function caps(setupNeeded) { return { version: '0.124.0', fromHomebrewChannel: true, setupNeeded: setupNeeded }; }
var cases = [
  ['a running server opens the native app for first-run setup', [caps(true)], [true], 0, ''],
  ['a running server opens the native app after setup', [caps(false)], [true], 0, ''],
  ['a fresh installation opens the native app', [null, null, null, null, caps(true)], [true], 1, ''],
  ['an unavailable native app is registered by setup and retried', [caps(false)], [false, true], 1, ''],
  ['a refused launch is reported after one repair', [caps(false)], [false, false], 1,
    'Could not open the Native Tv app.']
];
var refusalCases = [
  [{ returnValue: false, errorText: ' Denied method call\nfor category / ', errorCode: -1 },
    'Could not start setup through Homebrew Channel. Denied method call for category /'],
  [{ returnValue: false, errorText: ' ', errorCode: -1 },
    'Could not start setup through Homebrew Channel. Error -1'],
  [{ returnValue: false }, 'Could not start setup through Homebrew Channel.'],
  [null, 'Setup failed.']
];

function nextRefusal(i) {
  if (i === refusalCases.length) return console.log('ALL test-hbc-launch.js assertions passed!\n');
  launch([null], refusalCases[i][0], [], function (closed, installs, attempts, status, retryClass) {
    assert.strictEqual(closed, false);
    assert.strictEqual(installs, 1);
    assert.strictEqual(attempts, 0);
    assert.strictEqual(status, refusalCases[i][1]);
    assert.strictEqual(retryClass, 'on');
    console.log('  ok - ' + status);
    nextRefusal(i + 1);
  });
}

(function next(i) {
  if (i === cases.length) return nextRefusal(0);
  var example = cases[i];
  launch(example[1], { returnValue: true, stdoutString: INSTALLED + '\n' }, example[2],
    function (closed, installs, attempts, status, retryClass) {
    assert.strictEqual(closed, !example[4], example[0]);
    assert.strictEqual(installs, example[3], example[0] + ': installs');
    assert.strictEqual(attempts, example[2].length, example[0] + ': launch attempts');
    assert.strictEqual(retryClass, example[4] ? 'on' : '');
    if (example[4]) assert.strictEqual(status, example[4]);
    console.log('  ok - ' + example[0]);
    next(i + 1);
  });
})(0);
