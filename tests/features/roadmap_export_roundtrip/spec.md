# roadmap_export_roundtrip — the roadmap export

Feature contract for **ANTS-3761**.
Parent spec: [`docs/specs/ANTS-3761-roadmap-export-format.md`](../../../docs/specs/ANTS-3761-roadmap-export-format.md)

§ 6 of the parent assigns INV-1, 2, 5, 12, 13, 18 and 19 to this one directory.
**In place today: INV-19.** The rest land here as the export writer is built;
this file grows with them rather than being written ahead of them.

## What this locks

**INV-19 — the serialiser conforms to RFC 8785 (JCS) on the RFC's own test
vectors.**

Four legs, and the split is not tidiness — leg 1 alone passes against exactly
the writer the parent spec rules out:

1. **The six published vector files** — `arrays`, `french`, `structures`,
   `unicode`, `values`, `weird` — byte for byte. Committed under `vectors/`
   with their provenance; see that directory's README.
2. **RFC 8785 Appendix B, Table 1** — all 24 number samples, addressed by their
   IEEE 754 bit patterns so the test says what the RFC says.
3. **The RFC's mandatory error case.** § 3.2.2.2 requires a conforming
   implementation to *terminate* on a lone surrogate; the parent's § 2.4 turns
   that into "abort and report the row", never a replacement character. A
   well-formed surrogate **pair** must still serialise — the check must reject
   invalid UTF-16, not all non-BMP text.
4. **Key order is JCS's**, not insertion or reading order. § 2.2 warns that the
   record shapes in § 2.3 are written readably for humans and are not
   byte-exact; this is what makes that warning testable.

## Must fail first

Each mutation applied to `src/jsoncanonical.cpp`, built, run, reverted:

- `numberToString()` delegating to `QJsonDocument::toJson(Compact)` — the exact
  mistake § 2.2 names → **leg 2 RED**.
- The lone-surrogate check removed → **leg 3 RED**: the string serialises with
  U+FFFD substituted, which is the outcome § 2.4 forbids.
- The key comparator reversed → **legs 4 and 1 RED**.

**Leg 1 stays GREEN under the first mutation, and that is the finding.** Qt
matches JCS on all six published files; it is only Appendix B's number table
that catches it, at 21 of 24. A test built from the vector files alone would
have certified the writer this spec exists to rule out — which is the same
shape as INV-18's reason for existing, one level down: self-consistency, and
even partial external agreement, are not conformance.
