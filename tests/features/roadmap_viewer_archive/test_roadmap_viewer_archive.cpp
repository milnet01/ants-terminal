// Feature-conformance test for tests/features/roadmap_viewer_archive/spec.md.
//
// Locks ANTS-1125 — the per-version archive feature for RoadmapDialog.
// 13 invariants spanning the static helpers (archiveDirFor / loadMarkdown
// / shouldLoadHistory) plus source-grep guards for the rebuild() wiring,
// the watcher hookup, the IPC-scope guard-rail, and the format-standard
// amendment. INV-14's rotation runs through roadmap_log op:"rotate_minor"
// (tests/features/roadmap_rotate_minor); packaging/rotate-roadmap.sh and its
// INV-14a..d clauses were retired by ANTS-5647.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include "support/testspawn.h"

#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    ADD_FAILURE_AT(__FILE__, __LINE__) << "[" << label << "] " << why;
    return 1;
}

// Write a synthetic file under `dir` and return its absolute path.
QString writeFile(const QString &dir, const QString &name,
                  const QByteArray &body) {
    const QString p = dir + QStringLiteral("/") + name;
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(body);
    return p;
}

}  // namespace

static int runMain() {
    // QCoreApplication app(argc, argv);  // ANTS-1217: bundle_main creates the app

    const std::string source = ants_test::slurpFile(ROADMAPDIALOG_CPP);
    const std::string header = ants_test::slurpFile(ROADMAPDIALOG_H);
    const std::string standard = ants_test::slurpFile(ROADMAP_FORMAT_MD);
    if (source.empty()) fail("INV-1", "roadmapdialog.cpp not readable");
    if (header.empty()) fail("INV-1", "roadmapdialog.h not readable");
    if (standard.empty())
        fail("INV-13", "roadmap-format.md not readable");

    using Preset = RoadmapDialog::Preset;

    // INV-1: archiveDirFor(canonical-resolved path)/docs/roadmap/ on
    // a real layout. We construct one in a temp dir.
    // ANTS-2176 — `baseTmp`/`baseRoot`: distinct from the per-INV inner
    // blocks below (which each declare their own `tmp`), so the outer pair
    // staying in scope no longer trips -Wshadow=compatible-local.
    QTemporaryDir baseTmp;
    if (!baseTmp.isValid()) fail("INV-1", "QTemporaryDir not created");
    const QString baseRoot = baseTmp.path();
    QDir(baseRoot).mkpath(QStringLiteral("docs/roadmap"));
    const QString roadmapPath =
        writeFile(baseRoot, QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral("# Current\n\n- 📋 [ANTS-9999] Test\n"));
    if (roadmapPath.isEmpty()) fail("INV-1", "ROADMAP.md not writable");
    {
        const QString got = RoadmapDialog::archiveDirFor(roadmapPath);
        const QString want = baseRoot + QStringLiteral("/docs/roadmap");
        if (QFileInfo(got).canonicalFilePath() !=
            QFileInfo(want).canonicalFilePath())
            fail("INV-1", "archiveDirFor() did not resolve to docs/roadmap");
    }

    // INV-1 negation A: no docs/ at all → empty.
    {
        QTemporaryDir tmpNo;
        if (!tmpNo.isValid()) fail("INV-1", "tmpNo not created");
        const QString solo = writeFile(tmpNo.path(),
                                       QStringLiteral("ROADMAP.md"),
                                       QByteArrayLiteral("# x\n"));
        if (!RoadmapDialog::archiveDirFor(solo).isEmpty())
            fail("INV-1", "missing docs/ should yield empty");
    }
    // INV-1 negation B: docs/roadmap is a regular file, not a dir.
    {
        QTemporaryDir tmpFile;
        if (!tmpFile.isValid()) fail("INV-1", "tmpFile not created");
        const QString r2 = writeFile(tmpFile.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# x\n"));
        QDir(tmpFile.path()).mkpath(QStringLiteral("docs"));
        writeFile(tmpFile.path() + QStringLiteral("/docs"),
                  QStringLiteral("roadmap"),  // regular file masquerading
                  QByteArrayLiteral("not a dir"));
        if (!RoadmapDialog::archiveDirFor(r2).isEmpty())
            fail("INV-1", "regular-file shadow should yield empty");
    }

    // INV-1a: broken symlink → empty.
    {
        QTemporaryDir tmpSym;
        if (!tmpSym.isValid()) fail("INV-1a", "tmpSym not created");
        const QString r3 = writeFile(tmpSym.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# x\n"));
        QDir(tmpSym.path()).mkpath(QStringLiteral("docs"));
        const QString broken =
            tmpSym.path() + QStringLiteral("/docs/roadmap");
        QFile::link(QStringLiteral("/no/such/target"), broken);
        if (!RoadmapDialog::archiveDirFor(r3).isEmpty())
            fail("INV-1a", "broken symlink should yield empty");
    }

    // INV-2: loadMarkdown(includeArchive=false) returns just the
    // current file's content. Independent of archive presence.
    writeFile(baseRoot + QStringLiteral("/docs/roadmap"),
              QStringLiteral("0.7.md"),
              QByteArrayLiteral("# 0.7\n\n- ✅ [ANTS-7777] shipped\n"));
    {
        const QString got = RoadmapDialog::loadMarkdown(roadmapPath, false);
        if (!got.contains(QStringLiteral("ANTS-9999")))
            fail("INV-2", "current file content missing");
        if (got.contains(QStringLiteral("ANTS-7777")))
            fail("INV-2", "archive bled in with includeArchive=false");
    }

    // INV-3: loadMarkdown(true) with no archive dir == loadMarkdown(false).
    {
        QTemporaryDir tmpNo;
        if (!tmpNo.isValid()) fail("INV-3", "tmpNo not created");
        const QString solo = writeFile(tmpNo.path(),
                                       QStringLiteral("ROADMAP.md"),
                                       QByteArrayLiteral("# only\n"));
        const QString a = RoadmapDialog::loadMarkdown(solo, false);
        const QString b = RoadmapDialog::loadMarkdown(solo, true);
        if (a != b)
            fail("INV-3", "missing archive dir should be a no-op");
    }

    // INV-3b: loadMarkdown(true) with an empty archive dir == false.
    {
        QTemporaryDir tmpEmpty;
        if (!tmpEmpty.isValid()) fail("INV-3b", "tmp not created");
        QDir(tmpEmpty.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r4 = writeFile(tmpEmpty.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# only\n"));
        const QString a = RoadmapDialog::loadMarkdown(r4, false);
        const QString b = RoadmapDialog::loadMarkdown(r4, true);
        if (a != b)
            fail("INV-3b", "empty archive dir should match missing-dir");
    }

    // INV-4 + INV-11: numeric descending sort. Place 0.5/0.6/0.7/0.10
    // and verify the order is 0.10, 0.7, 0.6, 0.5 — confirming both
    // multi-archive ordering AND the minor-10 lexical-trap regression.
    {
        QTemporaryDir tmpSort;
        if (!tmpSort.isValid()) fail("INV-4", "tmp not created");
        QDir(tmpSort.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r5 = writeFile(tmpSort.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# current\n"));
        writeFile(tmpSort.path() + QStringLiteral("/docs/roadmap"),
                  QStringLiteral("0.5.md"),
                  QByteArrayLiteral("# tag-0.5\n"));
        writeFile(tmpSort.path() + QStringLiteral("/docs/roadmap"),
                  QStringLiteral("0.6.md"),
                  QByteArrayLiteral("# tag-0.6\n"));
        writeFile(tmpSort.path() + QStringLiteral("/docs/roadmap"),
                  QStringLiteral("0.7.md"),
                  QByteArrayLiteral("# tag-0.7\n"));
        writeFile(tmpSort.path() + QStringLiteral("/docs/roadmap"),
                  QStringLiteral("0.10.md"),
                  QByteArrayLiteral("# tag-0.10\n"));
        const QString assembled =
            RoadmapDialog::loadMarkdown(r5, true);
        const int p10 = assembled.indexOf(QStringLiteral("tag-0.10"));
        const int p07 = assembled.indexOf(QStringLiteral("tag-0.7"));
        const int p06 = assembled.indexOf(QStringLiteral("tag-0.6"));
        const int p05 = assembled.indexOf(QStringLiteral("tag-0.5"));
        if (p10 < 0 || p07 < 0 || p06 < 0 || p05 < 0)
            fail("INV-4", "one of the archives didn't land");
        if (!(p10 < p07 && p07 < p06 && p06 < p05))
            fail("INV-4/INV-11",
                        "numeric desc order broken (saw lex sort?)");
        // Sentinel separator before each archive.
        if (!assembled.contains(QStringLiteral("---\n\n<!-- archive: 0.10.md -->")))
            fail("INV-4", "thematic-break sentinel missing for 0.10.md");
        if (!assembled.contains(QStringLiteral("<!-- archive: 0.7.md -->")))
            fail("INV-4", "comment sentinel missing for 0.7.md");
    }

    // INV-4a: filename filter rejects non-matching entries silently.
    {
        QTemporaryDir tmpFilter;
        if (!tmpFilter.isValid()) fail("INV-4a", "tmp not created");
        QDir(tmpFilter.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r6 = writeFile(tmpFilter.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# current\n"));
        const QString archDir = tmpFilter.path()
            + QStringLiteral("/docs/roadmap");
        writeFile(archDir, QStringLiteral("0.7.md"),
                  QByteArrayLiteral("# good-0.7\n"));
        writeFile(archDir, QStringLiteral("0.7.0.md"),
                  QByteArrayLiteral("BAD-PATCH-SUFFIX\n"));
        writeFile(archDir, QStringLiteral("latest.md"),
                  QByteArrayLiteral("BAD-NAME\n"));
        writeFile(archDir, QStringLiteral("0.7.MD"),
                  QByteArrayLiteral("BAD-CASE\n"));
        writeFile(archDir, QStringLiteral("0.7.md.bak"),
                  QByteArrayLiteral("BAD-EXT\n"));
        writeFile(archDir, QStringLiteral("README.md"),
                  QByteArrayLiteral("BAD-NAME-2\n"));
        const QString assembled = RoadmapDialog::loadMarkdown(r6, true);
        if (!assembled.contains(QStringLiteral("good-0.7")))
            fail("INV-4a", "valid 0.7.md was filtered out");
        if (assembled.contains(QStringLiteral("BAD-PATCH-SUFFIX")))
            fail("INV-4a", "0.7.0.md leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-NAME")))
            fail("INV-4a", "latest.md leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-CASE")))
            fail("INV-4a", "0.7.MD (uppercase) leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-EXT")))
            fail("INV-4a", "0.7.md.bak leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-NAME-2")))
            fail("INV-4a", "README.md leaked through filter");
    }

    // INV-4b (ANTS-4073): phase archives `P<N>[.<sub>].md` load too, as a
    // strictly older class after every version archive, descending by
    // (phase, sub). `FP03.md` and lower-case `p01.md` are not phase archives.
    {
        QTemporaryDir tmpPhase;
        if (!tmpPhase.isValid()) fail("INV-4b", "tmp not created");
        QDir(tmpPhase.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r7 = writeFile(tmpPhase.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# current\n"));
        const QString archDir = tmpPhase.path()
            + QStringLiteral("/docs/roadmap");
        writeFile(archDir, QStringLiteral("P01.md"), QByteArrayLiteral("tag-P01\n"));
        writeFile(archDir, QStringLiteral("P07.md"), QByteArrayLiteral("tag-P07\n"));
        writeFile(archDir, QStringLiteral("P07.5.md"), QByteArrayLiteral("tag-P07.5\n"));
        writeFile(archDir, QStringLiteral("P10.md"), QByteArrayLiteral("tag-P10\n"));
        writeFile(archDir, QStringLiteral("1.0.md"), QByteArrayLiteral("tag-1.0\n"));
        writeFile(archDir, QStringLiteral("FP03.md"), QByteArrayLiteral("BAD-FOLDIN\n"));
        writeFile(archDir, QStringLiteral("p02.md"), QByteArrayLiteral("BAD-LOWER\n"));
        const QString assembled = RoadmapDialog::loadMarkdown(r7, true);
        const int v10 = assembled.indexOf(QStringLiteral("tag-1.0"));
        const int p10 = assembled.indexOf(QStringLiteral("tag-P10"));
        const int p75 = assembled.indexOf(QStringLiteral("tag-P07.5"));
        const int p07 = assembled.indexOf(QStringLiteral("tag-P07\n"));
        const int p01 = assembled.indexOf(QStringLiteral("tag-P01"));
        if (v10 < 0 || p10 < 0 || p75 < 0 || p07 < 0 || p01 < 0)
            fail("INV-4b", "a phase archive was filtered out");
        if (!(v10 < p10 && p10 < p75 && p75 < p07 && p07 < p01))
            fail("INV-4b", "versions first, then phases descending by (phase, sub)");
        if (!assembled.contains(QStringLiteral("<!-- archive: P07.5.md -->")))
            fail("INV-4b", "comment sentinel missing for P07.5.md");
        if (assembled.contains(QStringLiteral("BAD-FOLDIN")))
            fail("INV-4b", "FP03.md leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-LOWER")))
            fail("INV-4b", "p02.md (lower case) leaked through filter");
    }

    // INV-5: per-file 8 MiB cap is a literal in the helper.
    if (!contains(source, "8 * 1024 * 1024"))
        fail("INV-5", "kPerFileCap (8 * 1024 * 1024) literal missing");
    // INV-5a: total 64 MiB cap literal.
    if (!contains(source, "64 * 1024 * 1024"))
        fail("INV-5a", "kAssembledCap (64 * 1024 * 1024) literal missing");
    // INV-5a sentinel string.
    if (!contains(source, "truncated past 64 MiB cap"))
        fail("INV-5a", "truncation sentinel string not in source");

    // INV-6: shouldLoadHistory triggers correctly.
    if (!RoadmapDialog::shouldLoadHistory(Preset::History, QString()))
        fail("INV-6", "History preset alone should trigger");
    if (RoadmapDialog::shouldLoadHistory(Preset::Full, QString()))
        fail("INV-6", "Full preset alone should NOT trigger");
    if (RoadmapDialog::shouldLoadHistory(Preset::Full, QStringLiteral("   ")))
        fail("INV-6", "whitespace-only search should NOT trigger");
    if (!RoadmapDialog::shouldLoadHistory(Preset::Full, QStringLiteral("OSC")))
        fail("INV-6", "non-empty search should trigger from any preset");
    if (RoadmapDialog::shouldLoadHistory(Preset::Custom, QString()))
        fail("INV-6", "Custom preset alone should NOT trigger");

    // INV-7: rebuild() consults wantsHistoryLoad() before loadMarkdown.
    // A prefix: ANTS-5088 added an error out-parameter after the flag.
    if (!contains(source, "loadMarkdown(m_roadmapPath, includeArchive") &&
        !contains(source, "loadRoadmapMarkdown(includeArchive"))
        fail("INV-7", "rebuild() does not gate loadMarkdown on wantsHistoryLoad");
    if (!contains(source, "wantsHistoryLoad()"))
        fail("INV-7", "wantsHistoryLoad() not invoked from rebuild()");

    // INV-8: directoryChanged is connected to scheduleRebuild
    // (the SAME slot fileChanged routes to).
    {
        // Pull the bytes between the two connect lines and require both.
        if (!contains(source,
                "QFileSystemWatcher::fileChanged") ||
            !contains(source,
                "QFileSystemWatcher::directoryChanged"))
            fail("INV-8", "fileChanged + directoryChanged signals not both connected");
        if (!contains(source, "scheduleRebuild"))
            fail("INV-8", "scheduleRebuild slot not referenced");
        // archive-dir watch path is guarded on non-empty result.
        if (!contains(source, "archiveDir.isEmpty()") &&
            !contains(source, "!archiveDir.isEmpty()"))
            fail("INV-3a/INV-8", "archive watcher not guarded on isEmpty()");
    }

    // INV-9: cross-archive ID search through parseBullets sees the
    // archive-only ID exactly once.
    {
        QTemporaryDir tmpId;
        if (!tmpId.isValid()) fail("INV-9", "tmp not created");
        QDir(tmpId.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r7 = writeFile(tmpId.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral(
            "## Current\n\n- 📋 [ANTS-2222] **only-current** body.\n"));
        writeFile(tmpId.path() + QStringLiteral("/docs/roadmap"),
                  QStringLiteral("0.5.md"),
                  QByteArrayLiteral(
            "## 0.5.0\n\n- ✅ [ANTS-1042] **only-archive** body.\n"));
        const QString assembled = RoadmapDialog::loadMarkdown(r7, true);
        const auto bullets = RoadmapDialog::parseBullets(assembled);
        int hits = 0;
        for (const auto &b : bullets) {
            if (b.id == QStringLiteral("ANTS-1042")) ++hits;
        }
        if (hits != 1)
            fail("INV-9", "archive-only ID not found exactly once");
    }

    // INV-10: History preset's load is strictly larger than Full's.
    {
        QTemporaryDir tmpGrow;
        if (!tmpGrow.isValid()) fail("INV-10", "tmp not created");
        QDir(tmpGrow.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r8 = writeFile(tmpGrow.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral(
            "## Current\n\n- 📋 [ANTS-3333] x\n"));
        // Make sure the archive is at least 100 bytes.
        QByteArray big;
        big.append("## 0.7.0\n\n");
        for (int i = 0; i < 50; ++i)
            big.append("- ✅ [ANTS-1000] shipped item\n");
        writeFile(tmpGrow.path() + QStringLiteral("/docs/roadmap"),
                  QStringLiteral("0.7.md"), big);
        const bool fullWants = RoadmapDialog::shouldLoadHistory(
            Preset::Full, QString());
        const bool historyWants = RoadmapDialog::shouldLoadHistory(
            Preset::History, QString());
        const QString full = RoadmapDialog::loadMarkdown(r8, fullWants);
        const QString hist = RoadmapDialog::loadMarkdown(r8, historyWants);
        if (hist.size() <= full.size() + 100)
            fail("INV-10",
                "History load not strictly larger by 100+ bytes");
    }

    // INV-12: guard-rail — `historyArchiveDir` (or `archiveDirFor`)
    // is referenced ONLY in roadmapdialog.{h,cpp} and the archive
    // test directory. Source-grep across src/ excluding the dialog.
    {
        // Open the project src/ directory and scan every .cpp / .h for
        // the static helper name `archiveDirFor`. Any leak outside
        // roadmapdialog.* is a contract break.
        QDir srcDir(SRC_DIR);
        const QStringList entries =
            srcDir.entryList(QStringList{QStringLiteral("*.cpp"),
                                         QStringLiteral("*.h")},
                             QDir::Files, QDir::NoSort);
        for (const QString &name : entries) {
            if (name == QStringLiteral("roadmapdialog.cpp") ||
                name == QStringLiteral("roadmapdialog.h")) continue;
            const std::string body = ants_test::slurpFile(
                (std::string(SRC_DIR) + "/" + name.toStdString()).c_str());
            if (contains(body, "archiveDirFor") ||
                contains(body, "historyArchiveDir")) {
                // Non-aborting so every leaking file is reported in one run
                // (ANTS-1467/2065 — loop-internal abort removal).
                ADD_FAILURE_AT(__FILE__, __LINE__)
                    << "[INV-12] archive helper leaked into "
                    << name.toStdString();
            }
        }
    }

    // INV-13: roadmap-format.md contains an "Archive rotation" heading
    // (case-insensitive).
    {
        std::string lower = standard;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (!contains(lower, "archive rotation"))
            fail("INV-13",
                "roadmap-format.md missing § Archive rotation");
    }

    std::fprintf(stderr, "OK — all archive INVs hold.\n");
    return 0;
}

TEST(RoadmapViewerArchive, Main) {
    ASSERT_EQ(0, runMain());
}
