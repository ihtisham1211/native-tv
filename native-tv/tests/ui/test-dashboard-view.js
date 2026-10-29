/**
 * test/test-dashboard-view.js - Unit tests for TV dashboard view builders
 */

var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-dashboard-view.js ...');

var tvAssets = path.join(__dirname, '..', '..', 'assets', 'google-tv');
var script = fs.readFileSync(path.join(tvAssets, 'shared', 'common.js'), 'utf8') + '\n;\n' +
             fs.readFileSync(path.join(tvAssets, 'pages', 'dashboard', 'dashboard.js'), 'utf8');

var dummyEl = {
  style: {},
  classList: { add: function () {}, remove: function () {} },
  children: [],
  scrollLeft: 0,
  getBoundingClientRect: function () { return { left: 0, width: 0 }; }
};
for (var i = 0; i < 10; i++) {
  dummyEl.children.push({
    style: {},
    classList: { add: function () {}, remove: function () {} },
    getBoundingClientRect: function () { return { left: 0, width: 0 }; }
  });
}

function QuietXHR() {}
QuietXHR.prototype.open = function () {};
QuietXHR.prototype.setRequestHeader = function () {};
QuietXHR.prototype.send = function () {};
QuietXHR.prototype.abort = function () { this.aborted = true; if (this.onabort) this.onabort(); };

var sandbox = {
  XMLHttpRequest: QuietXHR,
  assert: assert,
  t: function (k, d) { return d; },
  fetch: function () {
    var p = { then: function () { return p; }, catch: function () { return p; } };
    return p;
  },
  document: {
    getElementById: function () { return dummyEl; },
    querySelectorAll: function () { return []; },
    addEventListener: function () {}
  },
  window: { location: { search: '' }, addEventListener: function () {} },
  location: { search: '' },
  setInterval: function () {},
  setTimeout: function () {},
  clearInterval: function () {},
  clearTimeout: function () {}
};

// In Node 0.10, runInNewContext copies sandbox properties in once before execution
// and copies globals out once after execution. Assigning sandbox.rows or reading
// sandbox.sel from outside after execution does not observe the context's internal
// variables. Running the paging scenario inside the context ensures it works across
// all node versions (0.10 through 24).
var testPagingSnippet = [
  'var __testPagingResults = (function () {',
  '  rows = [',
  '    { label: "Row 0" },',
  '    { label: "Row 1" },',
  '    { label: "Row 2" },',
  '    { label: "Header A", head: true },',
  '    { label: "Row 4" },',
  '    { label: "Row 5" },',
  '    { label: "Row 6" },',
  '    { label: "Row 7" },',
  '    { label: "Row 8" },',
  '    { label: "Row 9" },',
  '    { label: "Row 10" }',
  '  ];',
  '  sel = 0;',
  '  var res = [];',
  '  page(1); res.push(sel);',
  '  page(1); res.push(sel);',
  '  page(1); res.push(sel);',
  '  page(-1); res.push(sel);',
  '  page(-1); res.push(sel);',
  '  page(-1); res.push(sel);',
  '  return res;',
  '})();'
].join('\n');

// runInNewContext rather than createContext and runInContext: node 0.10's
// createContext returns a copy instead of the sandbox itself.
vm.runInNewContext(script + '\n;' + testPagingSnippet, sandbox);

(function testRequestLifecycle() {
  // Keep the scenario in one context for Node 0.10's copied globals.
  var checks = function () {
    cancelTabRequest();
    render = function () {};
    buildRows = function (section, value) { return [{ label: value.label }]; };
    rows = []; data = {};
    fetchTab(true);
    var first = tabRequest.xhr;
    assert.strictEqual(first.timeout, 25000);
    fetchTab(true); fetchTab(true);
    assert.strictEqual(tabRequest.xhr, first, 'pending reads do not overlap');
    first.status = 200; first.responseText = '{"label":"old"}'; first.onload();
    assert.strictEqual(data.system, undefined, 'a pre-control snapshot is not rendered');
    var followup = tabRequest.xhr;
    assert.notStrictEqual(followup, first, 'one follow-up starts');
    followup.status = 200; followup.responseText = '{"label":"fresh"}'; followup.onload();
    assert.strictEqual(tabRequest, null);
    assert.strictEqual(data.system.label, 'fresh');

    fetchTab(true);
    var oldTab = tabRequest.xhr;
    openTab(1); openTab(0);
    var returned = tabRequest.xhr;
    assert.strictEqual(oldTab.aborted, true, 'tab changes abort the old read');
    oldTab.status = 200; oldTab.responseText = '{"label":"stale"}'; oldTab.onload();
    assert.strictEqual(data.system.label, 'fresh', 'A-B-A navigation rejects the stale A response');
    assert.strictEqual(tabRequest.xhr, returned);
    returned.ontimeout();
    assert.strictEqual(tabRequest, null, 'timeouts release the pending lock');
    fetchTab(true);
    tabRequest.xhr.status = 200; tabRequest.xhr.responseText = 'invalid JSON'; tabRequest.xhr.onload();
    assert.strictEqual(tabRequest, null, 'invalid JSON releases the pending lock');
    document.hidden = true;
    fetchTab(true);
    assert.strictEqual(tabRequest, null, 'hidden pages do not fetch');
  };
  vm.runInNewContext(script + '\n;(' + checks.toString() + ')();', sandbox);
  console.log('  ✓ tab reads coalesce, reject stale replies and recover from timeout/invalid JSON');
})();

