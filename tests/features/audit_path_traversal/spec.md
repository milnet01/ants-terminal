# Feature: audit reject `../` path traversal from user-rule findings

## Problem

`AuditDialog` consumes findings from three distinct sources:

1. Built-in regex rule pack (trusted — ships with the binary).
2. `audit_rules.json` in the project (trusted by opt-in — see
   `audit_command_rule_trust`).
3. External tools (cppcheck, semgrep, osv-scanner, etc.) producing
   file:line output parsed into `Finding{file, line, ...}`.

Any of these can emit a `Finding::file` of the shape
`../../etc/passwd`. Pre-0.7.20, the dialog's enrichment machinery
resolved this with a naive string concatenation:

```cpp
const QString abs = QFileInfo(f.file).isAbsolute()
                  ? f.file
                  : (m_projectPath + "/" + f.file);
```

`abs` then fed:

- `readSnippet(abs, …)` — reads ±3 lines around `f.line` for the
  snippet-preview UI and AI-triage payload. `/etc/passwd`, the
  user's SSH private key, and any file the Ants UID can read become
  part of the on-screen snippet (and, worse, part of any AI-triage
  POST).
- `lineIsCode(abs, …)` — opens the file to classify the finding's
  line as code vs. comment/string. Same disclosure surface.
- `inlineSuppressed(f)` — reads the file looking for
  `// ants-audit: disable` markers. Same surface.
- The `dropIfContextContains` filter — reads ±N lines around the
  capture-group line looking for suppression substrings.

The AI-triage surface is the most acute — `readSnippet` output is
serialized into the JSON POST body to the configured OpenAI-
compatible endpoint. A malicious `audit_rules.json` in a cloned
project can exfiltrate arbitrary files the UID can read, wrapped in
an ordinary-looking "review this code snippet" prompt.

## External anchors

- [CWE-22 — Improper Limitation of a Pathname to a Restricted Directory ('Path Traversal')](https://cwe.mitre.org/data/definitions/22.html).
- [OWASP LLM06: Sensitive Information Disclosure](https://owasp.org/www-project-top-10-for-large-language-model-applications/):
  the AI-triage exfiltration shape is a textbook LLM06 vector — an
  untrusted input controls what bytes are forwarded to the LLM.
- 0.7.12 /indie-review Tier 1 finding: *"Audit user-glob path
  canonicalization. `auditdialog.cpp:2323` (globToRegex),
  `auditdialog.cpp:2221-2223, 2064-2066` (readSnippet / lineIsCode).
  Enforce `startsWith(m_projectPath)` after canonicalization — reject
  `../` traversal in user rules."*

## Contract

### Invariant 1 — `resolveProjectPath` rejects `../` escape

For any relative path containing `..` components that, after
`canonicalFilePath()` resolution, lands outside the canonical project
directory, `resolveProjectPath(p)` returns `QString()`.

### Invariant 2 — `resolveProjectPath` rejects symlink escape

If the project contains a symlink whose target points outside the
project, following it via
`resolveProjectPath("link/sensitive.txt")` returns `QString()`
even though the relative path has no literal `..` component.
Canonicalization dereferences symlinks; the resulting absolute path
must still be inside the project root.

### Invariant 3 — `resolveProjectPath` accepts in-project paths

A normal in-project path like `src/main.cpp` resolves to the
canonical absolute path under the project root, with no false
rejections.

### Invariant 4 — `resolveProjectPath` returns empty for non-existent

For a relative path that doesn't exist on disk at all,
`resolveProjectPath` returns `QString()`. This is conservative —
better to skip enrichment than to probe for the file's existence
(TOCTOU timing side channel).

### Invariant 5 — all known call sites use `resolveProjectPath`

The following five call sites in `src/auditdialog.cpp` must use
`resolveProjectPath(...)` to resolve `f.file` (or the regex-captured
`relPath`), not raw `m_projectPath + "/" + ...` concatenation:

1. `dropFindingsInCommentsOrStrings` — the comment/string classifier
   gate on retained findings.
2. `inlineSuppressed` — the ants-audit marker scan.
3. The enrichment pass that feeds `readSnippet` for the snippet
   preview.
4. The single-finding AI-triage snippet fallback.
5. The batch AI-triage snippet fallback.

Plus the `dropIfContextContains` regex-capture file read.

Asserted via source-grep: the regex `m_projectPath\s*\+\s*"/"\s*\+\s*f\.file`
must not appear anywhere in `auditdialog.cpp`.

## How this test anchors to reality

The test:

1. Builds a real temporary directory tree:
   ```
   <TempLocation>/audit-traversal-UUID/
   ├── project/
   │   ├── src/good.cpp        (in-project; I3 target)
   │   └── link                 (symlink → ../outside; I2 target)
   └── outside/
       └── sensitive.txt       (exfil target)
   ```
2. Constructs an `AuditDialog` rooted at `project/`.
3. Calls `resolveProjectPath()` with:
   - `src/good.cpp` → expects a canonical path starting with
     the project root.
   - `../outside/sensitive.txt` → expects `QString()`.
   - `link/sensitive.txt` → expects `QString()`.
   - `nonexistent/file.txt` → expects `QString()`.
4. Source-greps `src/auditdialog.cpp`:
   - `resolveProjectPath(` must appear ≥5 times (once per call site).
   - `m_projectPath + "/" + f.file` must not appear at all.

If any invariant drifts — someone inlines the old concat at a new
call site, `resolveProjectPath` loses its canonicalization step,
the symlink branch stops following — this test fires.

## Regression history

- **Introduced:** 0.6.22-ish when `readSnippet` enrichment landed.
  The same pattern showed up in `dropFindingsInCommentsOrStrings`,
  `inlineSuppressed`, and the AI-triage snippet pass as they were
  added.
- **Flagged:** 2026-04-23 /indie-review Tier 1 audit-subsystem
  reviewer.
- **Fixed:** 0.7.20 — `AuditDialog::resolveProjectPath` helper
  introduced; all five raw-concat call sites migrated to use it.
  `QFileInfo::canonicalFilePath()` handles both `../` resolution
  and symlink dereferencing in a single call.
