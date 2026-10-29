/**
 * test-dashboard-nav.js - The phone dashboard's sections, tabs and theme: the markup and
 * tabs.js agree, old ?tab= links still land somewhere, and the Android bridge and the
 * theme attribute behave as the app and the stylesheet expect.
 */

var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-dashboard-nav.js ...');

var web = path.join(__dirname, '..', '..', 'web');
var html = fs.readFileSync(path.join(web, 'ui.html'), 'utf8');
var tabsJs = fs.readFileSync(path.join(web, 'ui', 'tabs.js'), 'utf8');
var themeJs = fs.readFileSync(path.join(web, 'ui', 'theme.js'), 'utf8');

function table(name) {
  var match = new RegExp('const ' + name + ' = \\{([\\s\\S]*?)\\};').exec(tabsJs);
  assert(match, name + ' is declared in tabs.js');
  var out = {};
  match[1].replace(/(\w+):\s*'([^']+)'/g, function (whole, key, value) { out[key] = value; });
  return out;
}

var TABS = table('TABS');
var SECTION_OF = table('SECTION_OF');
var panelIds = {};
Object.keys(TABS).forEach(function (key) { panelIds[TABS[key]] = true; });

(function testPanelsAndTabs() {
  assert.strictEqual(Object.keys(panelIds).length, 13, 'the 13 panels are all reachable');
  Object.keys(panelIds).forEach(function (id) {
    assert(new RegExp('<section[^>]*id="' + id + '"[^>]*role="tabpanel"').test(html), id + ' is a tab panel in ui.html');
  });
  assert.deepStrictEqual(Object.keys(SECTION_OF).sort(), Object.keys(TABS).sort(), 'every tab sits in a section');

  var sections = {};
  html.replace(/class="nav-item" data-dest="(\w+)"/g, function (whole, dest) { sections[dest] = true; });
  assert.deepStrictEqual(Object.keys(sections).sort(), ['apps', 'display', 'overview', 'privacy', 'settings'], 'five sections in the navigation');
  Object.keys(SECTION_OF).forEach(function (key) {
    assert(sections[SECTION_OF[key]], key + ' names a section that is in the navigation');
    assert(html.indexOf('id="screen-' + SECTION_OF[key] + '"') !== -1, SECTION_OF[key] + ' has a screen');
  });

  html.replace(/<button[^>]*class="tab"[^>]*data-tab="(\w+)"/g, function (whole, key) {
    assert(TABS[key], 'the tab button for ' + key + ' is a known tab');
  });
  ['home', 'oledcare', 'game', 'servicemenu'].forEach(function (key) {
    assert(html.indexOf('id="tab-btn-' + key + '"') !== -1, 'tab-btn-' + key + ' is kept for the scripts that reveal it');
  });
  assert(/system.*metrics|name === 'system'\) name = 'metrics'/.test(tabsJs), 'the old System tab still opens Metrics');
  console.log('  ok: panels, sections and tabs agree');
})();

(function testAndroidBridge() {
  var calls = [];
  var sandbox = {
    appBridge: { navState: function () { calls.push([].slice.call(arguments)); } },
    q: function () { return { textContent: 'Apps' }; }
  };
  var source = /function reportNav\(\) \{[\s\S]*?\n\}/.exec(tabsJs);
  assert(source, 'reportNav is in tabs.js');
  vm.runInNewContext(source[0] + '\nreportNav();', sandbox);
  assert.deepStrictEqual(calls, [[false, 'Apps', false]], 'the app is told the tabs are not a drawer');
  assert(tabsJs.indexOf("GlasshouseApp") !== -1 && tabsJs.indexOf('Glasshouse-Android') !== -1, 'the bridge and its user agent keep their names');
  console.log('  ok: the Android app is told false, with the section name');
})();

(function testTheme() {
  function run(search, stored, systemDark) {
    var attrs = {};
    var button = { attrs: {}, setAttribute: function (k, v) { this.attrs[k] = v; } };
    var sandbox = {
      t: function (key, english, vars) { return english.replace(/\{(\w+)\}/g, function (w, n) { return vars[n]; }); },
      q: function (id) { return id === 'theme-toggle' ? button : null; },
      location: { search: search },
      localStorage: { getItem: function () { return stored; }, setItem: function () {} },
      matchMedia: function () { return { media: '(prefers-color-scheme: dark)', matches: systemDark, addEventListener: function () {} }; },
      document: {
        documentElement: {
          setAttribute: function (k, v) { attrs[k] = v; },
          removeAttribute: function (k) { delete attrs[k]; },
          classList: { add: function () {}, remove: function () {} }
        },
        querySelector: function () { return null; },
        addEventListener: function () {}
      },
      URLSearchParams: URLSearchParams
    };
    vm.runInNewContext(themeJs, sandbox);
    return { attrs: attrs, button: button };
  }
  assert.strictEqual(run('?theme=light', null, true).attrs['data-theme'], 'light', '?theme=light forces light');
  assert.strictEqual(run('?theme=dark', null, false).attrs['data-theme'], 'dark', '?theme=dark forces dark');
  assert.strictEqual(run('', 'dark', false).attrs['data-theme'], 'dark', 'the stored choice applies');
  assert.strictEqual(run('', null, true).attrs['data-theme'], undefined, 'no choice follows the system: no attribute');
  assert.strictEqual(run('?theme=nonsense', null, true).attrs['data-theme'], undefined, 'an unknown choice follows the system');
  assert(/Theme: Auto/.test(run('', null, true).button.attrs['aria-label']), 'the toggle names the setting');
  console.log('  ok: ?theme=, the stored choice and data-theme');
})();

console.log('ALL test-dashboard-nav.js assertions passed!');
