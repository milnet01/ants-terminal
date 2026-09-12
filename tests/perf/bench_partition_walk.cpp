// bench_partition_walk — wall time of TestAuditEngine::partition, the tree
// walk TestAuditDialog runs on the GUI thread on dialog open, on
// editingFinished and before every dispatch (ANTS-5126).
//
// ANTS-1397 § 6 accepts that placement only while the walk fits roughly
// 50 ms. ANTS-5062 made the walk prune excluded directories, walk once per
// root and match relative paths, but nobody measured the result on a large
// tree — so the acceptance condition was carried unverified. This links the
// real engine rather than reproducing it, so the number is the walk the
// dialog actually runs.
//
// Output is one CSV line:
//
//   root,files,chunks,first_ms,warm_ms,iterations
//
// `first_ms` is the cold call, `warm_ms` the median of the rest — the
// dialog's repeat calls are the warm case, its open is the cold one.
//
// Root defaults to this project (ANTS_PROJECT_ROOT); override with
// ANTS_PERF_ROOT=<path> to measure a bigger tree. Iterations default to 5,
// override with ANTS_PERF_ITERS. Exits 0 unless ANTS_PERF_MAX_MS=<ms> is
// set and warm_ms exceeds it.

#include "testauditengine.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "perf_metric.h"

namespace {

double runOnce(const QString &root, int *files, int *chunks) {
    TestAuditEngine::PartitionRequest req;
    req.callerCwd  = root;
    req.scope      = QStringLiteral("auto");
    req.dimensions = QStringLiteral("auto");
    req.chunkSize  = 12;
    // ANTS-5126 — ANTS_PERF_NO_PREPASS=1 measures the partition the dialog's
    // panel refresh runs, which skips the grep pre-pass.
    {
        const char *np = std::getenv("ANTS_PERF_NO_PREPASS");
        req.prePass = !(np && *np && std::atoi(np) != 0);
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto res = TestAuditEngine::partition(req);
    const auto t1 = std::chrono::steady_clock::now();

    *files  = res.totalFiles;
    *chunks = res.chunksCount;
    if (!res.ok) {
        std::fprintf(stderr, "partition failed: %s (%s)\n",
                     res.error.toUtf8().constData(),
                     res.code.toUtf8().constData());
    }
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    const int n = std::atoi(v);
    return n > 0 ? n : fallback;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const char *envRoot = std::getenv("ANTS_PERF_ROOT");
    const QString root = (envRoot && *envRoot)
        ? QString::fromLocal8Bit(envRoot)
        : QStringLiteral(ANTS_PROJECT_ROOT);

    const int iters = envInt("ANTS_PERF_ITERS", 5);

    int files = 0, chunks = 0;
    const double firstMs = runOnce(root, &files, &chunks);

    std::vector<double> warm;
    warm.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        int f = 0, c = 0;
        warm.push_back(runOnce(root, &f, &c));
    }
    std::sort(warm.begin(), warm.end());
    const double warmMs = warm.empty() ? firstMs : warm[warm.size() / 2];

    std::printf("root,files,chunks,first_ms,warm_ms,iterations\n");
    std::printf("%s,%d,%d,%.1f,%.1f,%d\n",
                root.toUtf8().constData(), files, chunks,
                firstMs, warmMs, iters);

    // ANTS-5133 — the warm median is the dialog's repeat-call cost and the
    // cold call its open cost; both are tracked, because ANTS-1397 § 6 accepts
    // this placement only while the walk stays small and the two can move
    // independently.
    AntsPerf::reportLowerBetter("audit.partition_walk.warm_ms", warmMs, "ms");
    AntsPerf::reportLowerBetter("audit.partition_walk.cold_ms", firstMs, "ms");

    const int maxMs = envInt("ANTS_PERF_MAX_MS", 0);
    if (maxMs > 0 && warmMs > double(maxMs)) {
        std::fprintf(stderr,
                     "partition walk %.1f ms exceeds ANTS_PERF_MAX_MS=%d\n",
                     warmMs, maxMs);
        return 1;
    }
    return 0;
}
