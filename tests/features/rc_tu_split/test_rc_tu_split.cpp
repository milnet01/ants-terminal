// ANTS-3833 — the RemoteControl translation-unit split holds.
//
// src/remotecontrol.cpp was one 24,752-line TU; it is now eleven. Every case
// here defends one way that cut can stop being faithful WITHOUT the build or
// the suite going red. See spec.md for the contract and
// docs/specs/ANTS-3833-remotecontrol-decomposition.md for the design.
//
// Everything is derived from ANTS_RC_SOURCES — the compile definition CMake
// builds from the ANTS_RC_SOURCES_REL list. Nothing here hard-codes eleven:
// § 2.2 expects a twelfth TU once one reaches INV-6's cap, and a case that
// must be edited when that happens is a case that will be edited wrongly.
//
// Files are handled as RAW BYTES throughout. tests/ carries deliberately
// non-UTF-8 fixtures (tests/features/roadmap_migrate_read/fixtures/), and a
// scan that decodes them is a scan that can mangle what it is searching.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "../../_support/expect.h"

ANTS_TEST_SCOPE();

namespace {

// The TU list, absolute, in cut order — the same ';'-separated value
// slurpRemoteControl() walks.
QStringList rcSources() {
    return QString::fromUtf8(ANTS_RC_SOURCES)
        .split(QLatin1Char(';'), Qt::SkipEmptyParts);
}

QByteArray readBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// Byte-for-byte what ants_test::slurpRemoteControl() returns: every listed
// source, in list order, joined with a newline.
QByteArray concatenation() {
    QByteArray out;
    for (const QString &p : rcSources()) {
        if (!out.isEmpty()) out += '\n';
        out += readBytes(p);
    }
    return out;
}

// Offset of each TU's first byte within the concatenation — the eleven
// insertion points INV-10 is about.
std::vector<qsizetype> tuHeadOffsets() {
    std::vector<qsizetype> heads;
    qsizetype off = 0;
    for (const QString &p : rcSources()) {
        heads.push_back(off);
        off += readBytes(p).size() + 1;   // +1 for the '\n' join
    }
    return heads;
}

// Every regular file under `dir`, skipping this case's own directory. The
// exclusion is required, not cosmetic: INV-4 and INV-5 cannot search for a
// literal without naming it, and spec.md quotes those literals too. Without
// the filter each case matches itself and can never pass.
QStringList filesUnder(const QString &dir) {
    const QString selfDir =
        QDir(QStringLiteral(ANTS_RC_ROOT_DIR))
            .filePath(QStringLiteral("tests/features/rc_tu_split"));
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.startsWith(selfDir + QLatin1Char('/'))) continue;
        out << path;
    }
    out.sort();
    return out;
}

QString rootPath(const char *rel) {
    return QDir(QStringLiteral(ANTS_RC_ROOT_DIR)).filePath(QString::fromUtf8(rel));
}

