#!/usr/bin/env python3
"""
Check the two lists that are maintained by hand against what the code does.

All three fail silently, which is why they are checked rather than remembered:

  * docs/integrations/home-assistant-entities.md is the entity reference, so an entity added to
    the daemon's catalogue, renamed or dropped leaves the table wrong with
    nothing to notice. The entities are read from the discovery golden
    (native-tv/tests/fixtures/ha-golden.json), which the native-ha-golden test
    holds equal to what the daemon publishes.
  * native-tv/lifecycle/deploy.sh copies a hardcoded FILES list. An asset added
    to native-tv/assets and left out of it is simply never installed, and the TV
    falls back to whatever the previous deploy left there - so the feature
    works on the developer's set and on nobody else's.
  * native-tv/tools/runtime_payload.py stages the files the TV client opens
    (fonts, icons, scrim) in each app that runs it. A file the client names
    that is not on that list is installed nowhere, and the client draws
    nothing where it should.

    ./scripts/check-drift.py

Exits non-zero if either has fallen out of step.
"""
import importlib.util, json, re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
doc_path = root / 'docs' / 'integrations' / 'home-assistant-entities.md'
problems = []

# | `sensor` | `sensor.lg_tv_soc_temperature` | SoC Temperature | ... |
ROW = re.compile(r'^\|\s*`(\w+)`\s*\|\s*`(\w+)\.lg_tv_(\w+)`', re.M)
GOLDEN = root / 'native-tv' / 'tests' / 'fixtures' / 'ha-golden.json'
DEPLOY = root / 'native-tv' / 'lifecycle' / 'deploy.sh'
ASSETS = root / 'native-tv' / 'assets'
# What deploy.sh installs from native-tv/assets (the rest is the TV client's).
SHIPPED = ['app-icons', 'dashboard-app', 'early-service', 'fonts', 'google-tv', 'home-app',
           'screensavers', 'splash-app']


def published():
    """Discovery configs published on a fully-capable set with allowPower on."""
    entities = set()
    for entry in json.loads(GOLDEN.read_text(encoding='utf-8'))['discoveryBaseline']:
        m = re.match(r'homeassistant/(\w+)/lg_tv/(\w+)/config$', entry['topic'])
        if m:
            entities.add((m.group(1), m.group(2)))
    return entities


def documented():
    """
    Entities the reference lists, by the id Home Assistant gives them. The
    domain is written twice in each row - its own column and the entity id -
    and a row that disagrees with itself is a copy-and-paste left half-edited.
    """
    rows = ROW.findall(doc_path.read_text(encoding='utf-8'))
    for column, domain, name in rows:
        if column != domain:
            problems.append('%s: %s.lg_tv_%s is in a `%s` row'
                            % (doc_path.relative_to(root), domain, name, column))
    return {(domain, name) for _, domain, name in rows}


def check_entities(in_code, in_doc):
    where = doc_path.relative_to(root)
    for domain, name in sorted(in_code - in_doc):
        problems.append('%s: %s.lg_tv_%s is published but not documented' % (where, domain, name))
    for domain, name in sorted(in_doc - in_code):
        problems.append('%s: documents %s.lg_tv_%s, which is not published' % (where, domain, name))
    if not in_doc:
        problems.append('%s: no entity rows found - has the table changed shape?' % where)


def check_deploy():
    where = str(DEPLOY.relative_to(root))
    deploy = DEPLOY.read_text(encoding='utf-8')
    m = re.search(r'^FILES="(.*?)"', deploy, re.S | re.M)
    if not m:
        problems.append('%s: no FILES list found' % where)
        return set()
    listed = {f for f in m.group(1).replace('\\\n', ' ').split() if f.startswith('assets/')}
    on_disk = set()
    for d in SHIPPED:
        for f in (ASSETS / d).rglob('*'):
            if f.is_file() and '__pycache__' not in f.parts:
                on_disk.add('assets/' + str(f.relative_to(ASSETS)))
    if (ASSETS / 'favicon.svg').exists():
        on_disk.add('assets/favicon.svg')
    # The dashboard is installed from native-tv/web (deploy.sh maps assets/ui*).
    web = root / 'native-tv' / 'web'
    if web.exists():
        for f in web.rglob('*'):
            if f.is_file():
                on_disk.add('assets/' + str(f.relative_to(web)))
    for missing in sorted(on_disk - listed):
        problems.append('%s: %s is not in FILES, so it is never installed' % (where, missing))
    for gone in sorted(listed - on_disk):
        problems.append('%s: FILES lists %s, which does not exist' % (where, gone))
    return on_disk


def check_client_assets():
    """Every asset file the client's sources name is among those staged for it."""
    spec = importlib.util.spec_from_file_location(
        'runtime_payload', root / 'native-tv' / 'tools' / 'runtime_payload.py')
    payload = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(payload)
    try:
        staged = set(payload.client_assets())
    except ValueError as error:
        problems.append('runtime_payload.py: %s' % error)
        return 0
    named = {}
    for source in (root / 'native-tv' / 'src').rglob('*'):
        if source.suffix not in ('.cpp', '.hpp'):
            continue
        text = source.read_text(encoding='utf-8')
        found = re.findall(r'"local:([\w./-]+\.\w+)"', text)
        found += ['fonts/' + n for n in re.findall(r'"(Figtree-[\w-]+\.ttf)"', text)]
        found += re.findall(r'"/?(?:assets/)?(Roboto-\w+\.woff)"', text)
        for relative in found:
            named.setdefault(relative, source)
    for relative, source in sorted(named.items()):
        if relative not in staged:
            problems.append('%s names assets/%s, which runtime_payload.py does not stage for the client'
                            % (source.relative_to(root), relative))
    return len(staged)


in_code = published()
in_doc = documented()
check_entities(in_code, in_doc)
assets = check_deploy()
client = check_client_assets()

for p in problems:
    print(p)
print('%d entities published, %d documented, %d assets in FILES, %d staged for the client, %d problem%s'
      % (len(in_code), len(in_doc), len(assets), client, len(problems), '' if len(problems) == 1 else 's'))
sys.exit(1 if problems else 0)
