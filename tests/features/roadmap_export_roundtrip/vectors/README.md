# RFC 8785 test vectors

The published JCS test-vector set, committed as fixtures for **ANTS-3761**
INV-19 ("the serialiser conforms to RFC 8785 on the RFC's own test vectors").

- **Source:** <https://github.com/cyberphone/json-canonicalization>,
  `testdata/input/` and `testdata/output/`, the reference implementation
  repository RFC 8785 itself points at. Fetched 2026-07-30.
- **Licence:** Apache-2.0.
- **Renaming is the only change:** upstream `input/arrays.json` is here as
  `input-arrays.json`, so the pairs sit in one flat directory. Contents are
  byte-for-byte upstream.

`es6testfile100m.txt` — upstream's 100-million-case number file — is
deliberately **not** vendored. RFC 8785 Appendix B's 24-row table covers the
same rules with the edge cases the RFC itself chose to name, and it lives in
the test source rather than here because it is a table of doubles-as-hex, not
JSON.

Regenerating any of these files to make a test pass would defeat the point:
they are the external authority the writer is measured against.
