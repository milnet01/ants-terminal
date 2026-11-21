# ANTS-5219 — Cap `get_scrollback`'s lines and say when its reply is cut — review loop log

Record for [`../specs/ANTS-5219-scrollback-line-cap.md`](../specs/ANTS-5219-scrollback-line-cap.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-14 | 1 (user preference: keep token use down; one reviewer, one pass) | 1 | 1 | 1 | 0 | Verified 3, fixed 3, dismissed 1. [Q2] lines <= 0 read 50 lines while the spec promised unchanged text; today it returns an empty reply, so get_scrollback passes a fallback of 0, unclamped. [Q3] section 2.4 left open whether since_cursor content carries the cap line; it does not, the cap is reported by truncated and lines_dropped. [Q1, from the lane's open question] section 5 told callers to raise lines, which the since_cursor path never reads; removed. Dismissed: where capScrollbackRequest is defined changes nothing built, and a link failure would surface at build. |
