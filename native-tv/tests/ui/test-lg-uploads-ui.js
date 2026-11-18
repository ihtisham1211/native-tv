/**
 * test-lg-uploads-ui.js - The Privacy tab's "Log uploads to LG" section
 *
 * Runs renderLgSettings() and the click handler from ui/lgsettings.js. The rows come from
 * /api/privacy's lgSettings: the TV's ads and promotions, and the two log upload settings LG's
 * menus lack. The section shows only where the TV reports one, counts the switches that are on,
 * and a switch sends the same lgSetting command as an ad switch does, without the master row.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-lg-uploads-ui.js ...');

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'lgsettings.js'), 'utf8');
var elements = {};
function element(id) {
  if (!elements[id]) elements[id] = { id: id, hidden: false, textContent: '', innerHTML: '' };
  return elements[id];
}
var listeners = [];
var sent = [];
var reloaded = 0;
var ctx = {
  document: { addEventListener: function (type, fn) { listeners.push({ type: type, fn: fn }); } },
  t: function (key, text, vars) {
    return text.replace(/\{(\w+)\}/g, function (m, name) { return vars[name]; });
  },
  esc: function (text) { return String(text); },
  q: element,
  renderToggle: function (o) {
    return '<button ' + o.attrs + ' data-next="' + o.next + '">' + o.label + '</button>';
  },
  sendCommand: function (action, value) { sent.push([action, value]); return Promise.resolve(); },
  loadPrivacy: function () { reloaded++; }
};
vm.createContext(ctx);
vm.runInContext(src + '\nthis.renderLgSettings = renderLgSettings;', ctx);

function row(id, section, title, on) {
  return { id: id, section: section, title: title, desc: title + ' help', on: on };
}

// A TV with both kinds of row: the promotions as before, and the uploads in their own section.
ctx.renderLgSettings([
  row('homePromotion', 'promotions', 'Sponsored tiles on Home', true),
  row('dbgLogUpload', 'uploads', 'Debug log upload', true),
  row('faultLogUpload', 'uploads', 'Fault log upload', false)
]);
var uploads = element('pv-uploads');
assert.strictEqual(element('pv-uploads-d').hidden, false, 'the section opens where the TV has an upload setting');
assert.strictEqual(element('pv-uploads-cur').textContent, '1 of 2 on', 'it counts the switches that are on');
assert.ok(/data-id="dbgLogUpload" data-next="0">On</.test(uploads.innerHTML),
  'an upload that is on has a switch that turns it off');
assert.ok(/data-id="faultLogUpload" data-next="1">Off</.test(uploads.innerHTML),
  'one that is off has a switch that turns it on');
assert.ok(uploads.innerHTML.indexOf('Debug log upload help') !== -1, 'each row says what it does');
assert.ok(uploads.innerHTML.indexOf('data-ads') === -1 && uploads.innerHTML.indexOf('Sponsored') === -1,
  'there is no master switch, and the promotions are not in this section');
assert.ok(element('pv-promo').innerHTML.indexOf('data-ads="all"') !== -1 &&
  element('pv-promo').innerHTML.indexOf('data-id="homePromotion"') !== -1 &&
  element('pv-promo').innerHTML.indexOf('dbgLogUpload') === -1,
  'the promotions keep their master switch and none of the uploads');
assert.strictEqual(element('pv-promo-cur').textContent, '1 of 1 on');

// A TV that reports no upload setting shows no section.
ctx.renderLgSettings([row('homePromotion', 'promotions', 'Sponsored tiles on Home', false)]);
assert.strictEqual(element('pv-uploads-d').hidden, true, 'the section stays hidden');
assert.strictEqual(element('pv-uploads').innerHTML, '', 'and empty');
assert.strictEqual(element('pv-uploads-cur').textContent, '');

// A switch sends the command for its own row, then reads the tab again.
var clicker = listeners.filter(function (l) { return l.type === 'click'; })[0];
assert.ok(clicker, 'the rows listen for clicks');
var button = { dataset: { id: 'dbgLogUpload', next: '0' }, disabled: false };
var target = {
  closest: function (selector) { return selector.indexOf('#pv-uploads button[data-id]') !== -1 ? button : null; }
};
clicker.fn({ target: target }).then(function () {
  // JSON, as the page's objects come from another context and differ in prototype.
  assert.strictEqual(JSON.stringify(sent), JSON.stringify([['lgSetting', { id: 'dbgLogUpload', on: false }]]),
    'turning an upload off sends lgSetting with on false');
  assert.strictEqual(button.disabled, true, 'the switch waits for the answer');
  assert.strictEqual(reloaded, 1, 'the tab is read again');
  console.log('ALL test-lg-uploads-ui.js assertions passed!\n');
}).catch(function (error) { console.error(error); process.exit(1); });
