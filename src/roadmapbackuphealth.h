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

}  // namespace RoadmapBackupHealth
