// ANTS-1251 — subsystemmap: parses the `## Module map (src/)` section
// into [{name, summary}, ...] and caches the result keyed on the source
// file's mtime (no wall-clock TTL — INV-2). One copy lives on
// RemoteControl. See docs/specs/ANTS-1251.md.
//
// ANTS-1292 — the canonical module map moved out of CLAUDE.md into
// docs/subsystems.md so the ~130-line lane catalogue is not reloaded
// into every Claude session preamble. resolveSource() picks
// docs/subsystems.md when present and falls back to CLAUDE.md for
// projects that have not migrated. See docs/specs/ANTS-1292.md.

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
// `sourcePath`. Re-reads only when mtime changes (INV-2).
// On read error returns an empty vector. Thread-unsafe; intended to
// be called from the IPC thread (RemoteControl::dispatch chain).
QVector<Lane> cachedLanes(const QString &sourcePath);

// ANTS-1292 — resolve the canonical module-map source for a project,
// given a path to (or candidate for) the project's CLAUDE.md. Prefers
// `<root>/docs/subsystems.md` when it exists; otherwise returns
// `claudeMdPath` unchanged (back-compat for un-migrated projects).
// `<root>` is the directory containing `claudeMdPath`. Empty in →
// empty out.
QString resolveSource(const QString &claudeMdPath);

// ANTS-3481 — true iff `sourcePath` exists, is readable, and contains a
// `## Module map` H2 heading — the same heading `parse()` keys on. Lets a
// caller distinguish "no module map at all" from "heading present but no
// parseable lanes" (a `- <name> — <summary>` bullet-shape mismatch, or lanes
// that resolve to no source files), so an empty partition can be reported
// with an honest cause. Non-existent / unreadable file → false.
bool sourceHasModuleMap(const QString &sourcePath);

// Test-only: clear the cache so a fixture run starts clean.
void clearCacheForTests();

}  // namespace SubsystemMap

#endif
