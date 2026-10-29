/* Bounded, loopback-only UI fixture. It never invokes TV services or writes configuration. */
var http = require('http'), fs = require('fs'), path = require('path'), url = require('url');
var args = process.argv.slice(2), imagePath = '', wallpaperMode = 'none';
if (args.length && args[0].indexOf('--') !== 0) imagePath = args.shift();
args.forEach(function (arg) {
  var m = /^--wallpaper=(none|ready|missing)$/.exec(arg);
  if (m) wallpaperMode = m[1];
});
var image = fs.readFileSync(imagePath || path.join(__dirname, '../assets/icon.png'));
var wallpaperSlots = { morning: true, afternoon: true, evening: true, night: true };
var port = Number(process.env.TV_FIXTURE_PORT || 18081);
var apps = [{ id: 'fixture.one', title: 'Fixture One', image: true },
  { id: 'fixture.two', title: 'Fixture Two', image: false }];
var server = http.createServer(function (request, response) {
  var parsed = url.parse(request.url, true);
  function reply(data) {
    response.writeHead(200, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify(data));
  }
  if (request.method === 'POST' && parsed.pathname === '/api/home/recents') {
    var body = '';
    request.on('data', function (bytes) { body += bytes; if (body.length > 4096) request.destroy(); });
    request.on('end', function () {
      var action;
      try { action = JSON.parse(body); } catch (e) { response.writeHead(400); return response.end(); }
      if (action.action === 'close') apps = apps.filter(function (app) { return app.id !== action.id; });
      if (action.action === 'closeAll') apps = [];
      reply({ ok: true, apps: apps });
    });
    return;
  }
  if (request.method !== 'GET') { response.writeHead(404); return response.end(); }
  if (parsed.pathname === '/api/caps') return reply({ setupNeeded: false, allowControl: true });
  if (parsed.pathname === '/api/home/catalogue') return reply({ ok: true,
    apps: [{ id: 'fixture.one', title: 'Fixture One' }], devices: [], channels: [],
    order: { app: ['fixture.one'], dev: [], ch: [] }, sources: { channels: 0 },
    profiles: [{ id: 'main', name: 'Main', active: true }],
    layout: { picture: true, showLive: false, showInputs: false, appNames: false, hidden: {} },
    wallpapers: { available: wallpaperMode !== 'none', uploading: false,
      slots: wallpaperMode === 'none' ? {} : wallpaperSlots, revision: 'fixture-wallpaper-v1' } });
  if (parsed.pathname === '/api/home/discover') return reply({ ok: true, titlesEnabled: true,
    rows: [{ key: 'trending', items: [{ id: 7, type: 'movie', title: 'Fixture Film',
      overview: 'Carousel text remains clear while a wallpaper is visible.',
      poster: '/fixture-poster.jpg', backdrop: '/fixture-backdrop.jpg' }] }] });
  if (parsed.pathname === '/api/home/now') return reply({ kind: 'input', port: 4 });
  if (parsed.pathname === '/api/home/picture') return reply({ ok: true,
    signal: { width: 3840, height: 2160, hz: 23.98, hdr: 'hdr10' }, mode: { display: 'HDR Filmmaker' } });
  if (parsed.pathname === '/api/home/recents') return reply({ ok: true, apps: apps });
  if (parsed.pathname === '/api/home/wallpaper') {
    if (wallpaperMode !== 'ready') { response.writeHead(404); return response.end(); }
    response.writeHead(200, { 'Content-Type': 'image/png' }); return response.end(image);
  }
  if (parsed.pathname === '/api/home/icon' || parsed.pathname === '/api/home/recents/image' ||
      parsed.pathname === '/api/home/image') {
    response.writeHead(200, { 'Content-Type': 'image/png' }); return response.end(image);
  }
  response.writeHead(404); response.end();
});
server.listen(port, '127.0.0.1', function () { console.log('Native UI fixture PID ' + process.pid); });
setTimeout(function () { server.close(); }, 120000).unref();
