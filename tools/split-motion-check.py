#!/usr/bin/env python3
"""ANTS-1677 — check that a split commit only moved code.

Two checks. Each compares a commit with its parent.

INV-7 (the default) — motion identity. Every line removed from a file of the
class's source list reappears byte-identical exactly once in a file of that
list, and lines keep their relative order within each piece. Every other added
line is a kind the spec allows: a blank line, a piece's ordinal marker, an
#include, a preprocessor conditional repeating one the parent had, the
<stem>detail namespace lines, or a carved function's signature, braces or call.

INV-10 (--scrapes) — scrape windows. Derives every window a test under tests/
cuts from the class's text with substr, mid or left. Evaluates each window over
the class text at the parent and at the commit, and lists the windows whose
content differs. The commit must edit the call that cuts each listed window,
or the find/indexOf that locates its anchor. A window this tool cannot evaluate
is listed as unevaluated, for the commit message to name as checked by hand.

The class's files come from CMakeLists.txt's ANTS_<STEM>_SOURCES_REL list at
each revision. Before that list exists, the class is src/<stem>.cpp alone.

WHAT THE EVALUATOR UNDERSTANDS, AND WHAT IT DOES NOT. It evaluates literal
offsets and lengths, variables bound once, find/indexOf/rfind/lastIndexOf,
+ and -, std::min/std::max, and the srcgrep.h helpers slurpFunctionBody,
regionBetween, stripComments and mcpToolDescriptor. A variable assigned more
than once (a loop's cursor) or a value from any other function is unknown, and
its window is reported unevaluated, never guessed. Offsets are counted in
UTF-8 bytes whether the test holds a std::string or a QString. A QString window
over non-ASCII text therefore spans slightly different characters here than in
the test. Parent and commit are measured the same way, so a changed region
still shows as changed.

Usage:
    tools/split-motion-check.py --pre <rev> --post <rev> --stem <stem>
    tools/split-motion-check.py --scrapes --pre <rev> --post <rev> --stem <stem>

Exit status: 0 when the check holds, 1 when it does not, 2 on a usage or git
error.

Contract: docs/specs/ANTS-1677-large-file-decomposition.md § 2.2, INV-7, INV-10.
"""

from __future__ import annotations

import argparse
import bisect
import re
import subprocess
import sys
from collections import Counter
from difflib import SequenceMatcher

# stem -> (the srcgrep.h reader for the whole class, the single-file path
# macros § 2.3 deletes). A test naming either reads the class's text.
STEMS = {
    "auditdialog": ("slurpAuditDialog",
                    ("SRC_AUDIT_CPP", "SRC_AUDIT_CPP_PATH",
                     "SRC_AUDITDIALOG_CPP_PATH", "SRC_AUDITDIALOG_PATH")),
    "mainwindow": ("slurpMainWindow",
                   ("SRC_MAINWINDOW_CPP", "SRC_MAINWINDOW_CPP_PATH",
                    "SRC_MAINWINDOW_PATH", "MAINWINDOW_CPP")),
    "claudeintegration": ("slurpClaudeIntegration",
                          ("SRC_CLAUDE_INTEGRATION_CPP_PATH",)),
}

MAX_REPORTED = 40


class GitError(Exception):
    pass


# --------------------------------------------------------------------------
# git
# --------------------------------------------------------------------------

def git(*args: str) -> bytes:
    proc = subprocess.run(["git", *args], capture_output=True)
    if proc.returncode != 0:
        raise GitError("git %s: %s" % (" ".join(args),
                                       proc.stderr.decode(errors="replace").strip()))
    return proc.stdout


def show(rev: str, path: str) -> bytes | None:
    proc = subprocess.run(["git", "show", "%s:%s" % (rev, path)], capture_output=True)
    return proc.stdout if proc.returncode == 0 else None


def as_text(data: bytes) -> str:
    return data.decode("utf-8", errors="surrogateescape")


def source_list(rev: str, stem: str) -> list[str]:
    cmake = as_text(show(rev, "CMakeLists.txt") or b"")
    code = "\n".join(line.split("#", 1)[0] for line in cmake.split("\n"))
    m = re.search(r"set\(\s*ANTS_%s_SOURCES_REL\s+([^)]*)\)" % stem.upper(), code)
    return m.group(1).split() if m else ["src/%s.cpp" % stem]


HUNK = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")


def diff_lines(pre: str, post: str, path: str) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    """(removed, added) for one file, each a list of (line number, text)."""
    out = as_text(git("diff", "-U0", "--no-color", "--no-renames", "--no-ext-diff",
                      "--diff-algorithm=histogram", pre, post, "--", path))
    removed: list[tuple[int, str]] = []
    added: list[tuple[int, str]] = []
    old = new = 0
    in_hunk = False
    for line in out.split("\n"):
        m = HUNK.match(line)
        if m:
            old, new, in_hunk = int(m.group(1)), int(m.group(2)), True
        elif not in_hunk:
            continue
        elif line.startswith("-"):
            removed.append((old, line[1:]))
            old += 1
        elif line.startswith("+"):
            added.append((new, line[1:]))
            new += 1
    return removed, added


