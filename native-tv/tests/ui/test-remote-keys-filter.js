/**
 * The key filter the TV loads (assets/remotekeys-filter.js), run here against stand-ins for
 * the compositor's own objects (WebOS, Qt, KeyPolicy, lunaCall, applicationManager). It shows
 * what the script does with a key press for each kind of target. It cannot show what the
 * compositor does with the calls: whether its role may make them is for a TV to answer.
 */
'use strict';
var assert = require('assert');
var fs = require('fs');
var path = require('path');
var vm = require('vm');

console.log('Running test-remote-keys-filter.js ...');

var source = fs.readFileSync(path.join(__dirname, '..', '..', 'assets', 'remotekeys-filter.js'), 'utf8');

var NETFLIX = 1001, RED = 1002, GUIDE = 1003, VOICE = 1006, MOVIES = 1007, OTHER = 4242;

// `value` adds to nativetv.remoteKeys; `answers` are the bus's replies, by method.
function filter(keys, value, answers) {
  var sent = [], launched = [], logs = [];
  var context = {
    WebOS: { Key_webOS_Netflix: NETFLIX, Key_webOS_Red: RED, Key_webOS_TVGuide: GUIDE, Key_webOS_Voice: VOICE, Key_webOS_USP: MOVIES },
    Qt: { Key_Super_L: 1999, Key_Search: 1004, Key_Help: 1005 },
    KeyPolicy: { Accepted: 'accepted', NextPolicy: 'next' },
    lunaCall: {
      appId: 'com.webos.starfish.keyfilters',
      send: function (service, method, payload, callback) {
        sent.push({ service: service, method: method, payload: payload });
        if (answers && answers[method] && callback) callback(JSON.stringify(answers[method]));
      },
      subscribe: function () {}
    },
    applicationManager: { launch: function (id, params) { launched.push({ id: id, params: params }); } },
    genericTimer: { set: function () {} },
    console: { info: function () { logs.push(Array.prototype.join.call(arguments, ' ')); }, warn: function () { logs.push(Array.prototype.join.call(arguments, ' ')); } },
    JSON: JSON
  };
  vm.createContext(context);
  vm.runInContext(source, context);
  var remoteKeys = { keys: keys };
  for (var field in value || {}) remoteKeys[field] = value[field];
  context.__config = JSON.stringify({ configs: { 'nativetv.remoteKeys': remoteKeys } });
  vm.runInContext('nativeTvKeysChanged(__config)', context);
  // The calls made while the config was read (the boot service) are not what a key does.
  sent.length = 0;
  return {
    press: function (code, pressed, repeat) {
      context.__args = [code, pressed !== false, !!repeat];
      return vm.runInContext('handleNativeTvKeys(__args[0], __args[1], __args[2])', context);
    },
    sent: sent, launched: launched, logs: logs
  };
}

var power = 'com.webos.service.tvpower';

// Screen off and Power off.
var screenOff = filter({ netflix: 'action:screenOff', red: 'action:powerOff' });
assert.strictEqual(screenOff.press(NETFLIX), 'accepted', 'the key is taken');
assert.deepStrictEqual(screenOff.sent, [{ service: power, method: '/power/turnOffScreen', payload: '{}' }]);
assert.strictEqual(screenOff.launched.length, 0, 'no app is opened for an action');
assert.ok(screenOff.logs.some(function (line) { return /runs action:screenOff/.test(line); }), 'the press is logged');
console.log('  ok - a key set to Screen off turns the screen off, through the power service');

screenOff.sent.length = 0;
assert.strictEqual(screenOff.press(RED), 'accepted');
assert.deepStrictEqual(screenOff.sent, [{ service: power, method: '/power/powerOff', payload: '{}' }]);
console.log('  ok - a key set to Power off switches the TV off');

screenOff.sent.length = 0;
assert.strictEqual(screenOff.press(NETFLIX, true, true), 'accepted', 'a held key is still taken');
assert.strictEqual(screenOff.press(NETFLIX, false), 'accepted', 'so is its release');
assert.deepStrictEqual(screenOff.sent, [], 'a held key and a release do nothing more');
console.log('  ok - an action runs on the press only, not on the repeat or the release');

// A target the script does not know.
var unknown = filter({ netflix: 'action:selfDestruct', red: 'action:' });
assert.strictEqual(unknown.press(NETFLIX), 'accepted');
assert.strictEqual(unknown.press(RED), 'accepted');
assert.deepStrictEqual(unknown.sent, [], 'an unknown action calls nothing');
assert.deepStrictEqual(unknown.launched, [], 'and is not taken for an app');
console.log('  ok - an action it does not know does nothing, and is not opened as an app');

