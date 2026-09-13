// ANTS-5131 — wall time of a session save, split into serialize and write.
//
// SessionManager::saveSession runs on the GUI thread for every tab whose blob
// changed (ANTS-5030 already skips unchanged ones). ANTS-5131 asks for the cost
// to be measured before anything moves off that thread: serialize() reads the
// live grid and has to stay, while everything after it is a pure function of
// the produced bytes. This separates the two.
//
// Links the real SessionManager and TerminalGrid, so the numbers are the save
// the app runs. The grid is filled through VtParser with full-width lines of
// varied text, because a blank or repetitive scrollback compresses to almost
// nothing and would report a save far cheaper than a real one.
//
// XDG_DATA_HOME is pointed at a temp dir BEFORE Qt starts, so saveSession
// writes there. The benchmark refuses to run if the session path does not land
// inside that dir: a benchmark must never touch the user's real sessions.
//
// Scrollback defaults to 50000 lines, the app default. ANTS_PERF_SCROLLBACK and
// ANTS_PERF_COLS change the workload and ANTS_PERF_ITERATIONS the repeat count.
// Grid memory grows with lines x cols x sizeof(Cell), printed below; check it
// before asking for the 1M maximum.

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstdio>
#include <string>

#include "perf_metric.h"
#include "sessionmanager.h"
#include "terminalgrid.h"
#include "vtparser.h"

namespace {

int envInt(const char *name, int fallback) {
    bool ok = false;
    const int v = qgetenv(name).toInt(&ok);
    return (ok && v > 0) ? v : fallback;
}

// One line of `cols` printable characters, different on every line so the
// compressor sees text rather than a run it can collapse.
std::string textLine(int seq, int cols) {
    static const char kWords[] =
        "build ok warning unused variable in function src terminal grid "
        "session render cache flush 0x7f3a commit 4d07 push main origin ";
    const int nWords = int(sizeof(kWords)) - 1;
    std::string line = std::to_string(seq) + " ";
    int w = (seq * 7) % nWords;
    while (int(line.size()) < cols) {
        line += kWords[w];
        w = (w + 1 + seq % 3) % nWords;
    }
    line.resize(std::size_t(cols));
    return line;
}

}  // namespace

