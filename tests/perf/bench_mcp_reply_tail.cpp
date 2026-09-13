// ANTS-5072 — wall time of each phase of the MCP reply tail on a large body.
//
// ClaudeIntegration::finishToolDispatch runs these on the GUI thread for every
// verb, off-thread ones included: ANTS-2132 moved only the tool handler to the
// worker. ANTS-5072 asks for each phase to be timed on a 4 MiB reply before any
// of it moves. This links the real functions, so the numbers are the transforms
// the app runs, called in the order finishToolDispatch calls them.
//
// The body is a read_region-shaped envelope whose lines are real source text
// from this repository, repeated to the requested size, so JSON escaping and
// wrapMcpData's scrub see ordinary code rather than a uniform run.
//
// offloadBody writes a spill file. The spill dir is pointed at a temp dir with
// mcp::setSpillDirOverride before anything runs, so the benchmark never writes
// into the user's cache.
//
// ANTS_PERF_MB sizes the body (default 4) and ANTS_PERF_ITERATIONS the repeat
// count (default 5).

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

#include "claudeintegration.h"
#include "mcpprojection.h"
#include "mcpspill.h"
#include "perf_metric.h"

#ifndef ANTS_PROJECT_ROOT
#  error "ANTS_PROJECT_ROOT compile definition required"
#endif

namespace {

int envInt(const char *name, int fallback) {
    bool ok = false;
    const int v = qgetenv(name).toInt(&ok);
    return (ok && v > 0) ? v : fallback;
}

// A read_region-shaped reply of at least `targetBytes` UTF-8 bytes.
QString readRegionBody(qint64 targetBytes) {
    QFile src(QStringLiteral(ANTS_PROJECT_ROOT "/src/claudeintegration.cpp"));
    if (!src.open(QIODevice::ReadOnly)) return {};
    const QStringList source = QString::fromUtf8(src.readAll()).split(QLatin1Char('\n'));
    if (source.isEmpty()) return {};
    QJsonArray lines;
    qint64 bytes = 0;
    int n = 0;
    while (bytes < targetBytes) {
        const QString &line = source.at(n % source.size());
        lines.append(line);
        bytes += line.toUtf8().size() + 3;  // the quotes and the comma
        ++n;
    }
    QJsonObject env;
    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("path")] = QStringLiteral(ANTS_PROJECT_ROOT "/src/claudeintegration.cpp");
    env[QStringLiteral("start_line")] = 1;
    env[QStringLiteral("end_line")] = n;
    env[QStringLiteral("returned")] = n;
    env[QStringLiteral("truncated")] = false;
    env[QStringLiteral("lines")] = lines;
    return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
}

template <typename F>
double timeMs(int iterations, F &&f) {
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < iterations; ++i) f();
    return double(t.nsecsElapsed()) / 1e6 / iterations;
}

