/* Native config-port reader used by the TV deployment and app scripts. */
'use strict';
const assert = require('assert');
const child = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const binary = path.resolve(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'native-service-port-'));

function write(contents) {
  fs.writeFileSync(path.join(root, 'config.json'), contents);
}
function expect(port) {
  const output = child.execFileSync(binary, ['port', '--data-root', root], {
    encoding: 'utf8'
  });
  assert.strictEqual(output, String(port) + '\n');
}

try {
  write('{"port":8091}');
  expect(8091);
  write('{"port":"8092"}');
  expect(8092);
  write('{}');
  expect(8080);
  write('{"port":0}');
  expect(8080);
  write('{"port":65536}');
  expect(8080);
  write('{"port":8080.5}');
  expect(8080);
  write('{"port":"8093tail"}');
  expect(8080);
  write('{invalid');
  expect(8080);
  fs.unlinkSync(path.join(root, 'config.json'));
  expect(8080);

  const invalid = child.spawnSync(binary, ['port', '--proc-root', root], {
    encoding: 'utf8'
  });
  assert.notStrictEqual(invalid.status, 0, 'port rejects unrelated proc-root options');
  console.log('Native service port config contract passes');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
