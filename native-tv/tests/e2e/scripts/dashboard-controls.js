// Round-trips the dashboard's own settings controls on a real TV, through the page, as a person would.
// Not run by run.py (it needs a browser): open the dashboard of the TV under test (with its access
// token, as usual), paste this file into the page's console and read window.__rt when __rt.done is true.
//
// For each switch, drop-down and slider of the tabs below it changes the control, waits for the server,
// shows another tab and comes back (so the state shown is read again from the TV), checks that the change
// is there, puts the original back the same way and checks that too. A group of buttons of which one is
// selected (the colour chips) is changed by selecting another button and restored by selecting the
// original. Controls are found again by their label after a reload, because a switch that hides a tile
// moves it in its list.
//
// Skipped on purpose: what powers, restarts, installs, uninstalls, deletes or restores, timers, keys,
// ports and passwords, and the experimental Home layout and screen saver switches (E2E-104 to 119 cover
// the settings behind those through the API). window.confirm answers yes only to the OLED protection
// question and window.alert is recorded, so a refusal the page announces ends up in __rt.dialogs.
//
// window.__tabs = [] runs only the recipes below; window.__recipes = false leaves them out; window.__recipeOnly = /regex/
// on a recipe's ids (or ntp, homescreen) runs only those.
// window.__rt = { done, log: [{tab, label, type, before, after, restored, ok | skipped}], dialogs: [] }
window.__rt = { done: false, log: [], current: '', dialogs: [] };
window.confirm = function (m) { window.__rt.dialogs.push('confirm: ' + String(m).slice(0, 90)); return /burn-in/.test(m); };
window.alert = function (m) { window.__rt.dialogs.push('ALERT: ' + String(m).slice(0, 120)); };
(async function () {
  const TABS_TO_TRY = window.__tabs || ['apps', 'homescreen', 'home', 'oledcare', 'screensaver', 'game', 'privacy', 'advanced', 'servicemenu'];
  const ONLY = window.__only || null; // a RegExp on the label, to try a few controls
  const SKIP = /power|reboot|restart|screen off|screen on|timer|factory|reset|delete|remove|uninstall|install|clear|forget|ssh|debugger|pin\b|restore|backup|update|format|wipe|shut|standby|port|key|password|upload/i;
  const SKIP_ID = /^btn-home-allow$|exp|experimental/i;
  const wait = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
  const fire = function (e) { e.dispatchEvent(new Event('input', { bubbles: true })); e.dispatchEvent(new Event('change', { bubbles: true })); };

  function labelOf(el) {
    for (let p = el.parentElement, i = 0; i < 5 && p; i++, p = p.parentElement) {
      const t = (p.querySelector('.prot-title, .row-title, .title, h3, h4, label') || {}).innerText;
      if (t && t.trim().length > 2) return t.trim().split('\n')[0].slice(0, 60);
    }
    const lines = ((el.closest('.prot-row, .row, .card, section, div') || {}).innerText || '').split('\n')
      .map(function (s) { return s.trim(); }).filter(function (s) { return s && !/^(On|Off|Shown|Hidden)$/.test(s); });
    return (lines[0] || '?').slice(0, 60);
  }
  // A control is named by its row's label, and by its own name inside the row when the row holds several
  // (the tiles of an app list, the chips of a colour group).
  function nameOf(el) {
    const own = (el.getAttribute('aria-label') || '').slice(0, 40);
    const row = el.closest('li, tr, .tile-row, .home-tile-row, .prot-row');
    const inner = row && row !== el ? (row.innerText || '').split('\n')[0].trim().slice(0, 40) : '';
    return labelOf(el) + ' | ' + (inner || own);
  }
  function inGroup(el) { return !!el.closest('[role=group]'); }
  function controls(tab) {
    const pane = document.getElementById(TABS[tab]);
    return pane ? [].slice.call(pane.querySelectorAll('button.pill.sw, button[aria-pressed], select.pill-sel, input[type=range]'))
      .filter(function (e) { return e.offsetParent !== null; }) : [];
  }
  function read(e) {
    return e.tagName === 'BUTTON' ? (e.getAttribute('aria-pressed') || (e.classList.contains('good') ? 'on' : 'off')) : e.value;
  }
  async function reload(tab) { showTab(tab === 'apps' ? 'game' : 'apps'); await wait(1200); showTab(tab); await wait(3500); }
  function find(tab, name, n) { return controls(tab).filter(function (e) { return nameOf(e) === name; })[n || 0]; }

  for (const tab of TABS_TO_TRY) {
    showTab(tab); await wait(2800);
    const seen = {};
    const names = controls(tab).map(function (e) { const k = nameOf(e); seen[k] = (seen[k] || 0) + 1; return { name: k, n: seen[k] - 1, el: e }; });
    for (const item of names) {
      let el = find(tab, item.name, item.n); if (!el) continue;
      const rec = { tab: tab, label: item.name, type: el.tagName === 'BUTTON' ? (inGroup(el) ? 'chip' : 'switch') : el.tagName === 'SELECT' ? 'select' : 'range', before: read(el) };
      window.__rt.current = tab + ' ' + item.name;
      if (ONLY && !ONLY.test(item.name)) continue;
      if (SKIP.test(item.name) || SKIP_ID.test(el.id) || el.disabled) { rec.skipped = el.disabled ? 'disabled' : 'unsafe'; window.__rt.log.push(rec); continue; }
      let originalChip = null;
      if (rec.type === 'chip') {
        // The selected chip is what is restored: another one is selected now, and this one after.
        const group = el.closest('[role=group]');
        const chips = [].slice.call(group.querySelectorAll('button')).filter(function (b) { return !b.disabled; });
        originalChip = chips.filter(function (b) { return read(b) === 'true'; })[0];
        if (!originalChip || read(el) === 'true') { rec.skipped = 'selected chip'; window.__rt.log.push(rec); continue; }
        el.click();
      } else if (el.tagName === 'BUTTON') el.click();
      else if (el.tagName === 'SELECT') {
        const next = (el.selectedIndex + 1) % el.options.length;
        if (el.options.length < 2) { rec.skipped = 'one option'; window.__rt.log.push(rec); continue; }
        el.selectedIndex = next; fire(el);
      } else { const step = Number(el.step) || 1, v = Number(el.value); el.value = v + step <= Number(el.max) ? v + step : v - step; fire(el); }
      await wait(3500); await reload(tab);
      el = find(tab, item.name, item.n); rec.after = el ? read(el) : 'gone';
      if (rec.type === 'chip') {
        const chip = el; const changedChip = chip && read(chip) === 'true';
        const group = chip && chip.closest('[role=group]');
        const back = group && [].slice.call(group.querySelectorAll('button')).filter(function (b) { return b.getAttribute('aria-label') === originalChip.getAttribute('aria-label'); })[0];
        if (back) { back.click(); await wait(3500); await reload(tab); }
        const again = find(tab, item.name, item.n);
        rec.restored = again && again.closest('[role=group]').querySelector('[aria-label="' + originalChip.getAttribute('aria-label') + '"]').getAttribute('aria-pressed');
        rec.ok = changedChip && rec.restored === 'true';
      } else {
        if (rec.after !== rec.before && el) { if (el.tagName === 'BUTTON') el.click(); else { el.value = rec.before; fire(el); } await wait(3500); await reload(tab); }
        const again = find(tab, item.name, item.n); rec.restored = again ? read(again) : 'gone';
        rec.ok = rec.after !== rec.before && rec.restored === rec.before;
      }
      window.__rt.log.push(rec);
    }
  }
  // The tabs whose controls are not found by their look: each is clicked by id, the tab is shown again, and
  // the state must have changed and, after the second click, be back. The MQTT tab keeps its changes until
  // Save & restart (the server restarts and the page reconnects). The time server switch of the Settings
  // tab is left out: it does nothing until a server is entered.
  if (window.__recipes !== false) {
    const RECIPES = [
      { tab: 'server', ids: ['srv-watch', 'backup-daily', 'srv-prometheus'] },
      { tab: 'tools', ids: ['tools-live-btn', 'tools-autoscroll-btn', 'tools-redact'], sameView: true },
      { tab: 'mqtt', ids: ['cfg-icons', 'cfg-cat-diagnostics', 'cfg-ent-mute'], save: 'cfg-save-btn' }
    ];
    const stateOf = function (id) {
      const e = document.getElementById(id);
      if (!e) return 'gone';
      if (e.tagName === 'INPUT') return String(e.checked);
      return e.getAttribute('aria-pressed') || (/\b(on|good)\b/.test(e.className) ? 'on' : 'off');
    };
    const yes = function (m) { window.__rt.dialogs.push('confirm: ' + String(m).slice(0, 90)); return true; };
    const answered = window.confirm;
    // The experimental switches ask first; they are answered yes and switched back.
    RECIPES.push({ tab: 'server', ids: ['exp-tilehiding'], confirm: true });
    for (const recipe of RECIPES.filter(function (r) { return !window.__recipeOnly || window.__recipeOnly.test(r.ids.join(' ')); })) {
      window.confirm = recipe.confirm ? yes : answered;
      for (const id of recipe.ids) {
        window.__rt.current = recipe.tab + ' ' + id;
        const rec = { tab: recipe.tab, label: id, type: 'recipe' };
        const flip = async function () {
          document.getElementById(id).click(); await wait(recipe.save ? 500 : 2500);
          if (recipe.save) {
            // Saving restarts the server: wait until it answers again before the tab is shown.
            document.getElementById(recipe.save).click(); await wait(10000); // the server restarts a few seconds after the save
            for (let tries = 0; tries < 30; tries++) {
              try { if ((await fetch('/api/update', { cache: 'no-store' })).ok) break; } catch (e) { /* not back yet */ }
              await wait(1000);
            }
            await wait(1500);
          }
          if (!recipe.sameView) await reload(recipe.tab); // a view's own state (the log viewer) is not kept by the server
          await wait(1000);
        };
        showTab(recipe.tab); await wait(3000);
        rec.before = stateOf(id);
        await flip(); rec.after = stateOf(id);
        if (rec.after !== rec.before) await flip();
        rec.restored = stateOf(id); rec.ok = rec.after !== rec.before && rec.restored === rec.before;
        window.__rt.log.push(rec);
      }
    }
    window.confirm = answered;

    // The time server: a server is entered and saved, which switches the clock on, then switched off and an
    // empty server saved, which puts the form back as it was.
    if (!window.__recipeOnly || window.__recipeOnly.test('ntp')) {
      window.__rt.current = 'server ntp';
      const ntp = { tab: 'server', label: 'ntp (server entered, then removed)', type: 'recipe' };
      const field = function () { return document.getElementById('ntp-server'); };
      const saveServer = async function (value) {
        field().value = value; field().dispatchEvent(new Event('input', { bubbles: true }));
        document.getElementById('ntp-save').click(); await wait(4000); await reload('server');
      };
      showTab('server'); await wait(3000);
      ntp.before = stateOf('ntp-toggle') + '/' + field().value;
      await saveServer('pool.ntp.org'); ntp.after = stateOf('ntp-toggle') + '/' + field().value;
      if (stateOf('ntp-toggle') !== 'off') { document.getElementById('ntp-toggle').click(); await wait(3000); }
      // The form cannot save an empty server (Save with an empty field only focuses it), so the saved server
      // stays after the switch is off; it is cleared here with the page's own command, as E2E-115 does.
      await reload('server');
      const stored = (await (await fetch('/api/update', { cache: 'no-store' })).json()).ntpSettings;
      ntp.keptAfterOff = stored && stored.server;
      await sendCommand('setNtpServer', ''); await reload('server');
      const cleared = (await (await fetch('/api/update', { cache: 'no-store' })).json()).ntpSettings;
      ntp.restored = stateOf('ntp-toggle') + '/' + field().value;
      ntp.ok = ntp.after.indexOf('pool.ntp.org') > 0 && ntp.after !== ntp.before && ntp.restored === ntp.before &&
        stored && stored.enabled === false && cleared && cleared.server === '' && cleared.enabled === false;
      window.__rt.log.push(ntp);
    }

    // LG's Home screen layout (experimental): switched on, its tiles are switched one by one, then it is
    // switched off again. LG's layout is only changed when the TV next starts.
    if (!window.__recipeOnly || window.__recipeOnly.test('homescreen')) {
      window.confirm = yes;
      const tile = { tab: 'server', label: 'exp-homelayout', type: 'recipe' };
      showTab('server'); await wait(3000);
      tile.before = stateOf('exp-homelayout');
      document.getElementById('exp-homelayout').click(); await wait(4000); await reload('server');
      tile.after = stateOf('exp-homelayout');
      if (tile.after !== tile.before) {
        for (const id of ['btn-home-hdmi1', 'btn-home-hdmi2', 'btn-home-hdmi3', 'btn-home-hdmi4', 'btn-home-sideIcons', 'btn-home-quickCards', 'btn-home-recommended']) {
          window.__rt.current = 'homescreen ' + id;
          const rec = { tab: 'homescreen', label: id, type: 'recipe' };
          showTab('homescreen'); await wait(3000);
          const e = document.getElementById(id);
          if (!e || e.disabled) { rec.skipped = e ? 'disabled' : 'gone'; window.__rt.log.push(rec); continue; }
          rec.before = stateOf(id); e.click(); await wait(3500); await reload('homescreen');
          rec.after = stateOf(id);
          if (rec.after !== rec.before) { document.getElementById(id).click(); await wait(3500); await reload('homescreen'); }
          rec.restored = stateOf(id); rec.ok = rec.after !== rec.before && rec.restored === rec.before;
          window.__rt.log.push(rec);
        }
        showTab('server'); await wait(3000);
        document.getElementById('exp-homelayout').click(); await wait(4000); await reload('server');
      }
      tile.restored = stateOf('exp-homelayout'); tile.ok = tile.after !== tile.before && tile.restored === tile.before;
      window.__rt.log.push(tile);
      window.confirm = answered;
    }
  }
  window.__rt.done = true;
})();
