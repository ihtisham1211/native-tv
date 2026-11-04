// Makes the dashboard installable and, when the TV cannot be reached, says so instead of
// the browser's error page. It caches nothing: the page and /api/ always come from the TV,
// whose state a stale copy would misreport. Only navigations are handled; every other
// request, the API included, goes straight to the network.
const OFFLINE = '<!doctype html><html lang="en"><head><meta charset="utf-8">' +
  '<meta name="viewport" content="width=device-width,initial-scale=1">' +
  '<meta name="color-scheme" content="dark light"><title>Native Tv</title><style>' +
  'body{margin:0;min-height:100vh;box-sizing:border-box;display:grid;place-content:center;gap:16px;padding:24px;text-align:center;' +
  'font:16px/1.5 system-ui,sans-serif;background:#111411;color:#e1e3dd}' +
  '@media(prefers-color-scheme:light){body{background:#f7faf8;color:#191c19}}' +
  'h1{margin:0;font-size:24px;font-weight:500}p{margin:0;opacity:.75}' +
  'button{justify-self:center;padding:12px 24px;border:0;border-radius:24px;font:inherit;font-weight:500;' +
  'background:#5df08a;color:#00210c}</style></head><body><h1>Can’t reach the TV</h1>' +
  '<p>Check that it is on and that this device is on the same network.</p>' +
  '<button onclick="location.reload()">Try again</button>' +
  '<script>addEventListener("online",()=>location.reload())</script></body></html>';

addEventListener('install', () => skipWaiting());
addEventListener('activate', event => event.waitUntil(clients.claim()));

addEventListener('fetch', event => {
  if (event.request.mode !== 'navigate') return;
  event.respondWith(fetch(event.request).catch(() =>
    new Response(OFFLINE, { status: 503, headers: { 'Content-Type': 'text/html; charset=utf-8' } })));
});
