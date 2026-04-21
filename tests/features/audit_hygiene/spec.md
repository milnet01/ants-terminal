# Audit hygiene — external-tool calibration parsers

Origin: RetroDB audit-hygiene report 2026-04-21. A project that already
documents its accepted noise categories in `.semgrep.yml` +
`pyproject.toml` was getting ~1.3% signal rate because the audit tool was
ignoring those files and re-flagging everything the project had already
triaged.

The audit runner now splices project-local suppression lists into the
tool invocations:

- `.semgrep.yml` header: a comment block starting with "Excluded upstream
  rules" lists one rule ID per line (`#   rule.id.here`). Each is passed
  to semgrep as `--exclude-rule <id>`.
- `pyproject.toml` `[tool.ruff.lint.ignore]` (or `[tool.ruff] ignore`):
  any `S<nnn>` entry is an accepted bandit code. Each is mapped to
  `B<nnn>` and passed to bandit as `--skip B101,B104,…`.

## Invariants

1. **Semgrep empty input → empty list.** Missing `.semgrep.yml`, or one
   without the `Excluded upstream rules` marker, returns `[]`.
2. **Semgrep happy path.** Given a file whose header contains

       # Excluded upstream rules
       # -----------------------
       # These IDs are noise-only on this repo.
       #
       #   python.flask.security.audit.debug-enabled.debug-enabled
       #     Anchor: app.py:1238 is env-gated.
       #
       #   python.lang.security.insecure-hash-algorithms-md5.insecure-hash-algorithm-md5
       #     Anchor: API contract requirement.
       #
       rules: []

   the parser returns exactly the two rule IDs above, in encounter order,
   and stops at the first non-comment line (`rules: []`).
3. **Semgrep — prose and separator lines inside the block are ignored.**
   Lines like `# -----------------------`, `# These IDs are ...`, and
   `#` (empty comment) do NOT become rule IDs.
4. **Semgrep — dedup.** If the same rule ID is listed twice in the block,
   it appears once in the output.
5. **Bandit — missing `[tool.ruff.lint]` → empty.** If the TOML has no
   ruff section, the parser returns `[]`.
6. **Bandit — happy path.** Given `[tool.ruff.lint]` with `ignore = [...]`
   containing `S101`, `S104`, `S324`, mixed with non-S codes (`F401`,
   `E501`, `B007`), the parser returns `[B101, B104, B324]` — S-codes
   only, mapped 1:1 to B-codes, non-S entries dropped.
7. **Bandit — honors `[tool.ruff]` fallback.** When only `[tool.ruff]`
   exists (no `[tool.ruff.lint]`), the parser still reads the `ignore`
   array from it.
8. **Bandit — prefers `[tool.ruff.lint]` over `[tool.ruff]`.** When both
   sections exist, the `.lint` subsection's `ignore` wins (it's the
   newer canonical location per ruff's own docs).
9. **Bandit — section body stops at next header.** If a later
   `[tool.ruff.lint.per-file-ignores]` section appears, its `ignore`
   entries do NOT leak into the main list (sub-sections are treated as
   distinct).
10. **Bandit — `extend-ignore` is accepted as a synonym for `ignore`.**
    Ruff treats them equivalently.

## Acceptance

- `ctest -L fast -R audit_hygiene` passes.
- Run against the real RetroDB config files produces ≥13 semgrep rules
  and ≥13 bandit B-codes.

## Why this test exists

These parsers are regex-based and handle a loose (human-readable) input
format. When ruff or semgrep change their on-disk format, or when a
project uses an unusual layout (TOML single-line `ignore = []`, trailing
commas, etc.), the regex needs adjustment. This test pins the expected
behavior so the parser can be tightened without regressing.

A prior iteration of the bandit parser had an off-by-one bug where
`globalMatch(text, sectionStart + 1)` re-matched the same header line
because Qt's `capturedStart()` for a `^`-anchored match points *before*
the leading whitespace. Test #6 would have caught that at commit time.
