// ANTS-3661 — doc_symbols VERB conformance test (INV-4, INV-6). The handler
// itself needs a live MainWindow, so behavioural rows drive the pure helpers
// and wiring rows source-scrape the registration sites (the
// rc_get_text_byte_cap pattern, as ANTS-3601's verb test does).

#include "remotecontrol.h"
#include "docsymbols.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH) || !defined(SRC_DOCSYMBOLS_CPP_PATH)
#error "doc_symbols_verb test needs the test_claude source-path compile defs"
#endif

namespace {

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-4 — doc_symbols emits no severity and no defect wording, and never marks
// a finding auto-fixable. A behavioural test cannot hold a claim about
// vocabulary, so this one is deliberately a scrape.
//
// The engine's IMPLEMENTATION and the verb's response builder are scraped; the
// header is not, because it states the rule in prose and a scrape that forbids
// naming the rule forbids documenting it.
TEST(DocSymbolsVerb, Inv4NoSeverityVocabulary) {
    const QString eng = slurp(SRC_DOCSYMBOLS_CPP_PATH);
    ASSERT_FALSE(eng.isEmpty());
    for (const char *banned : {"severity", "broken", "invalid", "autoFixable = true"})
        EXPECT_FALSE(eng.contains(QString::fromUtf8(banned), Qt::CaseInsensitive))
            << "docsymbols.cpp must not carry defect vocabulary: " << banned;

    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const int b = rc.indexOf(QStringLiteral("RemoteControl::docSymbolsBuildResponse"));
    ASSERT_GE(b, 0);
    const QString body = rc.mid(b, 2600);
    EXPECT_FALSE(body.contains(QStringLiteral("severity"), Qt::CaseInsensitive));
    EXPECT_FALSE(body.contains(QStringLiteral("auto_fixable")));
}

// INV-6 — the verb-contract minimum: caller_cwd Required; a root-escaping
// `path` refuses bad_path; a well-formed non-existent in-root `path` is
// ok:true with an empty symbols[] (ANTS-3601 INV-15's shape).
TEST(DocSymbolsVerb, Inv6RefusalMinimums) {
    // (1) caller_cwd Required — declared at the call site AND in the static
    // table registerToolProvider asserts against.
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"doc_symbols\""));
    ASSERT_GE(reg, 0);
    EXPECT_TRUE(mw.mid(reg, 160).contains(QStringLiteral("CallerCwdContract::Required")));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"doc_symbols\")"));
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains(QStringLiteral("C::Required")));
    EXPECT_TRUE(ci.contains(QStringLiteral("docSym[\"name\"] = \"doc_symbols\"")));

    // (2) a supplied path is validated before any enumeration → bad_path.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const int h = rc.indexOf(QStringLiteral("RemoteControl::cmdDocSymbols"));
    ASSERT_GE(h, 0);
    const QString handler = rc.mid(h, 1400);
    EXPECT_TRUE(handler.contains(QStringLiteral("validatePath(")));
    EXPECT_TRUE(handler.contains(QStringLiteral("check.err")));

    // (3) nothing to scan → ok:true with an EMPTY symbols[], not a refusal.
    const QJsonObject empty = RemoteControl::docSymbolsBuildResponse({}, {}, false, {});
    EXPECT_TRUE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(empty.value(QStringLiteral("symbols")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("findings")).toArray().isEmpty());
    EXPECT_FALSE(empty.value(QStringLiteral("truncated")).toBool());
    const QJsonObject counts = empty.value(QStringLiteral("counts")).toObject();
    EXPECT_EQ(counts.value(QStringLiteral("total")).toInt(), 0);
    EXPECT_EQ(counts.value(QStringLiteral("not_checked")).toInt(), 0);
}

