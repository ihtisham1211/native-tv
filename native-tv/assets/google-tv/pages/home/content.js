/* Full-screen title details and the shared search/profile editor. Strict ES5. */
var GhContent = (function () {
  var api, mode = '', stack = [], serial = 0, keyboard = null, timer = 0;
  var editing = false, profile = null, query = '', results = [], resultIndex = 0, editorZone = 'keyboard', footerIndex = 0, saving = false;
  var editorReturn = null, overview = false;
  function $(id) { return document.getElementById(id); }
  function clamp(n, max) { return Math.max(0, Math.min(max, n)); }
  function begin(next) {
    mode = next; document.body.classList.add('content-open');
    $('contentView').className = 'show';
    $('titleView').style.display = next === 'details' ? 'block' : 'none';
    $('editorView').style.display = next === 'editor' ? 'block' : 'none';
    api.suspend();
  }
  function close() {
    serial++; clearTimeout(timer); timer = 0; stack = []; mode = ''; editing = false; saving = false;
    if (keyboard) { keyboard.destroy(); keyboard = null; }
    $('contentView').className = ''; $('dtCards').innerHTML = ''; $('editResults').innerHTML = '';
    $('dtBack').style.backgroundImage = ''; $('overviewView').className = 'gt-scrim'; overview = false;
    document.body.classList.remove('content-open');
    var returned = editorReturn; editorReturn = null;
    if (returned) returned(); else api.resume();
  }
  function image(item, hero) {
    var part = hero ? item.backdrop : item.backdrop || item.poster;
    return part ? '/api/home/image?u=' + encodeURIComponent('https://image.tmdb.org/t/p/' + (hero ? 'w1280' : item.backdrop ? 'w500' : 'w342') + part) + (hero ? '&priority=hero' : '') : '';
  }
  function facts(item) {
    var out = [item.year];
    if (item.rating !== null && item.rating !== undefined) out.push('★ ' + item.rating);
    if (item.runtime) out.push(t('home.details.minutes', '{n} min', { n: item.runtime }));
    if (item.seasons) out.push(t('home.details.seasons', '{n} seasons', { n: item.seasons }));
    if (item.genres && item.genres.length) out.push(item.genres.join(' · '));
    return out.filter(Boolean).join('  ·  ');
  }
  function cardsHtml(items, attr) {
    return items.map(function (item, i) {
      return '<div class="gt-card" data-' + attr + '="' + i + '" role="button" aria-label="' + esc(item.title) + '"><div class="gt-card-media"></div>' +
        '<div class="gt-card-body"><span class="gt-card-title">' + esc(item.title) + '</span><span class="gt-card-sub">' + esc(item.year || '') + '</span></div></div>';
    }).join('');
  }
  function mountArt(node, item) {
    if (!node || node.firstChild) return;
    var src = image(item, false);
    if (src) { var img = document.createElement('img'); img.alt = ''; img.src = src; node.appendChild(img); }
  }
  function current() { return stack[stack.length - 1]; }
  function detailFocus() {
    var state = current(); if (!state) return;
    var buttons = $('dtActions').children, cards = $('dtCards').children;
    for (var i = 0; i < buttons.length; i++) buttons[i].classList.toggle('focus', state.zone === 'actions' && state.col === i);
    $('dtClose').classList.toggle('focus', state.zone === 'back');
    for (i = 0; i < cards.length; i++) {
      cards[i].classList.toggle('focus', state.zone === 'similar' && state.card === i);
      if (i >= Math.max(0, state.card - 1) && i <= state.card + 5) mountArt(cards[i].firstChild, state.item.similar[i]);
    }
    var start = Math.max(0, state.card - 3);
    $('dtCards').style.webkitTransform = $('dtCards').style.transform = 'translateX(' + (-start * 356) + 'px)';
  }
  function renderDetails() {
    var state = current(), item = state.item;
    var actionInFocus = state.actions[state.col] && state.actions[state.col].key;
    $('dtType').textContent = item.type === 'tv' ? t('home.tv.type.tv', 'Series') : t('home.tv.type.movie', 'Film');
    $('dtTitle').textContent = item.title;
    $('dtTitle').className = 'title' + (item.title.length > 34 ? ' long' : item.title.length > 22 ? ' mid two' : '');
    $('dtFacts').textContent = facts(item);
    $('dtOverview').textContent = item.overview || t('home.details.noOverview', 'No overview available');
    $('dtCast').textContent = (item.cast || []).length ? t('home.details.cast', 'Cast: {names}', { names: item.cast.map(function (p) { return p.name; }).join(', ') }) : '';
    var src = image(item, true);
    $('dtBack').style.backgroundImage = src ? 'url("' + src.replace(/"/g, '%22') + '")' : '';
    state.actions = [{ key: 'open', title: t('home.details.open', 'Open') }];
    if (item.netflixId) state.actions.push({ key: 'netflix', title: t('home.details.netflix', 'Open on Netflix') });
    if (item.trailer) state.actions.push({ key: 'trailer', title: t('home.details.trailer', 'Trailer') });
    if (item.overview) state.actions.push({ key: 'overview', title: t('home.details.overview', 'Full overview') });
    if (state.error) state.actions.push({ key: 'retry', title: t('home.details.retry', 'Retry') });
    for (var ai = 0; ai < state.actions.length; ai++) if (state.actions[ai].key === actionInFocus) { state.col = ai; break; }
    $('dtActions').innerHTML = state.actions.map(function (action, i) { return '<button type="button" class="gt-btn" data-da="' + i + '">' + esc(action.title) + '</button>'; }).join('');
    state.col = clamp(state.col, state.actions.length - 1);
    $('dtStatus').textContent = state.error || (state.loading ? t('home.details.loading', 'Loading details…') : state.stale ? t('home.details.cached', 'Saved details · offline') : '');
    var similar = item.similar || [];
    $('dtMore').style.display = similar.length ? 'block' : 'none';
    $('dtCards').innerHTML = cardsHtml(similar, 'similar');
    if (!similar.length && state.zone === 'similar') state.zone = 'actions';
    detailFocus();
  }
  function readDetails(state) {
    var token = ++serial;
    state.loading = true; state.error = ''; renderDetails();
    api.getJson('/api/home/details?type=' + encodeURIComponent(state.item.type) + '&id=' + encodeURIComponent(state.item.id), function (r) {
      if (token !== serial || mode !== 'details' || current() !== state) return;
      state.loading = false;
      if (r && r.ok && r.enabled && r.item) {
        state.item = r.item; state.stale = !!r.stale;
        if (r.netflixEnabled) api.getJson('/api/home/netflix?type=' + encodeURIComponent(state.item.type) + '&id=' + encodeURIComponent(state.item.id), function (link) {
          if (token !== serial || mode !== 'details' || current() !== state) return;
          if (link && link.ok && link.netflixId) { state.item.netflixId = link.netflixId; renderDetails(); }
        });
      }
      else state.error = r && r.error || t('home.details.failed', 'Details are unavailable. Open or retry.');
      renderDetails();
    });
  }
  function showDetails(item) {
    if (stack.length >= 8) return api.toast(t('home.details.goBack', 'Go back before opening another title'));
    var state = { item: item, zone: 'actions', col: 0, card: 0, actions: [], error: '', loading: true, stale: false };
    stack.push(state); begin('details'); readDetails(state);
  }
  function detailAction() {
    var state = current();
    if (state.zone === 'back') return back();
    if (state.zone === 'similar') return showDetails(state.item.similar[state.card]);
    var action = state.actions[state.col]; if (!action) return;
    if (action.key === 'retry') return readDetails(state);
    if (action.key === 'overview') {
      overview = true; $('overviewText').textContent = state.item.overview; $('overviewText').scrollTop = 0; $('overviewView').className = 'gt-scrim show'; return;
    }
    var body = action.key === 'trailer' ? { kind: 'youtube', id: state.item.trailer } : { kind: action.key === 'netflix' ? 'netflix' : 'title', type: state.item.type, id: state.item.id };
    api.postJson('/api/home/open', body, function (r) { if (!r || !r.ok) api.toast(t('home.openFailed', 'That did not open'), r && r.error); });
  }
  function detailKey(code) {
    var state = current();
    if (code === 13) return detailAction();
    if (code === 38) state.zone = state.zone === 'similar' ? 'actions' : 'back';
    else if (code === 40) state.zone = state.zone === 'back' ? 'actions' : (state.item.similar || []).length ? 'similar' : 'actions';
    else if (code === 37 || code === 39) {
      var d = code === 39 ? 1 : -1;
      if (state.zone === 'actions') state.col = clamp(state.col + d, state.actions.length - 1);
      if (state.zone === 'similar') state.card = clamp(state.card + d, state.item.similar.length - 1);
    }
    detailFocus();
  }
  function editorFocus() {
    if (!keyboard) return;
    keyboard.active(editorZone === 'keyboard');
    $('editClose').classList.toggle('focus', editorZone === 'back');
    $('editClear').classList.toggle('focus', editorZone === 'footer' && footerIndex === 0);
    $('editDone').classList.toggle('focus', editorZone === 'footer' && footerIndex === 1);
    var nodes = $('editResults').children;
    for (var i = 0; i < nodes.length; i++) nodes[i].classList.toggle('focus', editorZone === 'results' && +nodes[i].getAttribute('data-result') === resultIndex);
  }
  function drawResults() {
    var start = Math.floor(resultIndex / 4) * 4, visible = results.slice(start, start + 4);
    $('editResults').innerHTML = visible.map(function (item, i) {
      return cardsHtml([item], 'result').replace('data-result="0"', 'data-result="' + (start + i) + '"');
    }).join('');
    var nodes = $('editResults').children;
    for (var i = 0; i < nodes.length; i++) mountArt(nodes[i].firstChild, visible[i]);
    $('editCount').textContent = results.length ? t('home.search.count', '{first}–{last} of {total}', { first: start + 1, last: Math.min(results.length, start + 4), total: results.length }) : '';
    editorFocus();
  }
  var searchVersion = 0;
  function runSearch() {
    clearTimeout(timer); timer = 0;
    if (!editing || profile || query.trim().length < 2 || document.hidden) return;
    var token = ++searchVersion, text = query.trim();
    $('editStatus').textContent = t('home.search.searching', 'Searching…');
    api.getJson('/api/home/search?q=' + encodeURIComponent(text), function (r) {
      if (!editing || profile || token !== searchVersion || text !== query.trim()) return;
      results = r && r.ok && r.enabled && Array.isArray(r.items) ? r.items : []; resultIndex = 0;
      $('editStatus').textContent = !r || !r.ok ? t('home.search.failed', 'Search is unavailable. Try again.') : r.enabled === false ? t('home.search.disabled', 'Search is unavailable') : results.length ? t('home.search.results', 'Results') : t('home.search.empty', 'No films or series found');
      if (editorZone === 'results' && !results.length) editorZone = 'keyboard';
      drawResults();
    });
  }
  function changed(value, cursor) {
    query = value; $('editQuery').value = value;
    try { $('editQuery').setSelectionRange(cursor, cursor); } catch (e) {}
    clearTimeout(timer); searchVersion++;
    if (profile) { $('editStatus').textContent = t('home.profile.limit', 'Up to 20 characters'); return; }
    results = []; resultIndex = 0; drawResults();
    $('editStatus').textContent = value.trim().length < 2 ? t('home.search.minimum', 'Enter at least 2 characters') : t('home.search.searching', 'Searching…');
    if (value.trim().length >= 2) timer = setTimeout(runSearch, 350);
  }
  function done() {
    if (!profile) {
      if (results.length) { editorZone = 'results'; editorFocus(); }
      else runSearch();
      return;
    }
    if (saving) return;
    var name = query.trim();
    if (!name) { $('editStatus').textContent = t('home.profile.empty', 'Enter a name'); return; }
    saving = true; $('editStatus').textContent = t('home.profile.saving', 'Saving…');
    var token = ++serial;
    api.postJson('/api/home/profile', { action: 'rename', id: profile.id, name: name }, function (r) {
      if (token !== serial || !editing) return;
      saving = false;
      if (!r || !r.ok) { $('editStatus').textContent = r && r.error || t('home.profile.failed', 'The name could not be saved. Try again.'); return; }
      api.reload(function () { if (token === serial && editing) close(); });
    });
  }
  function showEditor(person, onReturn) {
    serial++; searchVersion++; clearTimeout(timer); stack = []; editing = true; profile = person || null; query = person ? person.name : ''; saving = false;
    editorReturn = onReturn || null; results = []; resultIndex = 0; editorZone = 'keyboard'; footerIndex = 0;
    if (keyboard) keyboard.destroy();
    begin('editor');
    $('editorView').className = profile ? 'rename' : '';
    $('editLabel').textContent = profile ? t('home.profile.rename', 'Rename profile') : t('home.search.title', 'Search films and series');
    $('editDone').textContent = profile ? t('home.profile.save', 'Save') : t('home.search.results', 'Results');
    $('editQuery').setAttribute('aria-label', $('editLabel').textContent);
    keyboard = GhKeyboard.create($('editKeyboard'), { t: t, esc: esc, value: query, maxLength: profile ? 20 : 80, doneLabel: $('editDone').textContent,
      change: changed, done: done, focus: function () { editorZone = 'keyboard'; editorFocus(); } });
    changed(query, query.length);
  }
  function editorKey(code) {
    if (editorZone === 'keyboard') {
      if (keyboard.key(code)) return;
      if (code === 40) editorZone = 'footer';
      else if (code === 39 && results.length) editorZone = 'results';
      else if (code === 38) editorZone = 'back';
    } else if (editorZone === 'results') {
      if (code === 13) return showDetails(results[resultIndex]);
      if (code === 37 && resultIndex % 2 === 0) editorZone = 'keyboard';
      else {
        if (code === 38 && resultIndex < 2) editorZone = 'keyboard';
        else resultIndex = clamp(resultIndex + (code === 39 ? 1 : code === 37 ? -1 : code === 40 ? 2 : code === 38 ? -2 : 0), results.length - 1);
        drawResults();
      }
    } else if (editorZone === 'footer') {
      if (code === 13) return footerIndex ? done() : keyboard.clear();
      if (code === 38) editorZone = 'keyboard';
      if (code === 40 && results.length) editorZone = 'results';
      if (code === 37 || code === 39) footerIndex = code === 39 ? 1 : 0;
    } else if (code === 13) return back();
    else if (code === 40) editorZone = 'keyboard';
    editorFocus();
  }
  function back() {
    if (overview) { overview = false; $('overviewView').className = 'gt-scrim'; return; }
    if (mode === 'details') {
      serial++; stack.pop();
      if (stack.length) { begin('details'); renderDetails(); if (current().loading) readDetails(current()); return; }
      if (editing) { begin('editor'); editorFocus(); return; }
    } else if (editorZone === 'results') { editorZone = 'keyboard'; editorFocus(); return; }
    close();
  }
  function key(code) {
    if (code === 461) return back();
    if (overview) {
      if (code === 13) return back();
      if (code === 38 || code === 40) $('overviewText').scrollTop += code === 40 ? 100 : -100;
      return;
    }
    if (mode === 'details') detailKey(code); else if (mode === 'editor') editorKey(code);
  }
  function type(e) {
    if (mode !== 'editor' || e.ctrlKey || e.altKey || e.metaKey) return false;
    if (e.keyCode === 8) keyboard.erase();
    else if (e.key && e.key.length === 1 && !/[\x00-\x1f\x7f]/.test(e.key)) keyboard.insert(e.key);
    else return false;
    editorZone = 'keyboard'; editorFocus(); return true;
  }
  function up(el, attr) {
    while (el && el !== $('contentView')) { if (el.getAttribute && el.getAttribute(attr) !== null) return el; el = el.parentNode; }
    return null;
  }
  function init(handlers) {
    api = handlers;
    $('dtClose').textContent = $('editClose').textContent = t('home.content.back', 'Back');
    $('dtMoreTitle').textContent = t('home.details.more', 'More like this');
    $('editClear').textContent = t('home.search.clear', 'Clear');
    $('overviewClose').textContent = t('home.content.close', 'Close');
    $('contentView').addEventListener('click', function (e) {
      var el;
      if (overview) { if (e.target.id !== 'overviewText') back(); return; }
      if (e.target.id === 'dtClose' || e.target.id === 'editClose') return back();
      if (mode === 'details') {
        el = up(e.target, 'data-da'); if (el) { current().zone = 'actions'; current().col = +el.getAttribute('data-da'); detailFocus(); return detailAction(); }
        el = up(e.target, 'data-similar'); if (el) { current().zone = 'similar'; current().card = +el.getAttribute('data-similar'); return detailAction(); }
      } else {
        if (e.target.id === 'editClear') return keyboard.clear();
        if (e.target.id === 'editDone') return done();
        el = up(e.target, 'data-result'); if (el) { resultIndex = +el.getAttribute('data-result'); editorZone = 'results'; return showDetails(results[resultIndex]); }
      }
    });
    $('contentView').addEventListener('mousemove', function (e) {
      if (overview) return;
      var el;
      if (mode === 'details') {
        if (e.target.id === 'dtClose') current().zone = 'back';
        el = up(e.target, 'data-da'); if (el) { current().zone = 'actions'; current().col = +el.getAttribute('data-da'); }
        el = up(e.target, 'data-similar'); if (el) { current().zone = 'similar'; current().card = +el.getAttribute('data-similar'); }
        detailFocus();
      } else {
        if (e.target.id === 'editClose') editorZone = 'back';
        if (e.target.id === 'editClear' || e.target.id === 'editDone') { editorZone = 'footer'; footerIndex = e.target.id === 'editDone' ? 1 : 0; }
        el = up(e.target, 'data-result'); if (el) { editorZone = 'results'; resultIndex = +el.getAttribute('data-result'); }
        editorFocus();
      }
    });
  }
  return { init: init, details: showDetails, search: function () { showEditor(null, null); }, rename: showEditor, close: close, key: key, type: type,
    isOpen: function () { return !!mode; }, disableTitles: function () { if (mode && !profile) close(); }, hidden: function () { searchVersion++; clearTimeout(timer); },
    returned: function () { if (mode === 'editor' && !profile) runSearch(); } };
})();
