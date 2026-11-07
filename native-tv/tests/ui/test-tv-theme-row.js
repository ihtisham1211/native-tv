/**
 * test/test-tv-theme-row.js - The Server tab's TV theme row
 *
 * Runs renderTvTheme() and chooseTvTheme() from ui/server.js against the status shapes the server
 * sends: a server that knows themes, one that does not, one that cannot be written to, and a
 * choice the server accepts or refuses.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-tv-theme-row.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'server.js'), 'utf8');
var sel = {
  options: [], value: '', disabled: false,
  appendChild: function (option) { this.options.push(option); },
  set innerHTML(text) { this.options = []; },
  get innerHTML() { return ''; }
};
var els = {
  'tvtheme-row': { hidden: true },
  'tvtheme-sel': sel,
  'tvtheme-summary': { textContent: '' }
};
var sent = [], reloaded = 0, rendered = [];
var reply = { ok: true };
var ctx = {
  q: function (id) { return els[id]; },
  document: { createElement: function () { return { value: '', textContent: '', translate: true }; } },
  sendCommand: function (action, value) { sent.push([action, value]); return Promise.resolve(reply); }
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.renderTvTheme = renderTvTheme; this.chooseTvTheme = chooseTvTheme;', ctx);
ctx.renderServerStatus = function (status) { rendered.push(status); };
ctx.loadServerStatus = function () { reloaded++; };

var themes = [
  { id: 'n-google-tv', name: 'Google TV', summary: 'Ambient artwork and the white focus pill. The default.' },
  { id: 'n-oled-care', name: 'OLED Care', summary: 'True black, dimmed controls and a layout that drifts.' }
];

ctx.renderTvTheme(undefined, undefined, true);
assert.ok(els['tvtheme-row'].hidden, 'a server that knows no themes shows no row');
ctx.renderTvTheme('n-google-tv', [], true);
assert.ok(els['tvtheme-row'].hidden, 'an empty list shows no row either');

ctx.renderTvTheme('n-oled-care', themes, true);
assert.ok(!els['tvtheme-row'].hidden, 'the row shows when the server lists themes');
assert.deepStrictEqual(sel.options.map(function (o) { return o.value; }), ['n-google-tv', 'n-oled-care']);
assert.deepStrictEqual(sel.options.map(function (o) { return o.textContent; }), ['Google TV', 'OLED Care']);
assert.ok(sel.options.every(function (o) { return o.translate === false; }), 'the theme names are not translated');
assert.strictEqual(sel.value, 'n-oled-care');
assert.strictEqual(els['tvtheme-summary'].textContent, themes[1].summary);
assert.strictEqual(sel.disabled, false);

var first = sel.options[0];
ctx.renderTvTheme('n-google-tv', themes, true);
assert.strictEqual(sel.options[0], first, 'the same list is not rebuilt, so an open menu is not closed');
assert.strictEqual(sel.value, 'n-google-tv');
assert.strictEqual(els['tvtheme-summary'].textContent, themes[0].summary);

ctx.renderTvTheme('n-google-tv', themes, false);
assert.strictEqual(sel.disabled, true, 'a server that cannot be written to disables the choice');

ctx.renderTvTheme('n-gone', themes, true);
assert.strictEqual(els['tvtheme-summary'].textContent, '', 'a theme the list does not have shows no summary');

ctx.renderTvTheme('n-google-tv', themes.concat([{ id: 'n-apple-tv', name: 'Apple TV', summary: 'x' }]), true);
assert.strictEqual(sel.options.length, 3, 'a longer list is rebuilt');

sel.value = 'n-oled-care';
ctx.chooseTvTheme().then(function () {
  assert.deepStrictEqual(sent, [['setTvTheme', 'n-oled-care']], 'the choice is sent as setTvTheme');
  assert.strictEqual(rendered.length, 1, 'an accepted choice redraws from the answer');
  assert.strictEqual(reloaded, 0);
  reply = { ok: false, error: 'That theme does not exist' };
  sel.value = 'n-google-tv';
  return ctx.chooseTvTheme();
}).then(function () {
  assert.strictEqual(rendered.length, 1, 'a refused choice is not drawn');
  assert.strictEqual(reloaded, 1, 'it reads the real state back instead');
  console.log('test-tv-theme-row.js passed');
}).catch(function (error) {
  console.error(error);
  process.exit(1);
});
