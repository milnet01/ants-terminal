# Feature: SHA-256 envelope on session files

## Contract

`SessionManager::serialize` MUST wrap the qCompress payload in a V4
envelope: `[ENVELOPE_MAGIC][ENVELOPE_VERSION][SHA-256(payload)
(32 bytes)][payload length (uint32)][payload bytes]`. The hash covers
the compressed bytes, not the uncompressed `QDataStream` raw — hashing
post-compression keeps the verification fast (one digest over a small
blob) and lets us detect bit-flips introduced anywhere from disk to
decompressor input.

`SessionManager::restore` MUST detect the envelope, verify the hash,
and refuse to restore on any of:
- envelope magic present but version unrecognized,
- envelope length-field disagrees with the file's trailing-bytes count,
- hash mismatch.

Pre-V4 files (raw qCompress output, no envelope) MUST still load —
the magic peek is unambiguous because qCompress's leading 4 bytes are
a big-endian uncompressed-length, and any real session is well below
the envelope magic value.

## Rationale

`sessionmanager.cpp` previously had no payload integrity. Anyone with
write access to `$XDG_DATA_HOME/ants-terminal/sessions/` (the user's
own UID, a compromised local process, a runaway `pip install`
post-exec, an npm dependency) could plant a crafted session file that
fed arbitrary codepoints, foreground/background colors, and attribute
flags into the grid on next restore. The grid is a render surface,
not a sandbox — strange codepoints can paint terminal-emulator escape
sequences into the visible screen, and crafted attribute flags can
flip cells into a state the parser never reaches in normal operation.

A SHA-256 checksum over the compressed payload is the cheapest
defense: one hash per save, one hash per load, no schema break for
the inner `QDataStream` format (still V3). The envelope is a thin
wrapper.

## Invariants

**INV-1 — `serialize` writes the envelope.** Source-grep against
`src/sessionmanager.cpp`: the body of `SessionManager::serialize` MUST
contain `ENVELOPE_MAGIC`, `ENVELOPE_VERSION`, and a
`QCryptographicHash::hash(...QCryptographicHash::Sha256)` call. The
final return MUST be the envelope-wrapped buffer — not raw `qCompress`
output.

**INV-2 — `restore` peeks the envelope magic and verifies the hash.**
Source-grep: the body of `SessionManager::restore` MUST reference
`ENVELOPE_MAGIC`, MUST include a SHA-256 hash recomputation
(`QCryptographicHash::hash(...QCryptographicHash::Sha256)`), and MUST
contain a comparison that returns false on mismatch.

**INV-3 — Envelope constants live on `SessionManager`.** Source-grep
against `src/sessionmanager.h`: `static constexpr uint32_t
ENVELOPE_MAGIC` and `static constexpr uint32_t ENVELOPE_VERSION` MUST
exist. The header value 0x53484543 ("SHEC") is the agreed-upon magic;
test enforces both presence and the exact value.

**INV-4 — Envelope-version bump remains a deliberate change.** Source-
grep: `ENVELOPE_VERSION` MUST be `1` at this milestone. Bumping it
later requires updating this test in lockstep — making the change
visible during code review rather than silently breaking restore
compatibility.

## Scope

### In scope
- Outer envelope at `serialize()` write time.
- Envelope detection + verification at `restore()` read time.
- Backward-compat fall-through for legacy V1-V3 files.

### Out of scope
- Migrating legacy files to V4 on first read. Legacy files keep loading
  without an upgrade-on-load — a subsequent `saveSession` writes them
  out as V4 organically.
- HMAC instead of SHA-256. The threat model is integrity, not
  authentication; an attacker with write access to the sessions dir
  also has read access, so a keyed MAC adds no extra defense.
- Encryption of session payload. Out of scope at this milestone; the
  data already lives on the user's filesystem with 0600 perms.

## Regression history

- **V1 (initial):** raw qCompress output, no checksum. Plant attack
  via session-dir write access fed arbitrary cells into next restore.
- **V2:** added `cwd` after window title. No integrity change.
- **V3:** added `pinnedTitle` after `cwd`. No integrity change.
- **0.7.30 (this fix):** V4 envelope wraps the compressed payload with
  a SHA-256 checksum. Inner stream format unchanged (still V3); only
  the file framing gains the integrity layer.
