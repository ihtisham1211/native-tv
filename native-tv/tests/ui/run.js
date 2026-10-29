// The dashboard's scripts and the boot hook, run on the host: node native-tv/tests/ui/run.js [filter]
'use strict';
var child = require('child_process');
var fs = require('fs');
var path = require('path');

var filter = process.argv[2] ? new RegExp(process.argv[2]) : null;
var suites = fs.readdirSync(__dirname).filter(function (name) {
  return /^test-.*\.js$/.test(name) && (!filter || filter.test(name));
}).sort();
if (!suites.length) { console.error('no UI suite matches'); process.exit(1); }

var failed = 0;
suites.forEach(function (suite) {
  var run = child.spawnSync(process.execPath, [path.join(__dirname, suite)], { encoding: 'utf8' });
  var ok = run.status === 0;
  console.log((ok ? 'PASS ' : 'FAIL ') + suite);
  if (!ok) {
    failed++;
    process.stdout.write(run.stdout);
    process.stderr.write(run.stderr);
  }
});
console.log(suites.length - failed + '/' + suites.length + ' UI suites passed');
process.exit(failed ? 1 : 0);