// The tri-state reaches the wire as three distinct strings — the response is
// where a caller could still be told "does not exist" about a needle nobody
// looked up (§ 2.2).
TEST(DocSymbolsVerb, ResolutionReachesTheWireAsThreeStates) {
    QVector<DocSymbols::Symbol> syms;
    for (auto r : {DocSymbols::Resolution::Resolved, DocSymbols::Resolution::Unresolved,
                   DocSymbols::Resolution::NotChecked}) {
        DocSymbols::Symbol s;
        s.symbol     = QStringLiteral("Sym");
        s.docLine    = 1;
        s.resolution = r;
        syms.push_back(s);
    }
    const QJsonObject o = RemoteControl::docSymbolsBuildResponse(syms, {}, true, {});
    const QJsonArray arr = o.value(QStringLiteral("symbols")).toArray();
    ASSERT_EQ(arr.size(), 3);
    EXPECT_EQ(arr.at(0).toObject().value("resolution").toString(), QStringLiteral("resolved"));
    EXPECT_EQ(arr.at(1).toObject().value("resolution").toString(), QStringLiteral("unresolved"));
    EXPECT_EQ(arr.at(2).toObject().value("resolution").toString(), QStringLiteral("not_checked"));

    const QJsonObject counts = o.value(QStringLiteral("counts")).toObject();
    EXPECT_EQ(counts.value(QStringLiteral("resolved")).toInt(), 1);
    EXPECT_EQ(counts.value(QStringLiteral("unresolved")).toInt(), 1);
    EXPECT_EQ(counts.value(QStringLiteral("not_checked")).toInt(), 1);
}

// ANTS-3688 — the verb-vocabulary provider is installed AFTER m_remoteControl
// exists, and from the constructor rather than from setupClaudeMcpProviders().
//
// This is a regression row for a silent-ordering bug, not a style preference.
// setupClaudeMcpProviders() is called from setupStatusBarChrome() early in the
// constructor, long before `m_remoteControl = new RemoteControl(...)`. The
// install used to sit there behind `if (m_remoteControl)`, so the guard was
// false, the provider was never installed, and DocSymbols::Options::excludedNames
// held only the refusal codes scraped from mcp-error-codes.md — every MCP verb
// name became an unresolved_symbol finding, corpus-wide, for the verb whose
// whole job is to keep that list short.
//
// Nothing caught it because every OTHER registration in that function survives
// the same ordering: rcDelegate derefs m_remoteControl lazily at call time.
// This setter takes the object eagerly, which is what made it the odd one out.
//
// The two offset assertions together pin the call to the constructor after the
// allocation: a byte offset alone would be satisfied by the old site, since
// setupClaudeMcpProviders' DEFINITION sits later in the file than its CALL.
TEST(DocSymbolsVerb, Inv8VocabularyProviderInstalledAfterRemoteControlExists) {
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());

    const int install = mw.indexOf(QStringLiteral("setMcpVerbVocabularyProvider("));
    ASSERT_GE(install, 0) << "mainwindow.cpp never installs the doc_symbols verb "
                             "vocabulary provider — excludedNames would carry only "
                             "refusal codes (ANTS-3688)";
    EXPECT_EQ(mw.indexOf(QStringLiteral("setMcpVerbVocabularyProvider("), install + 1), -1)
        << "installed more than once; one of them is dead or contradictory";

    const int alloc = mw.indexOf(QStringLiteral("m_remoteControl = new RemoteControl("));
    ASSERT_GE(alloc, 0);
    EXPECT_GT(install, alloc)
        << "the provider is installed before m_remoteControl exists, so the install "
           "is a no-op and every MCP verb name becomes an unresolved_symbol finding";

    const int setupDef = mw.indexOf(QStringLiteral("void MainWindow::setupClaudeMcpProviders()"));
    ASSERT_GE(setupDef, 0);
    EXPECT_LT(install, setupDef)
        << "the provider is installed from setupClaudeMcpProviders(), which runs "
           "from setupStatusBarChrome() before m_remoteControl is allocated";
}
