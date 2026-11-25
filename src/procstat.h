// Fields of a /proc/<pid>/stat line, by their proc(5) names.

#ifndef ANTS_PROCSTAT_H
#define ANTS_PROCSTAT_H

#include <QString>
#include <QStringList>

#include <optional>

namespace procstat {

// The fields after "pid (comm) ", 0-based: state, ppid, pgrp, session,
// tty_nr, tpgid, ... The comm may itself hold ") ", so the split starts at
// the LAST ')'. Empty when the line has no ')'.
inline QStringList fieldsAfterComm(const QString &stat) {
    const qsizetype close = stat.lastIndexOf(QLatin1Char(')'));
    if (close < 0) return {};
    return stat.mid(close + 2).split(QLatin1Char(' '));
}

// The parent process id (proc(5) field 4, "ppid"), or 0.
inline qint64 ppid(const QString &stat) {
    const QStringList f = fieldsAfterComm(stat);
    return f.size() > 1 ? f[1].toLongLong() : 0;
}

// The terminal's foreground process group (proc(5) field 8, "tpgid"), or -1.
inline int tpgid(const QString &stat) {
    const QStringList f = fieldsAfterComm(stat);
    return f.size() > 5 ? f[5].toInt() : -1;
}

// When the process started, in clock ticks after boot (proc(5) field 22,
// "starttime"), or nothing when the line is short or the field is not a number.
inline std::optional<qulonglong> starttime(const QString &stat) {
    const QStringList f = fieldsAfterComm(stat);
    if (f.size() <= 19) return std::nullopt;
    bool ok = false;
    const qulonglong ticks = f[19].toULongLong(&ok);
    return ok ? std::optional<qulonglong>(ticks) : std::nullopt;
}

}  // namespace procstat

#endif  // ANTS_PROCSTAT_H
