// The real boot hook's Home launch decision, with only Luna and file paths replaced.
var assert = require('assert');
var child = require('child_process');
var fs = require('fs');
var os = require('os');
var path = require('path');

if (typeof child.execFileSync !== 'function') {
  console.log('Skipping test-home-boot.js on node without execFileSync');
  process.exit(0);
}

var source = fs.readFileSync(path.join(__dirname, '../../lifecycle/50-tvweb.sh'), 'utf8');
var dir = path.join(os.tmpdir(), 'tvweb-home-boot-' + process.pid);
fs.mkdirSync(dir);
var homeFile = path.join(dir, 'home.json');
var bootFile = path.join(dir, 'bootd.log');
var logFile = path.join(dir, 'boot.log');
var nativeDaemon = path.join(dir, 'nativetv-daemon');
var homeStage = path.join(dir, 'home-takeover');
fs.mkdirSync(homeStage);
fs.writeFileSync(path.join(homeStage, 'appinfo.json'), '{}\n');
var nativeCalls = path.join(dir, 'native-calls.log');
var lunaCalls = path.join(dir, 'luna-calls.log');
var fallbackCalls = path.join(dir, 'fallback-calls.log');
var homeOpened = path.join(dir, 'home-opened.log');
process.on('exit', function () {
  [homeFile, bootFile, logFile, nativeDaemon, path.join(homeStage, 'appinfo.json'), nativeCalls, lunaCalls,
    fallbackCalls, homeOpened].forEach(function (file) {
    try { fs.unlinkSync(file); } catch (e) {}
  });
  try { fs.rmdirSync(homeStage); } catch (e) {}
  try { fs.rmdirSync(dir); } catch (e) {}
});

