#!/usr/bin/env python3
"""ANTS-3766 INV-2's golden generator — an INDEPENDENT reimplementation.

The invariant this feeds says a project with no `docs/roadmap/` plans exactly
as it did before archives existed, and the golden's whole value is being
derived WITHOUT reference to src/roadmapmigrate.cpp. Generating it by running
the code under test would produce a golden that agrees with any implementation,
including the one whose mutation INV-2 exists to catch (namespacing or
renumbering unconditionally rather than only at index >= 1).

So this reimplements RoadmapIndex::slugifyHeading() and uniqueSlug() from their
STATED behaviour, and walks headings with the one structural rule that matters
here: a `##`/`###` inside a fenced block is not a heading.

Usage: tools/roadmap-slug-oracle.py <fixture-dir> > expected-section-slugs.json
"""
import json
import os
import sys
import unicodedata


def slugify(heading: str) -> str:
    out, prev_dash = [], True
    for ch in heading:
        low = ch.lower()
        # Qt's QChar::isLetterOrNumber() is the L* and N* general categories.
        if unicodedata.category(low)[0] in ("L", "N"):
            out.append(low)
            prev_dash = False
        elif not prev_dash:
            out.append("-")
            prev_dash = True
    while out and out[-1] == "-":
        out.pop()
    return "".join(out)


def unique(seen: set, base: str) -> str:
    # Mirrors uniqueSlug(): an EMPTY base is returned immediately, without
    # uniquing and WITHOUT being inserted into `seen`. That is the pre-existing
    # hole ANTS-3766 § 2.3 avoids for archives and deliberately leaves alone on
    # the live path, so the oracle has to reproduce it rather than fix it.
    if not base:
        return base
    if base not in seen:
        seen.add(base)
        return base
    n = 2
    while f"{base}-{n}" in seen:
        n += 1
    seen.add(f"{base}-{n}")
    return f"{base}-{n}"


def heading_level(line: str):
    stripped = line.lstrip()
    if not stripped.startswith("#"):
        return 0, ""
    n = len(stripped) - len(stripped.lstrip("#"))
    rest = stripped[n:]
    if rest and not rest.startswith(" "):
        return 0, ""
    return n, rest.strip()


def slugs_for(path: str):
    seen: set[str] = set()
    out: list[str] = []
    in_fence = False
    with open(path, encoding="utf-8") as fh:
        for line in fh.read().split("\n"):
            if line.strip().startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            lvl, text = heading_level(line)
            if lvl in (2, 3):
                out.append(unique(seen, slugify(text)))
    return out


def live_roadmap(root: str):
    for name in sorted(os.listdir(root)):
        if name.lower() == "roadmap.md" and os.path.isfile(os.path.join(root, name)):
            return os.path.join(root, name)
    return None


def main():
    base = sys.argv[1]
    result = {
        "_what": "ANTS-3766 INV-2 — the ordered live-source section-slug list "
                 "per fixture root, derived by tools/roadmap-slug-oracle.py, an "
                 "independent reimplementation of slugifyHeading/uniqueSlug. "
                 "Regenerate: tools/roadmap-slug-oracle.py "
                 "tests/features/roadmap_migrate_read/fixtures",
        "roots": {},
    }
    for name in sorted(os.listdir(base)):
        root = os.path.join(base, name)
        if not os.path.isdir(root) or name in ("discovery", "archives"):
            continue
        live = live_roadmap(root)
        if live:
            result["roots"][name] = slugs_for(live)
    # The archive roots' LIVE files matter too — INV-3 compares baseline
    # against noarchivedir, and both must agree with this oracle.
    arch = os.path.join(base, "archives")
    if os.path.isdir(arch):
        for name in sorted(os.listdir(arch)):
            root = os.path.join(arch, name)
            live = live_roadmap(root) if os.path.isdir(root) else None
            if live:
                result["roots"]["archives/" + name] = slugs_for(live)
    json.dump(result, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")


main()
