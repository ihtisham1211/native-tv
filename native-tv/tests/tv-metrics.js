/* Bounded, read-only native worker/API verification, run with node on the TV. ES5. */
var assert = require('assert');
var child = require('child_process');
var fs = require('fs');
var http = require('http');
var port = Number(process.argv[2] || 8080);
assert.ok(port > 0 && port <= 65535 && Math.floor(port) === port, 'valid server port required');
var results = {}, left = 6, maximumWorkers = 0;
var deadline = setTimeout(function () { throw new Error('native metrics TV verification timed out'); }, 15000);
var watcher = setInterval(function () {
  var count = 0;
  fs.readdirSync('/proc').forEach(function (pid) {
    if (!/^\d+$/.test(pid)) return;
    try {
      var args = fs.readFileSync('/proc/' + pid + '/cmdline', 'utf8').split('\0');
      if (args[0] === '/var/lib/tvweb/native/nativetv-service' && args[1] === 'cpu') count++;
    } catch (ignored) {}
  });
  maximumWorkers = Math.max(maximumWorkers, count);
}, 250);
function get(key, path) {
  http.get({ host: '127.0.0.1', port: port, path: path }, function (response) {
    assert.strictEqual(response.statusCode, 200);
    var bytes = '';
    response.on('data', function (data) { bytes += data; });
    response.on('end', function () {
      var data = JSON.parse(bytes);
      assert.strictEqual(data.ok, true, data.error);
      if (key !== 'system') assert.ok(Array.isArray(data.top));
      results[key] = data;
      if (--left === 0) finish();
    });
  }).on('error', function (error) { throw error; });
}
function getNativeSystem() {
  child.execFile('/var/lib/tvweb/native/nativetv-service', ['system'],
    { timeout: 12000, maxBuffer: 2 * 1024 * 1024 }, function (error, stdout) {
      assert.ifError(error);
      var envelope = JSON.parse(stdout);
      assert.strictEqual(envelope.protocol, 1);
      assert.strictEqual(envelope.result.ok, true, envelope.result.error);
      results.nativeSystem = envelope.result;
      if (--left === 0) finish();
    });
}
function finish() {
  clearInterval(watcher);
  clearTimeout(deadline);
  assert.strictEqual(maximumWorkers, 1, 'concurrent CPU reads must use one native worker');
  assert.deepEqual(results.cpu1, results.cpu2, 'coalesced callers must receive the same sample');
  assert.strictEqual(results.cpu1.windowMs, 5000);
  assert.ok(isFinite(results.cpu1.busy) && results.cpu1.busy >= 0);
  assert.ok(results.cpu1.top.length <= 10);
  assert.ok(results.memory.top.length <= 10);
  assert.strictEqual(results.all.top.length, results.all.count);
  assert.ok(results.all.count > 0 && results.all.totalMb > 0);
  var system = results.system;
  assert.ok(system.mem && system.mem.total > 0 && system.mem.avail >= 0);
  assert.ok(system.swap && system.swap.total >= 0 && system.swap.free >= 0);
  assert.ok(system.emmc && typeof system.emmc.wear === 'string');
  assert.ok(system.temp === null || (system.temp > 0 && system.tempMillidegrees > 0));
  assert.ok(system.cpuHz === null || system.cpuHz >= 100000000);
  assert.ok(system.gpuHz === null || system.gpuHz >= 0);
  assert.ok(system.appStorage && system.appStorage.totalKb > 0);
  assert.ok(system.appStorage.availKb >= 0 && system.appStorage.availKb <= system.appStorage.totalKb);
  assert.ok(system.cpuTimes && typeof system.cpuTimes === 'object', 'CPU times are published');
  assert.ok(system.uptime > 0 && system.btime > 0, 'uptime and boot time are readable');
  assert.ok(Array.isArray(system.loadavg) && system.loadavg.length === 3);
  assert.ok(Array.isArray(system.cores) && system.coresTotal >= system.cores.length);
  assert.ok(system.netTotal === null || (system.netTotal.rx >= 0 && system.netTotal.tx >= 0));
  assert.ok(system.wifi === null || isFinite(system.wifi.level));
  var nativeSystem = results.nativeSystem;
  assert.strictEqual(typeof nativeSystem.hasHdrStatus, 'boolean', 'HDR capability is published natively');
  assert.strictEqual(nativeSystem.hasHdrStatus, fs.existsSync('/proc/lg/pe/hdr_status'),
    'native HDR capability matches the TV filesystem');
  assert.strictEqual(typeof nativeSystem.hasHdmiProc, 'boolean', 'HDMI capability is published natively');
  assert.strictEqual(nativeSystem.hasHdmiProc, fs.existsSync('/proc/lg/hdmi20'),
    'native HDMI capability matches the TV filesystem');
  assert.ok(system.picture_engine === null ||
    (typeof system.picture_engine === 'object' &&
     Object.prototype.hasOwnProperty.call(system.picture_engine, 'colorimetry') &&
     Object.prototype.hasOwnProperty.call(system.picture_engine, 'hdr_mode') &&
     system.picture_engine.colorimetry === system.colorimetry),
  'picture-engine readings match the published colourimetry');
  assert.ok(system.remote === null ||
    (typeof system.remote === 'object' && system.remote.paired === true &&
     (system.remote.battery === null || isFinite(system.remote.battery))),
  'the paired remote facts are readable');
  console.log(JSON.stringify({ ok: true, maximumCpuWorkers: maximumWorkers,
    windowMs: results.cpu1.windowMs, active: results.cpu1.active,
    processCount: results.all.count, processRows: results.all.top.length,
    memoryKb: system.mem.total, temperature: system.temp, storageKb: system.appStorage.totalKb,
    network: system.netTotal ? system.netTotal.iface : null, wifiDbm: system.wifi ? system.wifi.level : null,
    colorimetry: system.picture_engine ? system.picture_engine.colorimetry : null,
    hasHdrStatus: nativeSystem.hasHdrStatus,
    hasHdmiProc: nativeSystem.hasHdmiProc,
    remoteModel: system.remote ? system.remote.model : null,
    remoteBattery: system.remote ? system.remote.battery : null }));
}
get('cpu1', '/api/cpu');
get('cpu2', '/api/cpu');
get('memory', '/api/processes');
get('all', '/api/processes?all=1');
get('system', '/api/stats');
getNativeSystem();