# --------------------------------------------------------------------------
# INV-7 — motion identity
# --------------------------------------------------------------------------

MARKER = re.compile(r"// ANTS-1677 (\w+) piece (\d+)/(\d+) — (\S.*)")
SNAKE = re.compile(r"\b[A-Za-z0-9]+_[A-Za-z0-9_]+\b")
BRACE = re.compile(r"\{|\}\s*;?\s*(//.*)?")
CARVED_SIGNATURE = re.compile(
    r"void\s+(?:MainWindow::)?(?:appendToolSchemas|registerMcpProviders)\w*\s*\([^;{}]*\)\s*\{?")
CARVED_CALL = re.compile(
    r"(?:appendToolSchemas|registerMcpProviders)\w*\s*\(\s*(?:tools)?\s*\)\s*;")


def normalise_directive(s: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"^#\s*", "#", s.strip()))


def allowed_added(text: str, stem: str, pre_conditionals: set[str]) -> bool:
    s = text.strip()
    if not s:
        return True
    if s.startswith("#"):
        word = re.match(r"#\s*(\w*)", s).group(1)
        if word in ("include", "else", "endif"):
            return True
        if word in ("if", "ifdef", "ifndef", "elif"):
            return normalise_directive(s) in pre_conditionals
        return False
    return (s in ("namespace %sdetail {" % stem, "using namespace %sdetail;" % stem)
            or bool(BRACE.fullmatch(s))
            or bool(CARVED_SIGNATURE.fullmatch(s))
            or bool(CARVED_CALL.fullmatch(s)))


def check_motion(pre: str, post: str, stem: str) -> int:
    pre_list, post_list = source_list(pre, stem), source_list(post, stem)
    entry = "src/%s.cpp" % stem
    problems: list[str] = []

    removed: dict[str, list[tuple[int, str]]] = {}
    added: dict[str, list[tuple[int, str]]] = {}
    for f in dict.fromkeys(pre_list + post_list):
        removed[f], added[f] = diff_lines(pre, post, f)

    pre_conditionals = set()
    for f in pre_list:
        for line in as_text(show(pre, f) or b"").split("\n"):
            if re.match(r"\s*#\s*(if|ifdef|ifndef|elif)\b", line):
                pre_conditionals.add(normalise_directive(line))

    # Markers: a piece's first line, numbered by its position in the list.
    for k, f in enumerate(post_list, start=1):
        first = as_text(show(post, f) or b"").split("\n", 1)[0]
        m = MARKER.fullmatch(first.strip())
        if f == entry:
            if m:
                problems.append("%s:1: entry 1 carries a piece marker" % f)
            continue
        if not m:
            problems.append("%s:1: a piece's first line is not its ordinal marker" % f)
        elif (m.group(1), int(m.group(2)), int(m.group(3))) != (stem, k, len(post_list)):
            problems.append("%s:1: marker says %s %s/%s, the list says %s %d/%d"
                            % (f, m.group(1), m.group(2), m.group(3), stem, k, len(post_list)))
        elif SNAKE.search(m.group(4)):
            problems.append("%s:1: marker concern carries a snake_case identifier" % f)

    # Removed lines, in list order. Blank lines and old markers need no match:
    # a cut may drop a separating blank line, and a marker is renumbered.
    pending = [(f, n, t) for f in pre_list for (n, t) in removed[f]
               if t.strip() and not MARKER.fullmatch(t.strip())]
    removed_total = len(pending)
    unmatched_added: list[tuple[str, int, str]] = []
    for f in post_list:
        cand = [(f, n, t) for (n, t) in added[f]
                if t.strip() and not MARKER.fullmatch(t.strip())]
        if not cand or not pending:
            unmatched_added += cand
            continue
        sm = SequenceMatcher(None, [p[2] for p in pending], [c[2] for c in cand], autojunk=False)
        used_r: set[int] = set()
        used_a: set[int] = set()
        for i, j, size in sm.get_matching_blocks():
            used_r.update(range(i, i + size))
            used_a.update(range(j, j + size))
        pending = [p for k, p in enumerate(pending) if k not in used_r]
        unmatched_added += [c for k, c in enumerate(cand) if k not in used_a]

    # A lone brace is too common for order to place it. Pair leftover braces
    # by text; a leftover statement is never paired, so a reorder still fails.
    spare_braces = Counter(t for (_, _, t) in unmatched_added if BRACE.fullmatch(t.strip()))
    still_pending = []
    for p in pending:
        if BRACE.fullmatch(p[2].strip()) and spare_braces[p[2]] > 0:
            spare_braces[p[2]] -= 1
        else:
            still_pending.append(p)

    for f, n, t in still_pending:
        problems.append("%s:%d: removed line appears nowhere else in the list: `%s`"
                        % (f, n, t.strip()))
    for f, n, t in unmatched_added:
        if not allowed_added(t, stem, pre_conditionals):
            problems.append("%s:%d: added line is neither motion nor an allowed kind: `%s`"
                            % (f, n, t.strip()))

    print("INV-7 motion: %s, %s..%s" % (stem, pre, post))
    print("  list at parent: %s" % " ".join(pre_list))
    print("  list at commit: %s" % " ".join(post_list))
    print("  removed lines needing a match: %d, unmatched: %d"
          % (removed_total, len(still_pending)))
    for p in problems[:MAX_REPORTED]:
        print("  FAIL " + p)
    if len(problems) > MAX_REPORTED:
        print("  … and %d more" % (len(problems) - MAX_REPORTED))
    print("  %s" % ("HOLDS" if not problems else "BROKEN"))
    return 0 if not problems else 1


