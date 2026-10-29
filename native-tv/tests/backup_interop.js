'use strict';

var assert = require('assert');
var childProcess = require('child_process');
var fs = require('fs');
var os = require('os');
var path = require('path');

// Where the last Node release kept its modules: read from its git tag, not this tree.
var LEGACY_LIB = ['server', 'lib'].join('/');
var nativeTests = process.argv[2];
var nodeReleaseTag = process.argv[3];
assert.ok(nativeTests, 'native backup test executable is required');
assert.ok(nodeReleaseTag, 'last Node release tag is required');

// The release is read from its git tag: without git, or a clone that lacks the tag (CI
// fetches it), there is nothing to compare with and the test is skipped.
(function () {
  var repository = path.resolve(__dirname, '../..');
  var probe = childProcess.spawnSync('git', ['rev-parse', '-q', '--verify', 'refs/tags/' + nodeReleaseTag],
    { cwd: repository, encoding: 'utf8' });
  if (probe.error || probe.status !== 0) {
    console.log('skipped: the release tag ' + nodeReleaseTag + ' is not available here');
    process.exit(77);
  }
})();

function runNative(args) {
  var result = childProcess.spawnSync(nativeTests, args, { encoding: 'utf8' });
  if (result.error) throw result.error;
  assert.strictEqual(result.status, 0,
    'native backup interop failed: ' + (result.stderr || result.stdout));
}

var root = fs.mkdtempSync(path.join(os.tmpdir(), 'nativetv-backup-interop-'));
var nodeFile = path.join(root, 'node-backup.json');
var nativeFile = path.join(root, 'native-backup.json');
var pendingFile = path.join(root, 'restore-apps.json');

try {
  var repositoryRoot = path.resolve(__dirname, '../..');
  var releaseRoot = path.join(root, 'last-node-release');
  var releaseArchivePath = path.join(root, 'last-node-release.tar');
  fs.mkdirSync(releaseRoot);
  childProcess.execFileSync('git', [
    'archive', '--format=tar', '--output', releaseArchivePath,
    nodeReleaseTag, LEGACY_LIB
  ], { cwd: repositoryRoot });
  childProcess.execFileSync('tar', ['-x', '-f', releaseArchivePath, '-C', releaseRoot]);
  var backup = require(path.join(releaseRoot, LEGACY_LIB, 'backup'));

  backup.init({
    lgSettings: {
      collect: function (_sections, callback) {
        callback({ ok: true, rows: [
          { id: 'homePromotion', type: 'switch', on: false },
          { id: 'soundMode', type: 'choice', value: 'cinema' }
        ] });
      }
    },
    config: { logging: { level: 'debug' } },
    about: function () { return { model: 'OLED65C4', version: '0.118.0' }; },
    home: { layout: function () { return { picture: 'movie', rows: ['node'] }; } },
    luna: function (_uri, _payload, callback) {
      callback({ returnValue: true, apps: [{
        id: 'com.example.nodeapp', title: 'Node app', version: '2.3.4',
        folderPath: '/media/developer/apps/com.example.nodeapp'
      }] });
    }
  });

  backup.create(function (nodeBackup) {
    fs.writeFileSync(nodeFile, JSON.stringify(nodeBackup));
  });
  runNative(['--read-node-backup', nodeFile]);

  runNative(['--write-native-backup', nativeFile]);
  var nativeBackup = JSON.parse(fs.readFileSync(nativeFile, 'utf8'));
  var changedSettings = null;
  var appliedSettings = [];
  var restoredHome = null;
  backup.init({
    lgSettings: {
      collect: function (_sections, callback) {
        callback({ ok: true, rows: [
          { id: 'homePromotion', type: 'switch', on: true },
          { id: 'soundMode', type: 'choice', value: 'standard',
            choices: [{ value: 'standard' }, { value: 'cinema' }] }
        ] });
      },
      set: function (id, value, callback) {
        appliedSettings.push([id, value]);
        callback({ ok: true });
      }
    },
    config: { logging: { level: 'info' }, syslog: { server: 'old.local' } },
    writeSettings: function (patch, callback) {
      changedSettings = patch;
      callback(null);
    },
    pendingFile: pendingFile,
    home: { setLayout: function (layout, callback) {
      restoredHome = layout;
      callback({ ok: true });
    } },
    luna: function (_uri, _payload, callback) {
      callback({ returnValue: true, apps: [] });
    }
  });
  // The Node release reads the names from before the rename; nothing else differs.
  nativeBackup.format = 'glasshouse-backup';
  nativeBackup.glasshouse = nativeBackup.nativetv;
  delete nativeBackup.nativetv;
  nativeBackup.from.glasshouse = nativeBackup.from.nativetv;
  delete nativeBackup.from.nativetv;
  backup.restore(nativeBackup, function (result) {
    assert.strictEqual(result.ok, true, 'Node rejected the native-written backup');
    assert.deepStrictEqual(appliedSettings, [
      ['homePromotion', false], ['soundMode', 'cinema']
    ]);
    assert.deepStrictEqual(changedSettings, {
      logging: { level: 'debug' }, syslog: { server: 'backup.local', port: 514 }
    });
    assert.deepStrictEqual(restoredHome, { picture: 'movie', rows: ['native'] });
    assert.deepStrictEqual(result.apps.toInstall, [
      { id: 'com.example.nativeapp', title: 'Native app' }
    ]);
    assert.deepStrictEqual(JSON.parse(fs.readFileSync(pendingFile, 'utf8')).apps,
      [{ id: 'com.example.nativeapp', title: 'Native app' }]);
  });
  process.stdout.write(nodeReleaseTag + '/native backup format interoperability passed\n');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
