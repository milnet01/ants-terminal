// ANTS-3794 § 2.5 — is the roadmap store still being backed up?
//
// Each backup job (the local snapshot, the claude-config export) writes a
// small key=value record after every run. assess() reads both and reports the
// jobs that have never run, are failing, or have not succeeded for more than a
// week and a day. A notification from the job itself cannot catch a timer that
// stopped firing; a reader of the record's age can.
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace RoadmapBackupHealth {

// Weekly jobs, plus a day of grace.
constexpr qint64 kStaleAfterSecs = 8LL * 24 * 60 * 60;

// $XDG_STATE_HOME/ants-terminal, or ~/.local/state/ants-terminal when that is
// unset. QStandardPaths::StateLocation is Qt 6.7+, above the 6.2 floor.
QString stateDir();

// An empty object when both jobs are healthy or there is no store; otherwise
// {jobs: {<job>: {state, success, error}}, hint}, listing only the unhealthy
// jobs, where state is never_run, failing or stale.
QJsonObject assess(const QString &stateDir, const QDateTime &nowUtc, bool storeExists);

// ANTS-5247 — the folder the pre-migration snapshot defaults to (ANTS-3855
// § 2.4 rungs 2 and 3). `configDir` is claude.roadmap_snapshot_dir and wins
// when set; otherwise the `dest=` line of the weekly snapshot record. An empty
// value counts as unset, as assess() reads empty. `source` is "config",
// "backup_record", or empty when neither names a folder.
struct SnapshotDest {
    QString folder;
    QString source;
};
SnapshotDest snapshotDest(const QString &configDir, const QString &stateDir);

}  // namespace RoadmapBackupHealth
