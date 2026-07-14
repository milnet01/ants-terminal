// ANTS-2119 — shared JSONL line-file primitives, extracted from the two
// model-switch ledgers (modelswitchledger / modelnearmissledger), which each
// carried a byte-identical private copy of readRawLines + writeLinesAtomic.
//
// The write path is security-critical (ANTS-1988 private 0700 dir + owner-only
// 0600 temp file, no world-readable create-then-chmod window). A single copy
// keeps that sequence from silently drifting between the two ledgers — a
// hardening fix to one can no longer miss the other.
//
// The Record-typed serialize / append / evict logic stays per-ledger: it
// genuinely differs (near-miss drops the oldest line unconditionally; the
// firing ledger pins in-flight "pending" lines and adds a hard ceiling), so
// templating it here would be premature abstraction, not reuse.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace JsonlFile {

// Read a JSONL file into its non-blank lines (the trailing '\n' is stripped and
// blank / whitespace-only lines are skipped). A missing or unreadable file
// yields an empty list rather than an error.
QList<QByteArray> readLines(const QString &path);

// Atomically rewrite `path` with `lines` (one per line, each '\n'-terminated)
// via QSaveFile. Creates the parent directory private (0700, ANTS-1988) and
// tightens the temp file to owner-only (0600) before the rename, so the final
// file is never briefly group/world-readable. Returns false on any I/O failure
// (the on-disk file is left untouched).
bool writeLinesAtomic(const QString &path, const QList<QByteArray> &lines);

}  // namespace JsonlFile
