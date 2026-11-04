// Typing into the text field the TV has open, from the Remote sheet. The TV's own
// text-input service takes the text (native-tv/src/service/keyboard.cpp).

function keyboardStatus(text) {
  const el = q('kbd-status');
  el.textContent = text;
  el.hidden = !text;
}

async function sendKeyboard(body) {
  try {
    const r = await fetch(api('/api/keyboard'), {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
    });
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || t('common.actionFailed', 'Action failed'));
    keyboardStatus('');
    return true;
  } catch (e) {
    keyboardStatus(e.message);
    return false;
  }
}

// The text stays in the box when the TV does not take it, to send again.
async function sendKeyboardText(event) {
  event.preventDefault();
  const input = q('kbd-input');
  if (!input.value) return;
  if (await sendKeyboard({ action: 'text', text: input.value })) input.value = '';
}

function keyboardKey(action) { sendKeyboard({ action }); }
