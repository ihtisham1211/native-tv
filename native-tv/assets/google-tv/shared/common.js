/*
 * The TV pages show their text in English only, as the web dashboard does.
 * t() keeps the call sites as they were, so the key is ignored and only {name}
 * placeholders are filled. ES5.
 */
function t(key, english, vars) {
  if (!vars) return english;
  return english.replace(/\{(\w+)\}/g, function (whole, name) {
    return vars[name] === undefined ? whole : String(vars[name]);
  });
}

/* One HTML escaper for every TV page. */
function esc(s) {
  return String(s === null || s === undefined ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}
