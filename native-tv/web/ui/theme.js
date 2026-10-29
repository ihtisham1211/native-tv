// The theme toggle: follow the system, or stay dark or light.

// The toggle's mark, from the sprite in the page.
const THEME_MARKS = { auto: 'contrast', light: 'light_mode', dark: 'dark_mode' };

(function initTheme() {
  const html = document.documentElement;
  const mq = matchMedia('(prefers-color-scheme: dark)');
  const urlTheme = new URLSearchParams(location.search).get('theme');
  let stored = null;
  try { stored = localStorage.getItem('theme'); } catch (e) { /* private window */ }
  let setting = urlTheme || stored || 'auto';
  if (setting !== 'auto' && setting !== 'dark' && setting !== 'light') setting = 'auto';
  apply();

  function themeName(s) {
    return s === 'light' ? t('theme.light', 'Light') : s === 'dark' ? t('theme.dark', 'Dark') : t('theme.auto', 'Auto');
  }

  // Browsers without prefers-color-scheme report the query as "not all"; the
  // sheet's light roles are what they get.
  function resolved() {
    if (setting !== 'auto') return setting;
    return mq.media !== 'not all' && mq.matches ? 'dark' : 'light';
  }

  // The sheet follows the system with no attribute, and a choice sets one.
  function apply() {
    if (setting === 'auto') html.removeAttribute('data-theme');
    else html.setAttribute('data-theme', setting);
    const btn = q('theme-toggle');
    if (btn) {
      btn.innerHTML = '<svg class="ic" aria-hidden="true"><use href="#i-' + THEME_MARKS[setting] + '"/></svg>';
      const label = t('theme.toggle', 'Theme: {setting} (click for {next})',
                      { setting: themeName(setting), next: themeName(nextSetting()) });
      btn.title = label;
      btn.setAttribute('aria-label', label);
    }
    const mc = document.querySelector('meta[name="theme-color"]');
    if (mc) mc.content = resolved() === 'light' ? '#f7faf8' : '#111411';
  }

  function nextSetting() {
    const order = ['auto', 'dark', 'light'];
    return order[(order.indexOf(setting) + 1) % order.length];
  }

  function onMediaChange() { if (setting === 'auto') apply(); }
  if (mq.addEventListener) mq.addEventListener('change', onMediaChange);
  else if (mq.addListener) mq.addListener(onMediaChange);   // Chrome <= 68 (webOS 5.x)

  document.addEventListener('click', function(e) {
    if (!e.target.closest('#theme-toggle')) return;
    e.preventDefault();
    html.classList.add('theming');
    setting = nextSetting();
    try { localStorage.setItem('theme', setting); } catch (err) { /* private window */ }
    apply();
    setTimeout(function () { html.classList.remove('theming'); }, 260);
  });
})();
