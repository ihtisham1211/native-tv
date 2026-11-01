// The token, q(), the activity pips, write leases and the fetch wrapper. t() is in i18n.js.

/* The token arrives once in the link (a bookmark, or the TV's QR codes) and
   is kept in the browser, so it can come out of the address bar and stay out
   of history and copied URLs. Where storage is blocked it stays in the URL,
   or a reload would lose it. */
const token = (() => {
  const s = new URLSearchParams(location.search);
  const k = s.get('k');
  if (k === null) {
    try { return localStorage.getItem('tvweb_k') || ''; } catch (e) { return ''; }
  }
  try { localStorage.setItem('tvweb_k', k); } catch (e) { return k; }
  s.delete('k');
  history.replaceState(history.state, '', location.pathname + (s.toString() ? '?' + s : '') + location.hash);
  return k;
})();
const q = id => document.getElementById(id);

let inFlight = 0;
let rxTimer = null;
function setBusy(on, stage) {
  const tx = q('act-tx');
  const rx = q('act-rx');
  if (on) {
    inFlight++;
    if (stage === 'rx') {
      if (tx) tx.classList.remove('lit');
      if (rx) rx.classList.add('lit');
      if (rxTimer) clearTimeout(rxTimer);
      rxTimer = setTimeout(() => {
        if (rx) rx.classList.remove('lit');
      }, 200);
    } else {
      if (tx) tx.classList.add('lit');
    }
  } else {
    inFlight = Math.max(0, inFlight - 1);
    if (inFlight === 0 && tx) {
      tx.classList.remove('lit');
    }
  }
}

const leases = {};
function setLease(key, val, ttl = 3000) {
  leases[key] = { val: val, exp: Date.now() + ttl };
}
function getLease(key, currentVal) {
  const l = leases[key];
  if (!l) return undefined;
  if (Date.now() > l.exp) {
    delete leases[key];
    return undefined;
  }
  if (currentVal !== undefined && currentVal !== null && currentVal === l.val) {
    delete leases[key];
    return undefined;
  }
  return l.val;
}
function clearLease(key) {
  delete leases[key];
}
let lastArOff = null;

/* Admin PIN. Once the owner sets one, the risky actions answer 403 with pin:true
   until a request carries an open unlock session (X-Admin-Session). The session
   is kept for the tab; the first action that needs it asks for the PIN, from
   askAdminPin() in server.js, and goes ahead when it is given. A 401 is the
   other case: the TV does not know this browser's token, so the passcode screen
   (askPin(true)) asks for the PIN, which hands the token over. */
let adminSession = '';
try { adminSession = sessionStorage.getItem('tvweb_admin') || ''; } catch (e) { /* kept in memory only */ }
function setAdminSession(session) {
  adminSession = session;
  try {
    if (session) sessionStorage.setItem('tvweb_admin', session);
    else sessionStorage.removeItem('tvweb_admin');
  } catch (e) { /* kept in memory only */ }
}
const isApiUrl = url => String(url).indexOf('/api/') !== -1;
const netFetch = window.fetch.bind(window);
const sendFetch = (url, init) => {
  if (!adminSession || !isApiUrl(url)) return netFetch(url, init);
  const headers = new Headers(init && init.headers);
  headers.set('X-Admin-Session', adminSession);
  return netFetch(url, Object.assign({}, init, { headers }));
};
// A request that never reaches the TV rejects with the browser's own wording
// ("Failed to fetch" in Chrome, "Load failed" in Safari), which reads as a
// fault in the page rather than a TV that is off.
window.fetch = (url, init) => sendFetch(url, init).then(async r => {
  // The sign-in and unlock calls answer for themselves; /api/admin/pin may need an unlock like any other.
  const answersItself = /\/api\/admin\/(unlock|lock|login)/.test(String(url));
  if (!isApiUrl(url) || answersItself) return r;
  // The TV does not know this browser's token: ask for the passcode, which gets it (and reloads).
  if (r.status === 401) { await askPin(true); return r; }
  if (r.status !== 403) return r;
  const body = await r.clone().json().catch(() => null);
  if (!body || body.pin !== true || !(await askAdminPin())) return r;
  return sendFetch(url, init);
}).catch(e => {
  if (e && e.name === 'AbortError') throw e;
  const x = new Error(t('common.unreachable', 'Can’t reach the TV'));
  x.offline = true;
  throw x;
});
