// Start-up: the opening tab, the column layout and the polling that drives the page.

/*
 * The restart drops this connection mid-flight, so failures here are expected
 * until the server is listening again. Give up after ~30s rather than spinning:
 * a config that stops the server booting needs the log, not another poll.
 */
async function waitForServer() {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    await new Promise(r => setTimeout(r, 1000));
    try {
      const r = await fetch(api('/api/caps'), { cache: 'no-store' });
      if (r.ok) {
        cfgMsg(t('mqtt.applied', 'Applied.'), 'good');
        const saveBtn = q('cfg-save-btn');
        if (saveBtn) { saveBtn.disabled = false; saveBtn.classList.remove('busy'); }
        if (typeof loadSettings === 'function') await loadSettings(true);
        setTimeout(() => {
          if (q('cfg-msg') && q('cfg-msg').textContent === t('mqtt.applied', 'Applied.')) {
            cfgMsg(t('mqtt.save.note', 'Saving restarts the server. The dashboard reconnects on its own.'));
          }
        }, 4000);
        tick();
        return;
      }
    } catch (e) { /* still down */ }
  }
  cfgMsg(t('mqtt.notBack', 'Saved, but the server did not come back. Check the log on the TV.'), 'bad');
  const saveBtn = q('cfg-save-btn');
  if (saveBtn) { saveBtn.disabled = false; saveBtn.classList.remove('busy'); }
}

/*
 * Which tab to open on. ?tab= names one; the older ?privacy=1 and ?controls=1
 * still work, since they are in bookmarks and in the screenshot tooling. Failing
 * both, the last tab this browser used.
 */
(function initTab() {
  const p = new URLSearchParams(location.search);
  let want = p.get('tab');
  if (!want && p.get('privacy') === '1') want = 'privacy';
  if (!want && p.get('apps') === '1') want = 'apps';
  if (!want && p.get('controls') === '1') want = 'control';
  // ?open= names a disclosure inside Metrics or Control, so it has to pick that tab too -
  // otherwise the link opens something on a panel that is not showing.
  if (!want && p.get('open')) want = p.get('open') === 'dpad' ? 'control' : 'metrics';
  if (!want) { try { want = localStorage.getItem('tab'); } catch (e) { /* private window */ } }
  showTab(want || 'control');
  if (p.get('expand') === '1') document.querySelectorAll('.pv-d').forEach(d => d.open = true);
})();

const openSection = new URLSearchParams(location.search).get('open');
if ((openSection === 'metrics' || openSection === 'system') && q('disc-hwinfo')) q('disc-hwinfo').open = true;

if (openSection === 'processes' && q('disc-proc')) q('disc-proc').open = true;

if (openSection === 'dpad') q('sheet-remote').showModal();

let pollTimer = null;

function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(tick, 5000);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    stopPolling();
    stopCpuPolling();
  } else {
    tick();
    startPolling();
    if (cpuPanelOpen()) startCpuPolling();
  }
});

tick();
startPolling();
loadServerStatus();
