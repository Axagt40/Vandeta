#!/usr/bin/env python3
"""Build the Vandeta SD-content pack (a release asset, not part of the image).

The firmware is one merged image, but two things have always lived on the
card: the Flipper IR libraries the Universal Remote and the IR Suite walk, and
the payloads the MITM / IR Suite features read. This packs the ones Vandeta
adds on top of the stock card layout, so a user flashing a fresh image has one
zip to unpack onto the card.

    python3 tools/make_sd_zip.py [--version 1.3]

Writes dist/vandeta-sd-files-<version>.zip. dist/ is git-ignored on purpose:
the pack is a release asset, not repository content.
"""
import argparse
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SD = os.path.join(ROOT, "sd_files")

# Everything the IR Suite, the universal remote menu and the MITM payloads read.
# Paths are relative to sd_files/ and keep their place on the card.
INCLUDE = [
    "infrared/Universal",
    "infrared/families",
    "infrared/TVs",
    "BruceIR/payloads",
    "BruceIR/images",
    "BruceSpoof",
]


# Never published: the rickroll payloads are private to this machine. They stay
# out of the zip for the same reason they stay out of git.
EXCLUDE_NAMES = {"rickroll.mitm", "rickroll.html"}


def add_tree(zf, rel):
    src = os.path.join(SD, rel)
    if not os.path.isdir(src):
        print("[skip] %s (not in the tree)" % rel)
        return 0
    count = 0
    skipped = 0
    for base, _dirs, files in os.walk(src):
        for name in sorted(files):
            if name in EXCLUDE_NAMES:
                skipped += 1
                continue
            path = os.path.join(base, name)
            arc = os.path.relpath(path, SD)
            zf.write(path, arc)
            count += 1
    print("[add ] %-24s %d files%s" % (rel, count, (" (%d private, skipped)" % skipped) if skipped else ""))
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.3")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    out = args.out or os.path.join(ROOT, "dist", "vandeta-sd-files-%s.zip" % args.version)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    total = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel in INCLUDE:
            total += add_tree(zf, rel)

    if total == 0:
        os.remove(out)
        print("[x] nothing packed - is this the right tree?", file=sys.stderr)
        return 1

    size = os.path.getsize(out)
    print("\n%s\n  %d files, %d bytes" % (out, total, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
