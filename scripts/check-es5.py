#!/usr/bin/env python3
"""
Check that the pages the TV's own browser runs are still ES5.

An ES6 construct is not a degraded feature in the TV's browser, it is a parse
error: the script stops with nothing on screen to say why. Nothing else in this
repo catches that - a laptop's browser parses all of it happily.

    ./scripts/check-es5.py [files...]

Defaults to the TV pages under native-tv/assets (google-tv/, the dashboard app
and the Home app), whose inline scripts are checked in place.
native-tv/web/ui.html and the scripts it loads from native-tv/web/ui/ run in a
phone or computer's browser and are not restricted.

Strings, template literals, regex literals and comments are blanked before the
scan, so a `=>` inside a string or a comment mentioning `const` is not a hit.

Syntax is checked everywhere; the calls and CSS the TV's engine lacks are
checked as well, as nothing runs those pages in CI, and a missing call there
stops the script with nothing on screen to say why - the TV app showed its
header and no rows for want of fetch.

Exits non-zero if anything is flagged.
"""
import re, sys, pathlib

# Constructs an ES5 engine cannot parse, and what to say about each.
RULES = [
    (r'(?<![.\w$])(?:let|const)\s+(?=[A-Za-z_$\[{])', 'let/const - use var'),
    (r'=>', 'arrow function - use function ()'),
    (r'(?<![.\w$])class(?![\w$])\s*[A-Za-z_$\{]', 'class - use a constructor function'),
    (r'\.\.\.', 'spread/rest - build the array or arguments explicitly'),
    (r'(?<![.\w$])async(?![\w$])\s*(?:function\b|\(|[A-Za-z_$])', 'async - use a callback'),
    (r'(?<![.\w$])await(?![\w$])\s', 'await - use a callback'),
    (r'(?<![.\w$])yield(?![\w$])', 'yield - generators are ES6'),
    (r'(?<![.\w$])function\s*\*', 'generator function'),
    (r'(?<![.\w$])for\s*\(\s*(?:var\s+|let\s+|const\s+)?[A-Za-z_$][\w$]*\s+of(?![\w$])',
     'for...of - use an index loop'),
    (r'(?<![.\w$])function\s*[A-Za-z_$\w]*\s*\([^)]*=[^)]*\)\s*\{', 'default parameter value'),
]


# Calls and CSS newer than Chromium 38 (webOS 3.x), for the pages the TV's own
# browser runs. Matched against code with strings and comments blanked.
TV_RULES = [
    (r'(?<![.\w$])fetch\s*\(', 'fetch - Chrome 42; use XMLHttpRequest, or the fallback dashboard.html defines'),
    (r'Object\.(?:assign|values|entries|fromEntries)\b', 'Object.assign/values/entries - Chrome 45+'),
    (r'\.(?:includes|startsWith|endsWith|padStart|padEnd|repeat|find|findIndex)\s*\(',
     'String/Array ES6 method - Chrome 41+; use indexOf or a loop'),
    (r'\.closest\s*\(', 'Element.closest - Chrome 41; walk parentNode'),
    (r'(?<![.\w$])(?:URLSearchParams|AbortController|IntersectionObserver|ResizeObserver|Symbol)\b',
     'API newer than Chrome 38'),
    (r'Array\.from\b|Number\.is(?:Finite|Integer|NaN)\b', 'ES6 built-in - Chrome 45+'),
    (r'new\s+(?:Map|Set|WeakMap|WeakSet|Proxy)\b', 'Map/Set/Proxy - incomplete or missing in Chrome 38'),
]
TV_CSS_RULES = [
    (r'var\(--', 'CSS custom property - Chrome 49'),
    (r'display\s*:\s*(?:inline-)?grid', 'CSS grid - Chrome 57'),
    (r'(?<![-\w])(?:row-|column-)?gap\s*:', 'gap - Chrome 57 for grid, 84 for flex'),
    (r'position\s*:\s*sticky', 'position: sticky - Chrome 56'),
    (r'(?<![-\w])(?:aspect-ratio|inset)\s*:', 'aspect-ratio/inset - Chrome 87'),
    (r'(?<![-\w.])(?:min|max|clamp)\(', 'CSS min()/max()/clamp() - Chrome 79'),
    (r':(?:is|where)\(', ':is()/:where() - Chrome 88'),
    (r'backdrop-filter', 'backdrop-filter - Chrome 76'),
]
# Pages the TV's own browser runs, and the scripts they load.
TV_PAGES = {'index.html'}


def is_tv(path):
    # The one-time phone handoff lives beside the setup page but uses a
    # modern phone browser. Every other shared/page asset runs on the TV.
    if 'google-tv' in path.parts:
        return 'setup-phone' not in path.parts
    return path.name in TV_PAGES


