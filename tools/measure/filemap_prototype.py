#!/usr/bin/env python3
"""Generate a file map: every tracked file, grouped, with a purpose line quoted
from a checkable source or the literal words "no stated purpose".

PROTOTYPE (ANTS-5314) — NOT ADOPTED. Measured and it did not win: see
docs/reviews/ANTS-5314-file-map-token-test-2026-09-24.md. Kept so the test
can be re-run, e.g. on a project with no code index. Purpose lines are never
written by this script. Each
comes from one of these sources, and its tag says which:

  [header]  the file's own leading comment block (C/C++/GLSL, shell, CMake,
            YAML, Python `#` comments) or its module docstring
  [purpose] a `Purpose:` line in a markdown document
  [title]   a markdown document's H1 title
  [README]  the first sentence of a directory's README.md
  [module]  the file's module line in docs/subsystems.md
  (none)    "no stated purpose"

A directory whose files are too many to list, and alike in shape, collapses to
one line that SAYS it collapsed and how many files it holds. A reader can then
tell an omission from an absence.

The map records a digest of its own body. `--check` regenerates and fails on
any difference, so a committed copy is current by construction, and a reader
holding a copy can run `--check` to find out whether it still is.

Usage: filemap.py [--root DIR] [--out FILE] (--write | --check | --stdout)
       [--collapse N]
"""
import argparse
import collections
import hashlib
import os
import re
import subprocess
import sys

GENERATOR = "tools/measure/filemap_prototype.py v1"
C_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".glsl", ".comp",
         ".frag", ".vert", ".mm", ".m"}
HASH_EXT = {".sh", ".bash", ".cmake", ".yml", ".yaml", ".toml", ".py", ".txt",
            ".spec", ".conf", ".desktop", ""}
MAX_PURPOSE = 160


