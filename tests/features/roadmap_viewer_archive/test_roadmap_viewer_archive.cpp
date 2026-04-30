// Feature-conformance test for tests/features/roadmap_viewer_archive/spec.md.
//
// Locks ANTS-1125 — the per-version archive feature for RoadmapDialog.
// 13 invariants spanning the static helpers (archiveDirFor / loadMarkdown
// / shouldLoadHistory) plus source-grep guards for the rebuild() wiring,
// the watcher hookup, the IPC-scope guard-rail, and the format-standard
// amendment. INV-14 (`/bump` rotation) is exercised by a sibling test
// once the recipe lands; for now we source-grep the recipe stub.
//
// Exit 0 = all invariants hold.

#include "roadmapdialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>

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

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
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

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const std::string source = slurp(ROADMAPDIALOG_CPP);
    const std::string header = slurp(ROADMAPDIALOG_H);
    const std::string standard = slurp(ROADMAP_FORMAT_MD);
    if (source.empty()) return fail("INV-1", "roadmapdialog.cpp not readable");
    if (header.empty()) return fail("INV-1", "roadmapdialog.h not readable");
    if (standard.empty())
        return fail("INV-13", "roadmap-format.md not readable");

    using Preset = RoadmapDialog::Preset;

    // INV-1: archiveDirFor(canonical-resolved path)/docs/roadmap/ on
    // a real layout. We construct one in a temp dir.
    QTemporaryDir tmp;
    if (!tmp.isValid()) return fail("INV-1", "QTemporaryDir not created");
    const QString root = tmp.path();
    QDir(root).mkpath(QStringLiteral("docs/roadmap"));
    const QString roadmapPath =
        writeFile(root, QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral("# Current\n\n- 📋 [ANTS-9999] Test\n"));
    if (roadmapPath.isEmpty()) return fail("INV-1", "ROADMAP.md not writable");
    {
        const QString got = RoadmapDialog::archiveDirFor(roadmapPath);
        const QString want = root + QStringLiteral("/docs/roadmap");
        if (QFileInfo(got).canonicalFilePath() !=
            QFileInfo(want).canonicalFilePath())
            return fail("INV-1", "archiveDirFor() did not resolve to docs/roadmap");
    }

    // INV-1 negation A: no docs/ at all → empty.
    {
        QTemporaryDir tmpNo;
        if (!tmpNo.isValid()) return fail("INV-1", "tmpNo not created");
        const QString solo = writeFile(tmpNo.path(),
                                       QStringLiteral("ROADMAP.md"),
                                       QByteArrayLiteral("# x\n"));
        if (!RoadmapDialog::archiveDirFor(solo).isEmpty())
            return fail("INV-1", "missing docs/ should yield empty");
    }
    // INV-1 negation B: docs/roadmap is a regular file, not a dir.
    {
        QTemporaryDir tmpFile;
        if (!tmpFile.isValid()) return fail("INV-1", "tmpFile not created");
        const QString r2 = writeFile(tmpFile.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# x\n"));
        QDir(tmpFile.path()).mkpath(QStringLiteral("docs"));
        writeFile(tmpFile.path() + QStringLiteral("/docs"),
                  QStringLiteral("roadmap"),  // regular file masquerading
                  QByteArrayLiteral("not a dir"));
        if (!RoadmapDialog::archiveDirFor(r2).isEmpty())
            return fail("INV-1", "regular-file shadow should yield empty");
    }

    // INV-1a: broken symlink → empty.
    {
        QTemporaryDir tmpSym;
        if (!tmpSym.isValid()) return fail("INV-1a", "tmpSym not created");
        const QString r3 = writeFile(tmpSym.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# x\n"));
        QDir(tmpSym.path()).mkpath(QStringLiteral("docs"));
        const QString broken =
            tmpSym.path() + QStringLiteral("/docs/roadmap");
        QFile::link(QStringLiteral("/no/such/target"), broken);
        if (!RoadmapDialog::archiveDirFor(r3).isEmpty())
            return fail("INV-1a", "broken symlink should yield empty");
    }

    // INV-2: loadMarkdown(includeArchive=false) returns just the
    // current file's content. Independent of archive presence.
    writeFile(root + QStringLiteral("/docs/roadmap"),
              QStringLiteral("0.7.md"),
              QByteArrayLiteral("# 0.7\n\n- ✅ [ANTS-7777] shipped\n"));
    {
        const QString got = RoadmapDialog::loadMarkdown(roadmapPath, false);
        if (!got.contains(QStringLiteral("ANTS-9999")))
            return fail("INV-2", "current file content missing");
        if (got.contains(QStringLiteral("ANTS-7777")))
            return fail("INV-2", "archive bled in with includeArchive=false");
    }

    // INV-3: loadMarkdown(true) with no archive dir == loadMarkdown(false).
    {
        QTemporaryDir tmpNo;
        if (!tmpNo.isValid()) return fail("INV-3", "tmpNo not created");
        const QString solo = writeFile(tmpNo.path(),
                                       QStringLiteral("ROADMAP.md"),
                                       QByteArrayLiteral("# only\n"));
        const QString a = RoadmapDialog::loadMarkdown(solo, false);
        const QString b = RoadmapDialog::loadMarkdown(solo, true);
        if (a != b)
            return fail("INV-3", "missing archive dir should be a no-op");
    }

    // INV-3b: loadMarkdown(true) with an empty archive dir == false.
    {
        QTemporaryDir tmpEmpty;
        if (!tmpEmpty.isValid()) return fail("INV-3b", "tmp not created");
        QDir(tmpEmpty.path()).mkpath(QStringLiteral("docs/roadmap"));
        const QString r4 = writeFile(tmpEmpty.path(),
                                     QStringLiteral("ROADMAP.md"),
                                     QByteArrayLiteral("# only\n"));
        const QString a = RoadmapDialog::loadMarkdown(r4, false);
        const QString b = RoadmapDialog::loadMarkdown(r4, true);
        if (a != b)
            return fail("INV-3b", "empty archive dir should match missing-dir");
    }

    // INV-4 + INV-11: numeric descending sort. Place 0.5/0.6/0.7/0.10
    // and verify the order is 0.10, 0.7, 0.6, 0.5 — confirming both
    // multi-archive ordering AND the minor-10 lexical-trap regression.
    {
        QTemporaryDir tmpSort;
        if (!tmpSort.isValid()) return fail("INV-4", "tmp not created");
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
            return fail("INV-4", "one of the archives didn't land");
        if (!(p10 < p07 && p07 < p06 && p06 < p05))
            return fail("INV-4/INV-11",
                        "numeric desc order broken (saw lex sort?)");
        // Sentinel separator before each archive.
        if (!assembled.contains(QStringLiteral("---\n\n<!-- archive: 0.10.md -->")))
            return fail("INV-4", "thematic-break sentinel missing for 0.10.md");
        if (!assembled.contains(QStringLiteral("<!-- archive: 0.7.md -->")))
            return fail("INV-4", "comment sentinel missing for 0.7.md");
    }

    // INV-4a: filename filter rejects non-matching entries silently.
    {
        QTemporaryDir tmpFilter;
        if (!tmpFilter.isValid()) return fail("INV-4a", "tmp not created");
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
            return fail("INV-4a", "valid 0.7.md was filtered out");
        if (assembled.contains(QStringLiteral("BAD-PATCH-SUFFIX")))
            return fail("INV-4a", "0.7.0.md leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-NAME")))
            return fail("INV-4a", "latest.md leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-CASE")))
            return fail("INV-4a", "0.7.MD (uppercase) leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-EXT")))
            return fail("INV-4a", "0.7.md.bak leaked through filter");
        if (assembled.contains(QStringLiteral("BAD-NAME-2")))
            return fail("INV-4a", "README.md leaked through filter");
    }

    // INV-5: per-file 8 MiB cap is a literal in the helper.
    if (!contains(source, "8 * 1024 * 1024"))
        return fail("INV-5", "kPerFileCap (8 * 1024 * 1024) literal missing");
    // INV-5a: total 64 MiB cap literal.
    if (!contains(source, "64 * 1024 * 1024"))
        return fail("INV-5a", "kAssembledCap (64 * 1024 * 1024) literal missing");
    // INV-5a sentinel string.
    if (!contains(source, "truncated past 64 MiB cap"))
        return fail("INV-5a", "truncation sentinel string not in source");

    // INV-6: shouldLoadHistory triggers correctly.
    if (!RoadmapDialog::shouldLoadHistory(Preset::History, QString()))
        return fail("INV-6", "History preset alone should trigger");
    if (RoadmapDialog::shouldLoadHistory(Preset::Full, QString()))
        return fail("INV-6", "Full preset alone should NOT trigger");
    if (RoadmapDialog::shouldLoadHistory(Preset::Full, QStringLiteral("   ")))
        return fail("INV-6", "whitespace-only search should NOT trigger");
    if (!RoadmapDialog::shouldLoadHistory(Preset::Full, QStringLiteral("OSC")))
        return fail("INV-6", "non-empty search should trigger from any preset");
    if (RoadmapDialog::shouldLoadHistory(Preset::Custom, QString()))
        return fail("INV-6", "Custom preset alone should NOT trigger");

    // INV-7: rebuild() consults wantsHistoryLoad() before loadMarkdown.
    if (!contains(source, "loadMarkdown(m_roadmapPath, includeArchive)") &&
        !contains(source, "loadRoadmapMarkdown(includeArchive)"))
        return fail("INV-7", "rebuild() does not gate loadMarkdown on wantsHistoryLoad");
    if (!contains(source, "wantsHistoryLoad()"))
        return fail("INV-7", "wantsHistoryLoad() not invoked from rebuild()");

    // INV-8: directoryChanged is connected to scheduleRebuild
    // (the SAME slot fileChanged routes to).
    {
        // Pull the bytes between the two connect lines and require both.
        if (!contains(source,
                "QFileSystemWatcher::fileChanged") ||
            !contains(source,
                "QFileSystemWatcher::directoryChanged"))
            return fail("INV-8", "fileChanged + directoryChanged signals not both connected");
        if (!contains(source, "scheduleRebuild"))
            return fail("INV-8", "scheduleRebuild slot not referenced");
        // archive-dir watch path is guarded on non-empty result.
        if (!contains(source, "archiveDir.isEmpty()") &&
            !contains(source, "!archiveDir.isEmpty()"))
            return fail("INV-3a/INV-8", "archive watcher not guarded on isEmpty()");
    }

    // INV-9: cross-archive ID search through parseBullets sees the
    // archive-only ID exactly once.
    {
        QTemporaryDir tmpId;
        if (!tmpId.isValid()) return fail("INV-9", "tmp not created");
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
            return fail("INV-9", "archive-only ID not found exactly once");
    }

    // INV-10: History preset's load is strictly larger than Full's.
    {
        QTemporaryDir tmpGrow;
        if (!tmpGrow.isValid()) return fail("INV-10", "tmp not created");
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
            return fail("INV-10",
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
            const std::string body = slurp(
                (std::string(SRC_DIR) + "/" + name.toStdString()).c_str());
            if (contains(body, "archiveDirFor") ||
                contains(body, "historyArchiveDir")) {
                std::fprintf(stderr,
                    "[INV-12] FAIL: archive helper leaked into %s\n",
                    name.toUtf8().constData());
                return 1;
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
            return fail("INV-13",
                "roadmap-format.md missing § Archive rotation");
    }

    // INV-14 cluster — `/bump` rotation contract. Drives the
    // packaging/rotate-roadmap.sh script against synthetic fixtures.
    const QString rotateScript = QStringLiteral(ROTATE_ROADMAP_SH);
    if (!QFileInfo(rotateScript).isExecutable())
        return fail("INV-14", "rotate-roadmap.sh not executable");

    auto runRotate = [&](const QString &cwd, const QStringList &args)
            -> std::pair<int, QString> {
        QProcess p;
        p.setWorkingDirectory(cwd);
        p.setProgram(QStringLiteral("bash"));
        QStringList full;
        full << rotateScript;
        full += args;
        p.setArguments(full);
        p.start();
        if (!p.waitForFinished(5000)) {
            p.kill();
            return {-1, QStringLiteral("timeout")};
        }
        return {p.exitCode(),
                QString::fromUtf8(p.readAllStandardError())};
    };

    auto writeText = [](const QString &dir, const QString &name,
                        const QByteArray &body) -> QString {
        const QString p = dir + QStringLiteral("/") + name;
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        f.write(body);
        return p;
    };

    auto readText = [](const QString &p) -> QByteArray {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return f.readAll();
    };

    // INV-14 + INV-14d (multi-section + content-preserving):
    // compose three sub-headings in 0.7 + one in 0.8, rotate 0.7,
    // verify all three rotated and 0.8 stayed.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-14", "tmp not created");
        const QString root = tmp.path();
        const QByteArray body =
            "# Test\n\n"
            "## 0.7.0 — first\n\n- ✅ [ANTS-7100] one\n\n"
            "## 0.7.7 — middle\n\n- ✅ [ANTS-7177] two\n\n"
            "## 0.7.12 — last\n\n- ✅ [ANTS-7112] three\n\n"
            "## 0.8.0 — open\n\n- 📋 [ANTS-8000] open work\n";
        writeText(root, QStringLiteral("ROADMAP.md"), body);
        const auto [rc, err] =
            runRotate(root, {QStringLiteral("0.7")});
        if (rc != 0)
            return fail("INV-14", "rotation exited non-zero");

        const QByteArray archive =
            readText(root + QStringLiteral("/docs/roadmap/0.7.md"));
        const QByteArray remaining =
            readText(root + QStringLiteral("/ROADMAP.md"));
        // (a) all three sub-bullets in archive byte-identical.
        if (!archive.contains("ANTS-7100") ||
            !archive.contains("ANTS-7177") ||
            !archive.contains("ANTS-7112"))
            return fail("INV-14", "rotated archive missing one of three sub-bullets");
        // (b) those bullets + the 0.7.0 heading absent from ROADMAP.
        if (remaining.contains("ANTS-7100") ||
            remaining.contains("ANTS-7177") ||
            remaining.contains("ANTS-7112"))
            return fail("INV-14", "bullet leaked into post-rotation ROADMAP.md");
        if (remaining.contains("## 0.7.0") ||
            remaining.contains("## 0.7.7") ||
            remaining.contains("## 0.7.12"))
            return fail("INV-14", "closed-minor heading leaked into post-rotation");
        // 0.8 stays put.
        if (!remaining.contains("ANTS-8000") ||
            !remaining.contains("## 0.8.0"))
            return fail("INV-14d", "0.8 section accidentally rotated");
        // (c) parseBullets still works on the post-rotation file.
        const auto parsed =
            RoadmapDialog::parseBullets(QString::fromUtf8(remaining));
        bool found8000 = false;
        for (const auto &b : parsed) {
            if (b.id == QStringLiteral("ANTS-8000")) found8000 = true;
        }
        if (!found8000)
            return fail("INV-14", "post-rotation ROADMAP.md doesn't parse cleanly");
    }

    // INV-14a CLI shape: invalid arg shape exits non-zero.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-14a", "tmp not created");
        writeText(tmp.path(), QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral("# x\n## 0.7.0\n- a\n"));
        const auto bad1 = runRotate(tmp.path(), {QStringLiteral("0.7.0")});
        if (bad1.first == 0) return fail("INV-14a", "0.7.0 (patch suffix) accepted");
        const auto bad2 = runRotate(tmp.path(), {QStringLiteral("latest")});
        if (bad2.first == 0) return fail("INV-14a", "'latest' accepted");
        const auto bad3 = runRotate(tmp.path(), {QStringLiteral("v0.7")});
        if (bad3.first == 0) return fail("INV-14a", "'v0.7' accepted");
        const auto bad4 = runRotate(tmp.path(), QStringList{});
        if (bad4.first == 0) return fail("INV-14a", "no-arg accepted");
    }

    // INV-14a regex-escape: `## 0X7` must NOT match `0.7`.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-14a", "tmp not created");
        writeText(tmp.path(), QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral(
            "# x\n## 0X7.5 — should not match\n- a\n"));
        const auto rc = runRotate(tmp.path(), {QStringLiteral("0.7")});
        if (rc.first != 0)
            return fail("INV-14a", "rotation script crashed on no-match");
        // ROADMAP.md should be unchanged because no `## 0.7.` matched.
        const QByteArray after =
            readText(tmp.path() + QStringLiteral("/ROADMAP.md"));
        if (!after.contains("0X7.5"))
            return fail("INV-14a", "regex escape failed — 0X7 matched 0.7");
    }

    // INV-14b idempotence + no-clobber. First run rotates; manual
    // edit to archive; second run preserves the manual edit.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-14b", "tmp not created");
        writeText(tmp.path(), QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral(
            "# x\n## 0.5.0\n- a\n## 0.8.0\n- b\n"));
        runRotate(tmp.path(), {QStringLiteral("0.5")});
        const QString archivePath =
            tmp.path() + QStringLiteral("/docs/roadmap/0.5.md");
        // Hand-edit the archive.
        {
            QFile f(archivePath);
            if (!f.open(QIODevice::Append))
                return fail("INV-14b", "archive not appendable");
            f.write("HAND_EDIT_LINE\n");
        }
        const QByteArray before = readText(archivePath);
        // Re-paste a 0.5.0 section into ROADMAP.md so the script has
        // something to snip on the second run.
        writeText(tmp.path(), QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral(
            "# x\n## 0.5.0 — re-pasted\n- z\n## 0.8.0\n- b\n"));
        const auto rc = runRotate(tmp.path(), {QStringLiteral("0.5")});
        if (rc.first != 0)
            return fail("INV-14b", "second run exited non-zero");
        const QByteArray after = readText(archivePath);
        if (before != after)
            return fail("INV-14b", "archive was clobbered on re-run");
        // ROADMAP.md should still have the snip happen — the
        // re-pasted section should be gone.
        const QByteArray remaining =
            readText(tmp.path() + QStringLiteral("/ROADMAP.md"));
        if (remaining.contains("re-pasted"))
            return fail("INV-14b", "re-pasted section not snipped");
    }

    // INV-14b1 sequential rotation of two minors.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-14b1", "tmp not created");
        writeText(tmp.path(), QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral(
            "# x\n## 0.5.0\n- five\n## 0.6.0\n- six\n## 0.7.0\n- seven\n"));
        const auto rc1 = runRotate(tmp.path(), {QStringLiteral("0.5")});
        const auto rc2 = runRotate(tmp.path(), {QStringLiteral("0.6")});
        if (rc1.first != 0 || rc2.first != 0)
            return fail("INV-14b1", "sequential rotation non-zero");
        const QByteArray a5 = readText(tmp.path()
            + QStringLiteral("/docs/roadmap/0.5.md"));
        const QByteArray a6 = readText(tmp.path()
            + QStringLiteral("/docs/roadmap/0.6.md"));
        if (!a5.contains("five") || a5.contains("six"))
            return fail("INV-14b1", "0.5 archive cross-contaminated");
        if (!a6.contains("six") || a6.contains("five"))
            return fail("INV-14b1", "0.6 archive cross-contaminated");
        const QByteArray rem =
            readText(tmp.path() + QStringLiteral("/ROADMAP.md"));
        if (rem.contains("five") || rem.contains("six"))
            return fail("INV-14b1", "rotated bullets remain in ROADMAP.md");
        if (!rem.contains("seven"))
            return fail("INV-14b1", "0.7.0 section accidentally rotated");
    }

    // INV-14c source-grep both atomic-write paths.
    {
        const std::string body = slurp(rotateScript.toUtf8().constData());
        if (body.empty())
            return fail("INV-14c", "rotate-roadmap.sh not readable");
        // Two distinct mktemp-then-mv pairs: one for the archive, one
        // for the ROADMAP.md rewrite.
        size_t mktempCount = 0, pos = 0;
        while ((pos = body.find("mktemp", pos)) != std::string::npos) {
            ++mktempCount;
            ++pos;
        }
        if (mktempCount < 2)
            return fail("INV-14c",
                "expected ≥2 mktemp invocations (archive + roadmap atomic writes)");
        if (!contains(body, "mv "))
            return fail("INV-14c", "mv (POSIX rename) call missing");
    }

    // INV-14d EOF case — closed minor runs to file end.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-14d", "tmp not created");
        writeText(tmp.path(), QStringLiteral("ROADMAP.md"),
                  QByteArrayLiteral(
            "# x\n## 0.7.0 — last section\n- a\n- b\n"));
        const auto rc = runRotate(tmp.path(), {QStringLiteral("0.7")});
        if (rc.first != 0)
            return fail("INV-14d", "EOF-case rotation non-zero");
        const QByteArray archive = readText(tmp.path()
            + QStringLiteral("/docs/roadmap/0.7.md"));
        if (!archive.contains("last section") ||
            !archive.contains("- a") || !archive.contains("- b"))
            return fail("INV-14d", "EOF section body not fully rotated");
    }

    std::fprintf(stderr, "OK — all archive INVs hold.\n");
    return 0;
}
