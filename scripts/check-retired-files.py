#!/usr/bin/env python3
"""
An upgrade from the last Node release must leave no stale Node files on the TV.

The release's deploy.sh installed a list of files under /var/lib/tvweb
(scripts/last-node-release-files.txt). The native deploy.sh ships its own FILES;
every file the Node release put there that the native one no longer ships has to
be deleted at start, or it stays for good: the dashboard's old scripts and, worse,
a tvweb.js that something may still start. Deleting is retired_files.cpp's job,
and a file it does not list is a file it does not delete.

    ./scripts/check-retired-files.py

Exits non-zero if a file of the last Node release is neither shipped nor retired.
"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
RELEASED = root / 'scripts' / 'last-node-release-files.txt'
DEPLOY = root / 'native-tv' / 'lifecycle' / 'deploy.sh'
RETIRED = root / 'native-tv' / 'src' / 'daemon' / 'retired_files.cpp'

old = {l.strip() for l in RELEASED.read_text(encoding='utf-8').splitlines()
       if l.strip() and not l.startswith('#')}
m = re.search(r'^FILES="(.*?)"', DEPLOY.read_text(encoding='utf-8'), re.S | re.M)
if not m:
    sys.exit('%s: no FILES list found' % DEPLOY.relative_to(root))
shipped = set(m.group(1).replace('\\\n', ' ').split())
retired = set(re.findall(r'"([^"]+)"', RETIRED.read_text(encoding='utf-8')))

stale = sorted(old - shipped - retired)
for f in stale:
    print('%s: installed by the last Node release, not shipped now and not in retired_files.cpp' % f)
print('%d files in the last Node release, %d shipped now, %d retired, %d stale'
      % (len(old), len(old & shipped), len(old & retired - shipped), len(stale)))
sys.exit(1 if stale else 0)
