// Fields of a /proc/<pid>/stat line, by their proc(5) names.

#ifndef ANTS_PROCSTAT_H
#define ANTS_PROCSTAT_H

#include <QString>
#include <QStringList>

namespace procstat {

// The fields after "pid (comm) ", 0-based: state, ppid, pgrp, session,
// tty_nr, tpgid, ... The comm may itself hold ") ", so the split starts at
// the LAST ')'. Empty when the line has no ')'.
inline QStringList fieldsAfterComm(const QString &stat) {
    const qsizetype close = stat.lastIndexOf(QLatin1Char(')'));
    if (close < 0) return {};
    return stat.mid(close + 2).split(QLatin1Char(' '));
}

// The terminal's foreground process group (proc(5) field 8, "tpgid"), or -1.
inline int tpgid(const QString &stat) {
    const QStringList f = fieldsAfterComm(stat);
    return f.size() > 5 ? f[5].toInt() : -1;
}

}  // namespace procstat

#endif  // ANTS_PROCSTAT_H
