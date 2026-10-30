#!/usr/bin/env python3
"""
Check the dashboards' strings. docs/development/strings.md has the rules this enforces.

Every string shown is written in English where it is used, with a key:

    <div data-t="server.status">Server status</div>
    t('server.install', 'Install v{version}', { version: v })

and every message the daemon sends a page, in native-tv/src:

    say::msg("srv.install.busy", "an install is already running")

The key names the string, so the same words are used wherever it appears.

Fails on:
  * a key used with two different English texts - one of them is stale
  * a key not written as lowercase words joined by dots and camelCase
  * t() called without a literal key and literal English
  * an element tagged data-t that holds other elements, or whose text the page's
    script rewrites - either way the markup is not the whole string
  * visible text, or a title/aria-label/placeholder, without a key inside a part
    of a page already converted (CONVERTED below)
  * a local variable named t in a page that uses t(), which would hide it
  * a page that uses keys but loads no script that defines t()

    ./scripts/check-strings.py              check
"""
import html.parser, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / 'native-tv' / 'web'
# The browser dashboard. The TV pages (google-tv/pages/) are English only.
PAGES = ['ui.html']

# Parts of pages whose visible text must all be keyed, by element id or, for an
# element without one, by its tag as '<header>'. A part is added here once
# converted, so the check holds it there from then on. Inside one, text that is
# the same in every language - a product name, a unit - carries translate="no".
CONVERTED = {
    'ui.html': ['<body>'],
}

KEY_RE = re.compile(r'^[a-z][a-zA-Z0-9]*(\.[a-zA-Z0-9]+)*$')
TEXT_ATTRS = ('title', 'aria-label', 'placeholder')
VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr'}

problems = []
uses = {}      # key -> {english: [where, ...]}


def squash(s):
    return re.sub(r'\s+', ' ', s).strip()


def note(key, english, where):
    if not KEY_RE.match(key):
        problems.append('%s: key "%s" is not dotted lowercase/camelCase words' % (where, key))
    uses.setdefault(key, {}).setdefault(english, []).append(where)


# ---- markup ---------------------------------------------------------------

class Markup(html.parser.HTMLParser):
    """Collects data-t strings, and text left unkeyed inside converted parts."""

    def __init__(self, name, scopes):
        super().__init__(convert_charrefs=True)
        self.name, self.scopes = name, set(scopes)
        self.stack = []          # [tag, keyed, in_scope, id, untranslated]
        self.keyed = None        # [key, text parts, line, has child element]
        self.in_code = 0
        self.keyed_ids = {}

    def where(self):
        return '%s:%d' % (self.name, self.getpos()[0])

    def in_scope(self):
        return any(entry[2] for entry in self.stack) and not any(entry[4] for entry in self.stack)

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag in ('script', 'style'):
            self.in_code += 1
        if self.keyed is not None:
            self.keyed[3] = True
        scope = a.get('id') in self.scopes or '<%s>' % tag in self.scopes or self.in_scope()
        untranslated = a.get('translate') == 'no'
        if untranslated:
            scope = False
        for attr in TEXT_ATTRS:
            val = a.get(attr)
            key = a.get('data-t-' + attr)
            if key:
                note(key, squash(val or ''), self.where())
            elif scope and val and re.search(r'[A-Za-z]{2}', val):
                problems.append('%s: %s="%s" has no data-t-%s key' % (self.where(), attr, squash(val), attr))
        if 'data-t' in a:
            self.keyed = [a['data-t'], [], self.where(), False]
            if a.get('id'):
                self.keyed_ids[a['id']] = self.where()
        if tag not in VOID:
            self.stack.append([tag, 'data-t' in a, scope, a.get('id'), untranslated])

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID and self.stack and self.stack[-1][0] == tag:
            self.stack.pop()

    def handle_endtag(self, tag):
        if tag in ('script', 'style'):
            self.in_code = max(0, self.in_code - 1)
        while self.stack:
            entry = self.stack.pop()
            if entry[0] == tag:
                break
        if self.keyed is not None and not any(e[1] for e in self.stack):
            key, parts, where, child = self.keyed
            self.keyed = None
            if child:
                problems.append('%s: data-t="%s" holds other elements; key the text alone' % (where, key))
            else:
                note(key, squash(''.join(parts)), where)

    def handle_data(self, data):
        if self.in_code:
            return
        if self.keyed is not None:
            self.keyed[1].append(data)
        elif self.in_scope() and re.search(r'[A-Za-z]{2}', data):
            problems.append('%s: "%s" has no data-t key' % (self.where(), squash(data)[:60]))


