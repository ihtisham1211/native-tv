/**
 * test/test-hdmi-grid.js - The Advanced tab's HDMI inputs table fits a phone
 *
 * Runs hdmiTable() from ui/lgsettings.js. Its columns once held a minimum of 150, 130 and 130 px
 * plus two 24 px gaps, which is wider than a 390 px phone: the Audio format column hung off the
 * right of the page. Every track has to be allowed to shrink to nothing.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-hdmi-grid.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'lgsettings.js'), 'utf8');
var ctx = {
  document: { addEventListener: function () {} },
  t: function (key, text) { return text; },
  esc: function (text) { return String(text); },
  q: function () { return null; },
  renderToggle: function () { return '<button></button>'; },
  sendCommand: function () {}
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.hdmiTable = hdmiTable;', ctx);

function row(id, port, column, choices) {
  return { id: id, port: port, column: column, type: 'choice', value: choices[0], choices: choices.map(function (v) { return { value: v, label: v }; }) };
}
var rows = [];
[1, 2, 3, 4].forEach(function (n) {
  rows.push(row('audioFormatHDMI' + n, n, 'audioFormat', ['bitstream', 'pcm']));
});
rows.push(row('deepColor', 0, 'deepColor', ['off', '4k']));
var html = ctx.hdmiTable(rows, { other: { gameInput: 'hdmi1' } }, { hdmi1: 'HDMI1' });

var tracks = /grid-template-columns:([^"]*)"/.exec(html);
assert.ok(tracks, 'the table sets its columns');
var minimums = tracks[1].match(/minmax\(\s*([0-9.]+)(px)?/g).map(function (m) { return m.replace(/minmax\(\s*/, ''); });
assert.strictEqual(minimums.length, 3, 'an input column and the two settings');
minimums.forEach(function (m) {
  assert.strictEqual(parseFloat(m), 0, 'a track can shrink to fit a phone, not hold ' + m);
});
console.log('ALL test-hdmi-grid.js assertions passed!\n');
