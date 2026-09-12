// bench_drift_lanes — wall time of the four in-process audit lanes
// AuditDialog runs synchronously on the GUI thread (ANTS-5067).
//
// Three of the four build a whole-tree source blob, and the two
// contract-doc lanes build byte-identical ones; every token then scans
// that blob linearly. The item was filed with "Duration not measured",
// and it asks for the measurement before the restructuring — this is it.
//
// Output is CSV, one row per lane plus a blob row:
//
//   lane,ms,output_lines
//
// Root defaults to this project (ANTS_PROJECT_ROOT); override with
// ANTS_PERF_ROOT=<path>. Exits 0 unless ANTS_PERF_MAX_MS=<ms> is set and
// the total exceeds it.

#include "featurecoverage.h"

#include <QCoreApplication>
#include <QString>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

struct Timed {
    double ms;
    int lines;
};

Timed timeLane(QString (*fn)(const QString &), const QString &root) {
    const auto t0 = std::chrono::steady_clock::now();
    const QString out = fn(root);
    const auto t1 = std::chrono::steady_clock::now();
    return {std::chrono::duration<double, std::milli>(t1 - t0).count(),
            out.isEmpty() ? 0 : int(out.count('\n')) + 1};
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const char *envRoot = std::getenv("ANTS_PERF_ROOT");
    const QString root = (envRoot && *envRoot)
        ? QString::fromLocal8Bit(envRoot)
        : QStringLiteral(ANTS_PROJECT_ROOT);

    std::printf("lane,ms,output_lines\n");

    // The blob alone, in the shape the contract-doc lanes ask for — the
    // work those two lanes each repeat.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const QString blob = FeatureCoverage::buildProjectSourceBlob(
            root, FeatureCoverage::BlobOptions{.includeMarkdownContents = false,
                                               .appendPathManifest = true});
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("blob_build_only,%.1f,%lld\n",
                    std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    static_cast<long long>(blob.size()));
    }

    double total = 0;
    const struct { const char *name; QString (*fn)(const QString &); } lanes[] = {
        {"spec_drift",            &FeatureCoverage::runSpecDriftCheck},
        {"contract_doc_standards", &FeatureCoverage::runContractDocDriftStandardsCheck},
        {"contract_doc_specs",     &FeatureCoverage::runContractDocDriftSpecsCheck},
        {"changelog_coverage",     &FeatureCoverage::runChangelogCoverageCheck},
    };
    for (const auto &l : lanes) {
        const Timed t = timeLane(l.fn, root);
        total += t.ms;
        std::printf("%s,%.1f,%d\n", l.name, t.ms, t.lines);
    }
    std::printf("TOTAL,%.1f,0\n", total);

    const char *maxEnv = std::getenv("ANTS_PERF_MAX_MS");
    const int maxMs = (maxEnv && *maxEnv) ? std::atoi(maxEnv) : 0;
    if (maxMs > 0 && total > double(maxMs)) {
        std::fprintf(stderr, "drift lanes %.1f ms exceeds ANTS_PERF_MAX_MS=%d\n",
                     total, maxMs);
        return 1;
    }
    return 0;
}
