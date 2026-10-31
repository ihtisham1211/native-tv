/*
 * Native Tv Home: tabs, a featured carousel and rows of cards, laid out as
 * Google TV's home. It reads /api/home/*; the order of the rows' tiles is
 * arranged on the dashboards. Strict ES5, as every page the TV's own browser
 * shows.
 */
(function () {
  var LG_HOME_TILE = 'lg-home';
  var KEY = { left: 37, up: 38, right: 39, down: 40, ok: 13, back: 461, red: 403 };
  var BEAT_MS = 5000;
  // The Apps tab's grid: six banners a row, a row 260px tall with its name.
  var GRID_COLUMNS = 6, GRID_PITCH = 260;
  // Where the rows start: the Home tab's under the carousel (#rows in home.css),
  // the Apps tab's near the top. A poster row is 285 tall, so it is raised to end inside the screen.
  var HOME_ROWS_TOP = 768, APPS_ROWS_TOP = 150, POSTER_LIFT = 75;

  var data = null, layout = null, guideData = {}, lastOn = { kind: null };
  var recents = { open: false, loading: false, busy: false, revision: 0, apps: [], list: [], col: 0, onX: false, onAll: false, stamp: 0 };
  var tabs = [], rows = [];
  var sectionSigs = {}, sectionRevs = {}, rowModels = {}, rowParts = {}, appCache = null;
  // At most four tabs, each retaining its bounded row/tile window. Apps also
  // reuses row shells from its previous window during rapid grid navigation.
  var views = {}, activeView = null, shownTab = '', tabsShown = '';
  // zone: 'row' is the content, 'nav' the bar; nav indexes the tabs, then the tools.
  var st = { tab: 0, zone: 'row', row: 0, nav: 0, cols: {} };
  var build = null, layoutVersion = null, beatFails = 0, loadRetry = null, initialEscape = false;
  var wallAvailable = false, wallSlots = null, wallRevision = '', wallShown = null, wallFront = 0, wallLoading = null;
  var okDown = false, backDown = false;
  // The row and tile nodes are found once per render; moving around the page
  // only repaints the two tiles whose focus changed.
  var rowEls = [], tileEls = [], rowsOff = null, focusTile = null;

  var elsById = {};
  function $(id) {
    var el = elsById[id];
    if (!el) { el = document.getElementById(id); if (el) elsById[id] = el; }
    return el;
  }
  function pad(n) { return (n < 10 ? '0' : '') + n; }
  function indexItems(list) {
    var index = {};
    (list || []).forEach(function (item) { index['$' + item.id] = item; });
    return index;
  }
  function fingerprint(key, value) {
    var sig = JSON.stringify(value);
    if (sectionSigs[key] !== sig) { sectionSigs[key] = sig; sectionRevs[key] = (sectionRevs[key] || 0) + 1; }
  }
  function rev(key) { return sectionRevs[key] || 0; }
  function fingerprintGuide() {
    fingerprint('guide', (layout && layout.channels || []).map(function (id) { return [id, (guideData[id] || [])[0] || null]; }));
  }
  function setCatalogue(r) {
    data = r; layout = r.layout;
    var hidden = layout.hidden || {};
    fingerprint('app', [r.apps, r.order.app, hidden.app, layout.showLgHome, layout.appNames]);
    fingerprint('dev', [r.devices, r.order.dev, hidden.dev]);
    fingerprint('ch', [r.channels, r.order.ch, hidden.ch, layout.channels, r.sources.channels]);
    fingerprint('shell', [layout.clockFormat, layout.colours, layout.appNames, layout.showLive, layout.showInputs, r.profiles,
      r.devices.length > 0, r.sources.channels === 0 ? 0 : r.sources.channels === null ? null : 1]);
    fingerprintGuide();
  }
  function clockTime(d) {
    var h = d.getHours();
    var text = (layout && layout.clockFormat === '12h' ? h % 12 || 12 : pad(h)) + ':' + pad(d.getMinutes());
    if (layout && layout.clockFormat === '12h') text += ' ' + (h < 12 ? t('home.clock.am', 'AM') : t('home.clock.pm', 'PM'));
    return text;
  }
  function dateText(d) {
    function part(opts) { try { return d.toLocaleDateString(undefined, opts); } catch (e) { return ''; } }
    return t('home.date', '{weekday} {day} {month}', { weekday: part({ weekday: 'long' }), day: d.getDate(), month: part({ month: 'long' }) });
  }

  function request(method, url, body, cb) {
    var x = new XMLHttpRequest();
    x.open(method, url, true);
    x.timeout = 8000;
    if (body !== null) x.setRequestHeader('Content-Type', 'application/json');
    x.onload = function () {
      var j = null;
      try { j = JSON.parse(x.responseText); } catch (e) {}
      cb(x.status >= 200 && x.status < 300 ? j : null, x.status);
    };
    x.onerror = x.ontimeout = function () { cb(null, 0); };
    x.send(body === null ? null : JSON.stringify(body));
  }
  function getJson(url, cb) { request('GET', url + (url.indexOf('?') === -1 ? '?' : '&') + 't=' + Date.now(), null, cb); }
  function postJson(url, body, cb) { request('POST', url, body || {}, cb || function () {}); }

  function fit() {
    var s = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
    var x = (window.innerWidth - 1920 * s) / 2, y = (window.innerHeight - 1080 * s) / 2;
    $('stage').style.webkitTransform = $('stage').style.transform = 'translate(' + x + 'px,' + y + 'px) scale(' + s + ')';
  }

  var toastTimer = null;
  function toast(main, detail) {
    var el = $('toast');
    el.innerHTML = esc(main) + (detail ? '<small>' + esc(detail) + '</small>' : '');
    el.className = 'show';
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.className = ''; }, 3600);
  }

  // ---------------------------------------------------------------- what is shown

  function tabList() {
    var out = [{ key: 'home', title: t('home.tv.tab.home', 'Home') }];
    if (layout.showLive === true && data && data.sources && data.sources.channels !== 0) out.push({ key: 'live', title: t('home.tv.tab.live', 'Live') });
    out.push({ key: 'apps', title: t('home.tv.tab.apps', 'Apps') });
    if (data && layout.showInputs !== false && data.devices.length) out.push({ key: 'inputs', title: t('home.tv.tab.inputs', 'Inputs') });
    return out;
  }

  // The ids of a kind in the layout's order, less what is hidden and, for apps, a LG Home tile switched off.
  function visibleIds(kind) {
    var hidden = {};
    ((layout.hidden && layout.hidden[kind]) || []).forEach(function (id) { hidden['$' + id] = true; });
    return (data.order[kind] || []).filter(function (id) {
      return !hidden['$' + id] && !(kind === 'app' && id === LG_HOME_TILE && layout.showLgHome === false);
    });
  }
  function appItems() {
    if (appCache && appCache.revision === rev('app')) return appCache.items;
    var out = [], index = indexItems(data.apps);
    visibleIds('app').forEach(function (id) {
      if (id === LG_HOME_TILE) return out.push({ kind: 'lg' });
      var app = index['$' + id];
      if (app) out.push({ kind: 'app', app: app });
    });
    appCache = { revision: rev('app'), items: out };
    return out;
  }
  function deviceItems() {
    var out = [], index = indexItems(data.devices);
    visibleIds('dev').forEach(function (id) { var d = index['$' + id]; if (d) out.push({ kind: 'dev', dev: d }); });
    return out;
  }
  function channelItems() {
    var out = [], index = indexItems(data.channels);
    visibleIds('ch').forEach(function (id) { var c = index['$' + id]; if (c) out.push({ kind: 'ch', ch: c }); });
    return out;
  }

  function hdmiPort(id) { var m = /^com\.webos\.app\.hdmi(\d)$/.exec(String(id)); return m ? +m[1] : 0; }
  function snapUrl(a) { return '/api/home/recents/image?id=' + encodeURIComponent(a.id) + '&v=' + recents.stamp; }

  // TMDB's source sizes; the local image endpoint keeps fetched art on disk.
  var IMG = 'https://image.tmdb.org/t/p/';
  // Every picture comes from the TV, which keeps what it fetched (lib/home-discover).
  function via(url, hero) { return url ? '/api/home/image?u=' + encodeURIComponent(url) + (hero ? '&priority=hero' : '') : ''; }
  var discover = { enabled: false, titlesEnabled: false, rows: [] };
  var DISCOVER_ROWS = {
    trending: t('home.tv.row.trending', 'Trending today'),
    movies: t('home.tv.row.movies', 'Trending films this week'),
    series: t('home.tv.row.series', 'Trending series this week'),
    anime: t('home.tv.row.anime', 'Anime airing this week'),
    youtube: t('home.tv.row.youtube', 'Popular on YouTube'),
    toprated: t('home.tv.row.toprated', 'Top rated in this feed'),
    recent: t('home.tv.row.recent', 'Recent releases in this feed')
  };
  function discoverRow(key) {
    for (var i = 0; i < discover.rows.length; i++) if (discover.rows[i].key === key) return discover.rows[i];
    return null;
  }
  function titleItems(row) { return row.items.map(function (x) { return { kind: 'title', title: x, row: row.key }; }); }
  function refreshDiscover() {
    getJson('/api/home/discover', function (r) {
      if (!r || !r.ok) return;
      discover = { enabled: !!r.enabled, titlesEnabled: r.titlesEnabled === true, rows: r.rows || [] };
      if (!discover.titlesEnabled) GhContent.disableTitles();
      fingerprint('discover', discover);
      discover.rows.forEach(function (row) { fingerprint('discover:' + row.key, row); });
      renderIfChanged();
    });
  }

  function makeRow(title, view, items, previous) {
    var sig = JSON.stringify([title, view, items, layout.appNames]);
    return previous && previous.__ghSignature === sig ? previous : { title: title, view: view, items: items, __ghSignature: sig };
  }
  function partRow(key, revision, title, view, items) {
    var cached = rowParts[key];
    if (!cached || cached.revision !== revision) {
      rowParts[key] = { revision: revision, row: makeRow(title, view, items(), cached && cached.row) };
    }
    return rowParts[key].row;
  }
  function modelStamp(key) {
    return key === 'home' ? rev('app') + ':' + rev('discover') : key === 'apps' ? rev('app') : key === 'live' ? rev('ch') : rev('dev');
  }
  function buildRows() {
    var key = tabs[st.tab].key, stamp = modelStamp(key), cached = rowModels[key];
    if (cached && cached.stamp === stamp) rows = cached.rows;
    else if (key === 'home') {
      rows = [];
      // Only what has a picture to show: an app's logo on a dark card is not worth a place.
      var top = discoverRow('trending');
      rows.push(partRow('home:app', rev('app'), t('home.tv.row.apps', 'Your apps'), 'sq', appItems));
      if (top) rows.push(partRow('home:trending', rev('discover:trending'), DISCOVER_ROWS.trending, 'disc', function () { return titleItems(top); }));
      var video = discoverRow('youtube');
      if (video) rows.push(partRow('home:youtube', rev('discover:youtube'), DISCOVER_ROWS.youtube, 'disc', function () { return titleItems(video); }));
      ['movies', 'series', 'anime', 'toprated', 'recent'].forEach(function (k) {
        var row = discoverRow(k);
        if (row) rows.push(partRow('home:' + k, rev('discover:' + k), DISCOVER_ROWS[k], 'poster', function () { return titleItems(row); }));
      });
    } else if (key === 'live') {
      rows = [partRow('live', rev('ch'), t('home.tv.row.onNow', 'On now'), 'live', channelItems)];
    } else if (key === 'apps') {
      var all = appItems();
      rows = [];
      for (var i = 0; i < all.length; i += GRID_COLUMNS) rows.push(makeRow('', 'grid', all.slice(i, i + GRID_COLUMNS), cached && cached.rows[i / GRID_COLUMNS]));
    } else {
      rows = [partRow('inputs', rev('dev'), t('home.row.inputs', 'Inputs'), 'inp', deviceItems)];
    }
    if (!rows.length) rows = [makeRow('', 'sq', [], null)];
    rowModels[key] = { stamp: stamp, rows: rows };
    if (st.row >= rows.length) st.row = rows.length - 1;
  }
  // Where each tab's rows were left, keyed by tab key: adding or removing a
  // tab swaps the indexes, and an index would carry a column to another tab.
  function colKey(ri) { return (tabs[st.tab] ? tabs[st.tab].key : '') + ':' + ri; }
  function colOf(ri) { return Math.max(0, Math.min(st.cols[colKey(ri)] || 0, rows[ri].items.length - 1)); }
  function focused() { return rows[st.row] && rows[st.row].items[colOf(st.row)]; }

  // ---------------------------------------------------------------- tiles

  function guideOn(ch) { var s = guideData[ch.id] || []; return s[0] || null; }
  function pct(show) { return Math.max(0, Math.min(100, (Date.now() - show.start) / (show.end - show.start) * 100)); }
  function minutesLeft(show) { return Math.max(1, Math.round((show.end - Date.now()) / 60000)); }

  // Line icons on a 24 grid, drawn alike: round caps and joins, one weight on screen (px of stroke, not of the grid).
  var ICONS = {
    hdmi: '<path d="M2.5 5.5h19v7.2l-3.4 5.8H5.9l-3.4-5.8z"/><path d="M8 9.2v3M12 9.2v3M16 9.2v3"/>',
    game: '<path d="M7.5 8h9a4.5 4.5 0 014.4 3.6l.7 3.5a2.6 2.6 0 01-4.5 2.2L15 15.5H9l-2.1 1.8a2.6 2.6 0 01-4.5-2.2l.7-3.5A4.5 4.5 0 017.5 8z"/><path d="M8 10.5v3M6.5 12h3"/><path d="M15.5 11.3h.01M17.5 13h.01"/>',
    speaker: '<rect x="6" y="2.8" width="12" height="18.4" rx="2.6"/><circle cx="12" cy="14.5" r="3.4"/><circle cx="12" cy="7.2" r=".9"/>',
    stick: '<rect x="3" y="8" width="14" height="8" rx="2"/><path d="M17 10.5h2.8a1.2 1.2 0 011.2 1.2v.6a1.2 1.2 0 01-1.2 1.2H17"/>',
    monitor: '<rect x="3" y="4.5" width="18" height="11.5" rx="2"/><path d="M9 20h6M12 16v4"/>',
    tv: '<rect x="3" y="7.5" width="18" height="12" rx="2"/><path d="M8 3l4 4.5L16 3"/>',
    home: '<path d="M4 11l8-7 8 7"/><path d="M6 9.6V20h12V9.6"/><path d="M10 20v-5.2h4V20"/>',
    volume: '<path d="M4 9.6v4.8h3.6L12.5 19V5L7.6 9.6z"/><path d="M15.8 9.2a4 4 0 010 5.6"/><path d="M18.4 6.6a7.6 7.6 0 010 10.8"/>',
    picture: '<rect x="3.5" y="4.5" width="17" height="15" rx="2.2"/><path d="M3.5 15.5l4.6-4.6 4 4 2.6-2.6 5.8 5.8"/><circle cx="15.6" cy="9.2" r="1.3"/>',
    light: '<circle cx="12" cy="12" r="3.8"/><path d="M12 3v2.2M12 18.8V21M3 12h2.2M18.8 12H21M5.6 5.6l1.6 1.6M16.8 16.8l1.6 1.6M5.6 18.4l1.6-1.6M16.8 7.2l1.6-1.6"/>',
    clock: '<circle cx="12" cy="13" r="7.5"/><path d="M12 9.2v4l2.8 1.8"/><path d="M9.6 3.2h4.8"/>',
    power: '<path d="M12 3.5v8"/><path d="M7.2 6.6a7.6 7.6 0 109.6 0"/>',
    profile: '<circle cx="12" cy="8.5" r="3.6"/><path d="M4.8 20c.6-3.8 3.6-6 7.2-6s6.6 2.2 7.2 6"/>',
    add: '<path d="M12 5v14M5 12h14"/>',
    gear: '<path d="M18.6 10L21.1 10.6V13.4L18.6 14 18.1 15.2 19.4 17.4 17.4 19.4 15.2 18.1 14 18.6 13.4 21.1H10.6L10 18.6 8.8 18.1 6.6 19.4 4.6 17.4 5.9 15.2 5.4 14 2.9 13.4V10.6L5.4 10 5.9 8.8 4.6 6.6 6.6 4.6 8.8 5.9 10 5.4 10.6 2.9H13.4L14 5.4 15.2 5.9 17.4 4.6 19.4 6.6 18.1 8.8z"/><circle cx="12" cy="12" r="2.8"/>'
  };
  function icon(name, px) {
    return '<svg class="ico" width="' + px + '" height="' + px + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="' + (3 * 24 / px).toFixed(2) +
      '" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + ICONS[name] + '</svg>';
  }
  // What a device is, from the name its owner gave it; an unnamed input is a plain HDMI port.
  function deviceGlyph(name) {
    var n = String(name || '');
    if (/play ?station|\bps[2-5]\b|xbox|nintendo|switch|console|game/i.test(n)) return 'game';
    if (/stereo|receiver|avr|sound ?bar|audio|denon|yamaha|marantz|sonos|onkyo|bose|sony ht|hifi|hi-fi/i.test(n)) return 'speaker';
    if (/apple ?tv|chromecast|fire ?(tv|stick)|roku|shield|stick|stream|cast|box/i.test(n)) return 'stick';
    if (/\bpc\b|computer|laptop|mac|monitor|desktop/i.test(n)) return 'monitor';
    return 'hdmi';
  }
  // An input as an icon and a label, with the device's name under it where it has one.
  function inputBody(port, name, unused) {
    var label = t('home.dev.port', 'HDMI {port}', { port: port });
    var sub = unused ? t('home.dev.unused', 'Nothing connected') : name && name !== label ? name : t('home.dev.connected', 'Connected');
    return icon(unused ? 'hdmi' : deviceGlyph(name), 52) + '<div class="txt"><b>' + esc(label) + '</b><span>' + esc(sub) + '</span></div>';
  }
  // The app's brand colour as 'r,g,b', left out where it is white, black or grey: those tint nothing.
  function brandRgb(app) {
    var m = app && /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/.exec(app.color || ''), c;
    if (!m) return null;
    c = [parseInt(m[1], 16), parseInt(m[2], 16), parseInt(m[3], 16)];
    return Math.max(c[0], c[1], c[2]) - Math.min(c[0], c[1], c[2]) < 40 ? null : c.join(',');
  }
  function glow() { return '<i class="glow"></i>'; }
  function titlePic(ti, view) {
    return ti.image ? ti.image : view === 'disc' ? (ti.backdrop ? IMG + 'w500' + ti.backdrop : ti.poster ? IMG + 'w342' + ti.poster : '') : (ti.poster ? IMG + 'w342' + ti.poster : '');
  }
  function imageSrc(it, view) {
    if (it.kind === 'app') return it.app.icon || '';
    if (it.kind === 'title') return via(titlePic(it.title, view));
    return '';
  }

  // Extend a flat icon background across the banner and fit the visible logo,
  // not its transparent padding. Sample only once per source, at 48px; keep
  // neither canvases nor decoded copies of the app catalogue in memory.
  var appArt = {}, appArtKeys = [], artCanvas = null;
  // Padding is not image detail: a small logo inside a large transparent
  // canvas must not be enlarged several times just to fill the banner.
  var APP_ART_MAX_SCALE = 1.25;
  function fitAppArt(img, shot, src, title) {
    var art = appArt['$' + src];
    if (!art) {
      var w = img.naturalWidth, h = img.naturalHeight;
      if (!w || !h) return;
      var fit = Math.min(240 / w, 135 / h, APP_ART_MAX_SCALE);
      art = { background: '', ink: '#ffffff', left: (240 - w * fit) / 2, top: (135 - h * fit) / 2, width: w * fit, height: h * fit, compact: false };
      var bounds = { left: 0, top: 0, right: 48, bottom: 48 };
      try {
        if (!artCanvas) { artCanvas = document.createElement('canvas'); artCanvas.height = 48; }
        // Reset origin cleanliness too, in case a previous icon was foreign.
        artCanvas.width = 48;
        var ctx = artCanvas.getContext('2d');
        ctx.clearRect(0, 0, 48, 48);
        ctx.drawImage(img, 0, 0, 48, 48);
        var pixels = ctx.getImageData(0, 0, 48, 48).data;
        var edge = [], clear = 0, x, y, at, i, candidate, matches = 0, rgb = null;
        for (y = 0; y < 48; y++) for (x = 0; x < 48; x++) {
          if (x !== 0 && x !== 47 && y !== 0 && y !== 47) continue;
          at = (y * 48 + x) * 4;
          if (pixels[at + 3] < 32) clear++;
          else if (pixels[at + 3] > 240) edge.push([pixels[at], pixels[at + 1], pixels[at + 2]]);
        }
        // A majority of matching edge pixels means a solid square's colour
        // can safely continue into the sides of the wide card.
        if (edge.length > 96) {
          candidate = edge[Math.floor(edge.length / 2)];
          for (i = 0; i < edge.length; i++) {
            if (Math.abs(edge[i][0] - candidate[0]) < 24 && Math.abs(edge[i][1] - candidate[1]) < 24 && Math.abs(edge[i][2] - candidate[2]) < 24) matches++;
          }
          if (matches > edge.length * .85) {
            rgb = candidate; art.background = 'rgb(' + rgb.join(',') + ')';
            if (rgb[0] * .299 + rgb[1] * .587 + rgb[2] * .114 > 160) art.ink = '#202124';
          }
        }
        if (rgb || clear > 96) {
          var left = 48, right = -1, top = 48, bottom = -1;
          for (y = 0; y < 48; y++) for (x = 0; x < 48; x++) {
            at = (y * 48 + x) * 4;
            if (pixels[at + 3] < 32 || (rgb && Math.abs(pixels[at] - rgb[0]) < 32 && Math.abs(pixels[at + 1] - rgb[1]) < 32 && Math.abs(pixels[at + 2] - rgb[2]) < 32)) continue;
            left = Math.min(left, x); right = Math.max(right, x); top = Math.min(top, y); bottom = Math.max(bottom, y);
          }
          if (right >= left && bottom >= top) {
            bounds = { left: left, top: top, right: right + 1, bottom: bottom + 1 };
            // A ready-made 16:9 banner keeps its composition. Wide sources
            // with substantial padding are logos and benefit from fitting.
            var banner = w / h >= 1.5 && w / h <= 2;
            if (!banner) {
              var scale = Math.min(196 / ((right - left + 1) * w / 48), 100 / ((bottom - top + 1) * h / 48), APP_ART_MAX_SCALE);
              art.width = w * scale; art.height = h * scale;
              art.left = 120 - (left + right + 1) * art.width / 96;
              art.top = 67.5 - (top + bottom + 1) * art.height / 96;
            }
          }
        }
      } catch (e) { /* A foreign or unreadable icon keeps the CSS contain fallback. */ }
      var markWidth = (bounds.right - bounds.left) * art.width / 48;
      var markHeight = (bounds.bottom - bounds.top) * art.height / 48;
      // A small source stays crisp as a logo/name lockup rather than a tiny
      // badge in an otherwise empty banner. Never magnify it to fill space.
      if (Math.max(markWidth, markHeight) < 72 || (Math.min(markWidth, markHeight) < 24 && Math.max(markWidth, markHeight) < 120)) {
        art.compact = true;
        var compactScale = Math.min(56 / ((bounds.right - bounds.left) * w / 48), 56 / ((bounds.bottom - bounds.top) * h / 48), APP_ART_MAX_SCALE);
        art.width = w * compactScale; art.height = h * compactScale;
        art.left = 44 - (bounds.left + bounds.right) * art.width / 96;
        art.top = 67.5 - (bounds.top + bounds.bottom) * art.height / 96;
      }
      appArt['$' + src] = art;
      appArtKeys.push(src);
      if (appArtKeys.length > 128) delete appArt['$' + appArtKeys.shift()];
    }
    // Clear the previous source's fit when a retained tile changes artwork.
    if (shot.__ghAppBackground === undefined) shot.__ghAppBackground = shot.style.background;
    shot.style.background = art.background || shot.__ghAppBackground;
    img.style.left = art.left + 'px'; img.style.top = art.top + 'px';
    img.style.width = art.width + 'px'; img.style.height = art.height + 'px';
    var name = shot.querySelector('.app-wordmark');
    if (art.compact) {
      if (name && name.parentNode) name.parentNode.removeChild(name);
      if (!name) name = document.createElement('span');
      var label = title || '';
      name.className = 'app-wordmark compact' + (label.length > 14 ? ' long' : '');
      name.style.color = art.ink;
      name.innerHTML = '<span>' + esc(label) + '</span>';
      shot.appendChild(name);
    } else if (name && name.parentNode) name.parentNode.removeChild(name);
  }

  function tileHtml(it, view, ri, ci) {
    var at = ' data-r="' + ri + '" data-c="' + ci + '"', body = '';
    if (view === 'disc' || view === 'poster') {
      var ti = it.title, pic = via(titlePic(ti, view));
      body = '<div class="face ' + view + (pic ? '' : ' nopic') + '"><div class="shot"></div>' +
        '<div class="cap">' + esc(ti.title) + '</div>' + '<div class="rim"></div><div class="ring"></div></div>';
    } else if (view === 'live') {
      var on = guideOn(it.ch);
      body = '<div class="face live"><div class="num">' + esc(it.ch.number) + '</div>' + (on ? '<span class="badge">' + esc(t('home.tv.live', 'LIVE')) + '</span>' : '') +
        '<div class="cap">' + esc(it.ch.name) + '</div>' + (on ? '<div class="bar"><i style="width:' + pct(on).toFixed(1) + '%"></i></div>' : '') + glow() + '<div class="rim"></div><div class="ring"></div></div>';
    } else if (view === 'inp') {
      var d = it.dev;
      body = '<div class="face inp' + (d.unused ? ' unused' : '') + '">' + inputBody(d.port, d.name, d.unused) + glow() + '<div class="rim"></div><div class="ring"></div></div>';
    } else {
      // The same branded banner on Home and in the Apps grid.
      var lg = it.kind === 'lg', tint = lg ? null : brandRgb(it.app);
      body = '<div class="face sq"><div class="shot' + (lg ? ' lgtile' : '') + '"' + (tint ? ' style="background:linear-gradient(150deg,rgba(' + tint + ',.75),rgba(' + tint + ',.35))"' : '') + '>' +
        '<span class="app-wordmark' + (!lg && String(it.app.title).length > 14 ? ' long' : '') + '">' + (lg ? icon('home', 36) : '') + esc(lg ? t('home.lgHome', 'LG Home') : it.app.title) + '</span></div>' +
        glow() + '<div class="rim"></div><div class="ring"></div></div><div class="name">' + esc(lg ? t('home.lgHome', 'LG Home') : it.app.title) + '</div>';
    }
    var label = it.kind === 'app' ? it.app.title : it.kind === 'lg' ? t('home.lgHome', 'LG Home') : it.kind === 'dev' ? it.dev.name : it.kind === 'ch' ? it.ch.name : it.title.title;
    // A title's tile is a pick: .title is the carousel's.
    var cls = 'tile ' + (it.kind === 'title' ? 'pick' : it.kind) + (view === 'grid' ? ' sqt grid' : view === 'sq' ? ' sqt' : '');
    return { cls: cls, label: label, body: body,
      html: '<div class="' + cls + '" role="button" aria-label="' + esc(label) + '"' + at + '>' + body + '</div>' };
  }

  function rowHeight(r) {
    if (r.view === 'grid') return layout.appNames === false ? 180 : GRID_PITCH;
    return (r.title ? 46 : 0) + 44 + (r.view === 'sq' ? (layout.appNames === false ? 135 : 215) : r.view === 'poster' ? 285 : 180);
  }
  function tileWidth(r) { return r.view === 'sq' || r.view === 'grid' ? 240 : r.view === 'poster' ? 190 : 320; }
  // How far the rows are scrolled for the one in focus: its own top, and a poster row's raised by POSTER_LIFT.
  function scrollOf(ri) { return activeView.tops[ri] + (rows[ri].view === 'poster' ? POSTER_LIFT : 0); }
  function itemKey(it) {
    return it.kind + ':' + (it.kind === 'app' ? it.app.id : it.kind === 'dev' ? it.dev.id : it.kind === 'ch' ? it.ch.id : it.kind === 'title' ? it.title.type + ':' + it.title.id : LG_HOME_TILE);
  }
  function makeTile(markup, ri, ci, recycled) {
    var node = recycled || document.createElement('div');
    delete node.__ghIconSrc;
    delete node.__ghPicSrc;
    node.__ghImageFailure = null;
    node.className = markup.cls;
    node.setAttribute('role', 'button');
    node.setAttribute('aria-label', markup.label);
    node.setAttribute('data-r', ri);
    node.setAttribute('data-c', ci);
    node.innerHTML = markup.body;
    return node;
  }
  function releaseImages(el) {
    var pictures = el.querySelectorAll('img');
    for (var i = 0; i < pictures.length; i++) {
      if (pictures[i].__ghRelease) pictures[i].__ghRelease(true);
      pictures[i].onload = pictures[i].onerror = null;
      pictures[i].removeAttribute('src');
    }
  }
  function cancelPendingImages(el) {
    var pictures = el.querySelectorAll('img');
    for (var i = 0; i < pictures.length; i++) if (pictures[i].__ghRelease) {
      cancelImage(pictures[i]);
    }
  }
  function cancelImage(img) {
    if (img.__ghRelease) img.__ghRelease(true);
    img.onload = img.onerror = null;
    img.removeAttribute('src');
    if (img.parentNode) img.parentNode.removeChild(img);
  }
  // Rows have known heights and tiles known widths. Empty space outside this
  // window keeps the same geometry without retaining off-screen DOM/images.
  function mountWindow() {
    if (!activeView) return;
    var grid = tabs[st.tab].key === 'apps';
    var first = Math.max(0, st.row - (grid ? GRID_ABOVE : 1));
    var last = Math.min(rows.length - 1, st.row + (grid ? gridBelow() : 2));
    var ri, ci, el, r, record, key, markup, track, count, start, end, before;
    var ready = activeView.mountedRevision === activeView.revision && activeView.first === first && activeView.last === last;
    for (ri = first; ready && ri <= last; ri++) {
      el = activeView.nodes[ri];
      var at = colOf(ri);
      if (!el || (el.__ghStart > 0 && at < el.__ghStart + 2) || (el.__ghEnd < rows[ri].items.length - 1 && at > el.__ghEnd - 2)) ready = false;
    }
    if (ready) return;
    for (key in activeView.nodes) {
      ri = +key;
      if (ri < first || ri > last || ri >= rows.length) {
        el = activeView.nodes[key];
        releaseImages(el);
        if (focusTile && el.contains(focusTile)) focusTile = null;
        if (el.parentNode) el.parentNode.removeChild(el);
        if (grid) {
          var pool = activeView.pool || (activeView.pool = []);
          if (pool.length < GRID_ABOVE + gridBelow() + 1) {
            el.__ghFreeTiles = [];
            for (var oldKey in el.__ghTiles) el.__ghFreeTiles.push(el.__ghTiles[oldKey].node);
            el.__ghTiles = {};
            el.__ghSignature = null; el.__ghTrack = null;
            delete el.__ghStart; delete el.__ghEnd;
            pool.push(el);
          }
        }
        delete activeView.nodes[key];
      }
    }
    rowEls = []; tileEls = [];
    for (ri = first; ri <= last; ri++) {
      r = rows[ri]; el = activeView.nodes[ri];
      if (!el) {
        el = grid && activeView.pool && activeView.pool.length ? activeView.pool.pop() : null;
        if (!el) {
          el = document.createElement('section');
          el.innerHTML = '<div class="rowhead"></div><div class="wrap"><div class="track"></div></div><div class="empty"></div>';
          el.__ghTiles = {};
          el.__ghFreeTiles = [];
        }
        el.className = 'row'; el.setAttribute('data-row', ri);
        el.querySelector('.wrap').setAttribute('data-rowwrap', ri);
        activeView.nodes[ri] = el;
      }
      if (el.__ghSignature !== activeView.signatures[ri]) {
        el.__ghTrack = null;
        el.__ghSignature = activeView.signatures[ri];
      }
      var rowClass = 'row' + (ri === st.row ? ' cur' : '') + (tabs[st.tab].key === 'home' && ri < st.row ? ' past' : '');
      if (el.className !== rowClass) el.className = rowClass;
      if (el.parentNode !== $('rows')) $('rows').appendChild(el);
      var top = activeView.tops[ri] + 'px', height = rowHeight(r) + 'px';
      if (el.style.top !== top) el.style.top = top;
      if (el.style.height !== height) el.style.height = height;
      var head = el.querySelector('.rowhead');
      if (head.textContent !== r.title) head.textContent = r.title;
      var headDisplay = r.title ? '' : 'none';
      if (head.style.display !== headDisplay) head.style.display = headDisplay;
      var empty = el.querySelector('.empty');
      var emptyDisplay = r.items.length ? 'none' : '';
      if (empty.style.display !== emptyDisplay) empty.style.display = emptyDisplay;
      if (!r.items.length && empty.textContent !== emptyNote(tabs[st.tab].key)) empty.textContent = emptyNote(tabs[st.tab].key);
      var wrap = el.querySelector('.wrap');
      var wrapDisplay = r.items.length ? '' : 'none';
      if (wrap.style.display !== wrapDisplay) wrap.style.display = wrapDisplay;
      track = el.querySelector('.track'); count = r.items.length;
      var col = colOf(ri), ahead = r.view === 'poster' ? 10 : IMAGE_AHEAD;
      start = grid ? 0 : Math.max(0, col - ahead);
      end = grid ? count - 1 : Math.min(count - 1, col + ahead);
      if (!grid && el.__ghStart !== undefined && (el.__ghStart === 0 || col >= el.__ghStart + 2) && (el.__ghEnd === count - 1 || col <= el.__ghEnd - 2)) {
        start = el.__ghStart; end = Math.min(count - 1, el.__ghEnd);
      }
      el.__ghStart = start; el.__ghEnd = end;
      var kept = {}, nodes = [], indexed = [];
      for (ci = start; ci <= end; ci++) {
        key = itemKey(r.items[ci]); record = el.__ghTiles[key];
        if (!record || record.revision !== activeView.signatures[ri]) {
          markup = tileHtml(r.items[ci], r.view, ri, ci);
          if (!record || record.html !== markup.html) record = {
            node: makeTile(markup, ri, ci, el.__ghFreeTiles.pop()), html: markup.html };
          record.revision = activeView.signatures[ri];
        }
        kept[key] = record; nodes.push(record.node); indexed[ci] = record.node;
      }
      for (ci = 0; ci < el.__ghFreeTiles.length; ci++) {
        var spare = el.__ghFreeTiles[ci];
        if (spare.parentNode) spare.parentNode.removeChild(spare);
      }
      // Retain the nodes that still belong here, including decoded images.
      for (key in el.__ghTiles) {
        record = el.__ghTiles[key];
        if (!kept[key] || kept[key].node !== record.node) {
          releaseImages(record.node);
          if (record.node.parentNode) record.node.parentNode.removeChild(record.node);
        }
      }
      before = track.firstChild;
      for (ci = 0; ci < nodes.length; ci++) {
        if (nodes[ci] !== before) track.insertBefore(nodes[ci], before);
        else before = before.nextSibling;
      }
      el.__ghTiles = kept;
      var padLeft = start * (tileWidth(r) + 24) + 'px';
      if (track.style.paddingLeft !== padLeft) track.style.paddingLeft = padLeft;
      rowEls[ri] = el; tileEls[ri] = indexed;
    }
    activeView.first = first; activeView.last = last; activeView.mountedRevision = activeView.revision;
  }

  // Chromium 38 ignores loading="lazy". Only mounted tiles have pictures;
  // nearby tiles keep theirs until they leave the bounded DOM window.
  var IMAGE_AHEAD = 8, TITLE_AHEAD = 5, TITLE_VISIBLE = 8, GRID_ABOVE = 2, GRID_BELOW = 3, imageTimer = 0;
  var IMAGE_BATCH = 2, IMAGE_INFLIGHT = 4, IMAGE_STEP_MS = 16, IMAGE_IDLE_MS = 180;
  var IMAGE_TIMEOUT_MS = 12000, IMAGE_RETRY_MS = 1000, IMAGE_RETRY_MAX_MS = 30000;
  var imageActive = [], imageNextAt = 0, imageTimerAt = 0;
  function gridBelow() { return layout.appNames === false ? GRID_BELOW + 2 : GRID_BELOW; }
  function imageTime() { return window.performance && window.performance.now ? window.performance.now() : Date.now(); }
  function imageRetryDelay(attempts) { return Math.min(IMAGE_RETRY_MAX_MS, IMAGE_RETRY_MS * Math.pow(2, attempts - 1)); }
  function imageStarted(img, tile, field) {
    imageActive.push(img);
    img.__ghTile = tile;
    var timer = setTimeout(function () { if (img.__ghRelease && img.onerror) img.onerror(); }, IMAGE_TIMEOUT_MS);
    img.__ghRelease = function (cancelled) {
      if (!img.__ghRelease) return;
      clearTimeout(timer);
      img.__ghRelease = null;
      img.__ghTile = null;
      imageActive.splice(imageActive.indexOf(img), 1);
      if (cancelled) delete tile[field];
      if (!cancelled) scheduleImages();
    };
    return img.__ghRelease;
  }
  function loadTileImage(task) {
    var tile = task.tile, it = task.item, field = it.kind === 'app' ? '__ghIconSrc' : '__ghPicSrc';
    var src = imageSrc(it, task.view), shot = tile.querySelector('.shot');
    if (!shot) return;
    // A retained tile can get a different source without different markup.
    // Cancel its older pending request; keep decoded art until the new one loads.
    cancelPendingImages(shot);
    tile[field] = src;
    var img = document.createElement('img'), done = imageStarted(img, tile, field);
    img.alt = '';
    img.onload = function () {
      if (!img.__ghRelease) return;
      tile.__ghImageFailure = null;
      done();
      var previous = shot.querySelectorAll('img');
      for (var i = 0; i < previous.length; i++) if (previous[i] !== img) cancelImage(previous[i]);
      if (it.kind === 'app') fitAppArt(img, shot, src, it.app.title);
      var mono = shot.querySelector('.mono'), face = tile.querySelector('.face');
      if (mono && mono.parentNode) mono.parentNode.removeChild(mono);
      if (face) face.className = face.className.replace(/ ?\bnopic\b/, '');
    };
    img.onerror = function () {
      if (!img.__ghRelease) return;
      var previous = tile.__ghImageFailure;
      var attempts = previous && previous.src === src ? Math.min(previous.attempts + 1, 6) : 1;
      tile.__ghImageFailure = { src: src, attempts: attempts,
        at: imageTime() + imageRetryDelay(attempts) };
      delete tile[field];
      done();
      var face = tile.querySelector('.face');
      if (it.kind === 'title' && face && (' ' + face.className + ' ').indexOf(' nopic ') < 0) face.className += ' nopic';
      cancelImage(img);
    };
    shot.appendChild(img);
    img.src = src + (it.kind === 'title' && task.priority === 3 ? '&priority=focus' : '');
  }
  function imagePriority(tile) {
    var ri = +tile.getAttribute('data-r'), col = +tile.getAttribute('data-c');
    if (!rows[ri] || !rowEls[ri] || tileEls[ri][col] !== tile) return 0;
    if (ri === st.row && col === colOf(st.row)) return 3;
    // Logical geometry avoids layout reads after focus/track style writes.
    var base = shownTab === 'apps' ? APPS_ROWS_TOP : HOME_ROWS_TOP, top = base + activeView.tops[ri] - (rowsOff || 0);
    var width = tileWidth(rows[ri]), metrics = rowEls[ri].__ghTrack;
    var left = col * (width + 24) - (metrics ? metrics.x : 0);
    return top < 1080 && top + rowHeight(rows[ri]) > base && left < 1728 && left + width > 0 ? 2 : 1;
  }
  function prioritizeImages(now, idle) {
    if (!idle) imageActive.slice().forEach(function (img) {
      if (imagePriority(img.__ghTile) < 2) cancelImage(img);
    });
    var tile = tileEls[st.row] && tileEls[st.row][colOf(st.row)], item = focused();
    if (!tile || !item || (item.kind !== 'app' && item.kind !== 'title')) return;
    var src = imageSrc(item, rows[st.row].view), failure = tile.__ghImageFailure;
    if (!src || (item.kind === 'app' ? tile.__ghIconSrc : tile.__ghPicSrc) === src ||
        (failure && failure.src === src && failure.at > now) || imageActive.length < IMAGE_INFLIGHT) return;
    // Give the new focus a slot even when all older requests are still visible.
    var victim = null, score = -1;
    imageActive.forEach(function (img) {
      var t = img.__ghTile, distance = Math.abs(+t.getAttribute('data-r') - st.row) * GRID_COLUMNS + Math.abs(+t.getAttribute('data-c') - colOf(st.row));
      if (t !== tile && distance > score) { victim = img; score = distance; }
    });
    if (victim) cancelImage(victim);
  }
  function scheduleFocusedRetry(now) {
    var tile = tileEls[st.row] && tileEls[st.row][colOf(st.row)], failure = tile && tile.__ghImageFailure;
    if (failure && failure.at > now) scheduleImages(failure.at - now);
  }
  function loadImages() {
    if (!data || !layout || !tabs.length || document.hidden || recents.open) return;
    var key = tabs[st.tab].key;
    if (key !== 'home' && key !== 'apps') return;
    var now = imageTime(), idleFor = Date.now() - lastInputAt;
    if (now < imageNextAt) return scheduleImages(imageNextAt - now);
    var idle = idleFor >= IMAGE_IDLE_MS;
    prioritizeImages(now, idle);
    if (imageActive.length >= IMAGE_INFLIGHT) { scheduleFocusedRetry(now); return; }
    var first = st.row, last = st.row, ri, row, tiles, ci, start, end, focusCol, tasks = [], near = false, retryAt = Infinity;
    function consider(tile, item, view, rowIndex, col) {
      if (!tile || !item || (item.kind !== 'app' && item.kind !== 'title')) return;
      var src = imageSrc(item, view);
      if (!src || (item.kind === 'app' ? tile.__ghIconSrc : tile.__ghPicSrc) === src) return;
      var priority = imagePriority(tile);
      if (priority === 1 && !idle) { near = true; return; }
      var failure = tile.__ghImageFailure;
      if (failure && failure.src === src && failure.at > now) { retryAt = Math.min(retryAt, failure.at); return; }
      tasks.push({ tile: tile, item: item, view: view, priority: priority, order: tasks.length });
    }
    if (key === 'apps') {
      first = Math.max(0, st.row - GRID_ABOVE);
      last = Math.min(rows.length - 1, st.row + gridBelow());
    } else {
      last = Math.min(rows.length - 1, st.row + 1);
    }
    for (ri = first; ri <= last; ri++) {
      row = rows[ri]; tiles = tileEls[ri];
      if (!row || !tiles || !tiles.length) continue;
      focusCol = ri === st.row ? colOf(ri) : 0;
      if (row.view === 'grid') {
        start = 0; end = tiles.length - 1;
        for (ci = start; ci <= end; ci++) consider(tiles[ci], row.items[ci], row.view, ri, ci);
      } else if (row.view === 'sq') {
        start = Math.max(0, focusCol - IMAGE_AHEAD);
        end = Math.min(tiles.length - 1, focusCol + IMAGE_AHEAD);
        for (ci = start; ci <= end; ci++) consider(tiles[ci], row.items[ci], row.view, ri, ci);
      } else if (row.view === 'disc' || row.view === 'poster') {
        start = ri === st.row ? Math.max(0, focusCol - TITLE_AHEAD) : 0;
        end = ri === st.row ? Math.min(tiles.length - 1, focusCol + TITLE_AHEAD) : Math.min(tiles.length - 1, TITLE_VISIBLE - 1);
        for (ci = start; ci <= end; ci++) consider(tiles[ci], row.items[ci], row.view, ri, ci);
      }
    }
    tasks.sort(function (a, b) { return b.priority - a.priority || a.order - b.order; });
    var count = Math.min(tasks.length, IMAGE_BATCH, IMAGE_INFLIGHT - imageActive.length);
    for (var i = 0; i < count; i++) loadTileImage(tasks[i]);
    if (count) imageNextAt = now + IMAGE_STEP_MS;
    if (tasks.length > count && imageActive.length < IMAGE_INFLIGHT) scheduleImages(IMAGE_STEP_MS);
    else if (imageActive.length < IMAGE_INFLIGHT) {
      var delay = near ? Math.max(IMAGE_STEP_MS, IMAGE_IDLE_MS - idleFor) : Infinity;
      delay = Math.min(delay, retryAt - now);
      if (delay !== Infinity) scheduleImages(Math.max(IMAGE_STEP_MS, delay));
    }
    scheduleFocusedRetry(now);
  }
  // Start pictures after the key's focus and layout work, not inside it.
  function scheduleImages(delay) {
    if (document.hidden || recents.open || GhContent.isOpen()) return;
    var at = imageTime() + (delay || 0);
    if (imageTimer && imageTimerAt <= at) return;
    clearTimeout(imageTimer);
    imageTimerAt = at;
    imageTimer = setTimeout(function () {
      imageTimer = 0;
      if (!document.hidden) { mountWindow(); loadImages(); }
    }, delay || 0);
  }

  function render() {
    if (!data || !layout) return;
    applyColours(layout.colours);
    // Tabs are re-resolved from their keys, so a tab appearing or going away
    // (a channel added, Show Live switched) moves nothing else and the tab in
    // view stays; one that goes away falls back to Home.
    var selKey = tabs[st.tab] && tabs[st.tab].key, navKey = tabs[st.nav] && tabs[st.nav].key, onGear = tabs.length > 0 && st.nav === tabs.length, onSearch = tabs.length > 0 && st.nav === tabs.length + 1, onAvatar = st.nav === -1, at;
    tabs = tabList();
    for (at = 0; at < tabs.length; at++) if (tabs[at].key === selKey) break;
    if (at === tabs.length) { st.tab = 0; st.row = 0; } else st.tab = at;
    for (at = 0; at < tabs.length; at++) if (tabs[at].key === navKey) break;
    // The avatar before the tabs and the gear after them keep the focus through a redraw, the avatar while it is there.
    st.nav = onGear ? tabs.length : onSearch && discover.titlesEnabled ? tabs.length + 1 : onAvatar && profilesShown() ? -1 : at === tabs.length ? st.tab : at;
    renderAvatar();
    buildRows();
    var h = '';
    tabs.forEach(function (tb, i) { h += '<div class="tab" data-tab="' + i + '">' + esc(tb.title) + '</div>'; });
    if (h !== tabsShown) { $('tabs').innerHTML = h; tabsShown = h; }
    var key = tabs[st.tab].key, changedTab = shownTab !== key, top = 0;
    if (changedTab) {
      cancelPendingImages($('rows'));
      while ($('rows').firstChild) $('rows').removeChild($('rows').firstChild);
      rowsOff = null; focusTile = null;
    }
    activeView = views[key] || (views[key] = { nodes: {}, signatures: [], tops: [] });
    activeView.revision = (activeView.revision || 0) + 1;
    rows.forEach(function (r, ri) {
      activeView.signatures[ri] = r.__ghSignature + (key === 'live' ? ':' + rev('guide') + ':' + Math.floor(Date.now() / 60000) : '');
      activeView.tops[ri] = top;
      top += rowHeight(r);
    });
    // Drop tabs which disappeared, including their detached images.
    for (var saved in views) if (!tabs.some(function (tb) { return tb.key === saved; })) {
      for (var oldRow in views[saved].nodes) releaseImages(views[saved].nodes[oldRow]);
      delete views[saved];
      delete rowModels[saved];
    }
    shownTab = key;
    var stageClass = (key === 'apps' ? 'apps' : '') + (layout.appNames === false ? ' nonames' : '') + (recents.open ? ' recents' : '') + (st.sliding ? ' slide' : '');
    if ($('stage').className !== stageClass) $('stage').className = stageClass;
    mountWindow();
    shownSig = signature();
    applyFocus(changedTab);
    renderClock();
    hideBoot(false);
  }
  // The boot screen's colours drift until Home first renders, then fade and are taken out of the page.
  function hideBoot(instant) {
    var boot = $('boot');
    if (!boot || boot.__ghGone) return;
    boot.__ghGone = true;
    if (instant) { boot.style.display = 'none'; return; }
    boot.className = 'gt-boot gone';
    setTimeout(function () { boot.style.display = 'none'; }, 700);
  }

  // The rows are rebuilt only when what they show has changed, so a refresh in the background does not interrupt a key press.
  var shownSig = '';
  function signature() {
    // Fingerprints are computed once when their response arrives. Polls and
    // the clock compare small revision keys, without scanning the catalogue.
    var key = tabs[st.tab] ? tabs[st.tab].key : 'home';
    return key + ':' + rev('shell') + ':' + discover.titlesEnabled + ':' + modelStamp(key) +
      (key === 'live' ? ':' + rev('guide') + ':' + Math.floor(Date.now() / 60000) : '');
  }
  function renderIfChanged() {
    if (!data || document.hidden) return;
    if (signature() === shownSig) return;
    render();
  }

  function emptyNote(key) {
    if (key === 'live') return data.sources.channels === null ? t('home.row.noTuner', 'This TV has no channels tuned') : t('home.row.addChannels', 'Add channels in Native Tv’s Home settings');
    return t('home.tv.empty', 'Nothing here yet');
  }

  // Keeps the focused tile in view by moving the track, not the page. A row's
  // measurements are read once and kept with its nodes, so a key press only
  // changes a transform that actually moves.
  function trackMetrics(rowEl) {
    var m = rowEl.__ghTrack;
    if (m) return m;
    var track = rowEl.querySelector('.track'), wrap = rowEl.querySelector('.wrap');
    if (!track || !wrap) return null;
    // The stage is always 1920 logical pixels, with 96px gutters. Reading a
    // newly mounted row's clientWidth here would flush layout after focus
    // class changes on every downward move through a virtualized grid.
    var viewW = rowEl.id === 'recents' ? wrap.clientWidth - 44 : 1920 - 2 * 96;
    var r = rows[+rowEl.getAttribute('data-row')];
    var total = rowEl.id === 'recents' ? track.scrollWidth : r.items.length * (tileWidth(r) + 24) - 24;
    m = { track: track, viewW: viewW, max: Math.max(0, total - viewW), x: -(parseFloat(track.getAttribute('data-x')) || 0) };
    rowEl.__ghTrack = m;
    return m;
  }

  function scrollTrack(rowEl, tile, instant) {
    var m = trackMetrics(rowEl);
    if (!m) return;
    if (!tile) tile = m.track.querySelector('.tile');
    if (!tile) return;
    var ri = +rowEl.getAttribute('data-row'), width, left;
    // Recent cards, like regular tiles, have a fixed logical width and pitch.
    if (rowEl.id === 'recents') { left = +tile.getAttribute('data-c') * (372 + 32); width = 372; }
    else { width = tileWidth(rows[ri]); left = +tile.getAttribute('data-c') * (width + 24); }
    var x = m.x;
    if (left + width > x + m.viewW) x = left + width - m.viewW;
    if (left < x) x = left;
    x = Math.max(0, Math.min(x, m.max));
    if (x === m.x) return;
    if (instant) m.track.style.webkitTransition = m.track.style.transition = 'none';
    m.track.setAttribute('data-x', -x);
    m.track.style.webkitTransform = m.track.style.transform = 'translateX(' + (-x) + 'px)';
    m.x = x;
    if (instant) { void m.track.offsetWidth; m.track.style.webkitTransition = m.track.style.transition = ''; }
  }

  // hold: focus moved by the pointer, so the rows must not slide out from
  // under it (the pointer cannot follow a key's row move).
  function applyFocus(instant, hold) {
    if (!rowEls[st.row] || !tileEls[st.row] || (rows[st.row].items.length && !tileEls[st.row][colOf(st.row)])) mountWindow();
    var i, tabEls = $('tabs').children;
    for (i = 0; i < tabEls.length; i++) {
      var tabClass = 'tab' + (i === st.tab ? ' sel' : '') + (st.zone === 'nav' && st.nav === i ? ' focus' : '');
      if (tabEls[i].className !== tabClass) tabEls[i].className = tabClass;
    }
    var gearClass = 'tab gear' + (st.zone === 'nav' && st.nav === tabs.length ? ' focus' : '');
    if ($('gear').className !== gearClass) $('gear').className = gearClass;
    var searchClass = 'tab search-tool' + (discover.titlesEnabled ? ' on' : '') + (st.zone === 'nav' && st.nav === tabs.length + 1 ? ' focus' : '');
    if ($('searchTool').className !== searchClass) $('searchTool').className = searchClass;
    var avatarClass = (profilesShown() ? 'on' : '') + (st.zone === 'nav' && st.nav === -1 ? ' focus' : '');
    if ($('avatar').className !== avatarClass) $('avatar').className = avatarClass;
    var homeTab = tabs[st.tab].key === 'home';
    for (var ri = activeView.first; ri <= activeView.last; ri++) {
      if (!rowEls[ri]) continue;
      var rowClass = 'row' + (ri === st.row ? ' cur' : '') + (homeTab && ri < st.row ? ' past' : '');
      if (rowEls[ri].className !== rowClass) rowEls[ri].className = rowClass;
    }
    // Only the tile that loses focus and the one that gains it are touched.
    var nextTile = st.zone === 'row' && tileEls[st.row] ? tileEls[st.row][colOf(st.row)] : null;
    if (focusTile && focusTile !== nextTile) focusTile.className = focusTile.className.replace(/ ?\bfocus\b/, '');
    if (nextTile && nextTile !== focusTile) nextTile.className = nextTile.className.replace(/ ?\bfocus\b/, '') + ' focus';
    focusTile = nextTile;
    for (ri = activeView.first; ri <= activeView.last; ri++) {
      // Only the row being moved along needs its track moved; the others keep their place.
      if (rowEls[ri] && (instant || ri === st.row)) scrollTrack(rowEls[ri], tileEls[ri] ? tileEls[ri][colOf(ri)] : null, instant);
    }
    // A long grid scrolls to keep the focused row in view.
    var off = tabs[st.tab].key === 'apps' ? activeView.tops[Math.max(0, st.row - 2)] || 0 : 0;
    if (homeTab && rowEls[st.row]) {
      off = scrollOf(st.row);
    }
    if (!hold && rowsOff !== off) {
      var rowsEl = $('rows');
      rowsEl.style.webkitTransform = rowsEl.style.transform = off ? 'translateY(' + (-off) + 'px)' : '';
      rowsOff = off;
    }
    scheduleHero(instant);
    scheduleImages();
  }

  // ---------------------------------------------------------------- the hero

  function hueOf(text) {
    var h = 0, s = String(text);
    for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) % 360;
    return h;
  }
  function heroOf(it) {
    if (it.kind === 'app') return { eyebrow: t('home.tv.hero.app', 'App'), title: it.app.title, line: it.app.description || '', key: it.app.id, hue: hueOf(it.app.id), rgb: brandRgb(it.app), back: '' };
    if (it.kind === 'title') {
      var ti = it.title, meta = [ti.year, ti.rating ? '★ ' + ti.rating : ''].filter(Boolean).join('  ·  ');
      if (ti.type === 'youtube') return { eyebrow: t('home.tv.type.youtube', 'YouTube'), title: ti.title, line: ti.overview, key: 'ty' + ti.id, hue: 4, rgb: null, back: via(ti.wide, true) };
      return { eyebrow: ti.type === 'movie' ? t('home.tv.type.movie', 'Film') : t('home.tv.type.tv', 'Series'), title: ti.title, line: meta, desc: ti.overview,
        key: 't' + ti.type + ti.id, hue: null, rgb: null, back: ti.backdrop ? via(IMG + 'w1280' + ti.backdrop, true) : '' };
    }
    if (it.kind === 'lg') return { glyph: 'home', eyebrow: t('home.tv.hero.app', 'App'), title: t('home.lgHome', 'LG Home'), line: t('home.tv.hero.lgHome', 'LG’s own home screen'), key: 'lg', hue: null, back: '' };
    if (it.kind === 'dev') {
      var d = it.dev;
      return { eyebrow: t('home.tv.type.input', 'Input'), glyph: d.unused ? 'hdmi' : deviceGlyph(d.name), title: d.unused ? t('home.dev.port', 'HDMI {port}', { port: d.port }) : d.name,
        line: d.unused ? t('home.dev.unused', 'Nothing connected') : d.name === t('home.dev.port', 'HDMI {port}', { port: d.port }) ? t('home.dev.connected', 'Connected') : d.via ? t('home.dev.through', 'HDMI {port}, through {via}', { port: d.port, via: d.via }) : t('home.dev.connected', 'Connected'), key: d.id, hue: null, back: '' };
    }
    var ch = it.ch, on = guideOn(ch);
    return { glyph: 'tv', eyebrow: t('home.now.channel', 'Channel {number}', { number: ch.number }), title: on ? on.title : ch.name,
      line: ch.name + (on ? ', ' + t('home.tv.minLeft', '{n} min left', { n: minutesLeft(on) }) : ''), key: 'c' + ch.id, hue: 218, back: '' };
  }

  var spot = 0;
  function setSliding(on) {
    if (st.sliding === on) return;
    st.sliding = on;
    var stage = $('stage');
    stage.className = stage.className.replace(/ ?\bslide\b/, '') + (on ? ' slide' : '');
  }
  var heroKey = '', bloomShown = null, backOn = 'A';
  var heroBlank = false, heroPending = false;
  function setBloom(hue, rgb) {
    // Centred on the glyph's place in the art's box, 1516x856.
    var css = hue === null
      ? 'radial-gradient(ellipse 820px 500px at 1126px 300px,rgba(150,170,210,.16),rgba(0,0,0,0) 100%)'
      : 'radial-gradient(ellipse 900px 560px at 1126px 300px,' + (rgb ? 'rgba(' + rgb + ',.34)' : 'hsla(' + hue + ',70%,46%,.30)') + ',rgba(0,0,0,0) 100%)';
    if (css === bloomShown) return;
    bloomShown = css;
    $('bloomA').style.background = css;
  }
  // A picture is fetched before it fades in, so a slow one never shows as an empty frame.
  var backWanted = '', backLoaded = [], backLoading = null, backDeadline = 0, backRetryTimer = 0, backAttempts = 0;
  function setBack(url) {
    if (backWanted === url && backLoading) return;
    if (backWanted !== url) backAttempts = 0;
    clearTimeout(backDeadline); clearTimeout(backRetryTimer); backRetryTimer = 0;
    backWanted = url;
    if (backLoading) { backLoading.onload = backLoading.onerror = null; backLoading.src = ''; backLoading = null; }
    if (!url || backLoaded.indexOf(url) !== -1) return showBack(url);
    var img = new Image();
    backLoading = img;
    img.onload = function () {
      if (backLoading !== img) return;
      clearTimeout(backDeadline);
      backAttempts = 0;
      backLoading = null;
      backLoaded.push(url);
      if (backLoaded.length > 12) backLoaded.shift();
      if (backWanted === url) { if (!document.hidden) showBack(url); else heroKey = ''; }
    };
    img.onerror = function () {
      if (backLoading !== img) return;
      clearTimeout(backDeadline);
      backLoading = null;
      img.onload = img.onerror = null;
      img.removeAttribute('src');
      if (backWanted !== url) return;
      if (document.hidden || recents.open) { heroKey = ''; return; }
      showBack('');
      backAttempts = Math.min(backAttempts + 1, 6);
      backRetryTimer = setTimeout(function () {
        backRetryTimer = 0;
        if (document.hidden || recents.open) { heroKey = ''; return; }
        if (backWanted === url) setBack(url);
      }, imageRetryDelay(backAttempts));
    };
    backDeadline = setTimeout(function () { if (backLoading === img && img.onerror) img.onerror(); }, IMAGE_TIMEOUT_MS);
    img.src = url;
  }
  var backTimer = 0;
  function showBack(url) {
    var cur = $('back' + backOn), next = $('back' + (backOn === 'A' ? 'B' : 'A'));
    clearTimeout(backTimer);
    if (!url) { cur.className = 'back'; next.className = 'back'; cur.style.backgroundImage = next.style.backgroundImage = ''; return; }
    // Remote navigation swaps one ready image. Only the idle slideshow fades.
    if (!st.sliding) {
      cur.style.backgroundImage = 'url(' + url + ')'; cur.className = 'back on';
      next.className = 'back off'; next.style.backgroundImage = '';
      return;
    }
    // The layer being reused starts hidden, without a fade, beneath the one in view.
    next.className = 'back off';
    next.style.zIndex = 2;
    cur.style.zIndex = 1;
    next.style.backgroundImage = 'url(' + url + ')';
    void next.offsetWidth;
    next.className = 'back on';
    backOn = backOn === 'A' ? 'B' : 'A';
    // The old picture is dropped once the new one covers it, so only one layer is blended while it fades.
    backTimer = setTimeout(function () { cur.className = 'back off'; cur.style.backgroundImage = ''; }, 200);
  }
  // Text and artwork settle together after the focus has already moved.
  var heroTimer = 0, heroShownItem = null;
  var HERO_SETTLE_MS = 400;
  function scheduleHero(instant) {
    clearTimeout(heroTimer);
    if (GhContent.isOpen()) return;
    heroPending = !instant;
    if (instant) return renderHero(true);
    // Focus is visible now; skip titles crossed while keys follow each other.
    // Each hero change repaints the card: on a B3
    // (webOS 10.3.2), keys 220 ms apart through the rows ran at 47-51 fps with
    // 78-109 frames over 20 ms in 13 s settling after 120 ms, and at 56-57 fps
    // with 25-28 settling after 400 ms.
    heroTimer = setTimeout(function () {
      heroPending = false;
      if (!document.hidden && !recents.open) renderHero(false);
    }, HERO_SETTLE_MS);
  }
  function fillHero(h) {
    var el = $('hero'), mark = $('heroicon');
    mark.innerHTML = h.glyph && !h.back ? icon(h.glyph, 300) : '';
    mark.className = 'gt-carousel-icon' + (h.glyph && !h.back ? ' on' : '');
    el.innerHTML = '<div class="eyebrow">' + esc(h.eyebrow) + '</div><div class="title' + (h.title.length <= 14 ? '' : h.title.length <= 22 ? ' mid' : h.title.length <= 34 ? ' mid two' : ' long') + '">' + esc(h.title) + '</div>' +
      (h.line ? '<div class="line">' + esc(h.line) + '</div>' : '') + (h.desc ? '<div class="desc">' + esc(h.desc) + '</div>' : '');
  }
  // The carousel's dots: shown while it is the spotlight, one a title, at most ten.
  var dotsShown = '';
  function renderDots(count, at) {
    var n = Math.min(count, 10), selected = n ? at % n : -1, key = n + ':' + selected;
    if (key === dotsShown) return;
    dotsShown = key;
    var box = $('dots'), h = '', i;
    if (box.children.length !== n) {
      for (i = 0; i < n; i++) h += '<i data-dot="' + i + '"></i>';
      box.innerHTML = h;
    }
    for (i = 0; i < n; i++) {
      var cls = i === selected ? 'on' : '';
      if (box.children[i].className !== cls) box.children[i].className = cls;
    }
  }
  // What the carousel is showing: the focused item, or the spotlight while an
  // app is focused on the Home tab.
  function heroItem() {
    var it = focused();
    var top = discoverRow('trending');
    if (top && tabs.length && tabs[st.tab] && tabs[st.tab].key === 'home' && it && (it.kind === 'app' || it.kind === 'lg')) it = titleItems(top)[spot % top.items.length];
    return it;
  }
  function renderHero(instant) {
    var el = $('hero'), it = heroItem(), feature = $('feature');
    // With nothing focused, a row with nothing in it, the carousel is left out rather than shown empty.
    var featureClass = 'gt-carousel' + (it ? '' : ' empty');
    if (feature.className !== featureClass) feature.className = featureClass;
    if (!it || tabs[st.tab].key === 'apps') {
      if (!heroBlank) {
        clearTimeout(heroTimer);
        heroPending = false;
        el.innerHTML = '';
        $('heroicon').className = 'gt-carousel-icon';
        heroBlank = true;
        setBack('');
      }
      heroKey = '';
      heroShownItem = null;
      renderDots(0, 0);
      return;
    }
    heroBlank = false;
    // On an app, the carousel shows the spotlight instead: the app's name is under its tile.
    var top = discoverRow('trending');
    if (top && it.kind === 'title' && it.row === 'trending') renderDots(top.items.length, it === focused() ? colOf(st.row) : spot);
    else renderDots(0, 0);
    var h = heroOf(it);
    var key = JSON.stringify(h);
    // Repeating the same hero on a blocked key press only costs style work.
    heroShownItem = it;
    heroPending = false;
    if (key === heroKey) return;
    fillHero(h);
    el.className = 'gt-carousel-content';
    if (key !== heroKey) { setBloom(h.hue, h.rgb); setBack(h.back); }
    heroKey = key;
  }

  // ---------------------------------------------------------------- the top strip

  function renderClock() {
    if (document.hidden) return;
    var d = new Date(), time = clockTime(d), date = dateText(d);
    if ($('clockTime').textContent !== time) $('clockTime').textContent = time;
    if ($('date').textContent !== date) $('date').textContent = date;
  }

  var picture = null, pictureRevision = 0, pictureSource = null;
  var HDR_NAMES = { hdr10: 'HDR10', hlg: 'HLG', dolby_vision: t('home.plate.dolby', 'Dolby Vision') };
  function hz(v) { return String(Math.round(v * 100) / 100); }
  function refreshPicture() {
    if (document.hidden) return;
    var revision = ++pictureRevision, kind = lastOn.kind;
    if (!layout || layout.picture !== true || (kind !== 'input' && kind !== 'channel' && kind !== 'app')) { picture = null; pictureSource = null; return renderPicture(); }
    var which = kind === 'input' ? '&port=' + lastOn.port : kind === 'app' ? '&app=' + encodeURIComponent(lastOn.appId || '') : '';
    if (kind + which !== pictureSource) { pictureSource = kind + which; picture = null; renderPicture(); }
    getJson('/api/home/picture?kind=' + kind + which, function (r) {
      if (revision !== pictureRevision) return;
      picture = r && r.ok ? r : null;
      renderPicture();
    });
  }
  function renderPicture() {
    if (document.hidden) return;
    var p = picture, s = p && p.signal, parts = [];
    if (!p || !(s || p.mode)) { if ($('plate').className !== 'gt-carousel-plate none') $('plate').className = 'gt-carousel-plate none'; return; }
    if (s) {
      parts.push(s.hz ? t('home.plate.size', '{width} × {height} at {hz} Hz', { width: s.width, height: s.height, hz: hz(s.hz) }) : t('home.plate.sizeOnly', '{width} × {height}', { width: s.width, height: s.height }));
      if (HDR_NAMES[s.hdr]) parts.push(HDR_NAMES[s.hdr]);
    }
    if (p.mode) parts.push(p.mode.display);
    var text = parts.join('  ·  ');
    if ($('plate').textContent !== text) $('plate').textContent = text;
    if ($('plate').className !== 'gt-carousel-plate') $('plate').className = 'gt-carousel-plate';
  }

  // ---------------------------------------------------------------- opening things

  function openOf(it) {
    if (it.kind === 'app') return { kind: 'app', id: it.app.id };
    if (it.kind === 'lg') return { kind: 'app', id: LG_HOME_TILE };
    if (it.kind === 'dev') return { kind: 'dev', id: it.dev.id };
    if (it.kind === 'ch') return { kind: 'ch', id: it.ch.id };
    return null;
  }
  function activate(item) {
    var it = item || focused();
    if (!it) return;
    if (it.kind === 'dev' && it.dev.unused) return toast(t('home.tv.nothingOn', 'Nothing is connected to {port}', { port: t('home.dev.port', 'HDMI {port}', { port: it.dev.port }) }));
    if (it.kind === 'title') {
      if (it.title.type !== 'youtube' && discover.titlesEnabled) return GhContent.details(it.title);
      return openTitleDirect(it.title);
    }
    var target = openOf(it);
    if (!target) return;
    if (it.kind === 'lg') target = { kind: 'lg' };
    postJson('/api/home/open', target, function (r) {
      if (!r || !r.ok) toast(t('home.openFailed', 'That did not open'), r && r.error);
    });
  }
  function openTitleDirect(title) {
    var video = title.type === 'youtube';
    return postJson('/api/home/open', video ? { kind: 'youtube', id: title.id } : { kind: 'netflix', type: title.type, id: title.id }, function (r) {
      if (!r || !r.ok) toast(t('home.openFailed', 'That did not open'), r && r.error);
    });
  }

  // ---------------------------------------------------------------- quick settings

  // The inputs as the drawer lists them: the tiles' own, with the one the TV is on marked.
  function quickInputs() {
    if (!data) return [];
    return deviceItems().map(function (it) {
      var d = it.dev;
      return { id: d.id, title: d.unused ? t('home.dev.port', 'HDMI {port}', { port: d.port }) : d.name, unused: !!d.unused,
        on: lastOn.kind === 'input' && lastOn.port === d.port };
    });
  }
  // A profile's avatar: its colour, with the first letter of its name.
  function avatarHtml(p) {
    return '<span class="pf" style="background:' + esc(p.color) + '">' + esc(String(p.name).charAt(0).toUpperCase()) + '</span>';
  }
  // The avatar in the bar shows once there is more than one profile to choose from.
  function profilesShown() { return !!(data && data.profiles && data.profiles.length > 1); }
  function activeProfile() {
    var list = (data && data.profiles) || [];
    for (var i = 0; i < list.length; i++) if (list[i].active) return list[i];
    return null;
  }
  var avatarShown = '';
  function renderAvatar() {
    var p = profilesShown() ? activeProfile() : null, key = p ? p.color + p.name : '';
    if (key === avatarShown) return;
    avatarShown = key;
    var el = $('avatar');
    el.style.background = p ? p.color : '';
    el.textContent = p ? String(p.name).charAt(0).toUpperCase() : '';
    el.setAttribute('aria-label', p ? p.name : '');
  }
  function quickOpenInput(id) {
    var items = deviceItems();
    for (var i = 0; i < items.length; i++) if (items[i].dev.id === id) return activate(items[i]);
  }
  GhQuick.init({
    KEY: KEY,
    icon: icon,
    getJson: getJson,
    postJson: postJson,
    toast: toast,
    layout: function () { return layout; },
    inputs: quickInputs,
    openInput: quickOpenInput,
    // A setting changed in the drawer shows on Home at once.
    reload: function (cb) { load(cb); },
    profiles: function () { return (data && data.profiles) || []; },
    avatar: avatarHtml,
    rename: function (person) {
      GhQuick.close();
      GhContent.rename(person, function () { GhQuick.show('profile', 'avatar'); });
    },
    // A switch of profile: the new layout is read, and Home starts again on its first tab.
    switched: function () { st.tab = 0; st.row = 0; st.cols = {}; heroKey = ''; load(); },
    // The focus goes back to what opened it: the gear, or the avatar.
    closed: function (from) { st.zone = 'nav'; st.nav = from === 'avatar' && profilesShown() ? -1 : tabs.length; applyFocus(); }
  });
  $('gear').innerHTML = icon('gear', 36);
  $('searchTool').textContent = t('home.search.short', 'Search');
  GhContent.init({ getJson: getJson, postJson: postJson, toast: toast, reload: load,
    suspend: function () { clearTimeout(heroTimer); clearTimeout(imageTimer); imageTimer = 0; clearTimeout(tabTimer); tabTimer = 0; },
    resume: function () { if (data && layout) { applyFocus(true); scheduleImages(); } } });

  // ---------------------------------------------------------------- recent apps

  /*
   * Back's panel, in place of the one in LG's Home: the TV's recent apps, newest first
   * (lib/home-recent-apps). Up from the cards is Close all; down from a card
   * is its Close.
   */
  function openRecents() {
    if (recents.busy) return;
    recents.busy = true;
    var revision = ++recents.revision;
    recents.loading = true;
    getJson('/api/home/recents', function (r) {
      recents.busy = false;
      if (revision !== recents.revision) return;
      recents.loading = false;
      // No list from the TV: Back goes to the first tile instead.
      if (!r || !r.ok) { st.zone = 'row'; st.row = 0; return applyFocus(); }
      recents.open = true; recents.col = 0; recents.onX = false; recents.onAll = false;
      showRecents(r.apps);
      $('stage').className += ' recents';
    });
  }
  function closeRecents() {
    var wasOpen = recents.open;
    recents.revision++;
    recents.loading = false;
    recents.open = false;
    $('stage').className = $('stage').className.replace(/\s*\brecents\b/, '');
    scheduleImages();
    if (wasOpen && !heroKey) scheduleHero(false);
  }
  function showRecents(apps) {
    recents.apps = apps || [];
    recents.stamp = Date.now();
    recents.col = Math.max(0, Math.min(recents.col, recents.apps.length - 1));
    if (!recents.apps.length) { recents.onX = false; recents.onAll = false; }
    var html = '';
    recents.apps.forEach(function (a, i) {
      var shot = a.image ? '<img src="' + esc(snapUrl(a)) + '" alt="" onload="if (Math.abs(this.naturalWidth / this.naturalHeight - 16 / 9) > 0.2) this.className = \'fit\'">' : '<span class="mono">' + esc(String(a.title).charAt(0)) + '</span>';
      html += '<div class="tile rc" data-c="' + i + '"><div class="card"><div class="shot">' + shot + '</div><div class="rim"></div><div class="ring"></div></div><div class="nm">' + esc(a.title) + '</div><span class="x">' + esc(t('home.recents.close', 'Close')) + '</span></div>';
    });
    $('recentsTrack').innerHTML = html || '<div class="empty"><b>' + esc(t('home.recents.none', 'Nothing is open')) + '</b>' + esc(t('home.recents.noneDesc', 'Apps stay here while they are open.')) + '</div>';
    $('recents').__ghTrack = null;
    focusRecents(true);
  }
  function focusRecents(instant, hold) {
    var cards = $('recentsTrack').querySelectorAll('.rc');
    for (var i = 0; i < cards.length; i++) {
      var on = i === recents.col && !recents.onAll;
      var cls = 'tile rc' + (on ? ' focus' : '') + (on && recents.onX ? ' onx' : '');
      if (cards[i].className !== cls) cards[i].className = cls;
    }
    var allClass = recents.apps.length ? (recents.onAll ? 'focus' : '') : 'none';
    if ($('closeAll').className !== allClass) $('closeAll').className = allClass;
    if (!hold) scrollTrack($('recents'), cards[recents.col], instant);
  }
  function recentAction(action, app) {
    if (recents.busy) return;
    recents.busy = true;
    var revision = recents.revision;
    postJson('/api/home/recents', { action: action, id: app && app.id }, function (r) {
      recents.busy = false;
      if (revision !== recents.revision && recents.open) return;
      if (!r || !r.ok) return toast(action === 'open' ? t('home.openFailed', 'That did not open') : t('home.recents.closeFailed', 'That did not close'), r && r.error);
      if (action === 'open') return closeRecents();
      if (action === 'closeAll') { closeRecents(); return toast(t('home.recents.allClosed', 'Recent apps closed')); }
      showRecents(r.apps);
    });
  }
  function recentsKey(k) {
    var app = recents.apps[recents.col];
    if (k === KEY.back) return closeRecents();
    if (recents.onAll) {
      if (k === KEY.ok) return recentAction('closeAll');
      if (k === KEY.down) recents.onAll = false;
      return focusRecents();
    }
    if (!app) return;
    if (k === KEY.left || k === KEY.right) {
      recents.col = Math.max(0, Math.min(recents.apps.length - 1, recents.col + (k === KEY.left ? -1 : 1)));
      recents.onX = false;
    } else if (k === KEY.up) { if (recents.onX) recents.onX = false; else recents.onAll = true; }
    else if (k === KEY.down) recents.onX = true;
    else if (k === KEY.ok) return recentAction(recents.onX ? 'close' : 'open', app);
    else if (k === KEY.red) return recentAction('close', app);
    focusRecents();
  }

  // ---------------------------------------------------------------- keys

  /*
   * A tab's rows, and the Home tab's hero card, are redrawn when it is
   * shown: on a B3 (webOS 10.3.2), tabs switched every 220 ms ran at 50 fps
   * with 95 frames over 20 ms in 13 s.
   */
  // As the hero's: longer than the 280 ms between keys of a quick run of presses.
  var TAB_SETTLE_MS = 400, tabTimer = 0;
  function showNavTab() {
    clearTimeout(tabTimer);
    tabTimer = 0;
    if (!tabs[st.nav] || st.nav === st.tab) return;
    st.tab = st.nav; st.row = 0; heroKey = '';
    render();
  }

  function move(dr, dc) {
    var names = tabs.length;
    if (st.zone === 'nav') {
      if (dr > 0) { showNavTab(); st.zone = 'row'; }
      else if (dc) {
        // The avatar (with profiles to choose from), the tabs, then the gear.
        st.nav = Math.max(profilesShown() ? -1 : 0, Math.min(names + (discover.titlesEnabled ? 1 : 0), st.nav + dc));
        // Moving along the tabs focuses each and shows the one the focus stays on.
        clearTimeout(tabTimer);
        tabTimer = st.nav >= 0 && st.nav < names && st.nav !== st.tab ? setTimeout(showNavTab, TAB_SETTLE_MS) : 0;
      }
      return applyFocus();
    }
    if (dr < 0) {
      if (st.row > 0) st.row--; else { st.zone = 'nav'; st.nav = st.tab; }
      return applyFocus();
    }
    if (dr > 0) { if (st.row < rows.length - 1) st.row++; return applyFocus(); }
    if (dc) st.cols[colKey(st.row)] = Math.max(0, Math.min(rows[st.row].items.length - 1, colOf(st.row) + dc));
    applyFocus();
  }
  function press() {
    if (st.zone === 'nav' && st.nav === tabs.length + 1 && discover.titlesEnabled) return GhContent.search();
    if (st.zone === 'nav' && st.nav === tabs.length) return GhQuick.show();
    if (st.zone === 'nav' && st.nav === -1) return GhQuick.show('profile', 'avatar');
    if (st.zone === 'nav') { st.zone = 'row'; return applyFocus(); }
    activate();
  }
  // Back goes up a level at a time: a lower row to the first, then the bar, then recent apps.
  function back() {
    if (st.zone === 'row' && st.row > 0) { st.row = 0; return applyFocus(); }
    if (st.zone === 'row') { st.zone = 'nav'; st.nav = st.tab; return applyFocus(); }
    openRecents();
  }

  function onKeyDown(e) {
    if (!data || !layout) return;
    lastInputAt = Date.now();
    setSliding(false);
    var k = e.keyCode;
    if (GhContent.isOpen()) {
      e.preventDefault();
      if (GhContent.type(e)) return;
      if (k === KEY.ok) { okDown = true; return; }
      if (k === KEY.back) { if (backDown) return; backDown = true; }
      return GhContent.key(k);
    }
    // The quick settings take every key while they are open; OK acts at release, as on the rows.
    if (GhQuick.isOpen()) {
      e.preventDefault();
      if (k === KEY.ok) { okDown = true; return; }
      if (k === KEY.back) { if (backDown) return; backDown = true; }
      return GhQuick.key(k);
    }
    // Keys other than along the tabs act on the tab the focus is on.
    if (tabTimer && k !== KEY.left && k !== KEY.right) showNavTab();
    if (k === KEY.ok) { e.preventDefault(); okDown = true; return; }
    if (k === KEY.back) {
      e.preventDefault();
      if (backDown) return;
      backDown = true;
      if (recents.open || recents.loading) return closeRecents();
      return back();
    }
    if (recents.open) { e.preventDefault(); return recentsKey(k); }
    if (k === KEY.red && st.zone === 'row' && focused() && focused().kind === 'title') {
      e.preventDefault(); if (!e.repeat) openTitleDirect(focused().title); return;
    }
    if (k === KEY.left) move(0, -1);
    else if (k === KEY.right) move(0, 1);
    else if (k === KEY.up) move(-1, 0);
    else if (k === KEY.down) move(1, 0);
    else return;
    e.preventDefault();
  }
  // OK acts at release, so the key's release is not the new app's.
  function onKeyUp(e) {
    if (e.keyCode === KEY.back) { backDown = false; return; }
    if (e.keyCode !== KEY.ok || !okDown) return;
    e.preventDefault();
    okDown = false;
    if (GhContent.isOpen()) return GhContent.key(KEY.ok);
    if (GhQuick.isOpen()) return GhQuick.key(KEY.ok);
    if (recents.open) return recentsKey(KEY.ok);
    if (recents.loading) return;
    press();
  }

  // The Magic Remote's pointer: hovering focuses, clicking acts. A tab opens on
  // a click, the carousel card acts on what it shows, and its dots move the
  // spotlight. Where a row's hover slid a card out from under the pointer while
  // making room for it, a click on the row's padding still acts on the card in
  // focus.
  function up(el, attr) {
    while (el && el !== document.body) { if (el.getAttribute && el.getAttribute(attr) !== null) return el; el = el.parentNode; }
    return null;
  }
  function inTrack(wrap, x, y) {
    var track = wrap.querySelector('.track');
    if (!track) return false;
    var r = track.getBoundingClientRect();
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
  }
  function onPointer(e) {
    if (!data) return;
    // Pointing is use: it stops the spotlight moving on under the pointer.
    lastInputAt = Date.now();
    setSliding(false);
    if (GhQuick.isOpen() || GhContent.isOpen()) return;
    if (recents.open) {
      var card = up(e.target, 'data-c');
      if (card && /\brc\b/.test(card.className) && (+card.getAttribute('data-c') !== recents.col || recents.onAll)) {
        recents.col = +card.getAttribute('data-c'); recents.onAll = false; recents.onX = false; focusRecents(false, true);
      }
      return;
    }
    var tab = up(e.target, 'data-tab'), tile = up(e.target, 'data-r');
    if (up(e.target, 'data-search') && discover.titlesEnabled) { st.zone = 'nav'; st.nav = tabs.length + 1; return applyFocus(false, true); }
    if (up(e.target, 'data-gear')) { st.zone = 'nav'; st.nav = tabs.length; return applyFocus(false, true); }
    if (up(e.target, 'data-avatar') && profilesShown()) { st.zone = 'nav'; st.nav = -1; return applyFocus(false, true); }
    if (tab) { st.zone = 'nav'; st.nav = +tab.getAttribute('data-tab'); return applyFocus(false, true); }
    if (tile && (st.zone !== 'row' || st.row !== +tile.getAttribute('data-r') || colOf(+tile.getAttribute('data-r')) !== +tile.getAttribute('data-c'))) {
      st.zone = 'row'; st.row = +tile.getAttribute('data-r'); st.cols[colKey(st.row)] = +tile.getAttribute('data-c'); applyFocus(false, true);
    }
  }
  function onClick(e) {
    if (!data || !layout) return;
    lastInputAt = Date.now();
    setSliding(false);
    if (GhQuick.isOpen() || GhContent.isOpen()) return;
    if (up(e.target, 'data-search') && discover.titlesEnabled) { st.zone = 'nav'; st.nav = tabs.length + 1; applyFocus(); return GhContent.search(); }
    if (up(e.target, 'data-gear')) { st.zone = 'nav'; st.nav = tabs.length; applyFocus(); return GhQuick.show(); }
    if (up(e.target, 'data-avatar') && profilesShown()) { st.zone = 'nav'; st.nav = -1; applyFocus(); return GhQuick.show('profile', 'avatar'); }
    if (recents.open) {
      if (e.target.id === 'closeAll') return recentAction('closeAll');
      var rc = up(e.target, 'data-c');
      if (!rc || !/\brc\b/.test(rc.className)) return closeRecents();
      recents.col = +rc.getAttribute('data-c');
      return recentAction(/\bx\b/.test(e.target.className) ? 'close' : 'open', recents.apps[recents.col]);
    }
    var dot = up(e.target, 'data-dot');
    if (dot) {
      var top = discoverRow('trending'), at = +dot.getAttribute('data-dot'), cur = focused();
      if (top && top.items.length) {
        spot = at;
        if (cur && cur.kind === 'title' && cur.row === 'trending') st.cols[colKey(st.row)] = at;
        applyFocus();
      }
      return;
    }
    var tab = up(e.target, 'data-tab');
    if (tab) {
      var i = +tab.getAttribute('data-tab');
      st.zone = 'nav'; st.nav = i;
      if (i !== st.tab) { st.tab = i; st.row = 0; heroKey = ''; return render(); }
      return applyFocus();
    }
    var tile = up(e.target, 'data-r');
    if (tile) { onPointer(e); return press(); }
    if (!up(e.target, 'data-dots') && up(e.target, 'data-hero')) {
      var hero = heroShownItem;
      if (hero) activate(hero);
      return;
    }
    var wrap = up(e.target, 'data-rowwrap');
    if (wrap && st.zone === 'row' && rows[st.row] && rows[st.row].view !== 'grid') {
      var ri = +wrap.getAttribute('data-rowwrap');
      if (ri === st.row && inTrack(wrap, e.clientX, e.clientY)) press();
    }
  }

  // ---------------------------------------------------------------- wallpaper

  var WALL_SLOTS = [
    { key: 'night', from: 0, strength: 0.55 }, { key: 'morning', from: 6, strength: 0.7 }, { key: 'afternoon', from: 11, strength: 0.62 },
    { key: 'evening', from: 17, strength: 0.72 }, { key: 'night', from: 21, strength: 0.65 }
  ];
  var preview = { wall: (/[?&]wall=([a-z]+)/.exec(location.search) || [])[1], hour: (/[?&]hour=(\d+)/.exec(location.search) || [])[1] };
  function readLaunchParams() {
    var p = {};
    try { p = JSON.parse(window.PalmSystem.launchParams || '{}') || {}; } catch (e) {}
    preview = { wall: /^[a-z]+$/.test(String(p.wall || '')) ? p.wall : undefined, hour: /^\d{1,2}$/.test(String(p.hour || '')) ? p.hour : undefined };
  }
  function wallSlot() {
    if (preview.wall) for (var i = 1; i < WALL_SLOTS.length; i++) if (WALL_SLOTS[i].key === preview.wall) return WALL_SLOTS[i];
    var h = preview.hour !== undefined ? +preview.hour % 24 : new Date().getHours(), slot = WALL_SLOTS[0];
    for (var j = 0; j < WALL_SLOTS.length; j++) if (WALL_SLOTS[j].from <= h) slot = WALL_SLOTS[j];
    return slot;
  }
  function updateWall() {
    if (document.hidden) return;
    var slot = wallSlot();
    if (!wallAvailable || (wallSlots && !wallSlots[slot.key])) {
      cancelWall();
      if (wallShown === null) return;
      var blank = $('wall').querySelectorAll('.layer');
      for (var b = 0; b < blank.length; b++) { blank[b].style.opacity = 0; blank[b].style.backgroundImage = ''; }
      wallShown = null;
      return;
    }
    var revision = wallRevision, id = slot.key + '@' + slot.strength + '@' + revision;
    if (id === wallShown) return;
    if (wallLoading && wallLoading.id === id) return;
    cancelWall();
    var layers = $('wall').querySelectorAll('.layer'), next = layers[1 - wallFront], prev = layers[wallFront];
    var src = '/api/home/wallpaper?slot=' + slot.key + '&v=' + encodeURIComponent(revision), img = new Image();
    wallLoading = { id: id, image: img };
    img.onload = function () {
      if (!wallLoading || wallLoading.image !== img) return;
      wallLoading = null;
      if (document.hidden || !wallAvailable || wallRevision !== revision || wallSlot().key !== slot.key || (wallSlots && !wallSlots[slot.key])) return;
      wallShown = id;
      next.style.backgroundImage = 'url(' + src + ')';
      next.style.opacity = slot.strength;
      prev.style.opacity = 0;
      wallFront = 1 - wallFront;
    };
    img.onerror = function () { if (wallLoading && wallLoading.image === img) wallLoading = null; };
    img.src = src;
  }
  function cancelWall() {
    if (!wallLoading) return;
    var img = wallLoading.image;
    wallLoading = null;
    img.onload = img.onerror = null;
    img.removeAttribute('src');
  }

  // ---------------------------------------------------------------- colours

  var LOOK = $('look').textContent, lookShown = '';
  function shade(hex, f) {
    var n = parseInt(hex.slice(1), 16), out = '#';
    [n >> 16, (n >> 8) & 255, n & 255].forEach(function (v) { var x = Math.round(v * f); out += (x < 16 ? '0' : '') + x.toString(16); });
    return out;
  }
  // The dashboards' text and accent colour, swapped into this stylesheet.
  function applyColours(c) {
    var key = c ? c.text + c.accent : '';
    if (key === lookShown) return;
    lookShown = key;
    if (!c) { $('look').textContent = LOOK; return; }
    var swap = { '#ece7df': c.text, '#b3ada3': shade(c.text, 0.84), '#8d877d': shade(c.text, 0.66), '#e9ad55': c.accent };
    // Replace once: a chosen accent may itself be one of the original colours. The focus ring takes the accent.
    $('look').textContent = LOOK.replace(/8px #ece7df\b|#(?:ece7df|b3ada3|8d877d|e9ad55)\b/g,
      function (m) { return m === '8px #ece7df' ? '8px ' + c.accent : swap[m]; });
  }

  // ---------------------------------------------------------------- data

  var loadPending = false, loadAgain = false;
  var SNAPSHOT_KEY = 'nativetv-home-catalogue-v1';
  function rememberCatalogue(r) {
    try {
      var text = JSON.stringify({ at: Date.now(), catalogue: r });
      if (text.length <= 262144) sessionStorage.setItem(SNAPSHOT_KEY, text);
      else sessionStorage.removeItem(SNAPSHOT_KEY);
    } catch (e) {}
  }
  function restoreCatalogue() {
    try {
      var saved = JSON.parse(sessionStorage.getItem(SNAPSHOT_KEY) || 'null');
      var r = saved && saved.catalogue, age = saved && Date.now() - saved.at;
      if (!r || !r.ok || age < 0 || age > 300000 || !r.layout || !r.order || !r.sources ||
          !Array.isArray(r.apps) || !Array.isArray(r.devices) || !Array.isArray(r.channels) || !Array.isArray(r.layout.channels)) return;
      setCatalogue(r);
      layoutVersion = r.layoutVersion || null;
      // A catalogue left by the last visit shows at once, without the boot screen.
      hideBoot(true);
      render();
    } catch (e) { data = null; layout = null; }
  }
  // What waits on a read in flight, called when it ends.
  var loadWaiting = [];
  function load(cb) {
    if (loadPending) { loadAgain = true; if (cb) loadWaiting.push(cb); return; }
    loadPending = true;
    if (cb) loadWaiting.push(cb);
    cb = function (ok) { var waiting = loadWaiting; loadWaiting = []; waiting.forEach(function (fn) { fn(ok); }); };
    getJson('/api/home/catalogue?view=1', function (r) {
      loadPending = false;
      var again = loadAgain;
      loadAgain = false;
      if (!r || !r.ok) {
        if (!data) {
          $('fault').className = 'show';
          hideBoot(false);
          // A live HTTP server is not enough: Home needs its tiles and escape tile.
          if (!document.hidden && !initialEscape) { initialEscape = true; postJson('/api/home/open', { kind: 'lg', recovery: true }); }
          clearTimeout(loadRetry);
          loadRetry = setTimeout(function () { if (!document.hidden) load(); }, 2000);
        }
        return cb && cb(false);
      }
      var first = !data;
      setCatalogue(r);
      if (r.layoutVersion) layoutVersion = r.layoutVersion;
      clearTimeout(loadRetry);
      wallAvailable = !!(r.wallpapers && r.wallpapers.available);
      wallSlots = r.wallpapers && r.wallpapers.slots;
      wallRevision = r.wallpapers && r.wallpapers.revision || '';
      updateWall();
      if ($('fault').className !== '') $('fault').className = '';
      rememberCatalogue(r);
      if (!document.hidden) renderIfChanged();
      if (first) beat();
      if (!document.hidden) { refreshGuide(); refreshDiscover(); refreshNow(); }
      if (cb) cb(true);
      if (again && !document.hidden) load();
    });
  }
  function refreshNow() {
    getJson('/api/home/now', function (r) {
      if (!r) return;
      lastOn = r;
      refreshPicture();
    });
  }
  function refreshGuide() {
    // The guide draws the Live tab alone: it is not read while Show Live is off.
    if (!layout || layout.showLive !== true || !layout.channels.length) return;
    getJson('/api/home/guide', function (r) {
      if (!r || !r.guide) return;
      guideData = r.guide;
      fingerprintGuide();
      renderIfChanged();
    });
  }

  /*
   * The server watches for these: the view in front without them for ten
   * seconds is taken to be broken, and LG's Home opens instead. With the
   * server gone, the page says so; the page is LG's Home's app, so only the
   * server's watchdog can put LG's back (tvwebctl).
   */
  function beat() {
    if (!data || !layout || !shownTab) return;
    postJson('/api/home/beat', {}, function (r) {
      if (r && r.ok) {
        beatFails = 0;
        wallAvailable = !!(r.wallpapers && r.wallpapers.available);
        wallSlots = r.wallpapers && r.wallpapers.slots;
        wallRevision = r.wallpapers && r.wallpapers.revision || '';
        updateWall();
        if ($('fault').className !== '') $('fault').className = '';
        // A new release of the server: the page is the old one's until reloaded.
        if (build && r.build && r.build !== build) { if (!document.hidden) location.reload(); return; }
        build = r.build || build;
        if (r.layoutVersion && layoutVersion && r.layoutVersion !== layoutVersion && !document.hidden) load();
        return;
      }
      if (++beatFails === 3) $('fault').className = 'show';
    });
  }

  function returned() {
    initialEscape = false;
    okDown = false; backDown = false;
    GhQuick.close();
    GhContent.returned();
    closeRecents();
    readLaunchParams();
    renderClock();
    renderPicture();
    updateWall();
    // Keep the mounted screen and selection while fresh data arrives.
    if (data && layout) { renderIfChanged(); scheduleHero(false); scheduleImages(); }
    beat();
    load();
  }

  window.addEventListener('resize', function () {
    fit();
    for (var key in views) for (var ri in views[key].nodes) views[key].nodes[ri].__ghTrack = null;
    if (data && layout && !document.hidden) applyFocus(true);
  });
  document.addEventListener('keydown', onKeyDown, true);
  document.addEventListener('keyup', onKeyUp, true);
  // The pointer's cards, the bar's tabs, the rows and the panel's cards all act on a click.
  document.addEventListener('mousemove', onPointer);
  document.addEventListener('click', onClick);
  // Home pressed with the view already open reaches it as a relaunch.
  document.addEventListener('webOSRelaunch', returned);
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) returned();
    else {
      clearTimeout(heroTimer); clearTimeout(imageTimer); imageTimer = 0; heroPending = false;
      if (backRetryTimer) { clearTimeout(backRetryTimer); backRetryTimer = 0; heroKey = ''; }
      cancelWall(); cancelPendingImages($('rows'));
      GhContent.hidden();
    }
  });

  fit();
  renderClock();
  updateWall();
  restoreCatalogue();
  beat();
  load();
  setInterval(beat, BEAT_MS);
  // The spotlight is a slideshow while nobody is using the remote: on the apps it moves the hero on, on the top picks the focus too.
  var lastInputAt = Date.now(), SLIDE_MS = 8000;
  setInterval(function () {
    var top = discoverRow('trending'), it = focused();
    if (document.hidden || recents.open || GhContent.isOpen() || !top || !it || st.zone !== 'row' || tabs[st.tab].key !== 'home' || Date.now() - lastInputAt < SLIDE_MS) return;
    var count = top.items.length;
    if (it.kind === 'app' || it.kind === 'lg') spot = (spot + 1) % count;
    else if (it.kind === 'title' && it.row === 'trending') spot = st.cols[colKey(st.row)] = (colOf(st.row) + 1) % count;
    else return;
    setSliding(true);
    applyFocus();
  }, SLIDE_MS);
  setInterval(function () { if (!document.hidden) { renderClock(); renderIfChanged(); updateWall(); refreshPicture(); } }, 15000);
  setInterval(function () { if (!document.hidden) { refreshNow(); refreshGuide(); } }, 60000);
  setInterval(function () { if (!document.hidden) load(); }, 5 * 60000);
})();