def tracked(root):
    out = subprocess.run(["git", "-C", root, "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    return sorted(p for p in out.decode().split("\0") if p)


def first_sentence(text):
    text = " ".join(text.split())
    # A leading roadmap id is provenance, not purpose.
    text = re.sub(r"^\(?[A-Z][A-Z0-9]+-\d+\)?\s*[—:–-]+\s*", "", text)
    m = re.search(r"(?<=[a-z0-9)`'\"])[.!?](?=\s|$)", text)
    if m:
        text = text[:m.end()]
    if len(text) > MAX_PURPOSE:
        text = text[:MAX_PURPOSE - 1].rstrip() + "…"
    return text


def read_head(path, limit=80):
    try:
        with open(path, errors="replace") as f:
            return [next(f).rstrip("\n") for _ in range(limit)]
    except StopIteration:
        pass
    except OSError:
        return []
    with open(path, errors="replace") as f:
        return [l.rstrip("\n") for l in f]


def c_header(lines):
    buf, in_block = [], False
    for raw in lines:
        l = raw.strip()
        if in_block:
            end = "*/" in l
            buf.append(l.split("*/")[0].lstrip("*").strip())
            if end:
                break
            continue
        if not l or l.startswith("#pragma") or l.startswith("#include"):
            if buf:
                break
            continue
        if l.startswith("//"):
            body = l.lstrip("/").strip()
            if re.match(r"(SPDX|Copyright|clang-format)", body):
                continue
            buf.append(body)
            continue
        if l.startswith("/*"):
            body = l[2:]
            if "*/" in body:
                buf.append(body.split("*/")[0].strip("* ").strip())
                break
            in_block = True
            buf.append(body.strip("* ").strip())
            continue
        break
    return " ".join(b for b in buf if b)


def hash_header(lines):
    buf = []
    for i, raw in enumerate(lines):
        l = raw.strip()
        if i == 0 and l.startswith("#!"):
            continue
        if l.startswith("#") and not l.startswith("#!"):
            body = l.lstrip("#").strip()
            if re.match(r"(SPDX|Copyright|-\*-|vim:|shellcheck )", body):
                continue
            if not body and buf:
                break
            buf.append(body)
            continue
        if not l and not buf:
            continue
        break
    return " ".join(b for b in buf if b)


def py_docstring(lines):
    text = "\n".join(lines)
    m = re.match(r'\s*(?:#![^\n]*\n)?(?:#[^\n]*\n|\s)*[rbu]?("""|\'\'\')(.*?)\1',
                 text, re.S)
    return m.group(2) if m else ""


def md_purpose(lines):
    for l in lines:
        m = re.match(r"^\*{0,2}Purpose:?\*{0,2}\s*(.+)", l.strip())
        if m:
            return m.group(1), "purpose"
    for l in lines:
        m = re.match(r"^#\s+(.+)", l)
        if m:
            return m.group(1), "title"
    return "", None


def readme_sentence(path):
    lines = read_head(path, 60)
    para, started = [], False
    for l in lines:
        s = l.strip()
        if s.startswith("#") or s.startswith("<!--"):
            if started:
                break
            continue
        if not s:
            if started:
                break
            continue
        if s.startswith(("|", "```", "- ", "* ", ">")):
            if started:
                break
            continue
        started = True
        para.append(s)
    return first_sentence(" ".join(para)) if para else ""


def module_lines(root):
    """docs/subsystems.md module bullets: `- \\`name\\` — summary`."""
    path = os.path.join(root, "docs", "subsystems.md")
    out, cur = {}, None
    if not os.path.exists(path):
        return out
    with open(path, errors="replace") as fh:
        lines = fh.readlines()
    for l in lines:
        m = re.match(r"^- `([A-Za-z0-9_]+)`[^—]*—\s*(.*)", l)
        if m:
            cur = m.group(1)
            out[cur] = m.group(2).strip()
        elif cur and l.startswith("  ") and l.strip():
            out[cur] += " " + l.strip()
        else:
            cur = None
    return {k: first_sentence(v) for k, v in out.items()}


def purpose_of(root, rel, modules):
    path = os.path.join(root, rel)
    base = os.path.basename(rel)
    stem, ext = os.path.splitext(base)
    ext = ext.lower()
    text, tag = "", None
    if ext in C_EXT:
        text, tag = c_header(read_head(path)), "header"
    elif ext == ".py":
        lines = read_head(path)
        text, tag = py_docstring(lines), "header"
        if not text:
            text = hash_header(lines)
    elif ext == ".md":
        text, tag = md_purpose(read_head(path, 40))
    elif ext in HASH_EXT or base in ("CMakeLists.txt", "Makefile", "PKGBUILD"):
        text, tag = hash_header(read_head(path)), "header"
    text = first_sentence(text) if text else ""
    # A one- to three-word comment is a section label ("# Build"), not a
    # statement of purpose; quoting it as one would read exactly like a real
    # purpose line.
    if len(re.findall(r"\w+", text)) < 4:
        text = ""
    if not text and rel.startswith("src/") and stem in modules:
        text, tag = modules[stem], "module"
    return (text, tag) if text else ("", None)


def shape(rel_dir, files):
    """A subdirectory's file names with its own name abstracted, so
    `foo/test_foo.cpp` and `bar/test_bar.cpp` share one shape."""
    name = os.path.basename(rel_dir)
    return tuple(sorted(f.replace(name, "<name>") for f in files))


def render(root, collapse):
    files = tracked(root)
    modules = module_lines(root)
    by_dir = collections.defaultdict(list)
    for f in files:
        by_dir[os.path.dirname(f)].append(os.path.basename(f))
    under = collections.Counter()
    for f in files:
        d = os.path.dirname(f)
        while True:
            under[d] += 1
            if not d:
                break
            d = os.path.dirname(d)
    children = collections.defaultdict(set)
    for d in list(by_dir):
        while d:
            children[os.path.dirname(d)].add(d)
            d = os.path.dirname(d)

    out, stated, collapsed_files = [], 0, 0

    def dir_purpose(d):
        rd = os.path.join(root, d, "README.md")
        if os.path.exists(rd):
            s = readme_sentence(rd)
            if s:
                return f"{s} [README]"
        return "no stated purpose"

    def emit_dir(d, depth):
        nonlocal stated, collapsed_files
        subs = sorted(children.get(d, ()))
        own = sorted(by_dir.get(d, []))
        label = (d + "/") if d else "(repository root)"
        # Collapse: a large directory whose subdirectories share one shape.
        if d and under[d] > collapse and subs:
            shapes = collections.Counter(
                shape(s, by_dir.get(s, [])) for s in subs
                if not children.get(s))
            if shapes:
                top, n = shapes.most_common(1)[0]
                if n >= 0.6 * len(subs):
                    collapsed_files += under[d]
                    pat = " + ".join(top) if top else "(empty)"
                    out.append(
                        f"{'  ' * depth}- `{label}` — **collapsed: {under[d]} files "
                        f"in {len(subs)} subdirectories; {n} of them hold exactly "
                        f"`{pat}`, the rest vary.** {dir_purpose(d)}")
                    if own:
                        out.append(f"{'  ' * (depth + 1)}- plus {len(own)} files "
                                   f"directly in `{label}`: "
                                   + ", ".join(f"`{f}`" for f in own[:12])
                                   + (" …" if len(own) > 12 else ""))
                    return
        if d and len(own) > collapse and not subs:
            exts = collections.Counter(os.path.splitext(f)[1] for f in own)
            ext, n = exts.most_common(1)[0]
            if n >= 0.8 * len(own):
                collapsed_files += len(own)
                pre = os.path.commonprefix(own)
                out.append(
                    f"{'  ' * depth}- `{label}` — **collapsed: {len(own)} files, "
                    f"{n} of them `{pre}*{ext}`.** {dir_purpose(d)}")
                return
        out.append(f"{'  ' * depth}- `{label}` — {dir_purpose(d)}" if d
                   else "## Files")
        # Pair same-stem siblings (foo.h + foo.cpp) into one entry.
        stems = collections.OrderedDict()
        for f in own:
            stems.setdefault(os.path.splitext(f)[0], []).append(f)
        for stem_, group in stems.items():
            rels = [os.path.join(d, g) for g in group]
            text, tag = "", None
            for r in sorted(rels, key=lambda r: not r.endswith((".h", ".hpp"))):
                text, tag = purpose_of(root, r, modules)
                if text:
                    break
            name = group[0] if len(group) == 1 else (
                stem_ + ".{" + ",".join(os.path.splitext(g)[1][1:] for g in group) + "}")
            ind = "  " * (depth + (1 if d else 0))
            if text:
                stated += len(group)
                out.append(f"{ind}- `{name}` — {text} [{tag}]")
            else:
                out.append(f"{ind}- `{name}` — no stated purpose")
        for s in subs:
            emit_dir(s, depth + (1 if d else 0))

    emit_dir("", 0)
    listed = len(files) - collapsed_files
    body = "\n".join(out) + "\n"
    head = (f"{len(files)} tracked files: {listed} listed, {collapsed_files} "
            f"inside collapsed directories. {stated} of the listed carry a "
            f"stated purpose; the rest say \"no stated purpose\".\n")
    return head + "\n" + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--out", default="FILEMAP.md")
    ap.add_argument("--collapse", type=int, default=40)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--stdout", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    body = render(root, a.collapse)
    digest = hashlib.sha256(body.encode()).hexdigest()[:16]
    doc = (f"# File map\n\n"
           f"<!-- filemap digest={digest} generator=\"{GENERATOR}\" "
           f"collapse={a.collapse} -->\n"
           f"Generated by `{GENERATOR.split()[0]}`; do not edit. Every purpose "
           f"is quoted from the tagged source, or says \"no stated purpose\". "
           f"Check this copy is current: `python3 tools/measure/filemap_prototype.py --check`.\n\n"
           + body)
    path = os.path.join(root, a.out)
    if a.stdout:
        sys.stdout.write(doc)
    elif a.write:
        with open(path, "w") as f:
            f.write(doc)
    else:
        try:
            with open(path) as f:
                cur = f.read()
        except OSError:
            print(f"filemap: {a.out} missing", file=sys.stderr)
            return 1
        if cur != doc:
            print(f"filemap: {a.out} is stale; run tools/filemap.py --write",
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
