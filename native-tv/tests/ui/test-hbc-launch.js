/**
 * test-hbc-launch.js - The Homebrew Channel app's launch page sends a TV that has not done
 * first-run setup to the setup pages, not to the dashboard.
 *
 * Runs the page's own script (hbc/app/index.html) against stand-ins for the document, the
 * address, XMLHttpRequest and the Homebrew Channel's exec bridge. The page has no window.PalmSystem
 * here, as a page that does not get it from the TV would not: what it opens depends on what
 * /api/caps says.
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
function launch(answers, done) {
  var asked = 0, installs = 0, opened = null;
  var element = function () { return { textContent: '', className: '', addEventListener: function () {} }; };
  var sandbox = {
    document: { getElementById: element, addEventListener: function () {} },
    location: { pathname: '/media/app/index.html', replace: function (url) { opened = url; } },
    window: {},
    Date: Date,
    JSON: JSON,
    decodeURIComponent: decodeURIComponent,
    parseInt: parseInt,
    String: String,
    Math: Math,
    clearTimeout: function () {},
    setTimeout: function (fn, ms) { if (ms <= 1000) setImmediate(fn); return 0; },
    PalmServiceBridge: function () {
      this.call = function () {
        installs++;
        var bridge = this;
        setImmediate(function () {
          bridge.onservicecallback(JSON.stringify({ returnValue: true, stdoutString: INSTALLED + '\n' }));
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
    if (opened || ++waited > 200) return done(opened, installs);
    setTimeout(poll, 5);
  })();
}
function caps(setupNeeded) { return { version: '0.124.0', fromHomebrewChannel: true, setupNeeded: setupNeeded }; }
var cases = [
  ['a running server that says setup is needed opens the setup pages',
    [caps(true)], '/assets/google-tv/pages/setup/index.html', 0],
  ['a running server whose setup is done opens the dashboard',
    [caps(false)], '/assets/google-tv/pages/dashboard.html?tab=system', 0],
  ['a server put in place just now that says setup is needed opens the setup pages',
    [null, null, null, null, caps(true)], '/assets/google-tv/pages/setup/index.html', 1],
  ['a server put in place just now whose setup is done opens the dashboard',
    [null, null, null, null, caps(false)], '/assets/google-tv/pages/dashboard.html?tab=system', 1]
];
(function next(i) {
  if (i === cases.length) return console.log('ALL test-hbc-launch.js assertions passed!\n');
  launch(cases[i][1], function (opened, installs) {
    assert.ok(opened, cases[i][0] + ': the page opened nothing');
    assert.ok(opened.indexOf('http://localhost:8080' + cases[i][2]) === 0,
              cases[i][0] + ': opened ' + opened);
    assert.strictEqual(installs, cases[i][3], cases[i][0] + ': installs');
    console.log('  ok - ' + cases[i][0]);
    next(i + 1);
  });
})(0);
