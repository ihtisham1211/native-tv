// Runs on the TV, under its own Node: node - PORT. run.py puts `var PORT` and `var STEPS` (the case's
// flow) in front of this file and sends it over ssh. Plain ES5, as that Node may be old.
//
// Each step prints one JSON line {step, ok, ms, note}; the last line is {done, ok}. The exit code is
// 0 when every step held. A step is one of:
//   {api: "POST /api/home/enable", body?, expect?}   a request to the server on this TV (loopback is
//                                                     accepted without the token), the reply compared
//                                                     with expect by dotted path ("nativeHome.halted": false)
//   {foreground: "com.example", withinMs}            the app in front becomes this one
//   {stays: "com.example", forMs}                    the app in front is this one throughout
//   {wait: ms}
// The app in front is read from the app manager (getForegroundAppInfo), which only reports it.
var http = require('http');
var execFile = require('child_process').execFile;

var POLL_MS = 250;
var started = Date.now();
var failed = false;

function print(row) { process.stdout.write(JSON.stringify(row) + '\n'); }

function request(call, body, done) {
  var parts = call.split(' ');
  var text = body === undefined ? '' : JSON.stringify(body);
  var req = http.request({
    host: '127.0.0.1', port: PORT, method: parts[0], path: parts[1],
    headers: parts[0] === 'POST' ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) } : {}
  }, function (res) {
    var chunks = '';
    res.on('data', function (chunk) { chunks += chunk; });
    res.on('end', function () {
      var parsed = null;
      try { parsed = JSON.parse(chunks); } catch (e) {}
      done(null, res.statusCode, parsed);
    });
  });
  req.setTimeout(30000, function () { req.abort(); });
  req.on('error', function (error) { done(error); });
  if (parts[0] === 'POST') req.write(text);
  req.end();
}

function dotted(object, path) {
  var value = object;
  var names = path.split('.');
  for (var i = 0; i < names.length; i++) {
    if (value === null || typeof value !== 'object') return undefined;
    value = value[names[i]];
  }
  return value;
}

function foreground(done) {
  execFile('luna-send', ['-n', '1', '-w', '3000', 'luna://com.webos.applicationManager/getForegroundAppInfo', '{}'],
    function (error, stdout) {
      var reply = null;
      try { reply = JSON.parse(stdout); } catch (e) {}
      done(reply && reply.returnValue ? String(reply.appId || '') : null);
    });
}

function poll(app, limitMs, hold, done) {
  var begin = Date.now();
  var last = null;
  (function again() {
    foreground(function (now) {
      last = now;
      var elapsed = Date.now() - begin;
      if (hold) {
        if (now !== app) return done(false, 'in front: ' + now + ' after ' + elapsed + ' ms');
        if (elapsed >= limitMs) return done(true, '');
      } else {
        if (now === app) return done(true, elapsed + ' ms');
        if (elapsed >= limitMs) return done(false, 'still ' + last + ' after ' + elapsed + ' ms');
      }
      setTimeout(again, POLL_MS);
    });
  })();
}

function run(index) {
  if (index >= STEPS.length) {
    print({ done: true, ok: !failed });
    process.exit(failed ? 1 : 0);
  }
  var step = STEPS[index];
  var begin = Date.now();
  function finish(ok, note) {
    if (!ok) failed = true;
    print({ step: index, ok: ok, ms: Date.now() - begin, note: note || '' });
    // A step that fails ends the flow: the next ones rely on it.
    if (!ok) { print({ done: true, ok: false }); process.exit(1); }
    run(index + 1);
  }
  if (step.api) {
    request(step.api, step.body, function (error, status, reply) {
      if (error) return finish(false, String(error));
      var expect = step.expect || {};
      for (var path in expect) {
        if (Object.prototype.hasOwnProperty.call(expect, path) && dotted(reply, path) !== expect[path])
          return finish(false, path + ' is ' + JSON.stringify(dotted(reply, path)) + ', wanted ' +
            JSON.stringify(expect[path]) + (reply && reply.error ? ' (' + reply.error + ')' : ''));
      }
      finish(true, 'HTTP ' + status);
    });
  } else if (step.foreground) {
    poll(step.foreground, step.withinMs, false, finish);
  } else if (step.stays) {
    poll(step.stays, step.forMs, true, finish);
  } else if (step.wait !== undefined) {
    setTimeout(function () { finish(true, ''); }, step.wait);
  } else {
    finish(false, 'unknown step');
  }
}

print({ start: true, afterMs: Date.now() - started });
run(0);