# ---- script ---------------------------------------------------------------

def strip_code(src):
    """The script with comments blanked and every string/regex kept whole, and
    the spans of string literals, so t( inside a string or comment is ignored."""
    out, i, n = [], 0, len(src)
    last = ''                    # last significant character, to tell / apart
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j; continue
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j])); i = j; continue
        if c in '\'"`' or (c == '/' and (last == '' or last in '(,=:[!&|?{};+-*%<>~^')):
            q, j = c, i + 1
            in_class = False
            while j < n:
                if src[j] == '\\':
                    j += 2; continue
                if q == '/' and src[j] == '[':
                    in_class = True
                elif q == '/' and src[j] == ']':
                    in_class = False
                elif src[j] == q and not in_class:
                    break
                elif src[j] == '\n' and q != '`':
                    break
                j += 1
            out.append(src[i:j + 1]); last = q; i = j + 1; continue
        out.append(c)
        if not c.isspace():
            last = c
        i += 1
    return ''.join(out)


def js_string(src, i):
    """A string literal at src[i]: (value, end) or None."""
    q = src[i] if i < len(src) else ''
    if q not in '\'"`':
        return None
    j, val = i + 1, []
    while j < len(src) and src[j] != q:
        if q == '`' and src.startswith('${', j):
            return None
        if src[j] == '\\':
            nxt = src[j + 1]
            if nxt == 'u':
                val.append(chr(int(src[j + 2:j + 6], 16))); j += 6; continue
            val.append({'n': '\n', 't': '\t'}.get(nxt, nxt)); j += 2; continue
        val.append(src[j]); j += 1
    return ''.join(val), j + 1


def scan_script(name, src, offset_line):
    code = strip_code(src)
    for m in re.finditer(r'(?<![\w$.])t\(', code):
        if code[:m.start()].endswith('function '):
            continue   # the page's own definition of t()
        start = m.end()
        where = '%s:%d' % (name, offset_line + src.count('\n', 0, m.start()))
        rest = code[start:].lstrip()
        if rest.startswith(')'):
            continue                                   # a mention: "t()"
        i = start + (len(code[start:]) - len(rest))
        key = js_string(src, i)
        if not key:
            problems.append('%s: t() needs a literal key' % where)
            continue
        j = key[1]
        while src[j].isspace():
            j += 1
        if src[j] != ',':
            problems.append('%s: t(\'%s\') needs its English as the second argument' % (where, key[0]))
            continue
        j += 1
        while src[j].isspace():
            j += 1
        english = js_string(src, j)
        if not english:
            problems.append('%s: t(\'%s\', ...) needs literal English' % (where, key[0]))
            continue
        note(key[0], english[0], where)
    for m in re.finditer(r'(?:\b(?:const|let|var)\s+t\b|\(\s*t\s*[,)]|\bt\s*=>|function\s*\w*\s*\(\s*t\s*[,)])', code):
        problems.append('%s:%d: a local named t hides t()' % (name, offset_line + src.count('\n', 0, m.start())))
    return code


def uses_in(page):
    return any(w.startswith(page + ':') for texts in uses.values() for ws in texts.values() for w in ws)


def rewritten_ids(code):
    return set(re.findall(r"""q\(\s*['"]([\w-]+)['"]\s*\)\s*\.\s*(?:textContent|innerHTML|innerText)\s*=""", code))


# ---- run ------------------------------------------------------------------

