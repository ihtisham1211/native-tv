// Runs on the TV, under its own Node: node - PORT. run.py puts `var PORT` and `var STEPS` (the case's
// flow) in front of this file and sends it over ssh. Plain ES5, as that Node may be old.
//
// Each step prints one JSON line {step, ok, ms, note}; the last line is {done, ok}. The exit code is
// 0 when every step held. A step is one of:
//   {api: "POST /api/home/enable", body?, status?, expect?, same?}   a request to the server on this TV (loopback is
//                                                     accepted without the token), the reply compared
//                                                     with expect by dotted path ("nativeHome.halted": false);
//                                                     same pairs two paths of the reply that must hold the
//                                                     same value ("rows[id=x].value": "dimensions.sound.x");
//                                                     status is the HTTP status the reply must have (for a reply that is not JSON);
//                                                     keepSession holds the reply's admin session and withSession sends it back
//                                                     (X-Admin-Session), for the admin PIN's unlock;
//                                                     save {name: "path"} keeps a field of the reply, and a body value "$name"
//                                                     is that field in a later step (an id a first step was given)
//   {foreground: "com.example", withinMs}            the app in front becomes this one
//   {stays: "com.example", forMs}                    the app in front is this one throughout
//   {wait: ms}
// A path name may pick an array item: rows[id=soundOutput].value is the value of the item whose id is soundOutput.
// The app in front is read from the app manager (getForegroundAppInfo), which only reports it.
var http = require('http');
var execFile = require('child_process').execFile;

var POLL_MS = 250;
var started = Date.now();
var failed = false;
var adminSession = '';
var saved = {};

function print(row) { process.stdout.write(JSON.stringify(row) + '\n'); }

function request(call, body, withSession, done) {
  var parts = call.split(' ');
  // The server answers an empty JSON body with 400 malformed JSON, so a POST with nothing to say sends {}.
  var text = body !== undefined ? JSON.stringify(body) : (parts[0] === 'POST' ? '{}' : '');
  var headers = parts[0] === 'POST' ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) } : {};
  if (withSession && adminSession) headers['X-Admin-Session'] = adminSession;
  var req = http.request({
    host: '127.0.0.1', port: PORT, method: parts[0], path: parts[1], headers: headers
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

// The names of a path: split at the dots outside [..], since an id picked by rows[id=com.example.app] has dots.
function splitPath(path) {
  var names = [], current = '', depth = 0;
  for (var i = 0; i < path.length; i++) {
    var ch = path.charAt(i);
    if (ch === '[') depth++;
    if (ch === ']') depth--;
    if (ch === '.' && depth === 0) { names.push(current); current = ''; } else current += ch;
  }
  names.push(current);
  return names;
}

function dotted(object, path) {
  var value = object;
  var names = splitPath(path);
  for (var i = 0; i < names.length; i++) {
    if (value === null || typeof value !== 'object') return undefined;
    var pick = /^(.+)\[(.+)=(.+)\]$/.exec(names[i]);
    if (!pick) {
      value = value[names[i]];
      continue;
    }
    var list = value[pick[1]];
    value = undefined;
    for (var j = 0; Array.isArray(list) && j < list.length; j++)
      if (list[j] && String(list[j][pick[2]]) === pick[3]) { value = list[j]; break; }
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
    var body = step.body;
    if (body && typeof body === 'object') {
      body = JSON.parse(JSON.stringify(body));
      for (var key in body)
        if (Object.prototype.hasOwnProperty.call(body, key) && typeof body[key] === 'string' && body[key].charAt(0) === '$')
          body[key] = saved[body[key].slice(1)];
    }
    request(step.api, body, step.withSession, function (error, status, reply) {
      if (error) return finish(false, String(error));
      for (var name in (step.save || {}))
        if (Object.prototype.hasOwnProperty.call(step.save, name)) saved[name] = dotted(reply, step.save[name]);
      if (step.keepSession && reply && reply.session) adminSession = reply.session;
      if (step.status !== undefined && status !== step.status)
        return finish(false, 'HTTP ' + status + ', wanted ' + step.status);
      var expect = step.expect || {};
      for (var path in expect) {
        if (Object.prototype.hasOwnProperty.call(expect, path) && dotted(reply, path) !== expect[path])
          return finish(false, path + ' is ' + JSON.stringify(dotted(reply, path)) + ', wanted ' +
            JSON.stringify(expect[path]) + (reply && reply.error ? ' (' + reply.error + ')' : ''));
      }
      var same = step.same || {};
      for (var left in same) {
        if (!Object.prototype.hasOwnProperty.call(same, left)) continue;
        var a = dotted(reply, left), b = dotted(reply, same[left]);
        // undefined on both sides is a missing field, not agreement.
        if (a === undefined || a !== b)
          return finish(false, left + ' is ' + JSON.stringify(a) + ' but ' + same[left] + ' is ' + JSON.stringify(b));
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
