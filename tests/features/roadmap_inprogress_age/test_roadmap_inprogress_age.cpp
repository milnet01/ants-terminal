// Feature-conformance test for tests/features/roadmap_inprogress_age/spec.md.
//
// Locks ANTS-1237 — "Updated N days ago" line on 🚧 cards, derived
// from git blame on the bullet block. Two layers:
//   1. Renderer-layer (no git): static `renderCardsHtml` + `humanAge`.
//   2. Parser-layer (real git checkout in QTemporaryDir): static
//      `parseLastTouchDates` + the public `refreshLastTouchDatesIfStale`.
//
// Parser-layer tests skip gracefully when `git` is not on PATH.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

#include <gtest/gtest.h>

namespace {

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

// Markdown fixture with one 🚧 card whose body carries continuation
// lines, plus a ✅ card whose body mentions the 🚧 card's ID (audit
// trail) — locks INV-8 (audit-trail mention does NOT contribute).
QString fixtureMarkdown() {
    return QStringLiteral(
        "# Sample Roadmap\n"
        "\n"
        "## 0.8.0\n"
        "\n"
        "- 🚧 [ANTS-9999] **Active card.**\n"
        "  Layman: in-progress item.\n"
        "  Body continuation line.\n"
        "  Kind: implement.\n"
        "\n"
        "- ✅ [ANTS-9998] **Shipped card.**\n"
        "  Layman: shipped item (mentions ANTS-9999 in audit trail).\n"
        "  Kind: fix.\n");
}

QString writeFile(const QString &path, const QString &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&f);
    out << content;
    f.close();
    return path;
}

// Helper: build a CardRenderOptions with the 0.8.0 section opened
// (renderCardsHtml only emits cards under expanded sections).
RoadmapDialog::CardRenderOptions makeOptsWithSectionOpen() {
    RoadmapDialog::CardRenderOptions opts;
    opts.expandedSections.insert(QStringLiteral("0-8-0"));
    return opts;
}

bool gitOnPath() {
    return !QStandardPaths::findExecutable(QStringLiteral("git"))
                .isEmpty();
}

// Run a git subcommand in `cwd` with optional env vars; return true
// on success (exit 0).
bool runGit(const QString &cwd, const QStringList &args,
            const QProcessEnvironment &env = QProcessEnvironment()) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    if (!env.isEmpty()) p.setProcessEnvironment(env);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(2000)) return false;
    if (!p.waitForFinished(10000)) {
        p.kill();
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit
        && p.exitCode() == 0;
}

}  // namespace

