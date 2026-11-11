#!/usr/bin/env python3
"""Rename bare-id specs (docs/specs/ANTS-NNNN.md) to <ID>-<topic>.md.

The topic comes from the spec's H1 ("# ANTS-NNNN — Title"). Every tracked
text file that names an old filename is rewritten to the new one, except
the files rendered from the roadmap store (ROADMAP.md, docs/roadmap/*.md):
those change through `roadmap_log op:"replace_text"`, or the next render
reverts them. --print-map writes the old->new pairs for that step.

Dry run by default: prints every `from -> to` and the reference count per
file. --apply does the `git mv` and the rewrites. ANTS-3755.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPECS = ROOT / "docs" / "specs"
BARE = re.compile(r"^(ANTS-\d+)\.md$")
STOP = {"a", "an", "the", "and", "or", "of", "for", "to", "in", "on", "with",
        "by", "via", "is", "are", "be", "as", "at", "from", "its", "it"}
MAX_WORDS = 5
MAX_CHARS = 40
# Rendered from the roadmap store; a direct edit is reverted on the next write.
RENDERED = re.compile(r"^(ROADMAP\.md|docs/roadmap/[^/]+\.md)$")
REF = re.compile(r"(?<![A-Za-z0-9_-])(ANTS-\d+\.md)")


def topic(title: str) -> str:
    words = re.findall(r"[a-z0-9]+", title.lower())
    words = [w for w in words if w not in STOP] or words
    out = []
    for w in words:
        if len(out) == MAX_WORDS or len("-".join(out + [w])) > MAX_CHARS:
            break
        out.append(w)
    return "-".join(out)


def build_map() -> dict:
    mapping = {}
    for f in sorted(SPECS.iterdir()):
        m = BARE.match(f.name)
        if not m:
            continue
        h1 = next((l for l in f.read_text(encoding="utf-8").splitlines()
                   if l.startswith("# ")), "")
        title = re.sub(r"^# " + re.escape(m.group(1)) + r"\s*[—:-]?\s*", "", h1)
        slug = topic(title)
        if not slug:
            sys.exit(f"no topic derivable from the H1 of {f.name}")
        new = f"{m.group(1)}-{slug}.md"
        if (SPECS / new).exists():
            sys.exit(f"{new} already exists")
        mapping[f.name] = new
    return mapping


def tracked_text_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True,
                         capture_output=True).stdout
    for rel in out.decode().split("\0"):
        if not rel or RENDERED.match(rel):
            continue
        p = ROOT / rel
        if not p.is_file() or p.is_symlink():
            continue
        try:
            yield rel, p.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--apply", action="store_true", help="rename and rewrite")
    ap.add_argument("--print-map", action="store_true",
                    help="print old<TAB>new pairs only")
    args = ap.parse_args()

    mapping = build_map()
    if args.print_map:
        for old, new in mapping.items():
            print(f"{old}\t{new}")
        return 0

    edits = {}
    for rel, text in tracked_text_files():
        hits = sum(1 for m in REF.finditer(text) if m.group(1) in mapping)
        if hits:
            new_text = REF.sub(lambda m: mapping.get(m.group(1), m.group(1)),
                               text)
            edits[rel] = (hits, new_text)

    for old, new in mapping.items():
        print(f"{old} -> {new}")
    for rel, (hits, _) in sorted(edits.items()):
        print(f"  rewrite {rel}: {hits}")
    print(f"{len(mapping)} renames, "
          f"{sum(h for h, _ in edits.values())} references in "
          f"{len(edits)} files" + ("" if args.apply else " (dry run)"))
    if not args.apply:
        return 0

    for rel, (_, text) in edits.items():
        (ROOT / rel).write_text(text, encoding="utf-8")
    for old, new in mapping.items():
        subprocess.run(["git", "mv", f"docs/specs/{old}", f"docs/specs/{new}"],
                       cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
