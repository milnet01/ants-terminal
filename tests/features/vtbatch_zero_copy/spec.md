# VtBatch zero-copy across worker→GUI thread hop

## Why this test exists

`VtStream` runs on a worker QThread and parses raw PTY bytes into a
`VtBatch` (a `std::vector<VtAction>` plus the raw `QByteArray`).
Pre-fix, the worker emitted `void batchReady(const VtBatch &batch)`
across a `Qt::QueuedConnection` to the GUI thread. Qt's queued-
connection plumbing has to OWN the parameters it dispatches — so a
`const T &` signal parameter is deep-copied into the queued event,
regardless of any move-from-pending shaping the emitter does. On a
noisy build (`find /`, `yes`), `actions.size()` reaches the 4096-
action flush cap and `rawBytes` reaches 16 KB per batch; the
deep-copy on the hop ran into the tens of KB per emit, multiplied
by the ~hundreds of batches/second a noisy stream produces.

The fix wraps the payload in
`using VtBatchPtr = std::shared_ptr<const VtBatch>;` and emits
`void batchReady(VtBatchPtr batch)`. Qt copies the smart-pointer
across the queue (an atomic refcount bump, ~16 bytes); the
underlying `VtBatch` lives on the heap and is not duplicated.

## Invariants

### I1 — Signal signature uses `VtBatchPtr`.
`vtstream.h` declares `void batchReady(VtBatchPtr batch);` (no
`const VtBatch &`).

### I2 — `VtBatchPtr` is the documented alias.
`using VtBatchPtr = std::shared_ptr<const VtBatch>;` exists in
`vtstream.h`. The `Q_DECLARE_METATYPE` macro covers `VtBatchPtr` so
the queued connection can register it.

### I3 — Emitters use `std::make_shared<VtBatch>()`.
Both `VtStream::flushBatch` and `VtStream::onPtyFinished` build the
batch through `std::make_shared<VtBatch>()`, populate via `b->…`,
and emit via `emit batchReady(b)`. No copy of a stack-allocated
`VtBatch` survives in the emit path.

### I4 — Receiver dereferences via pointer.
`TerminalWidget::onVtBatch(VtBatchPtr batch)` accesses fields via
`batch->actions`, `batch->rawBytes`, etc. — no `batch.foo` dot
access remains.

### I5 — Behavioural equivalence preserved.
The action stream observed by the GUI is identical to the
pre-fix path. `tests/features/threaded_parse_equivalence/`
covers this and continues to pass.

### I6 — Metatype registered.
`vtstream.cpp::VtStream::VtStream` calls
`qRegisterMetaType<VtBatchPtr>("VtBatchPtr")`. (Without this, Qt's
queued-connection dispatch would warn at runtime.)

## Test shape

Source-grep harness, no Qt link. The behavioural contract is
already pinned by `threaded_parse_equivalence`; this spec exists
to lock the *shape* of the cross-thread signal so a future
refactor can't silently revert the zero-copy by changing the
signal back to `const VtBatch &`.
