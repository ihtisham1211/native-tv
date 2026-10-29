#!/usr/bin/env python3
"""
Check that every Home Assistant entity derives its state from something the TV
actually reports.

The display panel switch once published its state only when the command
arrived over MQTT, so blanking the panel from the dashboard or the remote left
Home Assistant asserting the opposite forever. Templates can also drift from
the telemetry payload silently - a renamed field just yields an entity stuck
at "unknown", which nobody notices for weeks.

This walks every discovery entity in the daemon's golden
(native-tv/tests/fixtures/ha-golden.json, which the native-ha-golden test holds
equal to what the daemon publishes), extracts the telemetry fields its
templates read (value_json.get('a') and (value_json.get('a') or {}).get('b')),
and resolves each against a /api/stats response.

    ./scripts/check-entities.py <tv-ip> [--token TOKEN] [--stats FILE]

A set with `token` configured - which the README recommends - answers /api/
with 401, so pass the same token here. TVWEB_TOKEN works too, and is the
better place for it: an argument is visible to every other process on the
machine for as long as the run takes.

--stats reads a saved /api/stats response instead of fetching one, so the
check can run against a recorded payload with no TV on the network.

A field the response lacks is fine if hardware the response's TV does not have
reports it (another fixture TV's stats, or the daemon's own code, names it);
it fails when nothing does, which is what a renamed or mistyped field looks like.

Exits non-zero if any field fails to resolve.
"""
import json, os, re, sys, urllib.request, urllib.error, urllib.parse, pathlib

args = sys.argv[1:]
ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'native-tv' / 'tests' / 'fixtures' / 'ha-golden.json'
FIXTURES = ROOT / 'native-tv' / 'tests' / 'fixtures' / 'tv'
TELEMETRY = 'tv/telemetry'


def take(flag):
    """Pull `--flag value` out of args, returning the value."""
    if flag not in args:
        return None
    i = args.index(flag)
    if i + 1 >= len(args):
        sys.exit(f'{flag} needs a value')
    args.pop(i)
    return args.pop(i)


stats_file = take('--stats')
token = take('--token') or os.environ.get('TVWEB_TOKEN')
tv = args[0] if args else None

if stats_file:
    try:
        stats = json.loads(pathlib.Path(stats_file).read_text(encoding='utf-8'))
    except Exception as e:
        sys.exit(f'could not read {stats_file}: {e}')
elif not tv:
    sys.exit('usage: check-entities.py <tv-ip> [--token TOKEN] [--stats FILE]')
else:
    url = f'http://{tv}:8080/api/stats'
    if token:
        url += '?k=' + urllib.parse.quote(token, safe='')
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            stats = json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            sys.exit(f'{tv} rejected the request: pass --token, or set TVWEB_TOKEN, '
                     'to match `token` in its config.json')
        sys.exit(f'could not reach {tv}: {e}')
    except Exception as e:
        sys.exit(f'could not reach {tv}: {e}')

if not isinstance(stats, dict) or not stats.get('ok'):
    sys.exit(f'not a usable /api/stats payload: {str(stats)[:120]}')

others = []
for f in sorted(FIXTURES.glob('stats-*.json')):
    try:
        others.append(json.loads(f.read_text(encoding='utf-8')))
    except ValueError:
        pass
daemon_source = '\n'.join(f.read_text(encoding='utf-8', errors='replace')
                          for f in (ROOT / 'native-tv' / 'src').rglob('*.cpp'))


def resolve(path, doc=None):
    cur = stats if doc is None else doc
    for part in path.split('.'):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            return False
    return True


def known_elsewhere(path):
    """Reported by another fixture TV, or a name the daemon's code builds."""
    return (any(resolve(path, doc) for doc in others)
            or all('"%s"' % part in daemon_source for part in path.split('.')))


GET = re.compile(r"""value_json\.get\(\s*['"](\w+)['"]\s*\)(\s+or\s+\{\}\s*\))?((?:\s*\.get\(\s*['"]\w+['"]\s*\))*)""")


def paths_of(config):
    """The telemetry fields a discovery config's templates read."""
    texts = [config.get(k) for k in ('value_template', 'json_attributes_template')]
    texts += [a.get('value_template') for a in config.get('availability') or []
              if a.get('topic') == TELEMETRY]
    found = set()
    for text in texts:
        for m in GET.finditer(text or ''):
            path = [m.group(1)]
            if m.group(2):          # (value_json.get('a') or {}).get('b'): a.b
                path += re.findall(r"""\.get\(\s*['"](\w+)['"]""", m.group(3))
            found.add('.'.join(path))
    return sorted(found)


entities = []
for entry in json.loads(GOLDEN.read_text(encoding='utf-8'))['discoveryBaseline']:
    m = re.match(r'homeassistant/(\w+)/lg_tv/(\w+)/config$', entry['topic'])
    if m:
        entities.append((m.group(1), m.group(2), json.loads(entry['payload'])))

failures, checked = [], 0
print(f'{"entity":30} {"state source":14} paths')
print('-' * 74)
for typ, eid, config in entities:
    topic = config.get('state_topic')
    source = ('telemetry' if topic == TELEMETRY or paths_of(config)
              else 'own topic' if topic else 'none')
    paths = paths_of(config)
    missing = [p for p in paths if not resolve(p)]
    bad = [p for p in missing if not known_elsewhere(p)]
    optional = [p for p in missing if p not in bad]
    checked += len(paths)
    status = ', '.join(paths) if paths else '(no template)'
    note = 'FAIL ' if bad else ('optional, absent: ' if optional else '')
    print(f'{eid:30} {source:14} {note}{status}')
    for p in bad:
        print(f'{"":46} MISSING: {p}')
        failures.append(f'{eid}: {p}')
    # Entities on their own topic cannot self-correct from the telemetry
    # payload, so flag them for a human to confirm something republishes them
    # from real state.
    if source == 'own topic':
        print(f'{"":46} note: own topic - confirm it is republished from real state')

print()
print(f'{checked} paths checked across {len(entities)} entities')
if failures:
    print(f'{len(failures)} unresolved:')
    for f in failures:
        print('  ' + f)
    sys.exit(1)
print('all entity templates resolve against the payload')
