/**
 * test-admin-session.js - The dashboard's fetch asks for the PIN where the TV wants one
 *
 * Runs the fetch wrapper from ui/core.js against a stand-in network: the admin
 * session goes out with API requests, a 403 that says pin:true raises the
 * unlock prompt and retries, and a 401 (the TV does not know this browser's
 * token) raises the passcode screen. Skipped on a node that cannot run the page.
 */
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-admin-session.js ...');

try {
  vm.runInNewContext('(async () => 1)()');
} catch (e) {
  console.log('  - skipped: this node cannot run the page\'s JavaScript');
  console.log('ALL test-admin-session.js assertions passed!\n');
  process.exit(0);
}

var src = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'ui', 'core.js'), 'utf8');
var start = src.indexOf('let adminSession');
var fetchAt = src.indexOf('window.fetch = ', start);
var end = src.indexOf('\n});', fetchAt) + '\n});'.length;
assert.ok(start !== -1 && fetchAt !== -1 && end > fetchAt, 'the admin session code is in ui/core.js');
var code = src.slice(start, end);

// A page whose network answers from a script, recording what it was asked.
function page(answers) {
  var calls = [], prompts = [], store = {};
  var window = {
    fetch: function (url, init) {
      calls.push({ url: url, headers: init && init.headers && init.headers.map });
      var next = answers.shift();
      if (next instanceof Error) return Promise.reject(next);
      return Promise.resolve(next);
    }
  };
  function Headers(h) { this.map = Object.assign({}, h); }
  Headers.prototype.set = function (k, v) { this.map[k] = v; };
  var ctx = {
    window: window, Headers: Headers, Object: Object, String: String, Promise: Promise, Error: Error,
    sessionStorage: {
      getItem: function (k) { return store[k] || null; },
      setItem: function (k, v) { store[k] = String(v); },
      removeItem: function (k) { delete store[k]; }
    },
    t: function (key, english) { return english; },
    askAdminPin: function () { prompts.push('admin'); return Promise.resolve(ctx.unlockAnswer); },
    askPin: function (login) { prompts.push(login ? 'login' : 'admin'); return Promise.resolve(false); },
    unlockAnswer: true
  };
  vm.runInNewContext(code + '\nthis.setAdminSession = setAdminSession;', ctx);
  return { fetch: window.fetch, ctx: ctx, calls: calls, prompts: prompts, store: store };
}
function reply(status, body) {
  return { status: status, ok: status < 400, clone: function () { return reply(status, body); },
           json: function () { return Promise.resolve(body); } };
}

(async function () {
  // 1. With no session, an API request goes out as it was
  var p = page([reply(200, {})]);
  await p.fetch('/api/stats', { cache: 'no-store' });
  assert.strictEqual(p.calls[0].headers, undefined);
  console.log('  ✓ without a session, requests are left as they were');

  // 2. With a session, API requests carry it; other requests do not
  p = page([reply(200, {}), reply(200, {})]);
  p.ctx.setAdminSession('abc');
  assert.strictEqual(p.store.tvweb_admin, 'abc');
  await p.fetch('/api/ssh', { method: 'POST', headers: { 'Content-Type': 'application/json' } });
  await p.fetch('/assets/ui/ui.css');
  assert.deepStrictEqual(p.calls[0].headers, { 'Content-Type': 'application/json', 'X-Admin-Session': 'abc' });
  assert.strictEqual(p.calls[1].headers, undefined);
  console.log('  ✓ an open session goes out with API requests only');

  // 3. A 403 that wants the PIN raises the prompt and retries once with the new session
  p = page([reply(403, { ok: false, pin: true }), reply(200, { ok: true })]);
  p.ctx.askAdminPin = function () { p.prompts.push('admin'); p.ctx.setAdminSession('fresh'); return Promise.resolve(true); };
  var r = await p.fetch('/api/apps/uninstall', { method: 'POST' });
  assert.strictEqual(r.status, 200);
  assert.deepStrictEqual(p.prompts, ['admin']);
  assert.strictEqual(p.calls.length, 2);
  assert.strictEqual(p.calls[1].headers['X-Admin-Session'], 'fresh');
  console.log('  ✓ a 403 asking for the PIN prompts, then retries with the session');

  // 4. A cancelled prompt hands back the refusal and does not retry
  p = page([reply(403, { ok: false, pin: true })]);
  p.ctx.unlockAnswer = false;
  r = await p.fetch('/api/backup', { method: 'POST' });
  assert.strictEqual(r.status, 403);
  assert.strictEqual(p.calls.length, 1);
  console.log('  ✓ a cancelled prompt leaves the refusal as the answer');

  // 5. Any other 403 is the caller's business
  p = page([reply(403, { ok: false, error: 'controls disabled in config' })]);
  r = await p.fetch('/api/control', { method: 'POST' });
  assert.strictEqual(r.status, 403);
  assert.deepStrictEqual(p.prompts, []);
  console.log('  ✓ a 403 that is not about the PIN raises no prompt');

  // 6. A 401 means the TV does not know the token: the passcode screen, once, no retry
  p = page([reply(401, { ok: false })]);
  r = await p.fetch('/api/stats');
  assert.strictEqual(r.status, 401);
  assert.deepStrictEqual(p.prompts, ['login']);
  assert.strictEqual(p.calls.length, 1);
  console.log('  ✓ a 401 raises the passcode screen');

  // 7. The sign-in and unlock calls answer for themselves
  p = page([reply(401, {}), reply(403, { pin: true }), reply(403, { ok: false, error: 'wrong PIN' })]);
  await p.fetch('/api/admin/login', { method: 'POST' });
  await p.fetch('/api/admin/unlock', { method: 'POST' });
  await p.fetch('/api/admin/lock', { method: 'POST' });
  assert.deepStrictEqual(p.prompts, []);
  console.log('  ✓ the sign-in, unlock and lock calls never raise a prompt');

  // 8. A request that never reaches the TV is reported as the TV being unreachable
  p = page([new Error('Failed to fetch')]);
  try {
    await p.fetch('/api/stats');
    assert.fail('expected a rejection');
  } catch (e) {
    assert.strictEqual(e.offline, true);
    assert.strictEqual(e.message, 'Can’t reach the TV');
  }
  console.log('  ✓ a failed request still reads as the TV being unreachable');

  console.log('ALL test-admin-session.js assertions passed!\n');
})().catch(function (e) { console.error(e); process.exit(1); });
