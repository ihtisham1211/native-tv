// A report of this TV to paste into an issue: the versions, and which readings the TV
// answered. It reads only the fields named here, so no address, MAC address, network or host
// name, device name or setting can get into it.

// Each group of readings, and the stats fields that show it was answered.
const REPORT_READINGS = [
  ['Picture', ['backlight', 'dimming', 'picture_engine', 'colorimetry']],
  ['Sound', ['sound', 'audio_output', 'volume_control']],
  ['HDMI signal', ['hdmi_links', 'hdmi_sources', 'signal', 'signal_format', 'signal_timing']],
  ['Temperature', ['temp', 'temps']],
  ['Storage', ['emmc', 'appStorage']],
  ['Network', ['wifi', 'netTotal']],
  ['OLED panel', ['oled']],
  ['Magic Remote', ['remote']],
  ['Light sensor', ['lightSensor']],
  ['Lights', ['lights']]
];

function reportAnswered(value) {
  if (value === null || value === undefined || value === false || value === '') return false;
  if (typeof value === 'object') return Object.keys(value).length > 0;
  return true;
}

// One table cell: a single line with no pipe, or a dash when there is nothing.
function reportCell(value) {
  const text = value === null || value === undefined ? '' : String(value).replace(/[|\r\n]+/g, ' ').trim();
  return text || '\u2014';
}

function tvReportText(d) {
  const stats = d.stats || {}, caps = d.caps || {}, home = d.home || {};
  const system = stats.system || {}, hardware = stats.hardware || {}, device = stats.device || {};
  const answered = [], missing = [];
  for (const [name, keys] of REPORT_READINGS)
    (keys.some(key => reportAnswered(stats[key])) ? answered : missing).push(name);
  const nativeHome = home.ok !== true ? ''
    : home.supported && home.supported.ok === false ? 'not supported on this webOS'
    : home.nativeHome && home.nativeHome.halted ? 'stopped after repeated failures' : 'supported';
  const rows = [
    ['Native Tv', stats.tvwebVersion || caps.version],
    ['Model', device.model],
    ['webOS', system.webos ? 'webOS ' + system.webos : ''],
    ['Firmware', system.firmware],
    ['SoC', hardware.soc_arch],
    ['Memory', hardware.ram],
    ['Installed from', caps.fromHomebrewChannel ? 'Homebrew Channel' : 'SSH or a script'],
    ['Controls', caps.allowControl ? 'allowed' : 'off in the config'],
    ['Power actions', caps.allowPower ? 'allowed' : 'off in the config'],
    ['Native Tv Home', nativeHome],
    ['Power state', stats.powerState]
  ];
  return [
    '**Native Tv TV report**',
    '',
    '| | |',
    '| --- | --- |'
  ].concat(rows.map(row => '| ' + row[0] + ' | ' + reportCell(row[1]) + ' |'), [
    '',
    'Answered: ' + (answered.join(', ') || '\u2014'),
    'Not answered: ' + (missing.join(', ') || '\u2014'),
    '',
    'Left out: addresses, MAC address, network and host names, the TV\'s name and every setting.'
  ]).join('\n') + '\n';
}

async function createTvReport() {
  const get = async path => {
    try { return await (await fetch(api(path), { cache: 'no-store' })).json(); } catch (e) { return {}; }
  };
  const [stats, caps, home] = await Promise.all([get('/api/stats'), get('/api/caps'), get('/api/home')]);
  q('report-text').value = tvReportText({ stats, caps, home });
  q('report-text').hidden = false;
  q('report-copy').hidden = false;
  q('report-msg').textContent = '';
}

async function copyTvReport() {
  const msg = q('report-msg');
  try {
    await copyToClipboard(q('report-text').value);
    msg.textContent = t('server.report.copied', 'Copied');
  } catch (e) {
    q('report-text').select();
    msg.textContent = t('server.report.select', 'Select the text and copy it');
  }
}
