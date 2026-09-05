// ANTS-3660 — doc_dedup VERB conformance test (INV-5, INV-8). The handler
// itself needs a live MainWindow, so behavioural rows drive the pure helper and
// wiring rows source-scrape the registration sites (the pattern ANTS-3601's and
// ANTS-3661's verb tests use).

#include "remotecontrol.h"
#include "docdedup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH) || !defined(SRC_DOCDEDUP_CPP_PATH)
#error "doc_dedup_verb test needs the test_claude source-path compile defs"
#endif

namespace {

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QString para(const QString &prefix, int n) {
    QStringList w;
    for (int i = 0; i < n; ++i) w << prefix + QString::number(i);
    return w.join(QLatin1Char(' '));
}

}  // namespace

// INV-5 — report-only. No finding is autoFixable and the engine writes nothing.
//
// Both halves are needed and neither substitutes for the other: the behavioural
// arm proves the flag on findings this run produced, and the scrape proves the
// engine has no write path at all — a claim no behavioural test can hold,
// because it is about the code that did NOT run.
TEST(DocDedupVerb, Inv5ReportOnly) {
    const QString stanza = para(QStringLiteral("w"), 40);
    DocDedup::Accumulator acc;
    acc.add(stanza, QStringLiteral("docs/a.md"), {});
    acc.add(stanza, QStringLiteral("docs/b.md"), {});
    const DocDedup::Result r = acc.finish();
    ASSERT_FALSE(r.findings.isEmpty()) << "the fixture must actually produce "
                                          "findings, or this row asserts nothing";
    for (const auto &f : r.findings) {
        EXPECT_FALSE(f.autoFixable);
        EXPECT_EQ(f.verb, QStringLiteral("doc_dedup"));
        EXPECT_EQ(f.kind, QStringLiteral("near_duplicate"));
    }
    // ...and it reaches the wire that way. ANTS-3664 INV-1 omits the key when
    // the flag is false, so the assertion is ABSENCE, not present-and-false.
    const QJsonObject o = RemoteControl::docDedupBuildResponse(r, {});
    const QJsonArray arr = o.value(QStringLiteral("findings")).toArray();
    ASSERT_FALSE(arr.isEmpty());
    for (const QJsonValue &v : arr)
        EXPECT_FALSE(v.toObject().contains(QStringLiteral("auto_fixable")));

    // The engine opens nothing for writing — and never opens anything at all.
    const QString eng = slurp(SRC_DOCDEDUP_CPP_PATH);
    ASSERT_FALSE(eng.isEmpty());
    for (const char *banned : {"WriteOnly", "QSaveFile", "QTextStream",
                               "ReadWrite", "Append"})
        EXPECT_FALSE(eng.contains(QString::fromUtf8(banned)))
            << "docdedup.cpp must not carry a write path: " << banned;
    EXPECT_FALSE(eng.contains(QStringLiteral("autoFixable = true")));
}

// INV-8 — the verb-contract minimum mcp-tools.md § Tests requires of every
// Required tool, in the arms ANTS-3601 INV-15 establishes.
TEST(DocDedupVerb, Inv8RefusalMinimums) {
    // (1) caller_cwd Required — declared at the call site AND in the static
    // table registerToolProvider asserts against.
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"doc_dedup\""));
    ASSERT_GE(reg, 0);
    // ANTS-3681 — the registration entry, bounded by the next one.
    EXPECT_TRUE(QString::fromStdString(ants_test::regionBetween(
                    mw.toStdString(), "registerToolProvider(\"doc_dedup\"",
                    "registerToolProvider("))
                    .contains(QStringLiteral("CallerCwdContract::Required")));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"doc_dedup\")"));
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains(QStringLiteral("C::Required")));
    EXPECT_TRUE(ci.contains(QStringLiteral("docDedup[\"name\"] = \"doc_dedup\"")));

    // (2) a supplied path is validated BEFORE any enumeration → bad_path.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const QString handler = QString::fromStdString(ants_test::slurpFunctionBody(
        rc.toStdString(), "RemoteControl::cmdDocDedup"));     // ANTS-3681
    ASSERT_FALSE(handler.isEmpty()) << "cmdDocDedup body not found";
    EXPECT_TRUE(handler.contains(QStringLiteral("validatePath(")));
    EXPECT_TRUE(handler.contains(QStringLiteral("check.err")));
    const int validate = handler.indexOf(QStringLiteral("validatePath("));
    const int enumerate = handler.indexOf(QStringLiteral("docIntegrityEnumerate("));
    ASSERT_GE(enumerate, 0);
    EXPECT_LT(validate, enumerate)
        << "the path must be validated before the walk, not after it";

    // (3) nothing to scan → ok:true with EMPTY arrays, not a refusal. This is
    // the shape a well-formed non-existent in-root path produces, since
    // docIntegrityEnumerate returns {} for one (ANTS-3601 INV-15).
    const QJsonObject empty = RemoteControl::docDedupBuildResponse({}, {});
    EXPECT_TRUE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(empty.value(QStringLiteral("findings")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("pairs")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("clusters")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("checked_docs")).toArray().isEmpty());
    EXPECT_EQ(empty.value(QStringLiteral("passages_total")).toInt(-1), 0);
    EXPECT_EQ(empty.value(QStringLiteral("passages_compared")).toInt(-1), 0);
    EXPECT_FALSE(empty.contains(QStringLiteral("truncated")));
}