def blank(src):
    """
    Replace comment, string, template and regex bodies with spaces, keeping
    every newline so line numbers still line up. Returns the blanked source and
    the lines on which a template literal was opened - backticks are themselves
    ES6, and blanking one would otherwise hide it.
    """
    out, templates = [], []
    i, n, line = 0, len(src), 1
    # A '/' opens a regex rather than dividing when the last meaningful token
    # cannot end an expression.
    prev = ''

    def keep(ch):
        out.append(ch)

    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            keep(c)
            i += 1
            continue

        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                keep(' ')
                i += 1
            continue

        if c == '/' and i + 1 < n and src[i + 1] == '*':
            while i < n and not (src[i] == '*' and i + 1 < n and src[i + 1] == '/'):
                keep('\n' if src[i] == '\n' else ' ')
                if src[i] == '\n':
                    line += 1
                i += 1
            for _ in range(min(2, n - i)):
                keep(' ')
                i += 1
            continue

        if c in '"\'':
            quote = c
            keep(' ')
            i += 1
            while i < n and src[i] != quote:
                if src[i] == '\\':
                    keep(' ')
                    i += 1
                if i < n:
                    keep('\n' if src[i] == '\n' else ' ')
                    if src[i] == '\n':
                        line += 1
                    i += 1
            keep(' ')
            i += 1
            prev = 'x'
            continue

        if c == '`':
            templates.append(line)
            depth = 0
            keep(' ')
            i += 1
            while i < n:
                if src[i] == '\\':
                    keep(' ')
                    i += 1
                    if i < n:
                        keep(' ')
                        i += 1
                    continue
                if src[i] == '$' and i + 1 < n and src[i + 1] == '{':
                    depth += 1
                elif src[i] == '}' and depth:
                    depth -= 1
                elif src[i] == '`' and not depth:
                    break
                keep('\n' if src[i] == '\n' else ' ')
                if src[i] == '\n':
                    line += 1
                i += 1
            keep(' ')
            i += 1
            prev = 'x'
            continue

        if c == '/' and prev not in ('x', ')', ']'):
            # Regex literal: run to the closing '/', skipping escapes and the
            # character class, where '/' is literal.
            keep(' ')
            i += 1
            klass = False
            while i < n and src[i] != '\n':
                if src[i] == '\\':
                    keep(' ')
                    i += 1
                    if i < n:
                        keep(' ')
                        i += 1
                    continue
                if src[i] == '[':
                    klass = True
                elif src[i] == ']':
                    klass = False
                elif src[i] == '/' and not klass:
                    break
                keep(' ')
                i += 1
            keep(' ')
            i += 1
            prev = 'x'
            continue

        if not c.isspace():
            prev = 'x' if (c.isalnum() or c in '_$)]') else c
        keep(c)
        i += 1

    return ''.join(out), templates


def check(path):
    src = path.read_text(encoding='utf-8')
    raw = src
    if path.suffix == '.css':
        if not is_tv(path):
            print('%s: phone stylesheet (TV CSS rules do not apply)' % path)
            return 0
        # google-tv/, the design system the TV pages inline: CSS rules only,
        # outside comments (the token table names what is missing).
        raw = re.sub(r'(?s)/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), raw)
        hits = [(raw[:m.start()].count('\n') + 1, why)
                for pattern, why in TV_CSS_RULES for m in re.finditer(pattern, raw)]
        for ln, why in sorted(hits):
            print('%s:%d: %s' % (path, ln, why))
        print('%s: %d CSS rule%s newer than Chromium 38' % (path, len(hits), '' if len(hits) == 1 else 's'))
        return len(hits)
    if path.suffix == '.html':
        # Inline scripts only, each left on its own lines so a hit reports the
        # page's line number; the markup around them becomes blank lines.
        src = re.sub(r'(?s)(^|</script>).*?(<script>|$)',
                     lambda m: '\n' * m.group(0).count('\n'), src)
    code, templates = blank(src)
    hits = [(ln, 'template literal - use string concatenation') for ln in templates]
    rules = list(RULES)
    if is_tv(path):
        rules += [r for r in TV_RULES
                  if not (r[0].startswith('(?<![.\\w$])fetch') and 'window.fetch = function' in raw)]
    for pattern, why in rules:
        for m in re.finditer(pattern, code):
            hits.append((code[:m.start()].count('\n') + 1, why))
    if is_tv(path) and path.suffix == '.html':
        # Style blocks and attributes; scripts become blank lines.
        css = re.sub(r'(?s)<script.*?</script>', lambda m: '\n' * m.group(0).count('\n'), raw)
        for pattern, why in TV_CSS_RULES:
            for m in re.finditer(pattern, css):
                hits.append((css[:m.start()].count('\n') + 1, why))
    for ln, why in sorted(hits):
        print('%s:%d: %s' % (path, ln, why))
    print('%s: %d line%s, %d ES6 construct%s'
          % (path, src.count('\n') + 1, '' if src.count('\n') == 0 else 's',
             len(hits), '' if len(hits) == 1 else 's'))
    return len(hits)


if len(sys.argv) > 1:
    targets = [pathlib.Path(a) for a in sys.argv[1:]]
else:
    root = pathlib.Path(__file__).resolve().parent.parent
    assets = root / 'native-tv' / 'assets'
    targets = [assets / 'home-app' / 'index.html']
    # The TV pages, on the design system, and its reference page.
    targets += sorted((assets / 'google-tv').glob('**/*.html'))
    targets += sorted((assets / 'google-tv').glob('**/*.js'))
    targets += sorted((assets / 'google-tv').glob('**/*.css'))

sys.exit(1 if sum(check(t) for t in targets) else 0)
