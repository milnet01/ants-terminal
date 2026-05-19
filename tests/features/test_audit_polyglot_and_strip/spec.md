# test_audit_partition — polyglot framework detection + Python literal strip

Combined spec for ANTS-1623 (polyglot framework detection) and
ANTS-1627 (Python literal/comment stripping in pre-pass).

## ANTS-1623 — invariants

- **INV-A1:** When `scope` is `path:<sub>` and `<sub>` contains a
  framework signal file, the framework probe matches `<sub>` first
  rather than the project root. Concretely: a project with
  `pyproject.toml` at root and `frontend/package.json` at
  `frontend/` returns `framework: "jest"` for
  `scope: "path:frontend"`.
- **INV-A2:** When `scope` is `path:<sub>` but `<sub>` has no signal
  file, the probe falls back to the project root (existing behaviour
  for sub-path filters inside single-framework projects).
- **INV-A3:** `additional_frameworks[]` lists every other framework
  whose signal file was detected at the project root OR at a top-
  level subdir, as `{name, root_rel}` pairs. Empty on single-
  framework projects.
- **INV-A4:** `additional_frameworks[]` excludes the primary
  framework even if its signal file is also visible at a subdir.
- **INV-A5:** The build-tree exclusion list (`build*`, `node_modules`,
  `.venv`, `_deps`, etc.) applies to the subdir scan so polyglot
  probing does not match `build/.../package.json` style fixtures.

## ANTS-1627 — invariants

- **INV-B1:** A `sleep_call` pattern (`\btime\.sleep\(`) inside a
  Python `'...'` or `"..."` string literal does NOT fire on a `.py`
  file. Covers the `test_no_bare_time_sleep_in_jobs` negative-grep
  shape where a test holds the needle as a string and asserts it is
  not present in production.
- **INV-B2:** A `sleep_call` pattern inside a Python triple-quoted
  string (`"""..."""` / `'''...'''`) does NOT fire. Covers module
  docstrings + multi-line fixture strings.
- **INV-B3:** A `sleep_call` pattern inside a Python `#` comment
  does NOT fire. Covers bug-description comments.
- **INV-B4:** A `hardcoded_password` pattern inside a Python string
  literal does NOT fire. Covers the `'password: admin'` negative-grep
  shape.
- **INV-B5:** A real `time.sleep(0.1)` call outside any string /
  comment context DOES still fire — strip is value-preserving for
  executable code.
- **INV-B6:** Stripping preserves line numbers exactly so a real
  finding at line N reports line N.
- **INV-B7:** String prefixes (`r"..."`, `b"..."`, `f"..."`,
  `rb"..."`, etc.) are handled — content inside such strings is
  stripped just like an unprefixed string.
