/* Public process API contract against the real native executable. */
'use strict';
const assert = require('assert');
const child = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const binary = path.resolve(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'native-process-contract-'));
const pageKb = Number(child.execFileSync('getconf', ['PAGESIZE'], { encoding: 'utf8' })) / 1024;
function processRow(pid, comm, pages, args) {
  const base = path.join(root, String(pid));
  fs.mkdirSync(base);
  fs.writeFileSync(path.join(base, 'stat'), pid + ' (' + comm + ') S 1 0 0');
  fs.writeFileSync(path.join(base, 'statm'), '9999 ' + pages + ' 0 0');
  fs.writeFileSync(path.join(base, 'cmdline'), args.split(' ').join('\0') + '\0');
}
try {
  processRow(3794, 'hal-gal', 10240, '/usr/sbin/hal-gal -d --config /etc/hal');
  processRow(4121, 'long-process-na', 3840, '/usr/bin/livingroom-feeder --room living room');
  processRow(2, 'kthreadd', 0, '');
  processRow(71, 'WebAppMgr', 512, '/usr/bin/WebAppMgr /usr/palm/applications/com.webos.app.browser/index.html');
  processRow(72, 'WebAppMgr', 256, '/usr/bin/WebAppMgr --type=renderer');
  for (let n = 0; n < 12; ++n) processRow(100 + n, 'worker' + n, n * 256, '/usr/bin/worker' + n);
  fs.mkdirSync(path.join(root, 'self'));
  fs.mkdirSync(path.join(root, '999')); // disappeared before its stat could be read
  // MB at 4 KiB per page; the CLI uses the host page size. Equal sizes retain PID order.
  const expectedRows = [
    [3794, 'hal-gal', 40], [4121, 'livingroom-feeder', 15],
    [111, 'worker11', 11], [110, 'worker10', 10], [109, 'worker9', 9],
    [108, 'worker8', 8], [107, 'worker7', 7], [106, 'worker6', 6],
    [105, 'worker5', 5], [104, 'worker4', 4], [103, 'worker3', 3],
    [71, 'WebAppMgr (browser)', 2], [102, 'worker2', 2],
    [72, 'WebAppMgr (renderer)', 1], [101, 'worker1', 1],
    [2, 'kthreadd', 0], [100, 'worker0', 0]
  ].map(([pid, name, mb]) => ({ pid, name, mb: mb * pageKb / 4 }));
  for (const all of [false, true]) {
    const args = ['processes', '--proc-root', root].concat(all ? ['--all'] : []);
    const envelope = JSON.parse(child.execFileSync(binary, args, { encoding: 'utf8' }));
    assert.strictEqual(envelope.protocol, 1);
    assert.deepStrictEqual(envelope.result, {
      ok: true, count: 17, totalMb: 124 * pageKb / 4,
      top: all ? expectedRows : expectedRows.slice(0, 10)
    });
  }
  const missing = JSON.parse(child.execFileSync(binary, ['processes', '--proc-root', root + '/absent'], { encoding: 'utf8' }));
  assert.strictEqual(missing.result.ok, false);
  assert.notStrictEqual(child.spawnSync(binary, ['processes', '--unexpected']).status, 0);
  console.log('Native process API contract passes for top/all, names, totals and errors');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
