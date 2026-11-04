/**
 * The Export the block list links on the Privacy tab (web/ui/privacy.js setBlockListLinks):
 * a download cannot send a header, so each link has to carry the token in its address.
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-block-list-export-ui.js ...');

var uiDir = path.join(__dirname, '..', '..', 'web', 'ui');

// The page's own api() and the one function, cut out of their files.
var apiLine = fs.readFileSync(path.join(uiDir, 'format.js'), 'utf8').split('\n')
  .filter(function (line) { return /^const api = /.test(line); })[0];
assert.ok(apiLine, 'format.js still defines api');
var privacy = fs.readFileSync(path.join(uiDir, 'privacy.js'), 'utf8');
var start = privacy.indexOf('function setBlockListLinks()');
var end = privacy.indexOf('async function loadPrivacy()');
assert.ok(start >= 0 && end > start, 'privacy.js still defines setBlockListLinks before loadPrivacy');

function links(token) {
  var elements = {};
  var context = {
    token: token,
    encodeURIComponent: encodeURIComponent,
    q: function (id) { return elements[id] || (elements[id] = { href: '' }); }
  };
  vm.createContext(context);
  vm.runInContext(apiLine + '\n' + privacy.slice(start, end) + '\nsetBlockListLinks();', context);
  return elements;
}

var open = links('');
assert.strictEqual(open['abx-hosts'].href, '/api/adblock/export?format=hosts');
assert.strictEqual(open['abx-pihole'].href, '/api/adblock/export?format=pihole');
assert.strictEqual(open['abx-adguard'].href, '/api/adblock/export?format=adguard');
console.log('  ok - without a token the links are plain');

var guarded = links('s3cret/token');
assert.strictEqual(guarded['abx-hosts'].href, '/api/adblock/export?format=hosts&k=s3cret%2Ftoken');
assert.strictEqual(guarded['abx-pihole'].href, '/api/adblock/export?format=pihole&k=s3cret%2Ftoken');
assert.strictEqual(guarded['abx-adguard'].href, '/api/adblock/export?format=adguard&k=s3cret%2Ftoken');
console.log('  ok - with a token each link carries it, encoded');

var html = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui.html'), 'utf8');
['abx-hosts', 'abx-pihole', 'abx-adguard'].forEach(function (id) {
  assert.ok(new RegExp('<a [^>]*id="' + id + '"[^>]*\\bdownload\\b').test(html), id + ' is a download link');
});
console.log('  ok - the page has the three download links');
console.log('ALL test-block-list-export-ui.js assertions passed!\n');
