// Feature-conformance test for tests/features/remote_control_roadmap_query/spec.md.
//
// Locks the ANTS-1117 v1 contract for the `roadmap-query` IPC verb:
// - `RoadmapDialog::parseBullets` correctness (behavioural)
// - `RemoteControl::cmdRoadmapQuery` registration + error shape
//   (source-grep)
//
// Exit 0 = all 9 invariants hold.

#include "roadmapdialog.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const std::string rcSrc = slurp(SRC_RC_CPP);
    const std::string rcHdr = slurp(SRC_RC_HEADER);
    if (rcSrc.empty()) return fail("INV-7", "remotecontrol.cpp not readable");
    if (rcHdr.empty()) return fail("INV-7", "remotecontrol.h not readable");

    // Behavioural INVs 1-6: drive parseBullets directly.
    {
        const QString doc = QStringLiteral(
            "## Section A\n"
            "\n"
            "- ✅ [ANTS-1042] **Done thing.** body line 1\n"
            "  body line 2 (continuation)\n"
            "  Kind: implement.\n"
            "  Lanes: AuditDialog, MainWindow.\n"
            "\n"
            "- 📋 [ANTS-1043] **Planned thing.** body.\n"
            "  Kind: fix.\n"
            "\n"
            "- 🚧 **In-progress thing.** body.\n"
            "\n"
            "- 💭 [ANTS-1099] **Considered thing.** body.\n"
            "\n"
            "- Plain narration bullet without emoji.\n");

        const auto bullets = RoadmapDialog::parseBullets(doc);

        // INV-1: only status-emoji bullets — 4 entries.
        if (bullets.size() != 4)
            return fail("INV-1",
                        "expected exactly 4 status-emoji bullets");

        // INV-3: emojis recognised in document order.
        const QString expectedStatuses[] = {
            QStringLiteral("✅"), QStringLiteral("📋"),
            QStringLiteral("🚧"), QStringLiteral("💭"),
        };
        for (int i = 0; i < 4; ++i) {
            if (bullets[i].status != expectedStatuses[i])
                return fail("INV-3", "status emoji mismatch in document order");
        }

        // INV-2: id matches ^ANTS-\d+$ when token present, else empty.
        if (bullets[0].id != QStringLiteral("ANTS-1042"))
            return fail("INV-2", "ANTS-1042 not extracted");
        if (bullets[1].id != QStringLiteral("ANTS-1043"))
            return fail("INV-2", "ANTS-1043 not extracted");
        if (!bullets[2].id.isEmpty())
            return fail("INV-2",
                        "no [ANTS-NNNN] token → id field must be empty");
        if (bullets[3].id != QStringLiteral("ANTS-1099"))
            return fail("INV-2", "ANTS-1099 not extracted");

        // INV-3 negative: plain narration bullet must NOT appear.
        for (const auto &b : bullets) {
            if (b.headline.contains(QStringLiteral("Plain narration")))
                return fail("INV-3",
                            "plain-narration bullet was incorrectly included");
        }

        // INV-5: multi-line body bullet keeps headline ("Done thing.").
        if (!bullets[0].headline.contains(QStringLiteral("Done thing")))
            return fail("INV-5",
                        "multi-line bullet should still expose Done thing headline");

        // INV-6: Kind / Lanes extraction.
        if (bullets[0].kind != QStringLiteral("implement"))
            return fail("INV-6",
                        "kind=implement not extracted from Kind: line");
        if (bullets[0].lanes.size() != 2 ||
            bullets[0].lanes[0] != QStringLiteral("AuditDialog") ||
            bullets[0].lanes[1] != QStringLiteral("MainWindow"))
            return fail("INV-6",
                        "lanes list not parsed correctly");
        if (bullets[1].kind != QStringLiteral("fix"))
            return fail("INV-6", "kind=fix not extracted");
        if (!bullets[1].lanes.isEmpty())
            return fail("INV-6",
                        "absent Lanes: should yield empty list");

        // INV-4: idempotent — second call returns byte-identical output.
        const auto bullets2 = RoadmapDialog::parseBullets(doc);
        if (bullets.size() != bullets2.size())
            return fail("INV-4", "idempotency size mismatch");
        for (int i = 0; i < bullets.size(); ++i) {
            if (bullets[i].id != bullets2[i].id ||
                bullets[i].status != bullets2[i].status ||
                bullets[i].headline != bullets2[i].headline ||
                bullets[i].kind != bullets2[i].kind ||
                bullets[i].lanes != bullets2[i].lanes)
                return fail("INV-4", "idempotency record mismatch");
        }
    }

    // INV-7: dispatch registers `roadmap-query`.
    if (!contains(rcSrc, "QLatin1String(\"roadmap-query\")"))
        return fail("INV-7", "dispatch missing roadmap-query branch");
    if (!contains(rcHdr, "cmdRoadmapQuery"))
        return fail("INV-7", "cmdRoadmapQuery not declared in remotecontrol.h");
    if (!contains(rcSrc, "RemoteControl::cmdRoadmapQuery"))
        return fail("INV-7", "cmdRoadmapQuery handler body missing");

    // INV-8: unified-shape error when no roadmap is loaded.
    if (!contains(rcSrc, "no_roadmap_loaded"))
        return fail("INV-8",
                    "no_roadmap_loaded error code missing from cmdRoadmapQuery");
    if (!contains(rcSrc, "roadmapPathForRemote"))
        return fail("INV-8",
                    "cmdRoadmapQuery must call MainWindow::roadmapPathForRemote()");

    // INV-9: cache fields wired up.
    const char *cacheFields[] = {
        "m_roadmapCachePath",
        "m_roadmapCacheMtimeMs",
        "m_roadmapCacheBullets",
    };
    for (const char *f : cacheFields) {
        if (!contains(rcHdr, f))
            return fail("INV-9", f);
        if (!contains(rcSrc, f))
            return fail("INV-9", f);
    }

    std::puts("OK remote_control_roadmap_query: 9/9 invariants");
    return 0;
}
