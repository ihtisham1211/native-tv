(function () {
  var code = (location.search.match(/[?&]c=([0-9a-f]+)/) || [])[1] || '';
  var $ = function (id) { return document.getElementById(id); };
  var msg = $('msg'), btn = $('save');

  // Moving to TLS usually means moving to its port too, so follow it unless
  // the port was set by hand.
  var portTouched = false;
  $('port').addEventListener('input', function () { portTouched = true; });
  $('tls').addEventListener('change', function () {
    if (!portTouched) $('port').value = this.checked ? '8883' : '1883';
  });

  function say(text, err) { msg.textContent = text; msg.className = err ? 'err' : ''; }

  btn.addEventListener('click', function () {
    if (btn.disabled) return;
    var host = $('host').value.trim();
    if (!host) { $('host').focus(); return say(t('phone.needHost', 'Enter the broker address.'), true); }
    var port = parseInt($('port').value, 10);
    if (!(port >= 1 && port <= 65535)) { $('port').focus(); return say(t('phone.badPort', 'The port is a number from 1 to 65535.'), true); }
    btn.disabled = true;
    say(t('phone.sending', 'Sending…'));
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/mqtt?c=' + encodeURIComponent(code), true);
    xhr.timeout = 15000;
    xhr.setRequestHeader('Content-Type', 'application/json');
    xhr.onload = function () {
      var r = {};
      try { r = JSON.parse(xhr.responseText); } catch (e) {}
      if (xhr.status === 200 && r.ok) {
        $('form').hidden = true;
        $('done').hidden = false;
        return;
      }
      btn.disabled = false;
      if (xhr.status === 403) return say(t('phone.expired', 'This link has expired. Start again on the TV.'), true);
      say(r.error ? t('phone.refused', 'The TV turned that down: {error}.', { error: r.error }) : t('phone.failed', 'That did not work. Try again.'), true);
    };
    xhr.onerror = xhr.ontimeout = function () {
      btn.disabled = false;
      say(t('phone.unreachable', 'Could not reach the TV. Check your phone is on the same network.'), true);
    };
    xhr.send(JSON.stringify({
      host: host, port: port, tls: $('tls').checked,
      username: $('user').value.trim(), password: $('pass').value
    }));
  });
})();
