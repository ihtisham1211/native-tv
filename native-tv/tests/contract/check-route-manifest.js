'use strict';

var fs = require('fs');
var path = require('path');

var root = path.resolve(__dirname, '../../..');
var manifestPath = path.join(root, 'native-tv/contract/routes.json');
var daemonDir = path.join(root, 'native-tv/src/daemon');
var manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
// Everything the daemon dispatches is in these files; the manifest is checked against them.
var source = fs.readdirSync(daemonDir).filter(function (name) { return /\.cpp$/.test(name); })
  .map(function (name) { return fs.readFileSync(path.join(daemonDir, name), 'utf8'); }).join('\n');
var errors = [];
var knownPaths = Object.create(null);
var knownGuards = Object.create(null);
var routeKeys = Object.create(null);

function fail(message) { errors.push(message); }

(manifest.routes || []).forEach(function (group, groupIndex) {
  if (!Array.isArray(group.paths) || !group.paths.length) {
    fail('route group ' + groupIndex + ' has no paths');
    return;
  }
  if (!Array.isArray(group.methods) || !group.methods.length) fail('route group ' + groupIndex + ' has no methods');
  if (!manifest.authClasses || !manifest.authClasses[group.auth]) fail('route group ' + groupIndex + ' has unknown auth class ' + group.auth);
  if (typeof group.csrf !== 'string' || !group.csrf) fail('route group ' + groupIndex + ' has no CSRF rule');
  if (!group.bodyLimit || !Object.prototype.hasOwnProperty.call(group.bodyLimit, 'max') || !group.bodyLimit.unit) {
    fail('route group ' + groupIndex + ' has no body limit');
  }
  if (group.owner !== 'native') fail('route group ' + groupIndex + ' is owned by ' + group.owner + ', not native');
  group.paths.forEach(function (routePath) {
    knownPaths[routePath] = true;
    (group.methods || []).forEach(function (method) {
      var key = method + ' ' + routePath;
      if (routeKeys[key]) fail('duplicate route ' + key);
      routeKeys[key] = true;
    });
  });
});

(manifest.routeGuards || []).forEach(function (guard) {
  knownGuards[guard.pathPrefix] = true;
  if (!guard.policy) fail('route guard ' + guard.pathPrefix + ' has no policy');
});

// Every API path the daemon names is in the manifest, and every concrete one
// the manifest lists is in the daemon.
var apiPaths = source.match(/"\/api(?:\/[^"\\ ]*)?"/g) || [];
var inSource = Object.create(null);
apiPaths.forEach(function (quoted) {
  var routePath = quoted.slice(1, -1);
  inSource[routePath] = true;
  if (!knownPaths[routePath] && !knownGuards[routePath]) fail('daemon API path is missing from manifest: ' + routePath);
});
Object.keys(knownPaths).forEach(function (routePath) {
  if (/^\/api\//.test(routePath) && !inSource[routePath]) fail('manifest API path is not in the daemon: ' + routePath);
});

// Moved pages and the optional icon route have a literal in the daemon too.
(manifest.redirects || []).forEach(function (redirect) {
  if (source.indexOf('"' + redirect.path + '"') === -1) fail('redirect is not in the daemon: ' + redirect.path);
});
if (!knownPaths['/icons/{appId}.png'] || source.indexOf('/icons/') === -1) {
  fail('optional /icons/{appId}.png route is missing from the manifest or the daemon');
}

if (errors.length) {
  errors.forEach(function (error) { console.error('route manifest: ' + error); });
  process.exit(1);
}

console.log('route manifest: ' + Object.keys(routeKeys).length + ' method/path entries, ' + Object.keys(knownGuards).length + ' route guards verified');