function quote(s) { return "'" + s.replace(/'/g, "'\\''") + "'"; }
function shellFunction(name) {
  var begin = source.indexOf(name + '() {');
  var end = source.indexOf('\n}\n', begin) + 2;
  assert.ok(begin !== -1 && end > begin, 'the boot hook keeps ' + name);
  return source.slice(begin, end);
}

function check(config, reason, front, expected, label) {
  fs.writeFileSync(homeFile, JSON.stringify(config));
  fs.writeFileSync(bootFile, "BootSequencer) 'com.webos.app.hdmi4' is foreground.\n");
  fs.writeFileSync(logFile, '');
  var decision = shellFunction('home_open_at_boot_compat')
    .replace(/\/var\/lib\/tvweb\/home\.json/g, homeFile)
    .replace(/\/var\/log\/bootd\.log/g, bootFile)
    .replace(/luna-send/g, 'fake_luna');
  var script = 'fake_luna() {\n' +
    'case "$*" in\n' +
    '*getPowerOnReason*) printf "%s\\n" ' + quote(JSON.stringify({ reason: reason })) + ' ;;\n' +
    '*getConfigs*) printf "%s\\n" \'{}\' ;;\n' +
    '*getForegroundAppInfo*) printf "%s\\n" ' + quote(JSON.stringify({ appId: front })) + ' ;;\n' +
    '*launch*) printf "%s\\n" \'{"returnValue":true}\' ;;\n' +
    'esac\n}\n' + 'bootlog() { printf "%s\\n" "$*" >>' + quote(logFile) + '; }\n' + decision +
    '\nhome_open_at_boot_compat\nwait\n';
  child.execFileSync('/bin/sh', ['-c', script], { encoding: 'utf8' });
  var output = fs.readFileSync(logFile, 'utf8');
  // The hook logs the open; every other outcome logs why Home was not opened.
  assert.strictEqual(/nativeTv Home opened over/.test(output), expected, label + ': ' + output);
}

check({ openOnPowerOn: true, lgAutoLaunch: null }, 'remoteKey', 'com.webos.app.hdmi4', true,
  'the explicit preference works without an earlier LG Home preference');
check({ openOnPowerOn: true, lgAutoLaunch: 'off' }, 'localKey', 'com.webos.app.hdmi4', true,
  'the explicit preference overrides Recent Input');
check({ openOnPowerOn: false, lgAutoLaunch: null }, 'remoteKey', 'com.webos.app.hdmi4', false,
  'following LG does not force Home when it was not chosen');
check({ lgAutoLaunch: 'on' }, 'remoteKey', 'com.webos.app.hdmi4', true,
  'an older layout retains its startup behavior');
check({ openOnPowerOn: true }, 'wakeOnLan', 'com.webos.app.hdmi4', false,
  'a background wake does not open Home');
check({ openOnPowerOn: true }, 'remoteKey', 'netflix', false,
  'an app chosen during startup stays in front');
check({ openOnPowerOn: true }, 'remoteKey', 'com.webos.app.hdmi2', false,
  'an input chosen during startup stays in front');

function checkFailedNativeDispatchDoesNotReplay() {
  fs.writeFileSync(homeFile, JSON.stringify({ openOnPowerOn: true }));
  fs.writeFileSync(bootFile, "BootSequencer) 'com.webos.app.hdmi4' is foreground.\n");
  fs.writeFileSync(logFile, '');
  fs.writeFileSync(nativeCalls, '');
  try { fs.unlinkSync(lunaCalls); } catch (e) {}
  fs.writeFileSync(nativeDaemon, '#!/bin/sh\nprintf "%s\\n" "$*" >> ' + quote(nativeCalls) + '\nexit 1\n');
  fs.chmodSync(nativeDaemon, 493); // 0755

  var compatibility = shellFunction('home_open_at_boot_compat')
    .replace(/\/var\/lib\/tvweb\/home\.json/g, homeFile)
    .replace(/\/var\/log\/bootd\.log/g, bootFile)
    .replace(/luna-send/g, 'fake_luna');
  var nativeWrapper = shellFunction('home_open_at_boot')
    .replace('"$DAEMON_BIN"', quote(nativeDaemon));
  var fakeLuna = 'fake_luna() {\n' +
    'printf "%s\\n" "$*" >> ' + quote(lunaCalls) + '\n' +
    'case "$*" in\n' +
    '*getPowerOnReason*) printf "%s\\n" \'{"reason":"remoteKey"}\' ;;\n' +
    '*getConfigs*) printf "%s\\n" \'{}\' ;;\n' +
    '*getForegroundAppInfo*) printf "%s\\n" \'{"appId":"com.webos.app.hdmi4"}\' ;;\n' +
    '*launch*) printf "%s\\n" \'{"returnValue":true}\' ;;\n' +
    'esac\n}\n';
  var script = fakeLuna +
    'bootlog() { printf "%s\\n" "$*" >> ' + quote(logFile) + '; }\n' +
    compatibility + '\n' + nativeWrapper + '\nhome_open_at_boot\nwait\n';
  child.execFileSync('/bin/sh', ['-c', script], { encoding: 'utf8' });

  assert.ok(/--open-home-at-boot/.test(fs.readFileSync(nativeCalls, 'utf8')),
    'the native Home boot policy is dispatched');
  assert.ok(/native Home boot policy failed after dispatch/.test(fs.readFileSync(logFile, 'utf8')),
    'a failed native dispatch is logged');
  assert.ok(!fs.existsSync(lunaCalls),
    'a failed native dispatch does not enter the shell compatibility policy');
}

checkFailedNativeDispatchDoesNotReplay();

function runNativeTakeover(exitCode) {
  fs.writeFileSync(nativeCalls, '');
  fs.writeFileSync(logFile, '');
  try { fs.unlinkSync(fallbackCalls); } catch (e) {}
  try { fs.unlinkSync(homeOpened); } catch (e) {}
  try { fs.unlinkSync(nativeDaemon); } catch (e) {}
  if (exitCode !== null) {
    fs.writeFileSync(nativeDaemon,
      '#!/bin/sh\nprintf "%s\\n" "$*" >> ' + quote(nativeCalls) + '\nexit ' + exitCode + '\n');
    fs.chmodSync(nativeDaemon, 493); // 0755
  } else {
    fs.writeFileSync(homeFile, JSON.stringify({ enabled: true }));
  }
  var dispatch = shellFunction('native_boot_dispatch')
    .replace('"$DAEMON_BIN"', quote(nativeDaemon));
  var boot = shellFunction('home_takeover_boot')
    .replace('"$DAEMON_BIN"', quote(nativeDaemon))
    .replace('/var/lib/tvweb/home.json', quote(homeFile));
  var takeover = shellFunction('native_home_takeover');
  var script =
    'bootlog() { printf "%s\\n" "$*" >> ' + quote(logFile) + '; }\n' +
    'legacy_home_takeover() { printf "fallback\\n" >> ' + quote(fallbackCalls) + '; }\n' +
    'home_open_at_boot() { printf "opened\\n" >> ' + quote(homeOpened) + '; }\n' +
    'home_os=' + (exitCode === null ? '1' : '0') + '\n' +
    'homestage=' + quote(homeStage) + '\n' + dispatch + '\n' + takeover +
    '\n' + boot + '\nhome_takeover_boot\n';
  child.execFileSync('/bin/sh', ['-c', script], { encoding: 'utf8' });
}

runNativeTakeover(1);
assert.ok(/--takeover-home/.test(fs.readFileSync(nativeCalls, 'utf8')),
  'the native Home takeover command is dispatched');
assert.ok(!/--blank-home-layout/.test(fs.readFileSync(nativeCalls, 'utf8')),
  'the native manager reads its own Home layout policy');
assert.ok(/native Home lifecycle failed after dispatch; not replaying/.test(
  fs.readFileSync(logFile, 'utf8')),
  'a failed native takeover is logged');
assert.ok(!fs.existsSync(fallbackCalls),
  'a failed native takeover does not replay through the shell fallback');
assert.ok(!fs.existsSync(homeOpened),
  'Home is not launched after a failed takeover');

runNativeTakeover(0);
assert.ok(fs.existsSync(homeOpened),
  'a successful native takeover opens Home under the boot policy');
assert.ok(!fs.existsSync(fallbackCalls),
  'a successful native takeover skips the compatibility path');

runNativeTakeover(3);
assert.ok(/--takeover-home/.test(fs.readFileSync(nativeCalls, 'utf8')),
  'the native Home decision still runs when its saved setting disables takeover');
assert.ok(/native Home lifecycle skipped by saved Home settings/.test(
  fs.readFileSync(logFile, 'utf8')),
  'a saved disabled setting is reported as a skip');
assert.ok(!fs.existsSync(fallbackCalls),
  'a disabled native takeover does not fall back to the shell lifecycle');
assert.ok(!fs.existsSync(homeOpened),
  'a disabled native takeover does not trigger the Home boot policy');

runNativeTakeover(4);
assert.ok(/--takeover-home/.test(fs.readFileSync(nativeCalls, 'utf8')),
  'firmware support is checked by the native Home lifecycle');
assert.ok(/firmware is unsupported or experimental Home is disabled/.test(
  fs.readFileSync(logFile, 'utf8')),
  'an unsupported native takeover is reported as a skip');
assert.ok(!fs.existsSync(fallbackCalls),
  'an unsupported native takeover does not run the shell compatibility lifecycle');
assert.ok(!fs.existsSync(homeOpened),
  'an unsupported native takeover does not trigger the Home boot policy');

runNativeTakeover(null);
assert.ok(fs.existsSync(fallbackCalls),
  'an install without the native daemon still uses the compatibility path');
console.log('ALL test-home-boot.js assertions passed!');
