#include "processdrain.h"

#include <QElapsedTimer>
#include <QProcess>

#include <algorithm>

namespace ProcessDrain {

namespace {
constexpr int kReadSliceMs = 200;
}  // namespace

Result run(QProcess &p, int timeoutMs, qsizetype cap, Keep keep) {
    Result r;
    cap = std::max<qsizetype>(cap, 0);
    const auto take = [&](const QByteArray &chunk) {
        if (chunk.isEmpty()) return;
        if (keep == Keep::Head) {
            const qsizetype room = cap - r.bytes.size();
            if (chunk.size() > room) r.truncated = true;
            if (room > 0) r.bytes.append(chunk.constData(), std::min(room, chunk.size()));
            return;
        }
        // Trimmed at twice the cap, so the copy-down stays rare.
        r.bytes.append(chunk);
        if (r.bytes.size() > 2 * cap) {
            r.bytes.remove(0, r.bytes.size() - cap);
            r.truncated = true;
        }
    };
    QElapsedTimer clock;
    clock.start();
    while (p.state() != QProcess::NotRunning) {
        int slice = kReadSliceMs;
        if (timeoutMs >= 0) {
            const qint64 left = timeoutMs - clock.elapsed();
            if (left <= 0) {
                r.timedOut = true;
                p.kill();
                p.waitForFinished(kKillGraceMs);
                break;
            }
            slice = static_cast<int>(std::min<qint64>(left, kReadSliceMs));
        }
        p.waitForReadyRead(slice);
        take(p.readAllStandardOutput());
    }
    take(p.readAllStandardOutput());  // what arrived last
    if (keep == Keep::Tail && r.bytes.size() > cap) {
        r.bytes.remove(0, r.bytes.size() - cap);
        r.truncated = true;
    }
    return r;
}

}  // namespace ProcessDrain
