# ANTS-4272 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-4272-plugin-io-events.md`](../specs/ANTS-4272-plugin-io-events.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-10-13 | 2 (neutral-lane; every lane held every question) | 1 | 3 | 0 | 0 | Verified 4, fixed 4, dismissed 0. Q1: echo off does not mark a password (measured: bash and zsh clear ECHO at their prompts; read -s and getpass keep ICANON on), so § 2.7 and INV-8 now test ECHO off with ICANON on. Q2: § 2.1's empty permission string skipped every engine (both lanes); § 2.4 read the row before asking the filter (both lanes); INV-3's "returns at once" could end the host function. Four open questions resolved clean: broadcast uses sendToPty, setPermissions runs only at load, setOsc133KeyForTest exists, the Pty-to-VtStream atomic is local detail. |