static int runMain() {
    const QString markdown = fixtureMarkdown();
    const unsigned filter = 0x0F;  // all four status categories on
    const QStringList currentBullets;
    const QString theme = QStringLiteral("light");

    // -----------------------------------------------------------------
    // Renderer-layer assertions (INV-1, INV-2, INV-3).
    // -----------------------------------------------------------------

    // ANTS-1237-INV-1: 🚧 card with lastTouchDates entry renders
    // `<span class="rm-date">· Updated 3d ago</span>`.
    {
        RoadmapDialog::CardRenderOptions opts = makeOptsWithSectionOpen();
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        opts.lastTouchDates.insert(QStringLiteral("ANTS-9999"),
                                   nowSec - 3 * 86400);
        const QString html = RoadmapDialog::renderCardsHtml(
            markdown, filter, currentBullets, theme,
            RoadmapDialog::SortOrder::Document,
            QString(), {}, opts);
        if (!html.contains(QStringLiteral(
                "<span class=\"rm-date\">· Updated 3d ago</span>")))
            fail("INV-1",
                "🚧 card with lastTouchDates entry did NOT emit "
                "the `· Updated 3d ago` span — the new emitCard "
                "branch isn't firing.");
    }

    // ANTS-1237-INV-2: ✅ card does NOT emit `· Updated` (the
    // ✅-branch still emits its shipped-date span via shippedDates
    // lookup; assertion is on the absence of `· Updated ` only).
    {
        RoadmapDialog::CardRenderOptions opts = makeOptsWithSectionOpen();
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        // Inject a lastTouchDates entry for the ✅ card to prove the
        // status gate (not the hash lookup) is what suppresses the
        // span.
        opts.lastTouchDates.insert(QStringLiteral("ANTS-9998"),
                                   nowSec - 5 * 86400);
        const QString html = RoadmapDialog::renderCardsHtml(
            markdown, filter, currentBullets, theme,
            RoadmapDialog::SortOrder::Document,
            QString(), {}, opts);
        if (html.contains(QStringLiteral("· Updated ")))
            fail("INV-2",
                "✅ card emitted a `· Updated` span — the status "
                "gate (`rec.status == \"🚧\"`) is broken or the "
                "branch is firing on the wrong status.");
    }

    // ANTS-1237-INV-3: empty lastTouchDates → no `· Updated` span on
    // any card, including 🚧.
    {
        RoadmapDialog::CardRenderOptions opts = makeOptsWithSectionOpen();
        // opts.lastTouchDates is empty by default.
        const QString html = RoadmapDialog::renderCardsHtml(
            markdown, filter, currentBullets, theme,
            RoadmapDialog::SortOrder::Document,
            QString(), {}, opts);
        if (html.contains(QStringLiteral("· Updated ")))
            fail("INV-3",
                "🚧 card emitted a `· Updated` span despite empty "
                "lastTouchDates — graceful-degradation path is "
                "broken (likely a constFind / constEnd misuse).");
    }

    // -----------------------------------------------------------------
    // ANTS-1237-INV-4: humanAge ladder + boundaries.
    // Direct calls on the file-scope helper (declared in
    // roadmapdialog.h, defined outside the anonymous namespace).
    // -----------------------------------------------------------------
    {
        struct Case { qint64 age; const char *expected; };
        const Case cases[] = {
            {0,                  "today"},
            {86399,              "today"},
            {86400,              "yesterday"},
            {2 * 86400LL,        "2d ago"},
            {7 * 86400LL,        "7d ago"},
            {13 * 86400LL,       "13d ago"},
            {14 * 86400LL - 1,   "13d ago"},
            {14 * 86400LL,       "2w ago"},
            {59 * 86400LL,       "8w ago"},
            {60 * 86400LL - 1,   "8w ago"},
            {60 * 86400LL,       "2mo ago"},
            {364 * 86400LL,      "12mo ago"},
            {365 * 86400LL - 1,  "12mo ago"},
            {365 * 86400LL,      "1y ago"},
            {-1,                 "today"},  // clamped at 0
        };
        for (const auto &c : cases) {
            const QString got = humanAge(c.age);
            if (got != QString::fromUtf8(c.expected))
                // Non-aborting so every boundary case reports in one run
                // (ANTS-1467/2065 — loop-internal abort removal).
                ADD_FAILURE_AT(__FILE__, __LINE__)
                    << "[INV-4] humanAge(" << static_cast<long long>(c.age)
                    << ") = \"" << got.toStdString() << "\", expected \""
                    << c.expected << "\"";
        }
    }

    // -----------------------------------------------------------------
    // Parser-layer assertions (INV-5, INV-6, INV-7+INV-8).
    // Skip gracefully when git is not on PATH.
    // -----------------------------------------------------------------
    if (!gitOnPath()) {
        std::fprintf(stderr,
            "OK — renderer-layer INVs hold. Parser-layer tests "
            "skipped (git not on PATH).\n");
        return 0;
    }

    // ANTS-1237-INV-6: non-git directory → empty hash. No git
    // commands are run inside this temp dir (no `git init`), so
    // git blame fails with "not a git repository".
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) fail("INV-6", "QTemporaryDir failed");
        const QString roadmapPath =
            writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
                      fixtureMarkdown());
        if (roadmapPath.isEmpty())
            fail("INV-6", "could not write fixture");
        const auto result =
            RoadmapDialog::parseLastTouchDates(roadmapPath);
        if (!result.isEmpty())
            fail("INV-6",
                "non-git directory returned non-empty hash — "
                "graceful-degradation path is broken.");
    }

    // ANTS-1237-INV-7 + INV-8: bullet-block MAX, audit-trail
    // exclusion. Set up a real git checkout with two distinct
    // commits at pinned timestamps; assert the 🚧 bullet's MAX
    // is T_A (the bullet's own commit), NOT T_B (the audit-trail
    // mention's commit).
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) fail("INV-7", "QTemporaryDir failed");
        const QString dir = tmp.path();
        const QString roadmapPath = dir + QStringLiteral("/ROADMAP.md");

        // git init + identity config (avoid hosts with no global git
        // identity).
        if (!runGit(dir, {QStringLiteral("init"),
                          QStringLiteral("--initial-branch=main")}))
            fail("INV-7", "git init failed");
        if (!runGit(dir, {QStringLiteral("config"),
                          QStringLiteral("user.name"),
                          QStringLiteral("test")}))
            fail("INV-7", "git config user.name failed");
        if (!runGit(dir, {QStringLiteral("config"),
                          QStringLiteral("user.email"),
                          QStringLiteral("test@example.com")}))
            fail("INV-7", "git config user.email failed");

        // Commit 1 — T_A = 2026-01-01T00:00:00Z = 1767225600.
        const QString T_A_iso = QStringLiteral("2026-01-01T00:00:00Z");
        const qint64 T_A = QDateTime::fromString(T_A_iso, Qt::ISODate)
                               .toSecsSinceEpoch();
        if (T_A != 1767225600)
            ADD_FAILURE_AT(__FILE__, __LINE__)
                << "[INV-7] QDateTime epoch for " << T_A_iso.toStdString()
                << " = " << static_cast<long long>(T_A)
                << "; expected 1767225600 — Qt::ISODate parser drifted?";
        const QString fileV1 = QStringLiteral(
            "# Sample Roadmap\n"
            "\n"
            "## 0.8.0\n"
            "\n"
            "- 🚧 [ANTS-9999] **Active card.**\n"
            "  Layman: in-progress item.\n"
            "  Body continuation line.\n"
            "  Kind: implement.\n");
        if (writeFile(roadmapPath, fileV1).isEmpty())
            fail("INV-7", "could not write fixture v1");
        if (!runGit(dir, {QStringLiteral("add"),
                          QStringLiteral("ROADMAP.md")}))
            fail("INV-7", "git add v1 failed");
        QProcessEnvironment envA = QProcessEnvironment::systemEnvironment();
        envA.insert(QStringLiteral("GIT_AUTHOR_DATE"), T_A_iso);
        envA.insert(QStringLiteral("GIT_COMMITTER_DATE"), T_A_iso);
        if (!runGit(dir, {QStringLiteral("commit"),
                          QStringLiteral("-m"),
                          QStringLiteral("add 9999")}, envA))
            fail("INV-7", "commit 1 failed");

        // Commit 2 — T_B = 2026-02-01T00:00:00Z = 1769904000.
        // Append the ✅ bullet with an audit-trail mention of
        // ANTS-9999.
        const QString T_B_iso = QStringLiteral("2026-02-01T00:00:00Z");
        const QString fileV2 = fileV1 + QStringLiteral(
            "\n"
            "- ✅ [ANTS-9998] **Shipped card.**\n"
            "  Layman: shipped item (mentions ANTS-9999 in audit trail).\n"
            "  Kind: fix.\n");
        if (writeFile(roadmapPath, fileV2).isEmpty())
            fail("INV-7", "could not write fixture v2");
        if (!runGit(dir, {QStringLiteral("add"),
                          QStringLiteral("ROADMAP.md")}))
            fail("INV-7", "git add v2 failed");
        QProcessEnvironment envB = QProcessEnvironment::systemEnvironment();
        envB.insert(QStringLiteral("GIT_AUTHOR_DATE"), T_B_iso);
        envB.insert(QStringLiteral("GIT_COMMITTER_DATE"), T_B_iso);
        if (!runGit(dir, {QStringLiteral("commit"),
                          QStringLiteral("-m"),
                          QStringLiteral("add 9998 audit-trail")}, envB))
            fail("INV-7", "commit 2 failed");

        // Run the parser; assert ANTS-9999 → T_A (not T_B).
        const auto result =
            RoadmapDialog::parseLastTouchDates(roadmapPath);
        if (!result.contains(QStringLiteral("ANTS-9999")))
            ADD_FAILURE_AT(__FILE__, __LINE__)
                << "[INV-7] parseLastTouchDates did NOT return ANTS-9999. "
                   "Hash size = " << static_cast<long long>(result.size())
                << ".";
        const qint64 got = result.value(QStringLiteral("ANTS-9999"));
        if (got != T_A)
            // Non-aborting so INV-5 (cache mtime) still runs (ANTS-1467/2065).
            ADD_FAILURE_AT(__FILE__, __LINE__)
                << "[INV-7/8] ANTS-9999 timestamp = "
                << static_cast<long long>(got) << "; expected T_A = "
                << static_cast<long long>(T_A)
                << " (NOT T_B = 1769904000). Either block-walk over-runs "
                   "(INV-7) or audit-trail mention contributes (INV-8).";

        // ANTS-1237-INV-5: cache mtime accessor.
        // Build a RoadmapDialog and verify the cache mtime
        // increases only when the file changes.
        RoadmapDialog dlg(roadmapPath, QStringLiteral("light"));
        dlg.refreshLastTouchDatesIfStale();
        const qint64 mtime1 = dlg.lastTouchDatesMtime();
        if (mtime1 < 0)
            fail("INV-5",
                "lastTouchDatesMtime() returned -1 after first refresh "
                "— refreshLastTouchDatesIfStale isn't writing back.");
        // Second call without touching the file → mtime unchanged.
        dlg.refreshLastTouchDatesIfStale();
        if (dlg.lastTouchDatesMtime() != mtime1)
            fail("INV-5",
                "lastTouchDatesMtime() changed across two refresh "
                "calls when the file was not touched — cache is "
                "re-parsing on every call.");
    }

    std::fprintf(stderr,
        "OK — RoadmapDialog 🚧 'Updated N days ago' INVs hold.\n");
    return 0;
}

TEST(RoadmapInProgressAge, Main) {
    ASSERT_EQ(0, runMain());
}