# --------------------------------------------------------------------------
# C++ scanning
# --------------------------------------------------------------------------

RAW_OPEN = re.compile(r'R"([^()\\\s"]{0,16})\(')


def is_ident(c: str) -> bool:
    return c.isalnum() or c == "_"


def scan(src: str) -> tuple[str, str]:
    """(code, mask), each as long as `src`.

    code: comments replaced by spaces, newlines kept, so offsets and line
          numbers match `src`.
    mask: code with every literal's contents replaced by 'x', so a bracket
          or semicolon inside a literal is not read as code.
    """
    n = len(src)
    code = list(src)
    mask = list(src)
    i = 0
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                code[k] = mask[k] = " "
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if src[k] != "\n":
                    code[k] = mask[k] = " "
            i = j
        elif (c == "R" and nxt == '"'
              and (i == 0 or not is_ident(src[i - 1])
                   or re.search(r"(?:^|\W)(?:u8|u|U|L)$", src[max(0, i - 3):i]))
              and RAW_OPEN.match(src, i)):
            m = RAW_OPEN.match(src, i)
            close = ")" + m.group(1) + '"'
            j = src.find(close, m.end())
            j = n if j < 0 else j
            for k in range(m.end(), j):
                if src[k] != "\n":
                    mask[k] = "x"
            i = min(n, j + len(close))
        elif c == '"' or (c == "'" and not (i > 0 and src[i - 1].isalnum() and nxt.isalnum())):
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                mask[k] = "x"
            i = j + 1
        else:
            i += 1
    return "".join(code), "".join(mask)


def matching(mask: str, open_at: int) -> int:
    """Offset of the bracket closing the one at `open_at`, or -1."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    opener, closer = mask[open_at], pairs[mask[open_at]]
    depth = 0
    for k in range(open_at, len(mask)):
        if mask[k] == opener:
            depth += 1
        elif mask[k] == closer:
            depth -= 1
            if depth == 0:
                return k
    return -1


def function_blocks(mask: str) -> list[tuple[int, int, str]]:
    """(open, close, name) of every brace block whose enclosing blocks are all
    namespaces — functions, TEST bodies, file-scope structs."""
    blocks = []
    stack: list[tuple[str, int, str]] = []
    boundary = 0
    for i, c in enumerate(mask):
        if c == "{":
            head = mask[boundary:i]
            if re.search(r"\bnamespace\b[\w\s:]*$", head) or re.search(r'\bextern\s+"x*"\s*$', head):
                stack.append(("ns", i, ""))
            elif all(kind == "ns" for kind, _, _ in stack):
                m = re.search(r"(\w+)\s*\([^{};]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{};]*)?$", head)
                stack.append(("fn", i, m.group(1) if m else ""))
            else:
                stack.append(("blk", i, ""))
            boundary = i + 1
        elif c == "}":
            if stack:
                kind, start, name = stack.pop()
                if kind == "fn":
                    blocks.append((start, i, name))
            boundary = i + 1
        elif c == ";":
            boundary = i + 1
    return blocks


class LineIndex:
    def __init__(self, text: str):
        self.starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def line(self, offset: int) -> int:
        return bisect.bisect_right(self.starts, offset)


# --------------------------------------------------------------------------
# Bindings: name = expression, within one block
# --------------------------------------------------------------------------

ASSIGN = re.compile(r"(?<![\w.>:])([A-Za-z_]\w*)\s*(=(?!=)|(?:[-+*/%&|^]|<<|>>)=|\+\+|--)")
PREFIX_STEP = re.compile(r"(?:\+\+|--)\s*([A-Za-z_]\w*)")
DIRECT_INIT = re.compile(
    r"\b(?:std::string|std::string_view|QString|QByteArray|QStringView|auto)\s*&?\s+([A-Za-z_]\w*)\s*[({]")


class Binding:
    def __init__(self, start: int, end: int, first_line: int, last_line: int):
        self.start, self.end = start, end
        self.first_line, self.last_line = first_line, last_line


def expression_end(mask: str, start: int, limit: int) -> int:
    depth = 0
    k = start
    while k < limit:
        c = mask[k]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return k
            depth -= 1
        elif depth == 0 and c in ";,":
            return k
        k += 1
    return limit


def bindings_in(code: str, mask: str, start: int, end: int, lines: LineIndex,
                skip: list[tuple[int, int]] = ()) -> dict[str, list[Binding | None]]:
    """name -> its bindings in [start, end). A None entry is an assignment
    this tool will not evaluate (compound, increment), which makes the name
    unknown."""
    found: dict[str, list[Binding | None]] = {}

    def inside_skip(off: int) -> bool:
        return any(s <= off <= e for s, e in skip)

    for m in ASSIGN.finditer(mask, start, end):
        if inside_skip(m.start()):
            continue
        name, op = m.group(1), m.group(2)
        if op == "=":
            rhs_end = expression_end(mask, m.end(), end)
            found.setdefault(name, []).append(
                Binding(m.end(), rhs_end, lines.line(m.start()), lines.line(rhs_end)))
        else:
            found.setdefault(name, []).append(None)
    for m in PREFIX_STEP.finditer(mask, start, end):
        if not inside_skip(m.start()):
            found.setdefault(m.group(1), []).append(None)
    for m in DIRECT_INIT.finditer(mask, start, end):
        if inside_skip(m.start()):
            continue
        close = matching(mask, m.end() - 1)
        if close < 0:
            continue
        found.setdefault(m.group(1), []).append(
            Binding(m.end(), close, lines.line(m.start()), lines.line(close)))
    return found


# --------------------------------------------------------------------------
# Expressions
# --------------------------------------------------------------------------

TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<raw>(?:u8|u|U|L)?R"(?P<d>[^()\\\s"]{0,16})\((?P<rawbody>.*?)\)(?P=d)")
  | (?P<str>(?:u8|u|U|L)?"(?P<strbody>(?:[^"\\\n]|\\.)*)")
  | (?P<chr>'(?P<chrbody>(?:[^'\\\n]|\\.)+)')
  | (?P<num>\d+[uUlLzZ]*)
  | (?P<id>[A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*)
  | (?P<op>->|::|==|!=|<=|>=|[().,+\-<>*?:\[\]&!=/%{}|^~;])
""", re.X | re.S)

ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'",
           "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}


