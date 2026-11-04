# ANTS-4513 — loop log

Spec: [`../specs/ANTS-4513-feedback-corpus-store.md`](../specs/ANTS-4513-feedback-corpus-store.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-11-04 | 2, cold, every lane held every question (neutral-lane) | 3 | 4 | 2 | 1 | Verified 10, fixed 10, dismissed 2. One Q1 was found while building the packet and fixed before dispatch: `QSaveFile` refuses to open a 0444 file, so the write path now restores owner-write first (Qt 6 probe). Both lanes found the `etag`-on-`revision` defect (the reply carries live roadmap status), the missing-file drift contradiction, the classifier `parse` cannot supply, and the unscoped pending query. Also fixed: the `export_path` key for a file not yet created (`canonicalFilePath()` is empty, probed), INV-12 moved to pass-on-both, retired-op cases in `mcp_feedback_log`, a false `include_tracking` clause, and `ON DELETE CASCADE` dropped to match `.claude/rules/roadmap-store.md`. Dismissed: test-store injection (bundle mains already sandbox `XDG_DATA_HOME`, ANTS-3856), CRLF import (no corpus file holds `\r`). |
