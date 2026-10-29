'use strict';

// Every suite of the legacy Node test directory has a recorded fate: ported to
// native tests that exist, or retired with a reason. See contract/test-migration.json.
var fs = require('fs');
var path = require('path');

var root = path.resolve(__dirname, '../../..');
var native = path.join(root, 'native-tv');
var matrix = JSON.parse(fs.readFileSync(path.join(native, 'contract/test-migration.json'), 'utf8'));
var errors = [];

var cmake = fs.readFileSync(path.join(native, 'CMakeLists.txt'), 'utf8');
var runtimeTests = '';
fs.readdirSync(path.join(native, 'tests')).filter(function (name) { return /\.py$/.test(name); })
  .forEach(function (name) { runtimeTests += fs.readFileSync(path.join(native, 'tests', name), 'utf8') + '\n'; });

function exists(reference) {
  var colon = reference.indexOf(':');
  var kind = reference.slice(0, colon);
  var name = reference.slice(colon + 1);
  if (kind === 'ctest') return new RegExp('NAME ' + name + '(\\s|\\))').test(cmake);
  if (kind === 'runtime') return runtimeTests.indexOf('def ' + name + '(') !== -1;
  if (kind === 'file') return fs.existsSync(path.join(root, name));
  return false;
}

var suites = matrix.suites || {};
var ported = 0;
var retired = 0;
Object.keys(suites).forEach(function (suite) {
  var entry = suites[suite];
  if (!/^test-.*\.js$/.test(suite)) errors.push('not a legacy suite name: ' + suite);
  if (entry.fate === 'ported') {
    ported++;
    if (!Array.isArray(entry.native) || !entry.native.length) errors.push(suite + ' is ported to nothing');
    (entry.native || []).forEach(function (reference) {
      if (!exists(reference)) errors.push(suite + ' cites a native test that does not exist: ' + reference);
    });
    if (entry.moved && !fs.existsSync(path.join(root, entry.moved))) errors.push(suite + ' moved to a missing file: ' + entry.moved);
  } else if (entry.fate === 'retired') {
    retired++;
    if (typeof entry.reason !== 'string' || !entry.reason) errors.push(suite + ' is retired without a reason');
  } else {
    errors.push(suite + ' has no fate (ported or retired)');
  }
});

// While the legacy directory is still in the tree, it must be fully accounted for.
var legacyTestDir = path.join(root, 'test');
if (fs.existsSync(legacyTestDir)) {
  fs.readdirSync(legacyTestDir).filter(function (name) { return /^test-.*\.js$/.test(name); }).forEach(function (suite) {
    if (!suites[suite]) errors.push('legacy suite has no recorded fate: ' + suite);
  });
}

if (errors.length) {
  errors.forEach(function (error) { console.error('test migration matrix: ' + error); });
  process.exit(1);
}

console.log('test migration matrix: ' + Object.keys(suites).length + ' legacy suites, ' + ported + ' ported, ' + retired + ' retired');