def unescape(body: str) -> bytes:
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            e = body[i + 1]
            if e == "x":
                m = re.match(r"[0-9A-Fa-f]+", body[i + 2:])
                if m:
                    out.append(chr(int(m.group(0), 16)))
                    i += 2 + len(m.group(0))
                    continue
            out.append(ESCAPES.get(e, e))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out).encode("utf-8", errors="surrogateescape")


class Unknown(Exception):
    pass


class Npos:
    """std::string::npos, or -1 from indexOf: an anchor that was not found."""


NPOS = Npos()
MISSING = b"\0<window start past the text>\0"

IDENTITY_CALLS = {
    "QStringLiteral", "QLatin1String", "QLatin1StringView", "QByteArrayLiteral",
    "std::string", "std::string_view", "QString", "QByteArray", "QStringView",
    "QString::fromUtf8", "QString::fromLatin1", "QString::fromStdString",
    "QString::fromLocal8Bit", "QByteArray::fromStdString",
}
IDENTITY_METHODS = {"toStdString", "toUtf8", "constData", "data", "c_str",
                    "toLatin1", "toLocal8Bit"}


def tokenize(text: str) -> list[tuple[str, re.Match]]:
    toks = []
    pos = 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise Unknown("cannot read `%s`" % text[pos:pos + 20])
        if m.lastgroup != "ws":
            kind = "id" if m.group("id") else ("raw" if m.group("raw") else m.lastgroup)
            if kind in ("d", "rawbody"):
                kind = "raw"
            elif kind in ("strbody",):
                kind = "str"
            elif kind in ("chrbody",):
                kind = "chr"
            toks.append((kind, m))
        pos = m.end()
    return toks


