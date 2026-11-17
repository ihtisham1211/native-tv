/**
 * test/test-remote-keys-tiles.js - The Apps tab's Remote keys tiles
 *
 * Runs renderRemoteKeys() and toggleRemoteKey() from ui/apps.js against the shape /api/apps
 * answers with: the regional and assistant buttons are grouped, a key set to Home Assistant
 * says so, and Home Assistant is a choice only on a key whose presses it receives.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-remote-keys-tiles.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'apps.js'), 'utf8');
var els = {};
['disc-remotekeys', 'apps-rk-list', 'apps-rk-summary'].forEach(function (id) {
  els[id] = { hidden: false, textContent: '', innerHTML: '', contains: function () { return false; }, querySelector: function () { return null; } };
});
var ctx = {
  q: function (id) { return els[id]; },
  t: function (key, text, vars) {
    return text.replace(/\{(\w+)\}/g, function (_, name) { return vars[name]; });
  },
  esc: function (text) { return String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;'); },
  api: function (url) { return url; },
  CSS: { escape: function (text) { return text; } },
  getComputedStyle: function () { return { gridTemplateColumns: '1fr 1fr 1fr' }; },
  document: { addEventListener: function () {}, activeElement: null, createElement: function () {
    return { innerHTML: '', style: { setProperty: function () {} }, setAttribute: function () {}, after: function () {}, querySelector: function () { return null; } };
  } },
  window: { addEventListener: function () {} }
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.renderRemoteKeys = renderRemoteKeys; this.toggleRemoteKey = toggleRemoteKey;', ctx);

function key(name, title, extra) {
  return Object.assign({ key: name, title: title, action: 'lg', id: '', opens: '', elsewhere: false }, extra);
}
var keys = [
  key('netflix', 'Netflix', { homeAssistant: true, action: 'open', id: 'action:homeAssistant', opens: 'Home Assistant' }),
  key('ivi', 'Ivi'),
  key('hulu', 'Hulu', { elsewhere: true }),
  key('globoplay', 'Globoplay', { elsewhere: true }),
  key('voice', 'Microphone', { homeAssistant: true, replacesVoice: true }),
  key('alexa', 'Alexa', { homeAssistant: true }),
  key('movies', 'Movies', { homeAssistant: true }),
  key('search', 'Search')
];
var choices = [
  { id: 'media:ok', title: 'OK', kind: 'media' },
  { id: 'action:homeAssistant', title: 'Home Assistant', kind: 'ha' },
  { id: 'org.example.app', title: 'An app', kind: 'app' }
];
ctx.renderRemoteKeys({ writable: true, remoteKeys: { keys: keys, choices: choices } });
var html = els['apps-rk-list'].innerHTML;

function tile(name) {
  var match = new RegExp('<button type="button" class="rk-key[^"]*" data-key="' + name + '"[\\s\\S]*?</button>').exec(html);
  assert.ok(match, name + ' has a tile');
  return match[0];
}
assert.ok(/To Home Assistant/.test(tile('netflix')), 'a key set to Home Assistant says so');
assert.ok(!/action:homeAssistant/.test(tile('netflix').replace(/aria-label="[^"]*"/, '')), 'and not the id it is stored as');

// Groups, in order: streaming buttons with the regional ones that apply, the voice buttons with the
// microphone first, other keys; a button of another country's remotes waits under the fold.
function between(title, next) {
  var from = html.indexOf('>' + title + '<');
  var to = html.indexOf(next);
  assert.ok(from >= 0 && to > from, 'a group titled ' + title + ' before ' + next);
  return html.slice(from, to);
}
var streaming = between('Streaming buttons', '>Voice assistant buttons<');
assert.ok(streaming.indexOf('data-key="netflix"') > 0 && streaming.indexOf('data-key="ivi"') > streaming.indexOf('data-key="netflix"'), 'Ivi follows Netflix');
assert.ok(streaming.indexOf('data-key="hulu"') < 0 && streaming.indexOf('data-key="globoplay"') < 0, 'a button of another country\'s remotes is not among them');
var assistant = between('Voice assistant buttons', '>Other keys<');
assert.ok(assistant.indexOf('data-key="voice"') > 0 && assistant.indexOf('data-key="voice"') < assistant.indexOf('data-key="alexa"'), 'the microphone leads the voice buttons');
var other = between('Other keys', '<details class="rk-more"');
assert.ok(other.indexOf('data-key="movies"') > 0 && other.indexOf('data-key="search"') > 0, 'Movies is with the other keys');
var more = html.slice(html.indexOf('<details class="rk-more"'));
assert.ok(/Buttons on remotes sold elsewhere \(2\)/.test(more), 'two buttons are folded away');
assert.ok(more.indexOf('data-key="hulu"') > 0 && more.indexOf('data-key="globoplay"') > 0, 'Hulu and Globoplay are among them');
console.log('  ok - the regional and assistant buttons are grouped, and Home Assistant shows as a key\'s target');

// The picker: Home Assistant only where the row says it receives the key's presses.
function picker(name) {
  var picked = null;
  var gridTile = { dataset: { key: name }, classList: { add: function () {}, remove: function () {}, contains: function () { return true; } }, setAttribute: function () {}, after: function () {}, getBoundingClientRect: function () { return { left: 0, width: 10 }; } };
  var grid = { children: [gridTile], getBoundingClientRect: function () { return { left: 0 }; } };
  gridTile.parentElement = grid;
  ctx.document.createElement = function () {
    var node = { innerHTML: '', style: { setProperty: function () {} }, setAttribute: function () {}, after: function () {}, remove: function () {}, querySelector: function () { return null; } };
    picked = node;
    return node;
  };
  ctx.toggleRemoteKey(gridTile);
  ctx.closeRemoteKeyPicker(false);
  return picked.innerHTML;
}
vm.runInContext('this.closeRemoteKeyPicker = closeRemoteKeyPicker', ctx);
var voice = picker('voice');
assert.ok(/data-value="action:homeAssistant"/.test(voice), 'the microphone can be sent to Home Assistant');
assert.ok(/no longer starts voice control/.test(voice), 'and it says setting it replaces voice control');
assert.ok(/>Buttons</.test(voice) && /data-value="media:ok"/.test(voice), 'OK is among the Buttons');
assert.ok(!/>Media</.test(voice), 'the heading is not Media any more');
var search = picker('search');
assert.ok(!/action:homeAssistant/.test(search), 'a key Home Assistant does not receive has no such choice');
assert.ok(!/no longer starts voice control/.test(search), 'and no note about voice control');
console.log('  ok - Home Assistant is offered only on the keys it receives, with the microphone\'s note');

console.log('ALL test-remote-keys-tiles.js assertions passed!\n');
