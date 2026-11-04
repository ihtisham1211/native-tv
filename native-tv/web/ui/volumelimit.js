// The volume ceiling, on the Advanced tab: a maximum, a quieter night limit and a start-up
// volume, kept by the server (native-tv/src/daemon/volume_ceiling_enforcer.cpp).

const VL_LEVELS = Array.from({ length: 20 }, (_, i) => (i + 1) * 5);
let volumeLimit = null;

function vlLabel(v) {
  if (v >= 100) return t('adv.volumeLimit.noLimit', 'No limit');
  return v === 0 ? t('adv.volumeLimit.silent', 'Silent') : String(v);
}

function vlStartupLabel(v) {
  return v < 0 ? t('adv.volumeLimit.startup.keep', 'Leave as it was') : String(v);
}

function renderVolumeLimit(d) {
  volumeLimit = d;
  const s = d.settings;
  const locked = !d.writable;
  updateToggle('vl-enabled', {
    cls: s.enabled ? 'good' : 'idle',
    label: s.enabled ? t('common.on', 'On') : t('common.off', 'Off'),
    on: s.enabled,
    disabled: locked
  });
  const note = q('vl-note');
  note.textContent = locked
    ? t('adv.volumeLimit.controlsOff', 'Controls are turned off in the server’s config, so the volume cannot be held.')
    : '';
  note.hidden = !locked;

  stOptions(q('vl-max'), VL_LEVELS, s.maxVolume, vlLabel);
  stOptions(q('vl-night-max'), [0].concat(VL_LEVELS), s.nightMaxVolume, vlLabel);
  stOptions(q('vl-startup'), [-1, 0].concat(VL_LEVELS), s.startupVolume, vlStartupLabel);
  q('vl-night-from').value = s.nightStart;
  q('vl-night-to').value = s.nightEnd;
  for (const id of ['vl-max', 'vl-night-max', 'vl-startup', 'vl-night-from', 'vl-night-to'])
    q(id).disabled = locked;
}

async function loadVolumeLimit() {
  try {
    const d = await (await fetch(api('/api/volumelimit'), { cache: 'no-store' })).json();
    if (d.ok) renderVolumeLimit(d);
  } catch (e) { /* the section keeps what it showed */ }
}

async function saveVolumeLimit(patch) {
  try {
    const r = await fetch(api('/api/volumelimit'), {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(patch)
    });
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || t('common.actionFailed', 'Action failed'));
    clearErr();
    renderVolumeLimit(d);
  } catch (e) {
    showErr(e.message);
    loadVolumeLimit();
  }
}

function toggleVolumeLimit() {
  if (volumeLimit) saveVolumeLimit({ enabled: !volumeLimit.settings.enabled });
}
