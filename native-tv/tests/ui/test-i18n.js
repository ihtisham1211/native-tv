/**
 * test/test-i18n.js - The dashboard's string loader (ui/i18n.js)
 *
 * Runs the real loader against stand-ins for the page, the address bar, local
 * storage and the network: which language is picked and in what order, the
 * dictionary lookup (and the English shown when a translation was made from
 * other words), placeholders, data-t in the markup, the header that tells the
 * server the language, and the Server tab's list.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-i18n.js ...');

var code = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'i18n.js'), 'utf8');

var DICT = {
  'server.health.title': { text: 'Estado del servidor', from: 'Server health' },
  'server.install': { text: 'Instalar v{version}', from: 'Install v{version}' },
  'old.words': { text: 'Texto viejo', from: 'The words it was made from' },
  'nav.help': { text: 'Ayuda', from: 'Help' },
  'search.open': { text: 'Buscar', from: 'Search' },
  'theme.auto': { text: 'Automático', from: 'Auto' }
};

function element(attrs, text) {
  return {
    attrs: attrs || {},
    textContent: text || '',
    getAttribute: function (name) { return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : null; },
    setAttribute: function (name, value) { this.attrs[name] = String(value); }
  };
}

// Loads the loader into a page. opts: search, stored, browser (navigator.languages),
// dict, dictStatus, elements, readyState, select.
function boot(opts) {
  opts = opts || {};
  var store = opts.stored === undefined ? {} : { lang: opts.stored };
  var page = { xhrs: [], fetches: [], assigned: null, listeners: {}, root: { attrs: {} } };
  function Headers(init) { this.map = {}; for (var k in init || {}) this.map[k.toLowerCase()] = init[k]; }
  Headers.prototype.set = function (name, value) { this.map[name.toLowerCase()] = String(value); };
  Headers.prototype.get = function (name) { return this.map[name.toLowerCase()]; };
  function XHR() { this.headers = {}; }
  XHR.prototype.open = function (method, url, async) { this.method = method; this.url = url; this.async = async; };
  XHR.prototype.setRequestHeader = function (name, value) { this.headers[name] = value; };
  XHR.prototype.send = function () {
    page.xhrs.push(this);
    if (/^\/assets\/i18n\//.test(this.url)) {
      if (opts.dictStatus === 'throw') throw new Error('network down');
      this.status = opts.dictStatus || 200;
      this.responseText = typeof opts.dict === 'string' ? opts.dict : JSON.stringify(opts.dict || DICT);
    }
  };
  var all = opts.elements || [];
  var ctx = {
    XMLHttpRequest: XHR,
    Headers: Headers,
    fetch: function (input, init) { page.fetches.push({ input: input, init: init }); return { then: function () {} }; },
    JSON: JSON, Object: Object, String: String, Error: Error,
    navigator: { languages: opts.browser || ['en-US'], language: (opts.browser || ['en-US'])[0] },
    localStorage: {
      getItem: function (k) { if (opts.storageBlocked) throw new Error('SecurityError'); return Object.prototype.hasOwnProperty.call(store, k) ? store[k] : null; },
      setItem: function (k, v) { if (opts.storageBlocked) throw new Error('SecurityError'); store[k] = String(v); },
      removeItem: function (k) { delete store[k]; }
    },
    location: {
      search: opts.search || '', pathname: '/', hash: '#top', origin: 'http://tv.local:8080',
      assign: function (url) { page.assigned = url; }
    },
    document: {
      readyState: opts.readyState || 'complete',
      documentElement: { setAttribute: function (k, v) { page.root.attrs[k] = v; } },
      addEventListener: function (name, fn) { page.listeners[name] = fn; },
      querySelectorAll: function (selector) {
        var attr = /^\[([\w-]+)\]$/.exec(selector)[1];
        return all.filter(function (el) { return el.getAttribute(attr) !== null; });
      },
      getElementById: function (id) { return id === 'lang-sel' ? opts.select || null : null; },
      createElement: function () { return element(); }
    }
  };
  ctx.window = ctx;
  vm.runInNewContext(code, ctx);
  page.ctx = ctx;
  page.store = store;
  return page;
}

// 1. English: nothing is fetched or wrapped, and the English comes back with its values
var p = boot();
assert.strictEqual(p.ctx.I18N.lang(), 'en');
assert.strictEqual(p.xhrs.length, 0, 'English fetches no dictionary');
assert.strictEqual(p.ctx.t('server.install', 'Install v{version}', { version: '1.2' }), 'Install v1.2');
assert.strictEqual(p.ctx.t('server.install', 'Install v{version}'), 'Install v{version}', 'a missing value stays as written');
p.ctx.fetch('/api/stats');
assert.strictEqual(p.fetches[0].init, undefined, 'English adds no header');
console.log('  ✓ English fetches nothing, wraps nothing and fills placeholders');

// 2. The browser's language: the dictionary is fetched once, synchronously
p = boot({ browser: ['es-MX', 'en'] });
assert.strictEqual(p.ctx.I18N.lang(), 'es');
assert.strictEqual(p.xhrs.length, 1);
assert.strictEqual(p.xhrs[0].url, '/assets/i18n/es.json');
assert.strictEqual(p.xhrs[0].async, false, 'the dictionary is read synchronously, so the first paint is in one language');
assert.strictEqual(p.root.attrs.lang, 'es');
assert.strictEqual(p.ctx.t('server.health.title', 'Server health'), 'Estado del servidor');
assert.strictEqual(p.ctx.t('server.install', 'Install v{version}', { version: '1.2' }), 'Instalar v1.2');
console.log('  ✓ the browser\'s language loads the dictionary and translates');

// 3. English that no longer matches what a translation was made from is shown as it is
assert.strictEqual(p.ctx.t('old.words', 'Newer words'), 'Newer words');
assert.strictEqual(p.ctx.t('old.words', 'The words it was made from'), 'Texto viejo');
assert.strictEqual(p.ctx.t('no.such.key', 'Left alone {n}', { n: 3 }), 'Left alone 3');
assert.strictEqual(p.ctx.t('constructor', 'Constructor'), 'Constructor', 'inherited properties are not keys');
assert.strictEqual(p.ctx.t('__proto__', 'Proto'), 'Proto');
console.log('  ✓ a translation made from other words, or none, shows the English');

// 4. Which language: ?lang= first, then the stored choice, then the browser's
var order = [
  // search, stored, browser, expected
  ['?lang=es', 'en', ['en-US'], 'es'],
  ['?lang=en', 'es', ['es-ES'], 'en'],
  ['?tab=privacy&lang=es', undefined, ['en'], 'es'],
  ['', 'es', ['en-US'], 'es'],
  ['', 'en', ['es-ES'], 'en'],
  ['', 'auto', ['es-419'], 'es'],
  ['', undefined, ['es'], 'es'],
  ['', undefined, ['fr-FR', 'es'], 'en'],
  ['', undefined, ['en-GB'], 'en'],
  ['?lang=fr', undefined, ['es'], 'en'],
  ['', 'klingon', ['es'], 'es'],
  ['?lang=ES', undefined, ['en'], 'es']
];
order.forEach(function (row) {
  var got = boot({ search: row[0], stored: row[1], browser: row[2] }).ctx.I18N.lang();
  assert.strictEqual(got, row[3], 'search ' + row[0] + ', stored ' + row[1] + ', browser ' + row[2] + ' -> ' + row[3] + ', got ' + got);
});
console.log('  ✓ ?lang= wins over the stored choice, which wins over the browser\'s language');

// 5. No dictionary, a bad one or blocked storage leaves English, not a half-translated page
['throw', 404, 500].forEach(function (status) {
  var q = boot({ browser: ['es'], dictStatus: status });
  assert.strictEqual(q.ctx.I18N.lang(), 'en', 'status ' + status);
  assert.strictEqual(q.ctx.t('server.health.title', 'Server health'), 'Server health');
  assert.strictEqual(q.root.attrs.lang, undefined);
});
var bad = boot({ browser: ['es'], dict: '{not json' });
assert.strictEqual(bad.ctx.I18N.lang(), 'en');
assert.strictEqual(boot({ browser: ['es'], storageBlocked: true }).ctx.I18N.lang(), 'es', 'blocked storage follows the browser');
console.log('  ✓ a missing or broken dictionary, and blocked storage, fall back cleanly');

// 6. data-t in the markup: text across lines, and the attributes
var heading = element({ 'data-t': 'server.health.title' }, '\n   Server\n   health \n');
var untouched = element({ 'data-t': 'old.words' }, 'Different words now');
var help = element({ 'data-t-aria-label': 'nav.help', 'aria-label': 'Help' });
var search = element({ 'data-t-title': 'search.open', title: 'Search' });
var option = element({ 'data-t': 'theme.auto' }, 'Auto');
p = boot({ browser: ['es'], elements: [heading, untouched, help, search, option] });
assert.strictEqual(heading.textContent, 'Estado del servidor');
assert.strictEqual(untouched.textContent, 'Different words now');
assert.strictEqual(help.attrs['aria-label'], 'Ayuda');
assert.strictEqual(search.attrs.title, 'Buscar');
p.ctx.I18N.apply();
assert.strictEqual(heading.textContent, 'Estado del servidor', 'applying again changes nothing');
assert.strictEqual(option.textContent, 'Automático');
var english = element({ 'data-t': 'server.health.title' }, 'Server health');
boot({ elements: [english] });
assert.strictEqual(english.textContent, 'Server health', 'English leaves the markup alone');
console.log('  ✓ data-t text and attributes are translated in place, once');

// 7. The server is told the language on the page's own requests only
p = boot({ browser: ['es'] });
p.ctx.fetch('/api/screensaver');
assert.strictEqual(p.fetches[0].init.headers.get('X-Native-Tv-Lang'), 'es');
p.ctx.fetch('http://tv.local:8080/api/stats');
assert.strictEqual(p.fetches[1].init.headers.get('X-Native-Tv-Lang'), 'es');
p.ctx.fetch('https://api.github.com/repos/x/y');
assert.strictEqual(p.fetches[2].init, undefined, 'another site gets no header, which would need a preflight');
p.ctx.fetch('/api/x', { headers: { Authorization: 'Bearer t' } });
assert.strictEqual(p.fetches[3].init.headers.get('authorization'), 'Bearer t', 'headers the page set are kept');
assert.strictEqual(p.fetches[3].init.headers.get('X-Native-Tv-Lang'), 'es');
var xhr = new p.ctx.XMLHttpRequest();
xhr.open('POST', '/api/apps/install/upload');
xhr.send();
assert.strictEqual(xhr.headers['X-Native-Tv-Lang'], 'es');
var elsewhere = new p.ctx.XMLHttpRequest();
elsewhere.open('GET', 'https://example.com/x');
elsewhere.send();
assert.strictEqual(elsewhere.headers['X-Native-Tv-Lang'], undefined);
console.log('  ✓ requests to the TV carry the language header; other sites\' do not');

// 8. The Server tab's list: Spanish is added, the current choice shown, a change stored and the address cleaned
function select() {
  return {
    value: '', options: [], listeners: {},
    appendChild: function (o) { this.options.push(o); },
    addEventListener: function (name, fn) { this.listeners[name] = fn; }
  };
}
var sel = select();
p = boot({ stored: 'es', select: sel });
assert.strictEqual(sel.options.length, 1);
assert.strictEqual(sel.options[0].value, 'es');
assert.strictEqual(sel.options[0].textContent, 'Español');
assert.strictEqual(sel.value, 'es');
sel.value = 'en';
sel.listeners.change();
assert.strictEqual(p.store.lang, 'en');
assert.strictEqual(p.assigned, '/#top');
sel.value = 'auto';
sel.listeners.change();
assert.strictEqual(p.store.lang, undefined, 'Auto forgets the choice');

sel = select();
p = boot({ search: '?tab=privacy&lang=es&x=1', stored: 'en', select: sel });
assert.strictEqual(sel.value, 'es', 'the list shows the language in use when the address forces one');
sel.value = 'en';
sel.listeners.change();
assert.strictEqual(p.assigned, '/?tab=privacy&x=1#top', 'the forced language leaves the address, or it would win over the choice');

sel = select();
p = boot({ search: '?lang=es', select: sel });
sel.value = 'auto';
sel.listeners.change();
assert.strictEqual(p.assigned, '/#top');

sel = select();
p = boot({ readyState: 'loading', select: sel, elements: [] });
assert.strictEqual(sel.options.length, 0, 'while the page is parsing, the list waits for it');
p.listeners.DOMContentLoaded();
assert.strictEqual(sel.options.length, 1);
console.log('  ✓ the Server tab\'s list offers Español, stores a choice and drops ?lang= before reloading');

console.log('ALL test-i18n.js assertions passed!\n');