// Resolve a C++ string-literal body to the bytes it denotes at run time. The
// anchors INV-10 recovers are literals as they appear IN TEST SOURCE, so
// `env[\"ok\"] = true;` has to become `env["ok"] = true;` before it can be
// looked up in the concatenation. Conservative on purpose: the escapes that
// actually occur in these anchors, and a backslash-anything passthrough for
// the rest.
std::string unescapeLiteral(const QString &raw) {
    const QByteArray in = raw.toUtf8();
    std::string out;
    out.reserve(static_cast<std::size_t>(in.size()));
    for (qsizetype i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            const char c = in[++i];
            switch (c) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            default:  out.push_back(c);    break;   // \" \\ and friends
            }
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// INV-3 — the list is in cut order, and the concatenation proves it.
//
// Breaks when a TU is appended to ANTS_RC_SOURCES_REL rather than inserted at
// its slice position, which silently reorders every two-anchor scrape window
// (find(A), then find(B, posA)).
// ---------------------------------------------------------------------------
TEST(RcTuSplit, TuOrdinalMarkersAscend) {
    expect_reset();
    const QStringList sources = rcSources();
    const qsizetype n = sources.size();
    ASSERT_GT(n, 1) << "ANTS_RC_SOURCES carries " << n
                    << " entry — CMake's \";\" escaping has collapsed the list "
                       "(see CMakeLists.txt's string(REPLACE) note). Verify "
                       "against build/compile_commands.json, not CMakeLists.txt.";

    const QByteArray rc = concatenation();
    ASSERT_FALSE(rc.isEmpty());

    qsizetype prev = -1;
    for (qsizetype k = 1; k <= n; ++k) {
        // N is derived, never the literal 11 — a twelfth TU is an expected
        // event and must not require editing this case.
        const QByteArray marker =
            QStringLiteral("TU %1/%2").arg(k).arg(n).toUtf8();

        const qsizetype at = rc.indexOf(marker);
        expect(at >= 0, "INV-3/marker-present",
               QStringLiteral("no `%1` head marker in the concatenation")
                   .arg(QString::fromUtf8(marker)));
        if (at < 0) continue;

        expect(rc.indexOf(marker, at + marker.size()) < 0,
               "INV-3/marker-unique",
               QStringLiteral("`%1` appears more than once")
                   .arg(QString::fromUtf8(marker)));

        expect(at > prev, "INV-3/ascending",
               QStringLiteral("`%1` is at offset %2, behind the previous "
                              "marker at %3 — the list is not in cut order")
                   .arg(QString::fromUtf8(marker)).arg(at).arg(prev));
        prev = at;
    }
    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------------------
// INV-4 — nothing names a single TU as "the RemoteControl source".
//
// Two assertions, and the split is required rather than stylistic:
// CMakeLists.txt must carry the literal src/remotecontrol.cpp forever (it is
// TU 1, and INV-11 requires the list naming it to exist), so a single command
// spanning both trees could never return 0.
// ---------------------------------------------------------------------------
TEST(RcTuSplit, NoSingleTuPathMacro) {
    expect_reset();

    // Arm 1 — tests/: the two retired macros AND the literal path. The literal
    // arm is what closes the eighth route: deleting a macro cannot fail a site
    // that never used one, so without it those reads keep scraping TU 1 in
    // silence.
    const QList<QByteArray> banned = {
        QByteArrayLiteral("SRC_REMOTECONTROL_CPP_PATH"),
        QByteArrayLiteral("SRC_RC_CPP"),
        QByteArrayLiteral("src/remotecontrol.cpp"),
    };
    const QString testsDir = rootPath("tests");
    ASSERT_TRUE(QFileInfo::exists(testsDir)) << testsDir.toStdString();

    QStringList offenders;
    for (const QString &path : filesUnder(testsDir)) {
        const QByteArray body = readBytes(path);
        for (const QByteArray &needle : banned) {
            if (body.contains(needle)) {
                offenders << QDir(QStringLiteral(ANTS_RC_ROOT_DIR)).relativeFilePath(path)
                                 + QStringLiteral(" [") + QString::fromUtf8(needle)
                                 + QStringLiteral("]");
                break;
            }
        }
    }
    expect(offenders.isEmpty(), "INV-4/tests",
           QStringLiteral("%1 file(s) still name one TU as the RemoteControl "
                          "source: %2")
               .arg(offenders.size())
               .arg(offenders.mid(0, 10).join(QStringLiteral(", "))));

    // Arm 2 — CMakeLists.txt: the two macro NAMES only. Not the literal.
    const QByteArray cmake = readBytes(rootPath("CMakeLists.txt"));
    ASSERT_FALSE(cmake.isEmpty());
    expect(!cmake.contains(banned[0]) && !cmake.contains(banned[1]),
           "INV-4/cmake",
           QStringLiteral("CMakeLists.txt still defines a retired "
                          "single-TU path macro"));

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------------------
// INV-5 — the internal header stays internal.
//
// A SUBSET check, matching the "only by" wording: a TU that happens to need
// none of the promoted helpers is entitled not to include it. The permitted
// set is the declared list, never a glob.
// ---------------------------------------------------------------------------
TEST(RcTuSplit, InternalHeaderStaysInternal) {
    expect_reset();

    QStringList permitted;
    for (const QString &p : rcSources()) permitted << QFileInfo(p).canonicalFilePath();

    const QByteArray include =
        QByteArrayLiteral("#include \"remotecontrol_internal.h\"");

    QStringList offenders;
    for (const QString &dir : {rootPath("src"), rootPath("tests")}) {
        ASSERT_TRUE(QFileInfo::exists(dir)) << dir.toStdString();
        for (const QString &path : filesUnder(dir)) {
            if (!readBytes(path).contains(include)) continue;
            if (permitted.contains(QFileInfo(path).canonicalFilePath())) continue;
            offenders << QDir(QStringLiteral(ANTS_RC_ROOT_DIR)).relativeFilePath(path);
        }
    }
    expect(offenders.isEmpty(), "INV-5",
           QStringLiteral("remotecontrol_internal.h is included outside the "
                          "ANTS_RC_SOURCES list by: %1")
               .arg(offenders.join(QStringLiteral(", "))));

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------------------
// INV-6 — no TU grows back into the old file.
//
// The cap is set from § 4's fit: at 6,000 lines a TU costs ~16 s to compile,
// still under a third of the pre-split 54.66 s. This is the regression the
// whole item exists to prevent, and the only one that returns silently.
// ---------------------------------------------------------------------------
TEST(RcTuSplit, NoTuExceedsLineCap) {
    expect_reset();
    constexpr qsizetype kMaxLines = 6000;

    for (const QString &path : rcSources()) {
        const QByteArray body = readBytes(path);
        expect(!body.isEmpty(), "INV-6/readable",
               QStringLiteral("listed source is empty or unreadable: %1").arg(path));
        if (body.isEmpty()) continue;

        const qsizetype lines = body.count('\n') + (body.endsWith('\n') ? 0 : 1);
        expect(lines <= kMaxLines, "INV-6",
               QStringLiteral("%1 is %2 lines, over the %3 cap — split it at a "
                              "member boundary (§ 2.2)")
                   .arg(QFileInfo(path).fileName()).arg(lines).arg(kMaxLines));
    }
    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------------------
// INV-10 — no seam lands inside a fixed-byte scrape window.
//
// The work-list is DERIVED, not embedded. § 2.4 enumerates seven anchors;
// this scan returned 20 sites over 15 distinct anchors on 2026-08-06, and
// hard-coding either number would leave a window added tomorrow silently
// uncovered — the exact class this invariant exists for.
//
// And the failure is not reliably loud, which is why the case is mandatory.
// A must-contain assertion whose window slid onto an include preamble goes red.
// A NEGATIVE assertion ("this region must not mention X") and a
// countOccurrences total over a shifted window both stay GREEN while testing
// nothing. One of the derived sites is exactly that shape.
// ---------------------------------------------------------------------------
TEST(RcTuSplit, NoSeamInsideAScrapeWindow) {
    expect_reset();

    const QByteArray rcBytes = concatenation();
    ASSERT_FALSE(rcBytes.isEmpty());
    const std::string rc(rcBytes.constData(), static_cast<std::size_t>(rcBytes.size()));
    const std::vector<qsizetype> heads = tuHeadOffsets();
    ASSERT_FALSE(heads.empty());

    // A window subject must hold slurpRemoteControl() text; scoping this per
    // TEST block rather than per file keeps a name that means remotecontrol in
    // one case and something else in the next from being confused.
    const QRegularExpression reRcVar(
        QStringLiteral(R"(\b(\w+)\s*=\s*(?:ants_test::)?slurpRemoteControl\(\))"));
    const QRegularExpression reSubstr(
        QStringLiteral(R"((\w+)\.substr\((\w+),\s*(\d{2,})\))"));

    int sites = 0;
    QStringList unresolved, violations;

    for (const QString &path : filesUnder(rootPath("tests"))) {
        if (!path.endsWith(QStringLiteral(".cpp"))) continue;
        const QString text = QString::fromUtf8(readBytes(path));
        if (!text.contains(QStringLiteral("slurpRemoteControl"))) continue;
        const QString rel = QDir(QStringLiteral(ANTS_RC_ROOT_DIR)).relativeFilePath(path);

        // Split into per-TEST blocks.
        QList<qsizetype> starts;
        for (qsizetype at = text.indexOf(QStringLiteral("\nTEST(")); at >= 0;
             at = text.indexOf(QStringLiteral("\nTEST("), at + 1)) {
            starts << at + 1;
        }
        for (qsizetype b = 0; b < starts.size(); ++b) {
            const qsizetype from = starts[b];
            const qsizetype to = (b + 1 < starts.size()) ? starts[b + 1] : text.size();
            const QString block = text.mid(from, to - from);

            QStringList rcVars;
            auto vit = reRcVar.globalMatch(block);
            while (vit.hasNext()) rcVars << vit.next().captured(1);
            if (rcVars.isEmpty()) continue;

            auto sit = reSubstr.globalMatch(block);
            while (sit.hasNext()) {
                const QRegularExpressionMatch site = sit.next();
                const QString subj = site.captured(1);
                const QString ident = site.captured(2);
                const qsizetype n = site.captured(3).toLongLong();
                if (!rcVars.contains(subj)) continue;
                ++sites;

                // Recover the anchor: the LAST `<ident> = <subj>.find("…")`
                // before this site. Last, not first — a block may rebind.
                const QString before = block.left(site.capturedStart());
                const auto findPattern = [](const QString &id, const QString &s) {
                    // Custom delimiter: the pattern itself contains `)"`,
                    // which would close a plain R"( … )" early.
                    return QRegularExpression(
                        QStringLiteral(R"RX(\b%1\s*=\s*%2\.find\(\s*"((?:[^"\\]|\\.)*)"\s*(?:,\s*(\w+)\s*)?\))RX")
                            .arg(id, s));
                };
                QRegularExpressionMatch anchorMatch;
                auto ait = findPattern(ident, subj).globalMatch(before);
                while (ait.hasNext()) anchorMatch = ait.next();

                if (!anchorMatch.hasMatch()) {
                    unresolved << QStringLiteral("%1: %2.substr(%3, %4) — no "
                                                 "find() produced the offset")
                                      .arg(rel, subj, ident).arg(n);
                    continue;
                }

                // A two-arg find() searches from a prior offset, so the anchor
                // must be resolved from there. Ignoring it can resolve to an
                // EARLIER identical string and clear a window that is really
                // in a different TU.
                std::size_t base = 0;
                const QString startIdent = anchorMatch.captured(2);
                if (!startIdent.isEmpty()) {
                    QRegularExpressionMatch baseMatch;
                    auto bit = findPattern(startIdent, subj)
                                   .globalMatch(before.left(anchorMatch.capturedStart()));
                    while (bit.hasNext()) baseMatch = bit.next();
                    if (baseMatch.hasMatch()) {
                        const std::size_t at =
                            rc.find(unescapeLiteral(baseMatch.captured(1)));
                        if (at != std::string::npos) base = at;
                    }
                }

                const std::string anchor = unescapeLiteral(anchorMatch.captured(1));
                const std::size_t a = rc.find(anchor, base);
                if (a == std::string::npos) {
                    unresolved << QStringLiteral("%1: anchor not in the "
                                                 "concatenation: \"%2\"")
                                      .arg(rel, anchorMatch.captured(1));
                    continue;
                }

                for (std::size_t k = 0; k < heads.size(); ++k) {
                    const auto head = static_cast<std::size_t>(heads[k]);
                    if (head >= a && head < a + static_cast<std::size_t>(n)) {
                        violations << QStringLiteral(
                                          "%1: window [%2, +%3) on \"%4\" "
                                          "contains the TU %5 head at %6")
                                          .arg(rel).arg(a).arg(n)
                                          .arg(anchorMatch.captured(1))
                                          .arg(k + 1).arg(head);
                    }
                }
            }
        }
    }

    // A derivation that finds nothing is not a pass — it is a broken scan
    // reporting green, which is the failure mode this whole case is about.
    expect(sites > 0, "INV-10/derivation",
           QStringLiteral("the scan found no windowed scrape of "
                          "slurpRemoteControl() text at all"));

    // An anchor that will not resolve means the derivation has gone blind on
    // that site, so the site is unchecked. Same class as finding nothing.
    expect(unresolved.isEmpty(), "INV-10/unresolved",
           QStringLiteral("%1 site(s) could not be resolved: %2")
               .arg(unresolved.size())
               .arg(unresolved.mid(0, 5).join(QStringLiteral(" | "))));

    // Remedy when this fires: move the seam to the next member boundary that
    // clears every window, or convert the offending window to a structural
    // bound (slurpFunctionBody) — the fix ANTS-3681 was already making.
    expect(violations.isEmpty(), "INV-10",
           QStringLiteral("%1 scrape window(s) span a TU seam: %2")
               .arg(violations.size())
               .arg(violations.mid(0, 5).join(QStringLiteral(" | "))));

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------------------
// INV-11 — the library consumes the list rather than paralleling it.
//
// Breaks when a TU is added to add_library() and not to ANTS_RC_SOURCES_REL:
// it links, INV-3 through INV-6 all pass, and every scrape reads a fraction of
// the class in silence — the failure INV-4 abolishes, arriving by a route
// INV-4 cannot see.
// ---------------------------------------------------------------------------
TEST(RcTuSplit, LibraryConsumesTheList) {
    expect_reset();

    const QString cmake = QString::fromUtf8(readBytes(rootPath("CMakeLists.txt")));
    ASSERT_FALSE(cmake.isEmpty());

    const qsizetype blockStart = cmake.indexOf(QStringLiteral("set(ANTS_RC_SOURCES_REL"));
    ASSERT_GE(blockStart, 0) << "the ANTS_RC_SOURCES_REL list is gone — INV-11 "
                                "has no block to be outside of";
    const qsizetype blockEnd = cmake.indexOf(QStringLiteral("\n)"), blockStart);
    ASSERT_GT(blockEnd, blockStart);

    // `remotecontrol` followed by `.` or `_` — so src/remotecontrolgate.cpp,
    // a different file that is not a TU, correctly does not match.
    const QRegularExpression rx(QStringLiteral(R"(src/remotecontrol(_[A-Za-z0-9_]+)?\.cpp)"));
    QStringList outside;
    auto it = rx.globalMatch(cmake);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() >= blockStart && m.capturedStart() < blockEnd) continue;
        outside << QStringLiteral("line %1: %2")
                       .arg(cmake.left(m.capturedStart()).count(QLatin1Char('\n')) + 1)
                       .arg(m.captured(0));
    }
    expect(outside.isEmpty(), "INV-11",
           QStringLiteral("%1 remotecontrol TU literal(s) outside the "
                          "ANTS_RC_SOURCES_REL block: %2")
               .arg(outside.size()).arg(outside.join(QStringLiteral(", "))));

    ASSERT_EQ(0, expect_finish());
}