(function testTabSettle() {
  var checks = function () {
    document.hidden = false;
    var renders = 0;
    render = function () { renders++; };
    setTimeout = function () { return 1; };
    openTab(0);
    rows = [{ label: 'a' }, { label: 'b' }]; sel = 1; renders = 0;
    switchTab(1); switchTab(-1); settleTab();
    assert.strictEqual(renders, 0, 'landing back on the shown tab does not redraw it');
    assert.strictEqual(sel, 1, 'and keeps its selection');
    assert.ok(tabRequest, 'and resumes its refresh');
    switchTab(1); settleTab();
    assert.strictEqual(renders, 1, 'a new tab is opened once it settles');
  };
  vm.runInNewContext(script + '\n;(' + checks.toString() + ')();', sandbox);
  console.log('  ✓ moving along the tabs opens only a tab that is not already shown');
})();

// 1. sysRows omits eMMC health when emmcWear is false or health is unknown
(function testEmmcHealthVisibility() {
  var rowsNoWear = sandbox.sysRows({
    ok: true,
    emmc: { health: 'unknown' },
    capabilities: { emmcWear: false }
  });
  assert.strictEqual(
    rowsNoWear.some(function (r) { return r.label === 'eMMC health'; }),
    false,
    'eMMC health omitted when emmcWear is false'
  );

  var rowsUnknown = sandbox.sysRows({
    ok: true,
    emmc: { health: 'unknown' }
  });
  assert.strictEqual(
    rowsUnknown.some(function (r) { return r.label === 'eMMC health'; }),
    false,
    'eMMC health omitted when health is unknown'
  );

  var rowsHealthy = sandbox.sysRows({
    ok: true,
    emmc: { health: '>90% (Healthy)' },
    capabilities: { emmcWear: true }
  });
  assert.strictEqual(
    rowsHealthy.some(function (r) { return r.label === 'eMMC health'; }),
    true,
    'eMMC health included when wear is available and healthy'
  );

  console.log('  ✓ sysRows omits eMMC health when unavailable or unknown');
})();

// 2. OLED Care is dropped on a TV without an OLED panel, as Game and Screen
// Saver are where the TV has neither
(function testOledTabOptional() {
  var oledTab = sandbox.TABS.filter(function (s) { return s.key === 'oled'; })[0];
  assert.ok(oledTab, 'the OLED Care tab exists');
  assert.strictEqual(oledTab.optional, true, 'and is dropped when /api/oledcare says it is unavailable');
  assert.deepEqual(sandbox.oledRows({ ok: true, isOled: false }), [], 'no rows on an LCD TV');
  console.log('  ✓ the OLED Care tab is optional, with no rows on a TV without an OLED panel');
})();

// 3. optionRows groups Power & standby and Timers with separate headers
(function testOptionRowsGrouping() {
  var rows = sandbox.optionRows({
    ok: true,
    quickBoot: true,
    wakeOnLan: true,
    powerTimers: {
      on: { enabled: true, time: '07:00', days: [1, 2, 3, 4, 5] },
      off: { enabled: false, time: '23:00', days: [] }
    }
  });
  var headers = rows.filter(function (r) { return r.head; }).map(function (r) { return r.label; });
  assert.ok(headers.indexOf('Power & standby') !== -1, 'has Power & standby section');
  assert.ok(headers.indexOf('Timers') !== -1, 'has separate Timers section');
  console.log('  ✓ optionRows groups Power & standby and Timers with distinct headers');
})();

