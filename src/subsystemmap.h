// ANTS-1251 — subsystemmap: parses CLAUDE.md's `## Module map (src/)`
// section into [{name, summary}, ...] and caches the result keyed on
// CLAUDE.md mtime (no wall-clock TTL — INV-2). One copy lives on
// RemoteControl. See docs/specs/ANTS-1251.md.

#ifndef ANTS_SUBSYSTEMMAP_H
#define ANTS_SUBSYSTEMMAP_H

#include <QString>
#include <QVector>

namespace SubsystemMap {

struct Lane {
    QString name;
    QString summary;
};

// Parse a CLAUDE.md body. Walks the `## Module map (src/)` H2 until
// the next H2; collects bullets shaped as ``-`name` — summary``.
// INV-3: anything not matching is dropped (defensive against drift).
// Empty / missing section → empty vector (INV-7).
QVector<Lane> parse(const QString &claudeMdBody);

// Synchronous mtime-keyed cache. Returns the parsed lanes for
// `claudeMdPath`. Re-reads only when mtime changes (INV-2).
// On read error returns an empty vector. Thread-unsafe; intended to
// be called from the IPC thread (RemoteControl::dispatch chain).
QVector<Lane> cachedLanes(const QString &claudeMdPath);

// Test-only: clear the cache so a fixture run starts clean.
void clearCacheForTests();

}  // namespace SubsystemMap

#endif