int main(int argc, char **argv) {
    QTemporaryDir home;
    if (!home.isValid()) {
        std::fprintf(stderr, "bench_session_save: no temp dir\n");
        return 1;
    }
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.path()));
    QCoreApplication app(argc, argv);

    const int lines = envInt("ANTS_PERF_SCROLLBACK", 50000);
    const int cols = envInt("ANTS_PERF_COLS", 80);
    const int iterations = envInt("ANTS_PERF_ITERATIONS", 3);
    const int rows = 24;

    const QString tabId = QStringLiteral("bench-session-save");
    const QString path = SessionManager::sessionPath(tabId);
    if (path.isEmpty() || !path.startsWith(home.path())) {
        std::fprintf(stderr,
                     "bench_session_save: session path %s is outside the temp "
                     "dir; refusing to write\n",
                     qPrintable(path));
        return 1;
    }

    std::printf("bench_session_save: %d lines x %d cols, sizeof(Cell)=%zu, "
                "about %.0f MiB of grid, %d iterations\n",
                lines, cols, sizeof(Cell),
                double(lines) * cols * sizeof(Cell) / (1024.0 * 1024.0),
                iterations);

    TerminalGrid grid(rows, cols);
    grid.setMaxScrollback(lines);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    std::string chunk;
    for (int i = 0; i < lines + rows; ++i) {
        chunk += textLine(i, cols);
        chunk += "\r\n";
        if (chunk.size() > (1 << 20)) {
            parser.feed(chunk.data(), int(chunk.size()));
            chunk.clear();
        }
    }
    parser.feed(chunk.data(), int(chunk.size()));

    // A save of an empty scrollback is fast and meaningless.
    const int filled = grid.scrollbackSize();
    if (filled < lines * 9 / 10) {
        std::fprintf(stderr,
                     "bench_session_save: scrollback holds %d of %d lines; "
                     "the measurement would describe a smaller grid\n",
                     filled, lines);
        return 1;
    }

    const QString cwd = home.path();
    QElapsedTimer t;

    // Warm: first serialize pays one-off allocation, first save creates the dir.
    (void)SessionManager::serialize(&grid, cwd, {});
    SessionManager::saveSession(tabId, &grid, cwd, {});

    t.start();
    qint64 blobBytes = 0;
    for (int i = 0; i < iterations; ++i)
        blobBytes = SessionManager::serialize(&grid, cwd, {}).size();
    const double serializeMs = double(t.nsecsElapsed()) / 1e6 / iterations;

    // Split serialize() without reaching into SessionManager. Its blob is a
    // fixed header — magic, version, SHA-256, payload length, the layout
    // ENVELOPE_HEADER_SIZE describes in sessionmanager.h — then the qCompress
    // payload, and qUncompress of that payload is exactly the stream
    // serializeStream built. So timing qCompress and the hash over those real
    // bytes leaves the grid walk as the rest of serialize(). The walk has to
    // stay on the GUI thread; the other two do not.
    //
    // serialize() compresses again only when a blob overshoots its file cap;
    // the length check below fails loudly rather than mis-splitting if the
    // layout ever changes.
    constexpr int kHeaderBytes = 4 + 4 + 32 + 4;
    const QByteArray blob = SessionManager::serialize(&grid, cwd, {});
    const bool layoutOk =
        blob.size() > kHeaderBytes && blob.startsWith("SHEC")
        && qFromBigEndian<quint32>(blob.constData() + kHeaderBytes - 4)
               == quint32(blob.size() - kHeaderBytes);
    if (!layoutOk) {
        std::fprintf(stderr, "bench_session_save: session blob layout is not "
                             "the one this split assumes; update the split\n");
        return 1;
    }
    const QByteArray payload = blob.mid(kHeaderBytes);
    const QByteArray raw = qUncompress(payload);
    if (raw.isEmpty()) {
        std::fprintf(stderr, "bench_session_save: payload did not decompress\n");
        return 1;
    }

    t.start();
    for (int i = 0; i < iterations; ++i) (void)qCompress(raw, 6);
    const double compressMs = double(t.nsecsElapsed()) / 1e6 / iterations;

    t.start();
    for (int i = 0; i < iterations; ++i)
        (void)QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    const double hashMs = double(t.nsecsElapsed()) / 1e6 / iterations;

    const double streamMs = serializeMs - compressMs - hashMs;

    t.start();
    for (int i = 0; i < iterations; ++i)
        SessionManager::saveSession(tabId, &grid, cwd, {});
    const double saveMs = double(t.nsecsElapsed()) / 1e6 / iterations;

    const qint64 fileBytes = QFileInfo(path).size();
    if (fileBytes <= 0) {
        std::fprintf(stderr, "bench_session_save: no session file was written\n");
        return 1;
    }
    const double writeMs = saveMs - serializeMs;

    std::printf("phase,ms,bytes\n");
    std::printf("serialize,%.4f,%lld\n", serializeMs, (long long)blobBytes);
    std::printf("  grid_walk_stream,%.4f,%lld\n", streamMs, (long long)raw.size());
    std::printf("  qcompress_level6,%.4f,%lld\n", compressMs, (long long)payload.size());
    std::printf("  sha256,%.4f,%lld\n", hashMs, (long long)payload.size());
    std::printf("save_total,%.4f,%lld\n", saveMs, (long long)fileBytes);
    std::printf("write_fsync_rename,%.4f,%lld\n", writeMs, (long long)fileBytes);
    std::printf("scrollback_lines,%d,0\n", filled);

    AntsPerf::reportLowerBetter("session.serialize_ms", serializeMs, "ms");
    // The part of serialize() that must stay on the GUI thread, and the two
    // parts that could leave it.
    AntsPerf::reportLowerBetter("session.stream_ms", streamMs, "ms");
    AntsPerf::reportLowerBetter("session.compress_ms", compressMs, "ms");
    AntsPerf::reportLowerBetter("session.hash_ms", hashMs, "ms");
    AntsPerf::reportLowerBetter("session.save_total_ms", saveMs, "ms");
    AntsPerf::reportLowerBetter("session.write_ms", writeMs, "ms");
    return 0;
}