// sendMcpResponse's work up to the socket write: the JSON-RPC envelope around
// the wrapped text, serialised, with the '\n' terminator.
QByteArray envelopeOf(const QString &wrapped) {
    QJsonObject block;
    block[QStringLiteral("type")] = QStringLiteral("text");
    block[QStringLiteral("text")] = wrapped;
    QJsonArray content;
    content.append(block);
    QJsonObject result;
    result[QStringLiteral("content")] = content;
    QJsonObject envelope;
    envelope[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    envelope[QStringLiteral("id")] = 1;
    envelope[QStringLiteral("result")] = result;
    QByteArray resp = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    resp.append('\n');
    return resp;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    QTemporaryDir spill;
    if (!spill.isValid()) {
        std::fprintf(stderr, "bench_mcp_reply_tail: no temp dir for spill files\n");
        return 1;
    }
    mcp::setSpillDirOverride(spill.path());

    const int mb = envInt("ANTS_PERF_MB", 4);
    const int iterations = envInt("ANTS_PERF_ITERATIONS", 5);
    const QString tool = QStringLiteral("read_region");

    const QString body = readRegionBody(qint64(mb) * 1024 * 1024);
    const qint64 bodyBytes = body.toUtf8().size();
    if (bodyBytes < qint64(mb) * 1024 * 1024) {
        std::fprintf(stderr, "bench_mcp_reply_tail: built a %lld-byte body, "
                             "short of %d MiB\n", (long long)bodyBytes, mb);
        return 1;
    }
    std::printf("bench_mcp_reply_tail: %s body of %lld bytes, %d iterations\n",
                qPrintable(tool), (long long)bodyBytes, iterations);

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("src/claudeintegration.cpp");
    args[QStringLiteral("caller_cwd")] = QStringLiteral(ANTS_PROJECT_ROOT);

    // Warm each phase once, and check the offload actually spilled: offloadBody
    // fails open and returns the body unchanged, which would time a no-op.
    bool unchanged = false;
    (void)ClaudeIntegration::applyEtagPattern(tool, args, body, &unchanged);
    const QString offloaded = mcp::offloadBody(tool, body);
    if (offloaded.size() >= body.size()) {
        std::fprintf(stderr, "bench_mcp_reply_tail: offloadBody did not spill "
                             "(fail-open); the offload timing would be a no-op\n");
        return 1;
    }
    const QString wrappedFull = ClaudeIntegration::wrapMcpData(tool, body);
    const QString wrappedOffloaded = ClaudeIntegration::wrapMcpData(tool, offloaded);

    const double etagMs = timeMs(iterations, [&] {
        bool u = false;
        (void)ClaudeIntegration::applyEtagPattern(tool, args, body, &u);
    });
    // Runs only when `compact` is requested, so it stays out of the totals.
    const double compactMs = timeMs(iterations, [&] {
        (void)mcp::compactEnvelope(body);
    });
    const double hintsMs = timeMs(iterations, [&] {
        (void)mcp::appendReadHints(tool, args, body, false);
    });
    // finishToolDispatch measures the body in UTF-8 bytes before the offload gate.
    const double utf8Ms = timeMs(iterations, [&] {
        (void)body.toUtf8().size();
    });
    const double offloadMs = timeMs(iterations, [&] {
        (void)mcp::offloadBody(tool, body);
    });
    const double wrapFullMs = timeMs(iterations, [&] {
        (void)ClaudeIntegration::wrapMcpData(tool, body);
    });
    const double wrapOffloadedMs = timeMs(iterations, [&] {
        (void)ClaudeIntegration::wrapMcpData(tool, offloaded);
    });
    const double envelopeFullMs = timeMs(iterations, [&] {
        (void)envelopeOf(wrappedFull);
    });
    const double envelopeOffloadedMs = timeMs(iterations, [&] {
        (void)envelopeOf(wrappedOffloaded);
    });

    // The two paths a large read reply takes: offloaded (the default for an
    // offload-eligible verb over the threshold) and sent whole.
    const double tailOffloadedMs =
        etagMs + hintsMs + utf8Ms + offloadMs + wrapOffloadedMs + envelopeOffloadedMs;
    const double tailWholeMs = etagMs + hintsMs + utf8Ms + wrapFullMs + envelopeFullMs;

    std::printf("phase,ms\n");
    std::printf("apply_etag_pattern,%.4f\n", etagMs);
    std::printf("compact_envelope (opt-in),%.4f\n", compactMs);
    std::printf("append_read_hints,%.4f\n", hintsMs);
    std::printf("body_utf8_size,%.4f\n", utf8Ms);
    std::printf("offload_body,%.4f\n", offloadMs);
    std::printf("wrap_mcp_data_whole,%.4f\n", wrapFullMs);
    std::printf("wrap_mcp_data_offloaded,%.4f\n", wrapOffloadedMs);
    std::printf("response_envelope_whole,%.4f\n", envelopeFullMs);
    std::printf("response_envelope_offloaded,%.4f\n", envelopeOffloadedMs);
    std::printf("tail_offloaded,%.4f\n", tailOffloadedMs);
    std::printf("tail_whole,%.4f\n", tailWholeMs);
    std::printf("offloaded_envelope_bytes,%lld\n",
                (long long)offloaded.toUtf8().size());

    AntsPerf::reportLowerBetter("mcp_tail.etag_ms", etagMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.compact_ms", compactMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.hints_ms", hintsMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.offload_ms", offloadMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.wrap_whole_ms", wrapFullMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.envelope_whole_ms", envelopeFullMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.total_offloaded_ms", tailOffloadedMs, "ms");
    AntsPerf::reportLowerBetter("mcp_tail.total_whole_ms", tailWholeMs, "ms");
    return 0;
}
