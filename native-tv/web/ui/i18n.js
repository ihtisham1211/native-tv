/*
 * Strings shown on the dashboard, and their translations.
 *
 * English is written where each string is used, next to a key that names it:
 *
 *   <div data-t="server.health.title">Server health</div>
 *   t('server.install', 'Install v{version}', { version: v })
 *
 * A translation lives in /assets/i18n/<lang>.json under the same key, with the
 * English it was made from:
 *
 *   { "server.health.title": { "text": "Estado del servidor", "from": "Server health" } }
 *
 * When the English changes, "from" no longer matches and the English is shown
 * until the translation is redone, rather than a translation of the old words.
 * docs/development/strings.md has the rules; scripts/check-strings.py enforces
 * them. The dictionaries are after Glasshouse's (MIT).
 */
(function (root) {
  // Languages that have a file in /assets/i18n/. English needs none.
  var LANGS = ['es'];
  // Each language in its own words, as the language list shows it, so it can
  // be found whatever the page is in.
  var NAMES = { en: 'English', es: 'Español' };

  var dict = {};
  var lang = 'en';

  // The choice made on this browser's Server tab: 'en', one of LANGS, or
  // 'auto' (as when nothing is stored) to follow the browser's own language.
  function stored() {
    var v = null;
    try { v = localStorage.getItem('lang'); } catch (e) { /* private window */ }
    return v === 'en' || LANGS.indexOf(v) >= 0 ? v : 'auto';
  }

  function choose(v) {
    try {
      if (v === 'auto') localStorage.removeItem('lang');
      else localStorage.setItem('lang', v);
    } catch (e) { /* private window */ }
  }

  // ?lang=xx wins, then the stored choice, then the browser's language.
  function pick() {
    var m = /[?&]lang=([a-z]{2})/i.exec(location.search);
    var chosen = stored();
    var want = m ? m[1] : chosen !== 'auto' ? chosen
             : ((navigator.languages && navigator.languages[0]) || navigator.language || 'en');
    want = String(want).slice(0, 2).toLowerCase();
    return LANGS.indexOf(want) < 0 ? 'en' : want;
  }

  /*
   * Synchronous, so every string on the page, and every one built after it,
   * comes out in the one language from the first paint. The file is small and
   * on the same TV; an asynchronous load would show English first and need
   * every script to start over once it arrived.
   */
  function load() {
    lang = pick();
    if (lang === 'en') return;
    try {
      var x = new XMLHttpRequest();
      x.open('GET', '/assets/i18n/' + lang + '.json', false);
      x.send(null);
      if (x.status !== 200) throw new Error('no dictionary');
      dict = JSON.parse(x.responseText);
      document.documentElement.setAttribute('lang', lang);
    } catch (e) {
      dict = {};
      lang = 'en';
    }
  }

  function fill(text, vars) {
    if (!vars) return text;
    return text.replace(/\{(\w+)\}/g, function (whole, name) {
      return vars[name] === undefined ? whole : String(vars[name]);
    });
  }

  // The key is what a translation is looked up by; the English is shown when
  // there is none, or when the one there was made from different words.
  function t(key, english, vars) {
    var entry = Object.prototype.hasOwnProperty.call(dict, key) ? dict[key] : null;
    return fill(entry && entry.from === english ? entry.text : english, vars);
  }

  // Markup text is written across lines like any other; compare it as it reads.
  function squash(s) {
    return String(s).replace(/\s+/g, ' ').replace(/^ | $/g, '');
  }

  // Text in the markup: data-t on the element holding it, data-t-title and the
  // like for an attribute. Nothing to do in English, which is already there.
  function apply(scope) {
    if (lang === 'en') return;
    var els = (scope || document).querySelectorAll('[data-t]');
    for (var i = 0; i < els.length; i++) {
      els[i].textContent = t(els[i].getAttribute('data-t'), squash(els[i].textContent));
    }
    var attrs = ['title', 'aria-label', 'placeholder'];
    for (var a = 0; a < attrs.length; a++) {
      var named = (scope || document).querySelectorAll('[data-t-' + attrs[a] + ']');
      for (var j = 0; j < named.length; j++) {
        var el = named[j];
        el.setAttribute(attrs[a], t(el.getAttribute('data-t-' + attrs[a]), squash(el.getAttribute(attrs[a]) || '')));
      }
    }
  }

  // Whether a request goes to the TV itself: a cross-origin one would need a
  // preflight for the header below.
  function sameOrigin(url) {
    var s = String(url);
    return !/^([a-z][a-z0-9+.-]*:|\/\/)/i.test(s) || s.indexOf(location.origin) === 0;
  }

  /*
   * Messages from the server - errors, and names such as a screen saver's -
   * come back in the page's language when it says which it is on its own
   * requests. English says nothing, and the server answers in English.
   */
  function tellServer() {
    if (lang === 'en') return;
    var open = XMLHttpRequest.prototype.open;
    XMLHttpRequest.prototype.open = function (method, url) {
      this.i18nLocal = sameOrigin(url);
      return open.apply(this, arguments);
    };
    var send = XMLHttpRequest.prototype.send;
    XMLHttpRequest.prototype.send = function () {
      if (this.i18nLocal) {
        try { this.setRequestHeader('X-Native-Tv-Lang', lang); } catch (e) { /* not opened */ }
      }
      return send.apply(this, arguments);
    };
    if (root.fetch && root.Headers) {
      var fetch = root.fetch;
      root.fetch = function (input, init) {
        if (!sameOrigin(typeof input === 'string' ? input : (input && input.url) || '')) return fetch.call(this, input, init);
        init = init || {};
        var headers = new root.Headers(init.headers || (input && input.headers) || {});
        headers.set('X-Native-Tv-Lang', lang);
        init.headers = headers;
        return fetch.call(this, input, init);
      };
    }
  }

  // The Server tab's list. English and Auto are in the markup; the languages
  // with a file are added here, so a new one is added in one place.
  function listLanguages() {
    var sel = document.getElementById('lang-sel');
    if (!sel) return;
    for (var i = 0; i < LANGS.length; i++) {
      var option = document.createElement('option');
      option.value = LANGS[i];
      option.textContent = NAMES[LANGS[i]];
      option.setAttribute('translate', 'no');
      sel.appendChild(option);
    }
    sel.value = /[?&]lang=[a-z]{2}/i.test(location.search) ? lang : stored();
    sel.addEventListener('change', function () {
      choose(sel.value);
      // ?lang= would win over the choice just made.
      var query = location.search.replace(/([?&])lang=[^&]*&?/i, '$1').replace(/[?&]$/, '');
      location.assign(location.pathname + query + location.hash);
    });
  }

  load();
  tellServer();
  // The scripts run once the markup above them is parsed; DOMContentLoaded
  // covers a page that loads them sooner. Applying twice changes nothing.
  apply();
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', function () { apply(); listLanguages(); });
  } else {
    listLanguages();
  }

  root.t = t;
  root.I18N = { t: t, apply: apply, lang: function () { return lang; }, languages: LANGS, names: NAMES,
                stored: stored, choose: choose };
})(window);
