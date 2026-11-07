# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Generate docs/settings/MASTER-SETTINGS.md from the module README '## Settings' tables.

Usage: python -I tools/gen_master_settings.py [--root PATH] [--check] [--stdout]

  (default)  write docs/settings/MASTER-SETTINGS.md
  --check    exit 1 when the committed file differs from a fresh generation
  --stdout   print instead of writing

Output is deterministic (sorted, no timestamps). It refuses to name the owner's
display (exit 1) so the wording rule cannot be bypassed through a table row.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import master_settings_common as C  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--stdout", action="store_true")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    text = C.render_master(root)
    if C.FL35.search(text):
        print("gen_master_settings: output names the owner's display; fix the source row", file=sys.stderr)
        return 1
    path = os.path.join(root, *C.MASTER_REL.split("/"))
    if args.stdout:
        sys.stdout.write(text)
        return 0
    if args.check:
        cur = open(path, encoding="utf-8").read() if os.path.isfile(path) else None   # universal newlines
        if cur != text:
            print("%s is out of date: run python -I tools/gen_master_settings.py" % C.MASTER_REL, file=sys.stderr)
            return 1
        print("%s is current" % C.MASTER_REL)
        return 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote %s" % C.MASTER_REL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
