#pragma once

// ProcessDrain — read a running QProcess's stdout as it is written, so
// QProcess never holds more than one read, and keep a bounded slice of it
// (ANTS-6497). Stdout only: under SeparateChannels stderr still collects in
// QProcess's buffer, so a caller that needs it bounded reads it itself.

#include <QByteArray>

class QProcess;

namespace ProcessDrain {

// Which bytes survive once the output passes the cap.
enum class Keep { Head, Tail };

struct Result {
    QByteArray bytes;        // at most `cap` bytes
    bool truncated = false;  // the output was longer than `cap`
    bool timedOut = false;   // the process was killed at the deadline
};

inline constexpr int kKillGraceMs = 2000;

// Drains `p`, already started, until it exits. A negative `timeoutMs` waits
// for ever; otherwise the process is killed at the deadline and given
// kKillGraceMs to exit.
Result run(QProcess &p, int timeoutMs, qsizetype cap, Keep keep);

}  // namespace ProcessDrain
