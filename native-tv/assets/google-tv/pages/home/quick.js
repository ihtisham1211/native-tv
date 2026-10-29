/*
 * nativeTv Home's quick settings: the kit's modal drawer (google-tv/shared,
 * the drawer section), opened from the gear in the bar. The main list has the
 * TV's volume, picture mode, OLED light, sound output, input and sleep timer,
 * the screensaver and Screen off, and Home's own settings. What the TV is on
 * is read from /api/stats when the drawer opens and after a change; changes go
 * to /api/control, as the dashboards' do. home.js hands over what it owns
 * (init) and routes the keys while the drawer is open. Strict ES5.
 */
var GhQuick = (function () {
  var api = null, shown = false, level = 'main', direct = false, anchor = 'gear', focus = 0, scrollY = 0, items = [];
  var stats = null, volume = { value: null, muted: false }, light = null;
  var stepSum = 0, stepBusy = false, lightTimer = 0, refreshTimer = 0;
  var SLEEP = ['off', '10', '30', '60', '90', '120'];
  var LAYOUT_SWITCHES = [
    { key: 'clockFormat', title: t('home.qs.clock24', '24-hour clock') },
    { key: 'appNames', title: t('home.qs.appNames', 'App names') },
    { key: 'showLive', title: t('home.qs.showLive', 'Live tab') },
    { key: 'showInputs', title: t('home.qs.showInputs', 'Inputs tab') },
    { key: 'showLgHome', title: t('home.qs.showLgHome', 'LG Home tile') },
    { key: 'picture', title: t('home.qs.picture', 'Picture details') }
  ];

  function $(id) { return document.getElementById(id); }
  function clamp(n, low, high) { return Math.max(low, Math.min(high, n)); }
  function up(el, attr) {
    while (el && el !== document.body) { if (el.getAttribute && el.getAttribute(attr) !== null) return el; el = el.parentNode; }
    return null;
  }

  function control(action, value, cb) {
    api.postJson('/api/control', { action: action, value: value }, function (r) {
      if (!r || !r.ok) api.toast(t('home.qs.failed', 'That did not work'), r && r.error);
      if (cb) cb(r);
    });
  }
  function readStats() {
    api.getJson('/api/stats', function (s) {
      if (!s || !shown) return;
      stats = s;
      volume = { value: typeof s.volume === 'number' ? s.volume : null, muted: !!s.muted };
      light = s.picture && typeof s.picture.backlight === 'number' ? s.picture.backlight : null;
      render(true);
    });
  }
  function readStatsSoon() {
    clearTimeout(refreshTimer);
    refreshTimer = setTimeout(readStats, 600);
  }

  function sleepLabel(v) { return v === 'off' ? t('home.qs.off', 'Off') : t('home.qs.minutes', '{n} min', { n: v }); }
  function volumeText() {
    if (volume.value === null) return '—';
    return volume.muted ? t('home.qs.muted', 'Muted') : String(volume.value);
  }
  function lightText() { return light === null ? '—' : String(light); }

  // What the list shows at the level it is on. id names the action; go is the level an item opens.
  function mainItems() {
    var s = stats || {}, current = api.inputs().filter(function (i) { return i.on; })[0];
    return [
      { id: 'volume', icon: 'volume', title: t('home.qs.volume', 'Volume'), trail: volumeText(), dim: volume.value === null },
      { id: 'picture', icon: 'picture', title: t('home.qs.pictureMode', 'Picture mode'), trail: s.picture ? s.picture.mode : '—', go: 'picture' },
      { id: 'light', icon: 'light', title: s.oled ? t('home.qs.oledLight', 'OLED light') : t('home.qs.backlight', 'Backlight'), trail: lightText(), dim: light === null },
      { id: 'sound', icon: 'speaker', title: t('home.qs.sound', 'Sound output'), trail: s.sound ? s.sound.output : s.audio_output || '—', go: 'sound' },
      { id: 'input', icon: 'hdmi', title: t('home.qs.input', 'Input'), trail: current ? current.title : '', go: 'input' },
      { id: 'sleep', icon: 'clock', title: t('home.qs.sleep', 'Sleep timer'), trail: s.sleepTimer ? sleepLabel(String(s.sleepTimer)) : '—', go: 'sleep' },
      { id: 'saver', icon: 'monitor', title: t('home.qs.screensaver', 'Screensaver') },
      { id: 'off', icon: 'power', title: t('home.qs.screenOff', 'Screen off') },
      { id: 'profile', icon: 'profile', title: t('home.qs.profile', 'Profile'), trail: activeProfile().name, go: 'profile' },
      { id: 'settings', icon: 'gear', title: t('home.qs.settings', 'Home settings'), go: 'settings' }
    ];
  }
  function activeProfile() {
    var list = api.profiles();
    for (var i = 0; i < list.length; i++) if (list[i].active) return list[i];
    return { name: '' };
  }
  function levelItems() {
    var s = stats || {}, out = [];
    if (level === 'main') return mainItems();
    if (level === 'picture') {
      ((s.picture && s.picture.modes) || []).forEach(function (m) { out.push({ id: m.value, title: m.label, radio: true, on: !!s.picture && m.value === s.picture.mode_raw }); });
    } else if (level === 'sound') {
      ((s.sound && s.sound.outputs) || []).forEach(function (o) { out.push({ id: o.id, title: o.name, radio: true, on: !!s.sound && o.id === s.sound.output_raw }); });
    } else if (level === 'input') {
      api.inputs().forEach(function (i) { out.push({ id: i.id, title: i.title, trail: i.unused ? t('home.dev.unused', 'Nothing connected') : '', radio: true, on: i.on, dim: i.unused }); });
    } else if (level === 'sleep') {
      SLEEP.forEach(function (v) { out.push({ id: v, title: sleepLabel(v), radio: true, on: !!s.sleepTimer && String(s.sleepTimer) === v }); });
    } else if (level === 'profile') {
      api.profiles().forEach(function (p) { out.push({ id: p.id, title: p.name, radio: true, on: p.active, avatar: p }); });
      out.push({ id: 'add', icon: 'add', title: t('home.qs.addProfile', 'Add profile') });
      out.push({ id: 'rename', title: t('home.profile.rename', 'Rename profile'), trail: activeProfile().name });
    } else if (level === 'settings') {
      var layout = api.layout() || {};
      LAYOUT_SWITCHES.forEach(function (sw) {
        var on = sw.key === 'clockFormat' ? layout.clockFormat === '24h' : sw.key === 'appNames' || sw.key === 'showInputs' || sw.key === 'showLgHome' ? layout[sw.key] !== false : layout[sw.key] === true;
        out.push({ id: sw.key, title: sw.title, sw: true, on: on });
      });
    }
    if (!out.length) out.push({ id: '', title: t('home.qs.nothing', 'Nothing to choose from'), dim: true });
    return out;
  }
  function levelTitle() {
    return { main: t('home.qs.title', 'Quick settings'), picture: t('home.qs.pictureMode', 'Picture mode'), sound: t('home.qs.sound', 'Sound output'),
      input: t('home.qs.input', 'Input'), sleep: t('home.qs.sleep', 'Sleep timer'), settings: t('home.qs.settings', 'Home settings'),
      profile: t('home.qs.whoWatching', 'Who’s watching?') }[level];
  }

  function itemHtml(it, i) {
    var ctl = it.radio ? '<span class="gt-radio' + (it.on ? ' on' : '') + '"></span>' : it.sw ? '<span class="gt-switch' + (it.on ? ' on' : '') + '"></span>' : '';
    var trail = ctl || (it.trail ? esc(it.trail) : '');
    return '<div class="gt-item dense' + (it.dim ? ' dim' : '') + '" data-q="' + i + '">' +
      (it.icon ? '<span class="gt-item-icon">' + api.icon(it.icon, 48) + '</span>' : '') +
      (it.avatar ? '<span class="gt-item-icon">' + api.avatar(it.avatar) + '</span>' : '') +
      '<span class="gt-item-text"><span class="gt-item-title">' + esc(it.title) + '</span></span>' +
      (trail ? '<span class="gt-item-trail">' + trail + '</span>' : '') + '</div>';
  }
  // keep: the list was redrawn for new values, so the focus and scroll stay where they were.
  function render(keep) {
    items = levelItems();
    if (!keep) { focus = clamp(focus, 0, items.length - 1); scrollY = 0; }
    focus = clamp(focus, 0, items.length - 1);
    $('qsTitle').textContent = levelTitle();
    $('qsList').innerHTML = items.map(itemHtml).join('');
    paintFocus();
  }
  function paintFocus() {
    var nodes = $('qsList').children, view = $('qsView'), drawer = $('qsDrawer');
    for (var i = 0; i < nodes.length; i++) {
      var cls = 'gt-item dense' + (items[i].dim ? ' dim' : '') + (i === focus ? ' focus' : '');
      if (nodes[i].className !== cls) nodes[i].className = cls;
    }
    var el = nodes[focus];
    if (el) {
      // The item and the gap under it, and while items follow, the 80px the fade covers, so the focus stays clear of it.
      var top = el.offsetTop, bottom = top + el.offsetHeight + 8 + (focus < items.length - 1 ? 80 : 0), height = view.clientHeight;
      if (bottom > scrollY + height) scrollY = bottom - height;
      if (top < scrollY) scrollY = top;
    }
    var list = $('qsList');
    list.style.webkitTransform = list.style.transform = scrollY ? 'translateY(' + (-scrollY) + 'px)' : '';
    var more = list.offsetHeight - scrollY > view.clientHeight + 2 ? ' more' : '';
    if (drawer.className !== 'gt-drawer' + more) drawer.className = 'gt-drawer' + more;
  }
  // A value that moves with the keys is changed in place, not by redrawing the list.
  function paintTrail(index, text) {
    var node = $('qsList').children[index], trail = node && node.querySelector('.gt-item-trail');
    if (trail) trail.textContent = text;
  }
  function indexOfItem(id) {
    for (var i = 0; i < items.length; i++) if (items[i].id === id) return i;
    return 0;
  }

  // Volume goes in steps, as the remote's key does; steps pressed while one is on its way go in one more.
  function flushVolume() {
    if (stepBusy || !stepSum) return;
    var n = stepSum;
    stepSum = 0; stepBusy = true;
    control('volumeStep', n, function () {
      stepBusy = false;
      if (stepSum) flushVolume(); else readStatsSoon();
    });
  }
  function stepVolume(d) {
    if (volume.value === null) return;
    volume.value = clamp(volume.value + d, 0, 100);
    stepSum += d;
    paintTrail(indexOfItem('volume'), volumeText());
    flushVolume();
  }
  function stepLight(d) {
    if (light === null) return;
    light = clamp(light + d, 0, 100);
    paintTrail(indexOfItem('light'), lightText());
    clearTimeout(lightTimer);
    lightTimer = setTimeout(function () { lightTimer = 0; control('backlight', light, readStatsSoon); }, 350);
  }

  function open(next) {
    level = next;
    focus = 0; scrollY = 0;
    render();
    // A list of choices opens on the one in force; a list of switches, on its first.
    for (var i = 0; i < items.length; i++) if (items[i].on && items[i].radio) { focus = i; break; }
    paintFocus();
  }
  function toMain(id) {
    level = 'main';
    render();
    focus = indexOfItem(id);
    paintFocus();
  }
  function back() {
    // A drawer opened on a list, from the avatar, closes from it.
    if (level === 'main' || direct) return close();
    toMain(level);
  }
  function profileChanged(r) {
    if (!r || !r.ok) return api.toast(t('home.qs.failed', 'That did not work'), r && r.error);
    close();
    api.switched();
  }
  function chosen(it) {
    if (level === 'profile') {
      if (it.id === 'rename') return api.rename(activeProfile());
      if (it.id === 'add') return api.postJson('/api/home/profile', { action: 'add', switch: true }, function (r) {
        profileChanged(r);
        if (r && r.ok) api.toast(t('home.qs.profileAdded', 'Profile added'));
      });
      if (it.on) return close();
      return api.postJson('/api/home/profile', { action: 'switch', id: it.id }, profileChanged);
    }
    if (level === 'picture') return control('pictureMode', it.id, function (r) {
      if (r && r.ok) { stats.picture.mode_raw = it.id; stats.picture.mode = it.title; }
      readStatsSoon(); if (shown) toMain('picture');
    });
    // The TV takes a moment to move the sound over: the server answers when it has.
    if (level === 'sound') return control('soundOutput', it.id, function (r) {
      if (r && r.ok) { stats.sound.output_raw = it.id; stats.sound.output = it.title; }
      readStatsSoon(); if (shown) toMain('sound');
    });
    if (level === 'sleep') return control('sleepTimer', it.id, function (r) {
      if (r && r.ok) stats.sleepTimer = it.id;
      readStatsSoon(); if (shown) toMain('sleep');
    });
    if (level === 'input') { close(); return api.openInput(it.id); }
    if (level === 'settings') {
      var body = {};
      body[it.id] = it.id === 'clockFormat' ? (it.on ? '12h' : '24h') : !it.on;
      api.postJson('/api/home/layout', body, function (r) {
        if (!r || !r.ok) return api.toast(t('home.qs.failed', 'That did not work'), r && r.error);
        api.reload(function () { if (shown) render(true); });
      });
    }
  }
  function press() {
    var it = items[focus];
    if (!it || it.dim && !it.go) return;
    if (level !== 'main') return chosen(it);
    if (it.go) return open(it.go);
    if (it.id === 'volume') {
      if (volume.value === null) return;
      volume.muted = !volume.muted;
      paintTrail(focus, volumeText());
      return control('mute', volume.muted, readStatsSoon);
    }
    if (it.id === 'saver') { close(); return control('screensaver'); }
    if (it.id === 'off') { close(); return control('screenOff'); }
  }
  function sideways(d) {
    var it = items[focus];
    if (level !== 'main') return d < 0 ? back() : undefined;
    if (it && it.id === 'volume') stepVolume(d);
    else if (it && it.id === 'light') stepLight(d * 5);
  }

  function key(k) {
    var K = api.KEY;
    if (k === K.back) return back();
    if (k === K.up || k === K.down) {
      var next = clamp(focus + (k === K.down ? 1 : -1), 0, items.length - 1);
      if (next !== focus) { focus = next; paintFocus(); }
    } else if (k === K.left || k === K.right) sideways(k === K.right ? 1 : -1);
    else if (k === K.ok) press();
  }

  // start: the list to open on ('main' by default), and where it was opened from, for the focus to go back to.
  function show(start, from) {
    shown = true;
    level = start || 'main'; direct = level !== 'main'; anchor = from || 'gear';
    focus = 0; scrollY = 0;
    render();
    if (direct) for (var i = 0; i < items.length; i++) if (items[i].on) { focus = i; break; }
    paintFocus();
    $('qs').className = 'gt-scrim show';
    readStats();
  }
  function close() {
    if (!shown) return;
    shown = false;
    // A change not yet sent goes now.
    if (stepSum) flushVolume();
    if (lightTimer) { clearTimeout(lightTimer); lightTimer = 0; control('backlight', light); }
    clearTimeout(refreshTimer);
    $('qs').className = 'gt-scrim';
    api.closed(anchor);
  }

  function init(handlers) {
    api = handlers;
    // The pointer: hovering focuses an item, a click acts on it, and a click beside the drawer closes it.
    $('qs').addEventListener('mousemove', function (e) {
      var node = up(e.target, 'data-q');
      if (node && +node.getAttribute('data-q') !== focus) { focus = +node.getAttribute('data-q'); paintFocus(); }
    });
    $('qs').addEventListener('click', function (e) {
      var node = up(e.target, 'data-q');
      if (!node) { if (!up(e.target, 'data-drawer')) close(); return; }
      focus = +node.getAttribute('data-q');
      paintFocus();
      press();
    });
  }

  return { init: init, show: show, close: close, key: key, isOpen: function () { return shown; } };
})();