class Parser:
    def __init__(self, text: str):
        self.text = text
        self.toks = tokenize(text)
        self.i = 0

    def peek(self, value: str | None = None) -> bool:
        if self.i >= len(self.toks):
            return False
        kind, m = self.toks[self.i]
        return value is None or (kind == "op" and m.group(0) == value)

    def take(self, value: str) -> None:
        if not self.peek(value):
            raise Unknown("expected `%s` in `%s`" % (value, self.text.strip()))
        self.i += 1

    def parse(self):
        node = self.ternary()
        if self.i != len(self.toks):
            raise Unknown("unsupported expression `%s`" % self.text.strip())
        return node

    def ternary(self):
        cond = self.comparison()
        if self.peek("?"):
            self.i += 1
            yes = self.ternary()
            self.take(":")
            no = self.ternary()
            return ("?:", cond, yes, no)
        return cond

    def comparison(self):
        node = self.additive()
        for op in ("==", "!=", "<=", ">=", "<", ">"):
            if self.peek(op):
                self.i += 1
                return ("cmp", op, node, self.additive())
        return node

    def additive(self):
        node = self.postfix()
        while self.peek("+") or self.peek("-"):
            op = self.toks[self.i][1].group(0)
            self.i += 1
            node = (op, node, self.postfix())
        return node

    def args(self, closer: str):
        out = []
        if self.peek(closer):
            self.i += 1
            return out
        while True:
            out.append(self.ternary())
            if self.peek(","):
                self.i += 1
                continue
            self.take(closer)
            return out

    def postfix(self):
        node = self.primary()
        while self.peek("."):
            self.i += 1
            if self.i >= len(self.toks) or self.toks[self.i][0] != "id":
                raise Unknown("unsupported member access in `%s`" % self.text.strip())
            name = self.toks[self.i][1].group(0)
            self.i += 1
            self.take("(")
            node = ("method", node, name, self.args(")"))
        if self.peek("->") or self.peek("["):
            raise Unknown("unsupported operator in `%s`" % self.text.strip())
        return node

    def primary(self):
        if self.i >= len(self.toks):
            raise Unknown("truncated expression `%s`" % self.text.strip())
        kind, m = self.toks[self.i]
        if kind == "num":
            self.i += 1
            return ("int", int(re.match(r"\d+", m.group(0)).group(0)))
        if kind in ("str", "raw"):
            data = b""
            while self.i < len(self.toks) and self.toks[self.i][0] in ("str", "raw"):
                k, mm = self.toks[self.i]
                data += (mm.group("rawbody").encode("utf-8", errors="surrogateescape")
                         if k == "raw" else unescape(mm.group("strbody")))
                self.i += 1
            return ("bytes", data)
        if kind == "chr":
            self.i += 1
            return ("bytes", unescape(m.group("chrbody")))
        if kind == "op" and m.group(0) == "(":
            self.i += 1
            node = self.ternary()
            self.take(")")
            return node
        if kind == "op" and m.group(0) == "-":
            self.i += 1
            return ("-", ("int", 0), self.postfix())
        if kind == "id":
            name = re.sub(r"\s+", "", m.group(0))
            start = m.start()
            self.i += 1
            if name in ("static_cast", "qsizetype", "std::size_t", "size_t", "int") and self.peek("<"):
                depth = 0
                while self.i < len(self.toks):
                    op = self.toks[self.i][1].group(0) if self.toks[self.i][0] == "op" else ""
                    self.i += 1
                    if op == "<":
                        depth += 1
                    elif op == ">":
                        depth -= 1
                        if depth == 0:
                            break
                self.take("(")
                inner = self.ternary()
                self.take(")")
                return inner
            for opener, closer in (("(", ")"), ("{", "}")):
                if self.peek(opener):
                    self.i += 1
                    args = self.args(closer)
                    end = self.toks[self.i - 1][1].end()
                    return ("call", name, args, self.text[start:end])
            return ("var", name)
        raise Unknown("unsupported token `%s`" % m.group(0))


# srcgrep.h helpers, byte for byte.

def py_slurp_function_body(src: bytes, anchor: bytes) -> bytes:
    sig = src.find(anchor)
    if sig < 0:
        return b""
    open_brace = src.find(b"{", sig)
    if open_brace < 0:
        return b""
    depth = 0
    n = len(src)
    i = open_brace
    while i < n:
        c = src[i:i + 1]
        if c == b'"' or c == b"'":
            i += 1
            while i < n and src[i:i + 1] != c:
                if src[i:i + 1] == b"\\" and i + 1 < n:
                    i += 1
                i += 1
        elif c == b"/" and src[i + 1:i + 2] == b"/":
            i += 2
            while i < n and src[i:i + 1] != b"\n":
                i += 1
        elif c == b"/" and src[i + 1:i + 2] == b"*":
            i += 2
            while i + 1 < n and not (src[i:i + 1] == b"*" and src[i + 1:i + 2] == b"/"):
                i += 1
            i += 1
        elif c == b"{":
            depth += 1
        elif c == b"}":
            depth -= 1
            if depth == 0:
                return src[open_brace:i + 1]
        i += 1
    return b""


def py_region_between(text: bytes, start: bytes, end: bytes) -> bytes:
    s = text.find(start)
    if s < 0:
        return b""
    e = text.find(end, s + len(start))
    return b"" if e < 0 else text[s:e]


