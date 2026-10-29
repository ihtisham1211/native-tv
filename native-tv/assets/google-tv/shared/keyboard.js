/* TV kit keyboard, shared by title search and profile naming. Strict ES5. */
var GhKeyboard = (function () {
  function create(root, opts) {
    var t = opts.t, esc = opts.esc;
    var value = opts.value || '', cursor = value.length, row = 1, col = 0, upper = false, symbols = false, active = true;
    var keys = [], nodes = [];
    var labels = { shift: t('home.key.shift', 'Shift'), delete: t('home.key.delete', 'Delete'), left: t('home.key.left', 'Move cursor left'),
      right: t('home.key.right', 'Move cursor right'), space: t('home.key.space', 'Space'), done: opts.doneLabel || t('home.key.done', 'Done') };
    function render() {
      keys = ['1234567890'.split(''), (symbols ? '!@#$%&*()?' : 'qwertyuiop').split(''),
        (symbols ? '[]{}+=/:;.' : 'asdfghjkl.').split(''), ['shift'].concat((symbols ? "'\"<>\\|~," : 'zxcvbnm,').split('')).concat(['delete']),
        ['symbols', 'left', 'right', 'space', '-', '_', 'done']];
      root.innerHTML = keys.map(function (line, ri) {
        return '<div class="gt-key-row">' + line.map(function (key, ci) {
          var image = labels[key] && key !== 'symbols', label = labels[key] || (key === 'symbols' ? (symbols ? 'ABC' : '123?') : upper ? key.toUpperCase() : key);
          return '<button type="button" class="gt-key' + (key === 'space' ? ' space' : key === 'done' ? ' done' : key === 'symbols' ? ' symbols' : '') + '" data-kr="' + ri + '" data-kc="' + ci + '" aria-label="' + esc(label) + '">' +
            (image ? '<img src="/assets/google-tv/shared/key-' + key + '.svg" alt="">' : esc(label)) + '</button>';
        }).join('') + '</div>';
      }).join('');
      nodes = root.querySelectorAll('.gt-key-row'); paint();
    }
    function paint() {
      for (var ri = 0; ri < nodes.length; ri++) for (var ci = 0; ci < nodes[ri].children.length; ci++) {
        var key = keys[ri][ci], cls = 'gt-key' + (key === 'space' ? ' space' : key === 'done' ? ' done' : key === 'symbols' ? ' symbols' : '') + (active && ri === row && ci === col ? ' focus' : '');
        if (nodes[ri].children[ci].className !== cls) nodes[ri].children[ci].className = cls;
      }
    }
    function changed() { opts.change(value, cursor); }
    function insert(text) {
      if (value.length + text.length > (opts.maxLength || 80)) return;
      value = value.slice(0, cursor) + text + value.slice(cursor); cursor += text.length; changed();
    }
    function erase() { if (cursor) { value = value.slice(0, cursor - 1) + value.slice(cursor); cursor--; changed(); } }
    function press() {
      var key = keys[row][col];
      if (key === 'done') return opts.done(value);
      if (key === 'shift') { upper = !upper; return render(); }
      if (key === 'symbols') { symbols = !symbols; return render(); }
      if (key === 'delete') return erase();
      if (key === 'left' || key === 'right') { cursor = Math.max(0, Math.min(value.length, cursor + (key === 'right' ? 1 : -1))); return changed(); }
      insert(key === 'space' ? ' ' : upper ? key.toUpperCase() : key);
    }
    function key(code) {
      if (code === 13) { press(); return true; }
      if (code === 37 || code === 39) {
        if (code === 39 && col === keys[row].length - 1) return false;
        col = Math.max(0, Math.min(keys[row].length - 1, col + (code === 39 ? 1 : -1)));
      } else if (code === 38 || code === 40) {
        if (code === 38 && row === 0) return false;
        if (code === 40 && row === keys.length - 1) return false;
        row = Math.max(0, Math.min(keys.length - 1, row + (code === 40 ? 1 : -1))); col = Math.min(col, keys[row].length - 1);
      } else return false;
      paint(); return true;
    }
    function pointer(e, click) {
      var el = e.target;
      while (el && el !== root && !el.hasAttribute('data-kr')) el = el.parentNode;
      if (!el || el === root) return;
      row = +el.getAttribute('data-kr'); col = +el.getAttribute('data-kc'); active = true;
      if (opts.focus) opts.focus();
      paint(); if (click) press();
    }
    function hover(e) { pointer(e, false); }
    function click(e) { pointer(e, true); }
    root.addEventListener('mousemove', hover); root.addEventListener('click', click);
    render();
    return { key: key, insert: insert, erase: erase, clear: function () { value = ''; cursor = 0; changed(); },
      active: function (on) { active = on; paint(); },
      destroy: function () { root.removeEventListener('mousemove', hover); root.removeEventListener('click', click); root.innerHTML = ''; } };
  }
  return { create: create };
})();
