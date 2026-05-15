# ANTS-1364 — `session_memory` Set: serialize the store once

**Status:** spec, ship-ready after cold-eyes pass.
**Source:** indie-review-2026-05-14 (self-observed).

## 1. Problem

`SessionMemoryEngine::execute` (Op::Set branch,
`sessionmemoryengine.cpp:221–258`) does the full-store JSON
serialization **twice** for every `Set` op:

1. `const qint64 total = serializedSize(next);` —
   `QJsonDocument(next).toJson(Compact).size()` (line 243).
2. `saveStore(path, next)` internally does
   `QByteArray body = QJsonDocument(store).toJson(Compact);` and
   writes (line 117).

Both calls serialize the same `QJsonObject` to the same bytes. The
size-cap check at line 245 then discards the bytes, and `saveStore`
recomputes them on its own.

At the 100 KiB cap with a near-full store, each serialise costs
~100 KB of JSON formatting + allocation. **Doubling that on every
Set** is the cheap win this spec captures.

The ROADMAP entry framed this as "cache `serializedSize` with a
per-key delta on insert/replace/delete". That framing assumes the
engine is long-lived. It isn't — `execute()` is stateless, reloads
the store from disk on every op. So delta accounting buys nothing
across calls. The real waste is the **intra-call** double serialise.

## 2. Design

Replace `saveStore(path, QJsonObject)` with
`saveStore(path, QByteArray body)`. Compute the bytes once in the
caller, check the cap, then pass the bytes to `saveStore`.

### 2.1 New signature

```cpp
// sessionmemoryengine.h
QString saveStore(const QString &path, const QByteArray &body);

// sessionmemoryengine.cpp
QString saveStore(const QString &path, const QByteArray &body) {
    // INV-10 — ensure parent dir exists with mode 0700.
    // ... (unchanged dir-mkpath block) ...
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { ... }
    if (sf.write(body) != body.size()) { sf.cancelWriting(); ... }
    if (!sf.commit()) { ... }
    QFile::setPermissions(path, ...);
    return QString();
}
```

### 2.2 Caller update — Set branch

```cpp
case Op::Set: {
    const qint64 valBytes = serializedSize(value);
    if (valBytes > kMaxValueBytes) { return makeErr(...); }
    QJsonObject next = store;
    next[key] = value;
    if (next.size() > kMaxStoreKeys) { return makeErr(...); }
    // ANTS-1364 — serialize once, reuse for both cap check + write.
    const QByteArray body = QJsonDocument(next)
        .toJson(QJsonDocument::Compact);
    const qint64 total = body.size();
    if (total > kMaxStoreBytes) { return makeErr(...); }
    const QString saveErr = saveStore(path, body);
    if (!saveErr.isEmpty()) { return makeErr(...); }
    r.bytesWritten = valBytes;
    r.totalBytes   = total;
    return r;
}
```

### 2.3 Caller update — Delete branch

Same refactor pattern. Delete also calls `saveStore(path, next)` then
`serializedSize(next)` — both replaced by a single serialise.

```cpp
case Op::Delete: {
    const bool had = store.contains(key);
    if (had) {
        QJsonObject next = store;
        next.remove(key);
        const QByteArray body = QJsonDocument(next)
            .toJson(QJsonDocument::Compact);
        const QString saveErr = saveStore(path, body);
        if (!saveErr.isEmpty()) { return makeErr(...); }
        r.totalBytes = body.size();
    } else {
        r.totalBytes = serializedSize(store);
    }
    r.found = had;
    return r;
}
```

### 2.4 `serializedSize(QJsonObject)` retained

The `qint64 serializedSize(const QJsonObject &)` overload is still
used by Get/List/Delete-no-op paths. Keep it; do not delete.

The `qint64 serializedSize(const QJsonValue &)` overload is unchanged
and still used by the per-value cap check.

## 3. Invariants

- **INV-1** Set / Delete paths serialize the store **once** per call.
  (Currently twice — the contract this spec fixes.)
- **INV-2** Public OpResult fields (`bytesWritten`, `totalBytes`,
  `found`, `ok`, `code`, `error`) are byte-identical to pre-fix.
- **INV-3** `saveStore` no longer accepts a `QJsonObject`. The
  function is publicly declared in the `SessionMemoryEngine`
  namespace (`sessionmemoryengine.h:46`), so the signature swap is
  source-incompatible with any external caller — but a grep over
  `src/` + `tests/` confirms only two in-tree callers
  (`execute()` Set + Delete branches at `sessionmemoryengine.cpp:251,
  265`) and one mention in `tests/features/session_memory_engine/
  spec.md`. No out-of-tree callers exist.
- **INV-4** Cap order preserved: per-value cap (16 KiB), then per-key
  count cap (32), then per-store byte cap (100 KiB). The serialise
  happens between the key-count cap and the byte cap.
- **INV-5** `saveStore` still uses `QSaveFile` for atomic rename.
  The serialise just moves up the stack.

## 4. Test surface

Existing engine-contract coverage:
- `tests/features/session_memory_engine/test_session_memory_engine.cpp`
  exercises Set/Delete/Get/List via `execute()`. Asserts on
  `bytesWritten`, `code`, `ok`, `found`. Does **not** assert
  `totalBytes` (verified via `grep totalBytes` — zero hits across
  this file and `tests/features/mcp_session_memory/`).
- `tests/features/mcp_session_memory/test_mcp_session_memory.cpp`
  exercises the MCP envelope; same coverage shape.

Add `tests/features/session_memory_serialize_once/` with:

1. **TotalBytesMatchesDiskAfterSet** — run a Set op against a
   non-empty store; read the on-disk JSON back; assert
   `result.totalBytes == on_disk_bytes.size()`. Newly defends INV-2.
2. **TotalBytesMatchesDiskAfterDelete** — same, after a Delete that
   removed a present key.

**Honest scope note:** these tests guard INV-2 (`totalBytes` equals
the bytes that actually hit the disk) — a property previously
untested. They do **not** guard INV-1 (the serialize-once invariant
itself), which is observable only through instrumentation the engine
deliberately doesn't expose. INV-1's correctness rides on code
review of the ~25 LOC diff. The refactor is small enough that this
is the right trade: instrument-and-test costs more than the
serialise it saves.

## 5. Build cost

- ~8 LOC `sessionmemoryengine.h` (signature update, comment).
- ~25 LOC `sessionmemoryengine.cpp` (signature impl + 2 callers).
- ~60 LOC new test (post-state-bytes consistency).

## 6. Out of scope

- No in-memory delta cache across calls. The engine is stateless;
  delta-caching would require a `RemoteControl`-side wrapper. Defer
  if `session_memory` profile data shows the per-call serialize is
  still a bottleneck after this fix.
- `serializedSize(QJsonValue)` per-value cap unchanged. The
  wrapper-bytes trick (lines 138–146) is correct and cheap.
- `saveStore`'s dir-mode + file-mode hardening unchanged.