def py_strip_comments(src: bytes) -> bytes:
    out = bytearray()
    n = len(src)
    i = 0
    while i < n:
        c = src[i:i + 1]
        if c == b'"' or c == b"'":
            out += c
            i += 1
            while i < n and src[i:i + 1] != c:
                if src[i:i + 1] == b"\\" and i + 1 < n:
                    out += src[i:i + 1]
                    i += 1
                out += src[i:i + 1]
                i += 1
            if i < n:
                out += src[i:i + 1]
        elif c == b"/" and src[i + 1:i + 2] == b"/":
            i += 2
            while i < n and src[i:i + 1] != b"\n":
                i += 1
            out += b"\n"
        elif c == b"/" and src[i + 1:i + 2] == b"*":
            i += 2
            while i + 1 < n and not (src[i:i + 1] == b"*" and src[i + 1:i + 2] == b"/"):
                i += 1
            i += 1
            out += b" "
        else:
            out += c
        i += 1
    return bytes(out)


def py_mcp_tool_descriptor(src: bytes, tool: bytes) -> bytes:
    name_assign = b'["name"] = "'
    anchor = name_assign + tool + b'"'
    pos = src.find(anchor)
    if pos < 0:
        return b""
    nxt = src.find(name_assign, pos + len(anchor))
    return src[pos:] if nxt < 0 else src[pos:nxt]


class Evaluator:
    """Evaluates expressions of one test file's block against one revision's
    class text."""

    def __init__(self, stem: str, code: str, scope: list[dict[str, list[Binding | None]]],
                 class_text: bytes, entry_text: bytes):
        self.slurp, self.macros = STEMS[stem]
        self.stem = stem
        self.code = code
        self.scope = scope
        self.class_text = class_text
        self.entry_text = entry_text
        self.used: set[int] = set()
        self.active: set[str] = set()

    def reads_class(self, raw: str) -> str | None:
        if re.search(r"\b%s\s*\(\s*\)" % self.slurp, raw):
            return "class"
        if (re.search(r"\b(?:%s)\b" % "|".join(self.macros), raw)
                or re.search(r'src/%s\.cpp"' % self.stem, raw)):
            return "entry"
        return None

    def lookup(self, name: str) -> Binding:
        for level in self.scope:
            if name in level:
                entries = level[name]
                if len(entries) != 1 or entries[0] is None:
                    raise Unknown("`%s` is assigned more than once" % name)
                return entries[0]
        raise Unknown("`%s` is not bound in this scope" % name)

    def value(self, node):
        kind = node[0]
        if kind == "int":
            return node[1]
        if kind == "bytes":
            return node[1]
        if kind == "var":
            if node[1] in ("std::string::npos", "npos", "QString::npos"):
                return NPOS
            if node[1] in self.active:
                raise Unknown("`%s` refers to itself" % node[1])
            b = self.lookup(node[1])
            self.used.update(range(b.first_line, b.last_line + 1))
            self.active.add(node[1])
            try:
                return self.value(Parser(self.code[b.start:b.end]).parse())
            finally:
                self.active.discard(node[1])
        if kind in ("+", "-"):
            a, b = self.value(node[1]), self.value(node[2])
            if a is NPOS or b is NPOS:
                return NPOS
            if isinstance(a, int) and isinstance(b, int):
                return a + b if kind == "+" else a - b
            if kind == "+" and isinstance(a, bytes) and isinstance(b, bytes):
                return a + b
            raise Unknown("unsupported arithmetic")
        if kind == "cmp":
            op, a, b = node[1], self.value(node[2]), self.value(node[3])
            if a is NPOS or b is NPOS:
                # npos only compares by equality: std::string reports a miss as
                # npos and QString as -1, so an ordering here would guess.
                if op in ("==", "!="):
                    return (a is b) if op == "==" else (a is not b)
                raise Unknown("an ordering comparison against a missing anchor")
            if isinstance(a, int) and isinstance(b, int):
                return {"==": a == b, "!=": a != b, "<=": a <= b, ">=": a >= b,
                        "<": a < b, ">": a > b}[op]
            raise Unknown("unsupported comparison")
        if kind == "?:":
            cond = self.value(node[1])
            if not isinstance(cond, bool):
                raise Unknown("a condition that is not a comparison")
            return self.value(node[2] if cond else node[3])
        if kind == "call":
            return self.call(node[1], node[2], node[3])
        if kind == "method":
            return self.method(node[1], node[2], node[3])
        raise Unknown("unsupported node")

    def call(self, name: str, args, raw: str):
        reads = self.reads_class(raw)
        short = name[len("ants_test::"):] if name.startswith("ants_test::") else name
        if reads == "class" and short == self.slurp:
            return self.class_text
        if short == "slurpFunctionBody" and len(args) == 2:
            return py_slurp_function_body(self.text_arg(args[0]), self.text_arg(args[1]))
        if short == "regionBetween" and len(args) == 3:
            return py_region_between(*(self.text_arg(a) for a in args))
        if short == "stripComments" and len(args) == 1:
            return py_strip_comments(self.text_arg(args[0]))
        if short == "mcpToolDescriptor" and len(args) == 2:
            return py_mcp_tool_descriptor(self.text_arg(args[0]), self.text_arg(args[1]))
        if short in IDENTITY_CALLS and len(args) == 1:
            return self.value(args[0])
        if short in ("std::min", "std::max", "qMin", "qMax") and len(args) == 2:
            a, b = self.value(args[0]), self.value(args[1])
            if isinstance(a, int) and isinstance(b, int):
                return min(a, b) if short in ("std::min", "qMin") else max(a, b)
            raise Unknown("unsupported %s arguments" % short)
        if reads is not None:
            # A reader: slurpFile(MACRO), readFile(QStringLiteral(MACRO)), …
            return self.class_text if reads == "class" else self.entry_text
        raise Unknown("`%s()` is not a function this tool evaluates" % name)

    def text_arg(self, node) -> bytes:
        v = self.value(node)
        if not isinstance(v, bytes):
            raise Unknown("expected text")
        return v

    def int_arg(self, node):
        v = self.value(node)
        if v is NPOS or isinstance(v, int):
            return v
        raise Unknown("expected an offset")

    def method(self, recv_node, name: str, args):
        if name in IDENTITY_METHODS and not args:
            return self.value(recv_node)
        recv = self.text_arg(recv_node)
        if name in ("size", "length") and not args:
            return len(recv)
        if name in ("find", "indexOf", "rfind", "lastIndexOf") and 1 <= len(args) <= 2:
            needle = self.text_arg(args[0])
            start = self.int_arg(args[1]) if len(args) == 2 else None
            if start is NPOS:
                return NPOS
            if name in ("find", "indexOf"):
                at = recv.find(needle, start or 0)
            else:
                at = recv.rfind(needle, 0, len(recv) if start is None else start + len(needle))
            return NPOS if at < 0 else at
        if name in ("substr", "mid") and 1 <= len(args) <= 2:
            pos = self.int_arg(args[0])
            count = self.int_arg(args[1]) if len(args) == 2 else NPOS
            if pos is NPOS or pos < 0 or pos > len(recv):
                return MISSING
            return recv[pos:] if count is NPOS or count < 0 else recv[pos:pos + count]
        if name == "left" and len(args) == 1:
            count = self.int_arg(args[0])
            return recv if count is NPOS or count < 0 else recv[:count]
        raise Unknown("`.%s()` is not a method this tool evaluates" % name)


