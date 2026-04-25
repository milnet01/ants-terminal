# Feature: pre-validate qCompress length prefix before `qUncompress`

## Contract

Before `SessionManager::restore` calls `qUncompress`, it MUST inspect
the leading 4 bytes of the compressed payload — qCompress's
big-endian uncompressed-length prefix — and reject any claim that
exceeds `MAX_UNCOMPRESSED` (500 MB). The post-decompression cap that
already exists is a backstop, not a primary defense; pre-flight
prevents the allocator from ever seeing the inflated request.

## Rationale

`qCompress` writes a custom format: 4-byte big-endian uncompressed
length, followed by zlib-compressed data. `qUncompress` reads the
prefix and pre-allocates the output `QByteArray` to that size before
streaming decompressed bytes into it.

Pre-fix, the read pipeline was:
1. `qUncompress(compressed)` → allocates `claimedUncompressed` bytes,
2. `if (raw.size() > 500MB) return false`.

A crafted file claiming 500 MB triggered a 500 MB allocation that
the post-hoc cap could only catch *after* the damage was done — and
the actual payload could be tiny, so the allocation pressure showed
up with no concomitant disk-space anomaly.

The fix peeks the 4-byte prefix and short-circuits before
`qUncompress` runs. Constant-time, no allocator pressure, no
false-positives on legitimate session files (real sessions are
~100 KB; the cap of 500 MB is generous).

## Invariants

**INV-1 — `restore` pre-validates the length prefix.** Source-grep
against `src/sessionmanager.cpp`: the `SessionManager::restore` body
MUST contain a manual big-endian uint32 reconstruction from the first
4 bytes of `compressed` AND a `return false` on the threshold. We
look for either `<< 24` shift-and-or pattern over `compressed.constData()`
or an equivalent reconstruction.

**INV-2 — Threshold matches the post-hoc cap.** The pre-flight
threshold MUST be `MAX_UNCOMPRESSED` referenced from the header (or
the literal 500 MB). Source-grep checks the header declares
`MAX_UNCOMPRESSED` and the cpp references it at the pre-flight site.

**INV-3 — Pre-flight runs BEFORE `qUncompress`.** Source-grep: in the
`restore` body, the `claimedUncompressed`-style reconstruction MUST
appear textually before the first `qUncompress(` call. Reordering
risks inverting the protection.

**INV-4 — Short-payload guard.** The pre-flight must also reject
payloads < 4 bytes (a payload too short to contain a length prefix
shouldn't be passed to qUncompress at all). Source-grep: a check
of the form `compressed.size() < 4` returning false.

## Scope

### In scope
- 4-byte length-prefix extraction.
- Threshold rejection before `qUncompress`.
- Short-payload guard.

### Out of scope
- Tightening the threshold below 500 MB. The figure is the existing
  post-decompression cap; matching it preserves user-visible
  behaviour while eliminating the over-allocation window.
- Replacing `qCompress` with a streaming codec. zlib over a small
  blob is fine; the issue is the pre-allocation, not the codec.

## Regression history

- **0.6.x – 0.7.29:** restore relied solely on the post-hoc
  `raw.size() > 500MB` check, which fires after `qUncompress` has
  already allocated and (partially) populated the output buffer.
- **0.7.30 (this fix):** pre-flight reads the 4-byte prefix and
  rejects oversize claims before `qUncompress` runs.
