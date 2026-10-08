---
paths:
  - "src/**"
---

## Key design decisions (non-obvious)

- Custom VT100 parser, no pyte/libvterm. Qt6 is the only runtime dep.
- Delayed-wrap (xterm-style) line wrapping.
- Alt-screen 1049 supported.
- Combining chars in a per-line side table.
- Image paste saves the image and inserts its path.
- Lua sandbox strips dangerous globals and has an instruction-count timeout.
- Session persistence via `QDataStream` + `qCompress`.
- `opacity` drives per-pixel terminal-area alpha only; chrome paints opaque.
  No `setWindowOpacity()`.
- Audit: the rule pack is JSON (`audit_rules.json` appends/overrides; hardcoded
  checks stay in C++). `clazy-standalone` for Qt-aware checks.
  `.audit_suppress` is JSONL v2. Calibration reads existing project configs;
  `.audit_allowlist.json` is only for custom grep rules. The audit test
  harness is shell-based against fixture dirs.
- Audit confidence (0–100): floor +10, severity×15, +20 cross-tool, +10
  external AST tool, −5 short grep finding, −20 test path. AI triage caps:
  FALSE_POSITIVE ≤ 30, TRUE_POSITIVE ≥ 80.
- SARIF exports carry `contextRegion` (±3 lines) and `properties.blame`.
  Generated files are skipped.
- Roadmap-query IPC caches parsed bullets with mtime and a 100 ms TTL.
