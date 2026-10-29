# Strings

Text shown on the dashboards is written in English where it is used, with a key
next to it. The key names the string, so the same words are used wherever it
appears, and `scripts/check-strings.py` catches the ones that drift apart.
nativeTv is English only: there are no translations.

`scripts/check-strings.py` enforces what follows, and runs in CI.

## Adding a string

In markup, key the element that holds the text, and nothing but the text:

```html
<div class="prot-title" data-t="server.checkDaily">Check daily</div>
<button data-t="server.checkNow">Check now</button>
```

An attribute takes `data-t-` and its name, with the English in the attribute:

```html
<div role="group" aria-label="Theme" data-t-aria-label="theme">
```

`title`, `aria-label` and `placeholder` are covered.

In the page's script, call `t()` with a literal key and literal English. Values
go in through named placeholders:

```js
t('server.install', 'Install v{version}', { version: v })
```

`t()` returns the English with its placeholders filled. The web dashboard's is
in `native-tv/web/ui/core.js`, the TV pages' in `native-tv/assets/google-tv/shared/common.js`.

## Keys

Lowercase words joined by dots, camelCase within a word:
`server.tvUpdates.block.desc`. Start with the tab or page (`server.`, `adv.`),
or `common.` for words used across it such as `common.on`.

One key, one English text. Where the same words appear in several places, such
as a description on both dashboards, use the same key, so the check notices when
one copy is edited and the others are not. Different wording needs a different
key.

## Rules

- **Whole sentences.** Never build a sentence from pieces such as
  `n + t('x', ' of ') + total`. Put the sentence in one string with
  placeholders: `'{n} of {total} on'`.
- **A value stays as it is.** Anything compared with what the TV reports, sent
  to the server or the TV, or used as a state is never put through `t()`. Keep
  state in a variable or `dataset`, never read it back from displayed text.
- **Script-owned text is keyed in the script.** An element whose text the script
  sets gets no `data-t`, and holds a placeholder such as `&mdash;` in the
  markup until the script fills it.
- **No local variable named `t`** in a page that uses `t()`. It would hide
  `t()` in that function.

## Messages from the server

Text the server sends a page - an error, a screen saver's name, an agreement's
description - is written with `say::msg()`, from `native-tv/src/domain/say.hpp`:

```cpp
say::msg("srv.install.busy", "an install is already in progress")
say::msg("srv.saver.stageFailed", "could not stage the screen saver: {error}", variables)
```

`say::msg()` returns the English, so the server, Home Assistant, MQTT and the log
all carry on in English. The key names the string, so the check holds the
server's wording and the page's to the same words.

Errors a page cannot cause, such as a malformed request, stay in English
without `say::msg()`. Names that come from the TV itself, such as its picture modes
and inputs, are shown as the TV gives them.

## Converting part of a page

Key every piece of text in it, then add the element's id to `CONVERTED` in
`scripts/check-strings.py`. From then on, text added there without a key fails
the check.