for page in PAGES:
    path = ASSETS / page
    src = path.read_text(encoding='utf-8')
    parser = Markup(page, CONVERTED.get(page, []))
    parser.feed(src)
    missing_scopes = [s for s in CONVERTED.get(page, [])
                      if ('<%s' % s.strip('<>') if s.startswith('<') else 'id="%s"' % s) not in src]
    for s in missing_scopes:
        problems.append('%s: converted part #%s no longer exists' % (page, s))
    # The page's own scripts, and the ones it loads from assets/ui/, which the
    # daemon puts back inline.
    code = ''
    for m in re.finditer(r'<script>([\s\S]*?)</script>', src):
        code += scan_script(page, m.group(1), src.count('\n', 0, m.start(1)) + 1)
    for rel in re.findall(r'<script src="/assets/(ui/[\w-]+\.js)"></script>', src):
        code += scan_script(rel, (ASSETS / rel).read_text(encoding='utf-8'), 1)
    # A page that uses keys needs t(), or its strings never reach the screen.
    if (parser.keyed_ids or uses_in(page)) and not re.search(r'function\s+t\s*\(', code):
        problems.append('%s: uses keys but loads no script that defines t()' % page)
    for el_id in rewritten_ids(code) & set(parser.keyed_ids):
        problems.append('%s: #%s is tagged data-t but the script rewrites it; key it in the script instead'
                        % (parser.keyed_ids[el_id], el_id))

# The daemon's messages: the same rules, with say::msg() in place of t().
CPP_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0'}


def cpp_literals(src, i):
    """Adjacent C++ string literals at src[i] ("a" "b" is one): (value, end) or None."""
    val, found = [], False
    n = len(src)
    while True:
        while i < n and src[i].isspace():
            i += 1
        if src.startswith('//', i):
            i = src.find('\n', i)
            i = n if i < 0 else i
            continue
        if src.startswith('/*', i):
            i = src.find('*/', i + 2)
            i = n if i < 0 else i + 2
            continue
        m = re.compile(r'u8R"([^(\s]{0,16})\(|R"([^(\s]{0,16})\(').match(src, i)
        if m:                                              # raw string literal
            delim = m.group(1) if m.group(1) is not None else m.group(2)
            end = src.find(')' + delim + '"', m.end())
            if end < 0:
                return None
            val.append(src[m.end():end]); i = end + len(delim) + 2; found = True
            continue
        if i < n and src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == '\\':
                    nxt = src[j + 1]
                    if nxt == 'u':
                        val.append(chr(int(src[j + 2:j + 6], 16))); j += 6; continue
                    val.append(CPP_ESCAPES.get(nxt, nxt)); j += 2; continue
                val.append(src[j]); j += 1
            i = j + 1; found = True
            continue
        return (''.join(val), i) if found else None


SERVER = sorted((ROOT / 'native-tv' / 'src').rglob('*.cpp'))
SERVER = [f for f in SERVER if 'ui' not in f.relative_to(ROOT / 'native-tv' / 'src').parts[:1]]
for path in SERVER:
    src = path.read_text(encoding='utf-8')
    if 'msg(' not in src:
        continue
    name = str(path.relative_to(ROOT))
    for m in re.finditer(r'(?<![\w.>])(?:say::)?msg\(', src):
        line = src[src.rfind('\n', 0, m.start()) + 1:m.start()]
        if line.lstrip().startswith('//') or re.search(r'\bstd::string\s+$', line):
            continue                                       # a comment, or say::msg's own definition
        where = '%s:%d' % (name, 1 + src.count('\n', 0, m.start()))
        key = cpp_literals(src, m.end())
        if not key:
            continue                                       # a wrapper passing its caller's key on
        j = key[1]
        while src[j].isspace():
            j += 1
        if src[j] != ',':
            problems.append("%s: msg('%s') needs its English as the second argument" % (where, key[0]))
            continue
        english = cpp_literals(src, j + 1)
        if not english:
            continue                                       # English built by the caller
        note(key[0], english[0], where)

for key, texts in sorted(uses.items()):
    if len(texts) > 1:
        detail = '; '.join('"%s" at %s' % (e, ', '.join(w)) for e, w in texts.items())
        problems.append('key "%s" has different English in different places: %s' % (key, detail))

for p in problems:
    print(p)
print('%d strings keyed, %d problem%s'
      % (len(uses), len(problems), '' if len(problems) == 1 else 's'))
sys.exit(1 if problems else 0)