// What was there before still works.
var before = filter({ netflix: 'org.example.app', red: 'media:play', guide: 'media:volumeUp' });
assert.strictEqual(before.press(NETFLIX), 'accepted');
assert.deepStrictEqual(before.launched, [{ id: 'org.example.app', params: '{}' }]);
before.sent.length = 0;
assert.strictEqual(before.press(RED), 'accepted');
assert.deepStrictEqual(before.sent, [{ service: 'com.webos.service.networkinput', method: '/test/sendKeyCode', payload: JSON.stringify({ keyCode: 207 }) }]);
assert.strictEqual(before.press(OTHER), 'next', 'a key that is not set goes on to LG');
console.log('  ok - an app and a media key still work, and a key that is not set is passed on');

// Home Assistant: the key does nothing on the TV; its press reaches the daemon from the remote's
// own input device. Not opened as an app, not sent anywhere, and no longer left to LG.
var homeAssistant = filter({ voice: 'action:homeAssistant', netflix: 'action:homeAssistant' });
assert.strictEqual(homeAssistant.press(VOICE), 'accepted', 'the microphone is kept from LG');
assert.strictEqual(homeAssistant.press(VOICE, true, true), 'accepted', 'so is a hold');
assert.strictEqual(homeAssistant.press(NETFLIX), 'accepted');
assert.deepStrictEqual(homeAssistant.sent, [], 'nothing is called');
assert.deepStrictEqual(homeAssistant.launched, [], 'nothing is opened');
assert.ok(!homeAssistant.logs.some(function (line) { return /runs/.test(line); }), 'no action is run');
console.log('  ok - a key set to Home Assistant does nothing on the TV and is kept from LG');

// OK is KEY_ENTER, once per press and not while held; Movies and the microphone are keys a
// target can be set on like any other.
var ok = filter({ netflix: 'media:ok', movies: 'org.example.movies' });
assert.strictEqual(ok.press(NETFLIX), 'accepted');
assert.strictEqual(ok.press(NETFLIX, true, true), 'accepted');
assert.strictEqual(ok.press(NETFLIX, false), 'accepted');
assert.deepStrictEqual(ok.sent, [{ service: 'com.webos.service.networkinput', method: '/test/sendKeyCode', payload: JSON.stringify({ keyCode: 28 }) }]);
assert.strictEqual(ok.press(MOVIES), 'accepted');
assert.deepStrictEqual(ok.launched, [{ id: 'org.example.movies', params: '{}' }]);
assert.strictEqual(filter({ voice: 'org.example.assistant' }).press(VOICE), 'accepted');
console.log('  ok - a key set to OK sends the remote\'s OK once, and Movies and the microphone can be set');

// An unset key with actions around it.
var passed = filter({ netflix: 'action:screenOff' });
assert.strictEqual(passed.press(RED), 'next');
assert.deepStrictEqual(passed.sent, []);
console.log('  ok - setting one key to an action leaves the others to LG');

// Home pressed while Native Tv Home is starting is held from LG's filters, and opens Home only once
// sam runs Native Tv Home (the native client, started by nativetv-home) under LG's Home's id.
var HOME = 1999;
var starting = { starting: 'Native Tv Home is starting' };
var ready = filter({}, starting, { '/getAppInfo': { returnValue: true, appInfo: { id: 'com.webos.app.home', type: 'native', main: 'nativetv-home' } } });
assert.strictEqual(ready.press(HOME), 'accepted');
assert.strictEqual(ready.press(HOME, false), 'accepted', 'its release is held too');
assert.deepStrictEqual(ready.launched, [{ id: 'com.webos.app.home', params: JSON.stringify({ activateType: 'home', launchType: 'homeKey' }) }]);
assert.strictEqual(ready.press(HOME), 'next', 'later presses are LG\'s');
var lgs = filter({}, starting, { '/getAppInfo': { returnValue: true, appInfo: { id: 'com.webos.app.home', type: 'native', main: 'flutter-client' } } });
assert.strictEqual(lgs.press(HOME), 'accepted');
assert.deepStrictEqual(lgs.launched, [], 'LG\'s own Home is not opened');
assert.ok(lgs.sent.some(function (call) { return call.method === '/createToast'; }), 'the starting notice is shown');
console.log('  ok - Home is held while Native Tv Home starts, and opened once sam runs it');

console.log('ALL test-remote-keys-filter.js assertions passed!\n');