# --------------------------------------------------------------------------
# INV-10 — scrape windows
# --------------------------------------------------------------------------

WINDOW_CALL = re.compile(r"\.\s*(substr|mid|left)\s*\(")


def receiver_start(mask: str, dot: int) -> int:
    """Start of the expression a `.method(` at `dot` is called on."""
    i = dot
    while True:
        j = i
        while j > 0 and mask[j - 1].isspace():
            j -= 1
        if j > 0 and mask[j - 1] == ")":
            depth = 0
            k = j - 1
            while k >= 0:
                if mask[k] == ")":
                    depth += 1
                elif mask[k] == "(":
                    depth -= 1
                    if depth == 0:
                        break
                k -= 1
            j = k
            while j > 0 and mask[j - 1].isspace():
                j -= 1
        while j > 0 and (is_ident(mask[j - 1]) or mask[j - 1] == ":"):
            j -= 1
        if j == i:
            return i
        k = j
        while k > 0 and mask[k - 1].isspace():
            k -= 1
        if k > 0 and mask[k - 1] == ".":
            i = k - 1
            continue
        return j


class Site:
    def __init__(self, path: str, first_line: int, last_line: int, text: str):
        self.path, self.first_line, self.last_line, self.text = path, first_line, last_line, text


def class_texts(rev: str, stem: str) -> tuple[bytes, bytes]:
    files = source_list(rev, stem)
    whole = b"\n".join(show(rev, f) or b"" for f in files)
    return whole, show(rev, "src/%s.cpp" % stem) or b""


