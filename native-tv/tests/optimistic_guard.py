#!/usr/bin/env python3
"""Fails when a page of the TV client writes to the server or the TV without going through the
optimistic-update layer (src/ui/optimistic.hpp).

A page reaches the world through its ScreenHost: `host.optimistic().set(...)` or `.run(...)` for
anything it changes, `host.services().read(...)` for what it reads. `host.services().write(...)` is
the raw path the layer itself is built on; this check keeps new code off it.

  optimistic_guard.py [--src native-tv/src/ui]

The Application's own files may write (they hold the Services the layer wraps). The pages that
still do are listed with how many raw writes each may keep, in optimistic_guard.json: the count
only goes down, and the file leaves the list when it reaches zero.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE_WRITE = re.compile(r"\bservices\(\)\s*\.\s*write\s*\(")
APPLICATION_WRITE = re.compile(r"\bservices_\s*\.\s*write\s*\(")
# What implements the Application (it owns the Services); a page may not reach services_ at all.
APPLICATION_FILES = {"application.cpp", "navigation.cpp", "home_motion.cpp", "data_controller.cpp", "input.cpp",
                     "recents_controller.cpp", "picture_controller.cpp", "menu_controller.cpp", "optimistic.cpp"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main(argv):
    source = os.path.join(HERE, "..", "src", "ui")
    if "--src" in argv:
        source = argv[argv.index("--src") + 1]
    with open(os.path.join(HERE, "optimistic_guard.json")) as file:
        allowed = json.load(file)["pagesStillWriting"]
    problems = []
    for folder, _, names in os.walk(source):
        for name in sorted(names):
            if not name.endswith((".cpp", ".hpp")):
                continue
            with open(os.path.join(folder, name)) as file:
                text = strip_comments(file.read())
            pages = len(PAGE_WRITE.findall(text))
            internal = len(APPLICATION_WRITE.findall(text))
            if pages > allowed.get(name, 0):
                problems.append("%s: %d raw write(s) through host.services(), %d allowed; use host.optimistic()"
                                % (name, pages, allowed.get(name, 0)))
            if allowed.get(name, 0) > pages:
                problems.append("%s: %d raw write(s) left; lower its count in optimistic_guard.json to %d"
                                % (name, pages, pages))
            if internal and name not in APPLICATION_FILES:
                problems.append("%s: writes through services_, which only the Application's files may" % name)
    for name, count in allowed.items():
        if count and not os.path.exists(os.path.join(source, name)):
            problems.append("optimistic_guard.json lists %s, which is gone" % name)
    print("\n".join(problems) if problems else "no raw writes outside the layer")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
