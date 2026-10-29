#!/usr/bin/env python3
"""
No Home Assistant entity a release published may disappear without a reason
on record.

Home Assistant keys an entity on its unique id and domain. One that goes, is
renamed or changes domain leaves the old entity unavailable, and any
automation, dashboard or script that used it stops working, with nothing to
say why. This reads every entity the daemon publishes from its discovery golden
(native-tv/tests/fixtures/ha-golden.json, held equal to what the daemon
publishes by the native-ha-golden test) and fails on any that the last release
had (scripts/released-entities.txt) and the code no longer does, unless it is
in scripts/retired-entities.txt. Entities added are listed and allowed.

    ./scripts/check-entity-stability.py            check
    ./scripts/check-entity-stability.py --record <tag>
                                                   rewrite released-entities.txt
                                                   from the golden, when <tag> is cut
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = ROOT / 'native-tv' / 'tests' / 'fixtures' / 'ha-golden.json'
RELEASED = ROOT / 'scripts' / 'released-entities.txt'
RETIRED = ROOT / 'scripts' / 'retired-entities.txt'


def entities():
    import json
    found = set()
    for entry in json.loads(GOLDEN.read_text(encoding='utf-8'))['discoveryBaseline']:
        m = re.match(r'homeassistant/(\w+)/lg_tv/(\w+)/config$', entry['topic'])
        if m:
            found.add(m.group(1) + '.' + m.group(2))
    return found


def listed(path):
    out = set()
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            out.add(line.split()[0])
    return out


def release_tag():
    for line in RELEASED.read_text(encoding='utf-8').splitlines():
        m = re.match(r'#\s*release:\s*(\S+)', line)
        if m:
            return m.group(1)
    return 'the last release'


def main():
    args = sys.argv[1:]
    now = entities()
    if args[:1] == ['--record']:
        if len(args) != 2:
            sys.exit('usage: check-entity-stability.py --record <tag>')
        RELEASED.write_text(
            '# Home Assistant entities the last release published, as <domain>.<id>.\n'
            "# check-entity-stability.py compares the daemon's discovery golden with this\n"
            '# list; --record rewrites it from the golden when a release is cut.\n'
            '# release: %s\n%s\n' % (args[1], '\n'.join(sorted(now))), encoding='utf-8')
        print('recorded %d entities for %s' % (len(now), args[1]))
        return 0
    tag = release_tag()
    before = listed(RELEASED)
    allowed = listed(RETIRED)
    gone = sorted(before - now - allowed)
    for e in sorted(now - before):
        print('new since %s: %s' % (tag, e))
    for e in sorted((before - now) & allowed):
        print('retired: %s' % e)
    for e in gone:
        print('%s was published by %s and is gone. Restore it, or list it in scripts/retired-entities.txt with why.' % (e, tag))
    print('%d entities in %s, %d now, %d gone without a reason' % (len(before), tag, len(now), len(gone)))
    return 1 if gone else 0


sys.exit(main())
