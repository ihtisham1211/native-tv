// The boot hook's sam_reread_app, with only luna-send replaced: sam rereads an app on a block
// followed by a release, and nothing is blocked where sam has no release (a G3 on webOS 8.4.0
// blocked LG's Home and could not release it, so it would not launch until a reboot).
var assert = require('assert');
var child = require('child_process');
var fs = require('fs');
var os = require('os');
var path = require('path');

console.log('Running test-sam-reread-app.js ...');

var source = fs.readFileSync(path.join(__dirname, '../../lifecycle/50-tvweb.sh'), 'utf8');
var begin = source.indexOf('sam_reread_app() {');
var end = source.indexOf('\n}\n', begin) + 2;
assert.ok(begin !== -1 && end > begin, 'the boot hook keeps sam_reread_app');
var fn = source.slice(begin, end).replace(/luna-send/g, 'fake_luna').replace(/sleep 1/g, 'true');

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tvweb-sam-reread-'));
var calls = path.join(dir, 'calls.log');
process.on('exit', function () {
  try { fs.unlinkSync(calls); } catch (e) {}
  try { fs.rmdirSync(dir); } catch (e) {}
});

// release: what sam answers a release; block: what it answers a block.
function run(release, block) {
  fs.writeFileSync(calls, '');
  var script = 'fake_luna() {\n' +
    'case "$*" in\n' +
    '*releaseBlockSystemApp*) echo release >> "' + calls + '"; printf "%s\\n" \'' + release + '\' ;;\n' +
    '*blockSystemApp*) echo block >> "' + calls + '"; printf "%s\\n" \'' + block + '\' ;;\n' +
    'esac\n}\n' + fn + '\nsam_reread_app com.webos.app.home\n';
  var status = 0;
  try {
    child.execFileSync('/bin/sh', ['-c', script], { encoding: 'utf8' });
  } catch (e) {
    status = e.status;
  }
  return { status: status, calls: fs.readFileSync(calls, 'utf8').split('\n').filter(Boolean) };
}

var ok = '{"returnValue": true}';
var unknown = '{"returnValue":false,"errorCode":-1,"errorText":"Unknown method \\"releaseBlockSystemApp\\" for category \\"/\\""}';

var result = run(ok, ok);
assert.strictEqual(result.status, 0, 'a block and a release where sam has both');
assert.deepStrictEqual(result.calls, ['release', 'block', 'release'], 'the release is asked first');

result = run(unknown, ok);
assert.strictEqual(result.status, 1, 'the caller falls back where sam has no release');
assert.deepStrictEqual(result.calls, ['release'], 'and nothing is blocked');

result = run('', ok);
assert.strictEqual(result.status, 1, 'no answer to the release blocks nothing either');
assert.deepStrictEqual(result.calls, ['release']);

console.log('test-sam-reread-app.js passed');
