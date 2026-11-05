/**
 * The Privacy tab's summary (web/ui/privacy.js renderSimple): each area that still has
 * something on gets a Switch off of its own, which sends privacyAreaOff with the area's id.
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-privacy-area-ui.js ...');

var uiDir = path.join(__dirname, '..', '..', 'web', 'ui');
var privacy = fs.readFileSync(path.join(uiDir, 'privacy.js'), 'utf8');
var start = privacy.indexOf('function renderSimple(d)');
var end = privacy.indexOf('async function switchAllOff()');
assert.ok(start >= 0 && end > start, 'privacy.js still defines renderSimple before switchAllOff');

function render(d) {
  var elements = {};
  var context = {
    t: function (key, english, vars) {
      return english.replace(/\{(\w+)\}/g, function (m, k) { return vars && k in vars ? String(vars[k]) : m; });
    },
    esc: function (s) { return String(s).replace(/[<>"&]/g, function (c) { return '&#' + c.charCodeAt(0) + ';'; }); },
    q: function (id) {
      return elements[id] || (elements[id] = { hidden: false, className: '', textContent: '', innerHTML: '', dataset: {}, disabled: false });
    }
  };
  vm.createContext(context);
  vm.runInContext(privacy.slice(start, end) + '\nrenderSimple(' + JSON.stringify(d) + ');', context);
  return elements;
}

var areas = [
  { id: 'watching', name: 'Screen recognition', detail: 'd', items: [] },
  { id: 'ads', name: 'Ad tracking', detail: 'd', items: [{ label: 'Personalised advertising' }, { label: 'Limit ad tracking is off' }] },
  { id: 'reports', name: 'Usage reports', detail: 'd', items: [{ label: 'Additional usage data' }] }
];
var summary = { total: 3, areas: areas, kept: [] };

var shown = render({ simple: summary, consentWritable: true });
var buttons = shown['ps-areas'].innerHTML.match(/data-area="[a-zA-Z]+"/g);
assert.deepStrictEqual(buttons, ['data-area="ads"', 'data-area="reports"'], 'one switch per area with something on');
assert.strictEqual((shown['ps-areas'].innerHTML.match(/switchAreaOff\(this\)/g) || []).length, 2);
assert.ok(shown['ps-areas'].innerHTML.indexOf('>Switch off<') >= 0, 'the switch is labelled');
assert.strictEqual(shown['ps-go'].disabled, false, 'the switch-all-off button stays');
console.log('  ok - each area with something on gets its own Switch off');

var readOnly = render({ simple: summary, consentWritable: false });
assert.strictEqual(readOnly['ps-areas'].innerHTML.indexOf('data-area='), -1, 'no switch when consents cannot be written');
assert.strictEqual(readOnly['ps-go'].disabled, true);
console.log('  ok - a TV whose consents cannot be written gets no switch');

var sender = privacy.slice(privacy.indexOf('async function switchAreaOff(button)'));
sender = sender.slice(0, sender.indexOf('\n}\n'));
assert.ok(/sendCommand\('privacyAreaOff', button\.dataset\.area\)/.test(sender), 'the switch sends the area id');
assert.ok(/loadPrivacy\(\)/.test(sender), 'the summary is read again afterwards');
console.log('  ok - switchAreaOff sends privacyAreaOff with the area id');
console.log('ALL test-privacy-area-ui.js assertions passed!\n');
