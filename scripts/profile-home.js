/* Paste into Home's Web Inspector console, then call ghHomeProfile.start()
 * and ghHomeProfile.stop(). Opt-in only; never loaded by the app. ES5 for
 * older TVs. Frame callbacks measure rendering opportunities, not physical
 * screen presentation. Collect a DevTools trace alongside these samples. */
(function () {
  if (window.ghHomeProfile && window.ghHomeProfile.stop) window.ghHomeProfile.stop();
  var perf = window.performance;
  var raf = window.requestAnimationFrame || window.webkitRequestAnimationFrame;
  var cancel = window.cancelAnimationFrame || window.webkitCancelAnimationFrame;
  var running = false, frame = null, last = null, started = 0, frames = [], keys = [], pending = [];
  var MAX_SAMPLES = 12000, focusSamples = [], previousFocus = null, observer = null, focus = null;
  function now() { return perf && perf.now ? perf.now() : Date.now(); }
  function summary(values) {
    var sorted = values.slice().sort(function (a, b) { return a - b; });
    function quantile(q) { return sorted.length ? Math.round(sorted[Math.ceil(sorted.length * q) - 1] * 10) / 10 : null; }
    return { samples: sorted.length, medianMs: quantile(0.5), p95Ms: quantile(0.95), p99Ms: quantile(0.99), maxMs: quantile(1) };
  }
  function eventTime(e) {
    var at = +e.timeStamp, current = now();
    if (at > 1000000000000 && perf && perf.timing) at -= perf.timing.navigationStart;
    return at > 0 && Math.abs(current - at) < 10000 ? at : current;
  }
  function key(e) {
    var code = e.keyCode;
    if (document.hidden || (code !== 37 && code !== 38 && code !== 39 && code !== 40)) return;
    if (pending.length < MAX_SAMPLES && keys.length < MAX_SAMPLES) pending.push(eventTime(e));
  }
  function changed() {
    var next = document.querySelector('#rows .tile.focus');
    if (next !== previousFocus) { previousFocus = next; focus = now(); }
  }
  function tick(at) {
    if (!running) return;
    // Use one clock throughout; old engines can give rAF a different origin.
    var current = now();
    if (!document.hidden) {
      if (last !== null && frames.length < MAX_SAMPLES) frames.push(current - last);
      last = current;
      while (pending.length && keys.length < MAX_SAMPLES) keys.push(Math.max(0, current - pending.shift()));
      if (focus !== null && focusSamples.length < MAX_SAMPLES) { focusSamples.push(Math.max(0, current - focus)); focus = null; }
    } else { last = null; pending = []; focus = null; }
    frame = raf.call(window, tick);
  }
  function visibility() { last = null; pending = []; focus = null; }
  function state() {
    var images = document.querySelectorAll('#rows img'), rgba = 0;
    for (var i = 0; i < images.length; i++) rgba += (images[i].naturalWidth || 0) * (images[i].naturalHeight || 0) * 4;
    return { rows: document.querySelectorAll('#rows .row').length, tiles: document.querySelectorAll('#rows .tile').length,
      images: images.length, mountedImageRgbaEstimateBytes: rgba,
      usedJsHeapBytes: perf && perf.memory ? perf.memory.usedJSHeapSize : null };
  }
  function report() {
    var images = [], entries = perf && perf.getEntriesByType ? perf.getEntriesByType('resource') : [];
    for (var i = 0; i < entries.length; i++) {
      var entry = entries[i];
      if (entry.startTime >= started && (entry.initiatorType === 'img' || /\/api\/home\/(?:image|icon|wallpaper)/.test(entry.name))) images.push(entry.duration);
    }
    var over25 = 0, over50 = 0;
    for (i = 0; i < frames.length; i++) { if (frames[i] > 25) over25++; if (frames[i] > 50) over50++; }
    return {
      elapsedMs: Math.round(now() - started), userAgent: navigator.userAgent,
      viewport: { width: window.innerWidth, height: window.innerHeight, dpr: window.devicePixelRatio || 1 },
      frameIntervals: summary(frames), frameGapsOver25Ms: over25, frameGapsOver50Ms: over50,
      keyEventToFrameCallback: summary(keys), focusMutationToFrameCallback: summary(focusSamples),
      completedImageResources: summary(images), dom: state(), cappedAtSamples: MAX_SAMPLES,
      notes: 'Callbacks are proxies, not screen presentation. RGBA estimates exclude backgrounds, caches, textures and buffering; JS heap excludes GPU/native memory. Resource entries may be limited by the browser buffer.'
    };
  }
  window.ghHomeProfile = {
    start: function () {
      if (!raf) throw new Error('requestAnimationFrame is unavailable');
      if (running) this.stop();
      frames = []; keys = []; pending = []; focusSamples = []; last = null; focus = null;
      started = now(); running = true;
      previousFocus = document.querySelector('#rows .tile.focus');
      document.addEventListener('keydown', key, true);
      document.addEventListener('visibilitychange', visibility);
      if (window.MutationObserver) {
        observer = new window.MutationObserver(changed);
        observer.observe(document.getElementById('rows'), { subtree: true, attributes: true, childList: true, attributeFilter: ['class'] });
      }
      frame = raf.call(window, tick);
      return { started: true, initialDom: state() };
    },
    stop: function () {
      running = false;
      if (cancel && frame !== null) cancel.call(window, frame);
      frame = null;
      document.removeEventListener('keydown', key, true);
      document.removeEventListener('visibilitychange', visibility);
      if (observer) { observer.disconnect(); observer = null; }
      return report();
    }
  };
})();