// 4. page() jumps through rows skipping headers and clamps to bounds
(function testPagePaging() {
  assert.strictEqual(typeof sandbox.page, 'function', 'page function exists');
  var res = sandbox.__testPagingResults;
  assert.ok(res && res.length === 6, 'paging results recorded');
  // Page down by 6 selectable rows: 0 -> 1 -> 2 -> (skip 3) -> 4 -> 5 -> 6 -> 7
  assert.strictEqual(res[0], 7, 'pages down 6 selectable rows skipping header');
  // Page down again: from 7, only 8, 9, 10 remain (3 rows) -> clamps to last row 10
  assert.strictEqual(res[1], 10, 'clamps to last row at bottom');
  // Page down again when already at bottom: stays at 10
  assert.strictEqual(res[2], 10, 'stays at last row when already at bottom');
  // Page up by 6 selectable rows: from 10 -> 9 -> 8 -> 7 -> 6 -> 5 -> 4
  assert.strictEqual(res[3], 4, 'pages up 6 selectable rows');
  // Page up again: from 4 -> (skip 3) -> 2 -> 1 -> 0 -> clamps to 0
  assert.strictEqual(res[4], 0, 'clamps to first row at top');
  // Page up again: stays at 0
  assert.strictEqual(res[5], 0, 'stays at first row when already at top');
  console.log('  ✓ page() jumps 6 rows, skips headers, and respects list bounds');
})();

(function testSelfUpdaterRemovalAndFirmwareControl() {
  assert.strictEqual(sandbox.TABS.some(function (tab) { return tab.key === 'update'; }), false,
    'the nativeTv self-updater tab is removed');
  var rows = sandbox.privacyRows({ ok: true, tvUpdates: { blocked: true } });
  var firmware = rows.filter(function (r) { return r.label === 'Block TV software updates'; })[0];
  assert.ok(firmware, 'the LG firmware update control remains in Privacy');
  assert.strictEqual(firmware.act, 'tvupd');
  assert.strictEqual(firmware.on, true);
  console.log('  ✓ self-updater removed while LG firmware control remains');
})();

// 6. Promise and fetch polyfills activate when missing (webOS 2 WebKit #746)
(function testPromiseAndFetchPolyfill() {
  var xhrs = [];
  function FakeXHR() {
    this.headers = {};
    this.status = 200;
    this.responseText = '{"ok":true,"test":123}';
    xhrs.push(this);
  }
  FakeXHR.prototype.open = function (m, u) { this.method = m; this.url = u; };
  FakeXHR.prototype.setRequestHeader = function (k, v) { this.headers[k] = v; };
  FakeXHR.prototype.send = function (b) {
    this.body = b;
    if (this.onload) this.onload();
  };

  var webos2Sandbox = {
    t: function (k, d) { return d; },
    XMLHttpRequest: FakeXHR,
    TypeError: TypeError,
    Promise: undefined,
    fetch: undefined,
    document: {
      getElementById: function () { return dummyEl; },
      querySelectorAll: function () { return []; },
      addEventListener: function () {}
    },
    location: { search: '' },
    setInterval: function () {},
    setTimeout: setTimeout,
    clearTimeout: clearTimeout,
    addEventListener: function () {}
  };
  webos2Sandbox.window = webos2Sandbox;

  vm.runInNewContext(script, webos2Sandbox);
  assert.strictEqual(typeof webos2Sandbox.window.Promise, 'function', 'polyfilled window.Promise');
  assert.strictEqual(typeof webos2Sandbox.window.fetch, 'function', 'polyfilled window.fetch');

  // Verify Promise chaining and resolution
  var pResolved = false;
  var fetchResolved = false;
  var safetyTimer = setTimeout(function () {
    assert.strictEqual(pResolved, true, 'Promise chaining resolved before timeout');
    assert.strictEqual(fetchResolved, true, 'Fetch over XHR resolved before timeout');
  }, 3000);

  function checkDone() {
    if (pResolved && fetchResolved) {
      clearTimeout(safetyTimer);
      console.log('  ✓ Promise and fetch polyfills support webOS 2 WebKit without native ES6');
      console.log('ALL test-dashboard-view.js assertions passed!\n');
    }
  }

  webos2Sandbox.window.Promise.resolve('webos2')
    .then(function (val) {
      return webos2Sandbox.window.Promise.resolve(val + '-ok');
    })
    .then(function (val) {
      assert.strictEqual(val, 'webos2-ok');
      pResolved = true;
      checkDone();
    });

  // Verify fetch over FakeXHR
  webos2Sandbox.window.fetch('/api/test')
    .then(function (res) {
      assert.strictEqual(res.ok, true);
      assert.strictEqual(res.status, 200);
      return res.json();
    })
    .then(function (data) {
      assert.deepEqual(data, { ok: true, test: 123 });
      fetchResolved = true;
      checkDone();
    });
})();
