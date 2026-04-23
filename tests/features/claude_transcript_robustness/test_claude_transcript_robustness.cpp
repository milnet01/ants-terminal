// Feature-conformance test for spec.md — asserts that the Claude Code
// transcript parser handles events larger than 32 KB without losing state
// and that decodeProjectPath preserves hyphens in directory names when a
// filesystem probe can disambiguate them.
//
// Links against src/claudeintegration.cpp. GUI-free (QCoreApplication only).
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "claudeintegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int inv1TailGrowsPastLargeEvent() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "[inv1] temp dir unusable\n");
        return 1;
    }

    // Construct a transcript whose FINAL line is a user tool_result whose
    // inline content exceeds the historical 32 KB tail window. Ordering
    // matters: the fat event has to be the last line for the old firstLine-
    // skip path to actually eat it. If we put a small event after it, the
    // old code gets away with reading the small event unharmed.
    //
    // Under the old window:
    //   seek(size - 32KB) lands inside the fat event's line.
    //   readLine() consumes up to the newline after the fat event.
    //   firstLine=true → that content is skipped.
    //   atEnd → loop exits with zero events → state unchanged.
    //
    // Under the growing window:
    //   32 KB → no newline in buffer → double → eventually finds the
    //   newline preceding the fat event → trim → parse the fat event.
    //   type=user with a tool_result → newState=Thinking, detail="processing result".
    const QString blob(40 * 1024, QLatin1Char('x'));
    const QByteArray fatTrailingEvent =
        QStringLiteral("{\"type\":\"user\",\"message\":"
                       "{\"content\":[{\"type\":\"tool_result\","
                       "\"tool_use_id\":\"t1\",\"content\":\"%1\"}]}}")
            .arg(blob)
            .toUtf8();

    const QString path = tmp.path() + "/large.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return 1;
    f.write(R"({"type":"user","message":{"content":"do a thing"}})"
            "\n");
    f.write(R"({"type":"assistant","message":{"stop_reason":"tool_use",)"
            R"("content":[{"type":"tool_use","name":"Read","input":{}}]}})"
            "\n");
    f.write(fatTrailingEvent);
    f.write("\n");
    f.close();

    ClaudeIntegration ci;
    QSignalSpy stateSpy(&ci, &ClaudeIntegration::stateChanged);
    ci.parseTranscriptForState(path);
    QCoreApplication::processEvents();

    const ClaudeState state = ci.currentState();
    const bool ok = (state == ClaudeState::Thinking) && stateSpy.count() >= 1;
    std::fprintf(stderr,
                 "[inv1 tail-grows-past-large-event] state=%d signals=%lld  %s\n",
                 static_cast<int>(state),
                 static_cast<long long>(stateSpy.count()),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv2TailGrowthBounded() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    // Pathological: 6 MiB of bytes with ZERO newlines. The decoder must
    // give up at the 4 MiB cap and return cleanly rather than spin.
    const QString path = tmp.path() + "/pathological.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return 1;
    const QByteArray junk(6 * 1024 * 1024, 'x');
    f.write(junk);
    f.close();

    ClaudeIntegration ci;
    ci.parseTranscriptForState(path);  // must not hang / OOM
    QCoreApplication::processEvents();

    std::fprintf(stderr, "[inv2 tail-growth-bounded]  PASS (returned)\n");
    return 0;
}

int inv3LeafHyphenPreserved() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    const QString leaf = "my-project";
    QDir root(tmp.path());
    if (!root.mkdir(leaf)) {
        std::fprintf(stderr, "[inv3] could not mkdir %s\n",
                     qUtf8Printable(leaf));
        return 1;
    }

    // Encode: tmp path is e.g. /tmp/qt_XXXXXX/my-project.
    // Claude encoding: every `/` → `-`. So for /tmp/qt_XXXXXX/my-project
    // the encoded form is `-tmp-qt_XXXXXX-my-project`.
    const QString realPath = tmp.path() + "/" + leaf;
    const QString encoded = "-" + realPath.mid(1).replace('/', '-');

    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const bool ok = (decoded == realPath);
    std::fprintf(stderr,
                 "[inv3 leaf-hyphen-preserved] encoded=%s decoded=%s want=%s  %s\n",
                 qUtf8Printable(encoded), qUtf8Printable(decoded),
                 qUtf8Printable(realPath), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv4IntermediateHyphenPreserved() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    QDir root(tmp.path());
    if (!root.mkpath("my-project/sub")) return 1;

    const QString realPath = tmp.path() + "/my-project/sub";
    const QString encoded = "-" + realPath.mid(1).replace('/', '-');

    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const bool ok = (decoded == realPath);
    std::fprintf(stderr,
                 "[inv4 intermediate-hyphen-preserved] decoded=%s want=%s  %s\n",
                 qUtf8Printable(decoded), qUtf8Printable(realPath),
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv5MissingPathFallsBackToSeparator() {
    // No filesystem entries created: decoder cannot probe, must fall back
    // to the legacy "every dash is a slash" behavior so well-formed cases
    // (no embedded hyphens) don't regress.
    const QString encoded = "-nonexistent-top-level-missing-path-segment";
    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const QString want = "/nonexistent/top/level/missing/path/segment";
    const bool ok = (decoded == want);
    std::fprintf(stderr,
                 "[inv5 missing-path-falls-back-to-separator] decoded=%s  %s\n",
                 qUtf8Printable(decoded), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int inv6LegacyCaseStillWorks() {
    // Anchor: the path this very checkout lives at.
    // Encoded: all slashes flipped to dashes, leading `/` replaced by `-`.
    const QString encoded = "-mnt-Storage-Scripts-Linux-Ants";
    const QString want = "/mnt/Storage/Scripts/Linux/Ants";
    if (!QDir(want).exists()) {
        std::fprintf(stderr,
                     "[inv6 legacy-case] repo path %s missing on this host — skipping\n",
                     qUtf8Printable(want));
        return 0;
    }
    const QString decoded = ClaudeIntegration::decodeProjectPath(encoded);
    const bool ok = (decoded == want);
    std::fprintf(stderr,
                 "[inv6 legacy-case-still-works] decoded=%s  %s\n",
                 qUtf8Printable(decoded), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    int failures = 0;
    failures += inv1TailGrowsPastLargeEvent();
    failures += inv2TailGrowthBounded();
    failures += inv3LeafHyphenPreserved();
    failures += inv4IntermediateHyphenPreserved();
    failures += inv5MissingPathFallsBackToSeparator();
    failures += inv6LegacyCaseStillWorks();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