def check_scrapes(pre: str, post: str, stem: str) -> int:
    slurp, macros = STEMS[stem]
    reader = re.compile(r"\b%s\s*\(|\b(?:%s)\b|src/%s\.cpp\"" % (slurp, "|".join(macros), stem))
    pre_texts, post_texts = class_texts(pre, stem), class_texts(post, stem)

    paths = [p for p in as_text(git("ls-tree", "-r", "--name-only", pre, "--", "tests")).split("\n")
             if p.endswith(".cpp")]
    readers = 0
    evaluated = 0
    changed_edited: list[Site] = []
    changed_unedited: list[Site] = []
    unevaluated: list[tuple[Site, str]] = []

    for path in paths:
        src = as_text(show(pre, path) or b"")
        if not reader.search(src):
            continue
        readers += 1
        code, mask = scan(src)
        lines = LineIndex(src)
        blocks = function_blocks(mask)
        file_scope = bindings_in(code, mask, 0, len(mask), lines, [(s, e) for s, e, _ in blocks])
        removed_lines = None

        # Callables whose body cuts windows: file functions and lambdas bound
        # to a name. Such a callable handed class text cuts windows this tool
        # cannot evaluate from the call site.
        callables: dict[str, list[tuple[int, int]]] = {}
        for s, e, name in blocks:
            if name and WINDOW_CALL.search(mask, s, e):
                callables.setdefault(name, []).append((s, e))

        for s, e, _ in blocks:
            block_scope = bindings_in(code, mask, s, e, lines)
            for name, entries in block_scope.items():
                for b in entries:
                    if b and code[b.start:b.end].lstrip().startswith("[") \
                            and WINDOW_CALL.search(mask, b.start, b.end):
                        callables.setdefault(name, []).append((b.start, b.end))
            scope = [block_scope, file_scope]

            tainted: set[str] = set()
            grew = True
            while grew:
                grew = False
                for level in scope:
                    for name, entries in level.items():
                        if name in tainted:
                            continue
                        for b in entries:
                            if b is None:
                                continue
                            rhs = code[b.start:b.end]
                            if reader.search(rhs) or any(re.search(r"\b%s\b" % re.escape(t), rhs)
                                                         for t in tainted):
                                tainted.add(name)
                                grew = True
                                break

            def touches_class(text: str) -> bool:
                return bool(reader.search(text)) or any(
                    re.search(r"\b%s\b" % re.escape(t), text) for t in tainted)

            for m in WINDOW_CALL.finditer(mask, s, e):
                start = receiver_start(mask, m.start())
                if start == m.start():
                    continue
                close = matching(mask, m.end() - 1)
                if close < 0:
                    continue
                if not touches_class(code[start:m.start()]):
                    continue
                site = Site(path, lines.line(start), lines.line(close), " ".join(code[start:close + 1].split()))
                try:
                    parsed = Parser(code[start:close + 1]).parse()
                    before = Evaluator(stem, code, scope, *pre_texts)
                    region_pre = before.value(parsed)
                    after = Evaluator(stem, code, scope, *post_texts)
                    region_post = after.value(parsed)
                except Unknown as why:
                    unevaluated.append((site, str(why)))
                    continue
                evaluated += 1
                if region_pre == region_post:
                    continue
                if removed_lines is None:
                    removed_lines = {n for n, _ in diff_lines(pre, post, path)[0]}
                touched = set(range(site.first_line, site.last_line + 1)) | before.used
                (changed_edited if touched & removed_lines else changed_unedited).append(site)

            for name, spans in callables.items():
                for cm in re.finditer(r"\b%s\s*\(" % re.escape(name), mask[s:e]):
                    at = s + cm.end() - 1
                    close = matching(mask, at)
                    if close < 0 or not touches_class(code[at:close + 1]):
                        continue
                    for hs, he in spans:
                        if hs <= at <= he:
                            continue
                        for wm in WINDOW_CALL.finditer(mask, hs, he):
                            site = Site(path, lines.line(wm.start()), lines.line(wm.start()),
                                        " ".join(code[receiver_start(mask, wm.start()):
                                                      matching(mask, wm.end() - 1) + 1].split()))
                            unevaluated.append((site, "inside `%s`, called with class text at line %d"
                                                % (name, lines.line(at))))

    seen = set()
    unique_unevaluated = []
    for site, why in unevaluated:
        key = (site.path, site.first_line, site.text)
        if key not in seen:
            seen.add(key)
            unique_unevaluated.append((site, why))

    print("INV-10 scrape windows: %s, %s..%s" % (stem, pre, post))
    print("  test files reading the class: %d" % readers)
    print("  windows evaluated: %d, content changed: %d, unevaluated: %d"
          % (evaluated, len(changed_edited) + len(changed_unedited), len(unique_unevaluated)))
    for site in changed_unedited:
        print("  FAIL changed, call not edited  %s:%d  `%s`" % (site.path, site.first_line, site.text))
    for site in changed_edited:
        print("  ok   changed, call edited      %s:%d  `%s`" % (site.path, site.first_line, site.text))
    for site, why in unique_unevaluated:
        print("  UNEVALUATED  %s:%d  `%s`  — %s" % (site.path, site.first_line, site.text, why))
    if readers == 0:
        print("  FAIL no test file under tests/ reads the %s class — the derivation is blind" % stem)
        return 1
    print("  %s" % ("HOLDS" if not changed_unedited else "BROKEN"))
    return 0 if not changed_unedited else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pre", required=True, help="the parent revision")
    ap.add_argument("--post", required=True, help="the split commit")
    ap.add_argument("--stem", required=True, choices=sorted(STEMS))
    ap.add_argument("--scrapes", action="store_true", help="run INV-10 instead of INV-7")
    args = ap.parse_args()
    try:
        for rev in (args.pre, args.post):
            git("rev-parse", "--verify", "--quiet", rev + "^{commit}")
        if args.scrapes:
            return check_scrapes(args.pre, args.post, args.stem)
        return check_motion(args.pre, args.post, args.stem)
    except GitError as err:
        print("split-motion-check: %s" % err, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