// pairs[] and clusters[] are this verb's own envelope — DocFinding::Finding has
// no second-location field, so a findings row cannot carry both ends. ANTS-3663
// hoists both arrays whole, which is why the element shape is asserted here
// rather than left to the composer.
TEST(DocDedupVerb, PairAndClusterShapeReachTheWire) {
    const QStringList base = [] {
        QStringList w;
        for (int i = 0; i < 40; ++i) w << QStringLiteral("t") + QString::number(i);
        return w;
    }();
    const QString stanza = base.join(QLatin1Char(' '));

    DocDedup::Accumulator acc;
    acc.add(stanza, QStringLiteral("docs/a.md"), {});
    acc.add(stanza, QStringLiteral("docs/b.md"), {});
    acc.add(stanza, QStringLiteral("docs/c.md"), {});
    const QJsonObject o = RemoteControl::docDedupBuildResponse(acc.finish(), {});

    const QJsonArray pairs = o.value(QStringLiteral("pairs")).toArray();
    ASSERT_EQ(pairs.size(), 3);
    const QJsonObject p0 = pairs.at(0).toObject();
    EXPECT_EQ(p0.value("a").toObject().value("file").toString(),
              QStringLiteral("docs/a.md"));
    EXPECT_EQ(p0.value("a").toObject().value("line").toInt(), 1);
    EXPECT_TRUE(p0.contains(QStringLiteral("similarity")));
    EXPECT_DOUBLE_EQ(p0.value("similarity").toDouble(), 1.0);

    const QJsonArray clusters = o.value(QStringLiteral("clusters")).toArray();
    ASSERT_EQ(clusters.size(), 1) << "three docs sharing one stanza are ONE "
                                     "thing to fix, not three findings";
    const QJsonObject c0 = clusters.at(0).toObject();
    EXPECT_EQ(c0.value("size").toInt(), 3);
    EXPECT_EQ(c0.value("passages").toArray().size(), 3);
    EXPECT_DOUBLE_EQ(c0.value("max_similarity").toDouble(), 1.0);

    // counts is per-kind and covers the whole list (ANTS-3664).
    EXPECT_EQ(o.value(QStringLiteral("counts")).toObject()
                  .value(QStringLiteral("near_duplicate")).toInt(), 3);
}

// ANTS-4460 — doc_dedup is ETag-eligible (isEtagSupportedTool) and was the one
// doc verb emitting no docs_digest. The central etag hashes the RESPONSE, so
// without a fingerprint of the checked set two runs over DIFFERENT doc sets
// that both find nothing hash identically — and the second answers a 304
// `unchanged` for a question it never asked. Its three sibling doc verbs fold
// the set in for exactly this reason.
//
// The assertion is that the digest MOVES with the set, not merely that the key
// is present: a constant would satisfy presence and still permit the false 304.
TEST(DocDedupVerb, Ants4460DigestTracksTheCheckedSet) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/docs")));

    const auto write = [&](const QString &rel, const QString &body) {
        QFile f(root + QLatin1Char('/') + rel);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body.toUtf8());
    };
    // Two unrelated docs, so neither run reports a near-duplicate: the findings
    // are empty either way, which is the case the digest has to separate.
    write(QStringLiteral("docs/a.md"),
          QStringLiteral("# A\n\n") + para(QStringLiteral("alpha"), 40) +
              QStringLiteral("\n"));

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;

    RemoteControl rc(nullptr);
    const QJsonObject one = rc.cmdDocDedup(req).object();
    ASSERT_TRUE(one.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(one).toJson().toStdString();
    const QString digestOne =
        one.value(QStringLiteral("docs_digest")).toString();
    EXPECT_FALSE(digestOne.isEmpty())
        << "an ETag-eligible doc verb must fingerprint its checked set";

    write(QStringLiteral("docs/b.md"),
          QStringLiteral("# B\n\n") + para(QStringLiteral("beta"), 40) +
              QStringLiteral("\n"));

    const QJsonObject two = rc.cmdDocDedup(req).object();
    ASSERT_TRUE(two.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(two).toJson().toStdString();
    ASSERT_EQ(two.value(QStringLiteral("checked_docs")).toArray().size(), 2)
        << "the second run must actually have compared a LARGER set, or this "
           "case proves nothing";
    EXPECT_NE(two.value(QStringLiteral("docs_digest")).toString(), digestOne)
        << "the digest did not move with the checked set, so the central etag "
           "can still 304 across two different sets";
}
