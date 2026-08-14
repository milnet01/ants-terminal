// ANTS-3368 — feature-conformance test for co_change_family.
// One case per live invariant of docs/specs/ANTS-3368-co-change-family.md
// (INV-1..INV-7, INV-9..INV-14; INV-8 is withdrawn and has no case).
// Pure-seam cases drive CoChangeFamily::* directly; the wiring cases
// source-grep the registration, the schema and the handler.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include "cochangefamily.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <string>

#include <gtest/gtest.h>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

using namespace CoChangeFamily;

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

QStringList words(const char *s) { return splitWords(QString::fromLatin1(s)); }

const QStringList kFamily = {QStringLiteral("claude"), QStringLiteral("mcp"),
                             QStringLiteral("enabled")};

RawMatch mk(const char *path, int line, const char *text, int start, int end) {
    RawMatch m;
    m.path       = QString::fromLatin1(path);
    m.line       = line;
    m.text       = QString::fromLatin1(text);
    m.matchStart = start;
    m.matchEnd   = end;
    return m;
}

Stem stemOf(const char *name, int minRun) {
    Stem s;
    s.name   = QString::fromLatin1(name);
    s.words  = splitWords(s.name);
    s.minRun = minRun;
    return s;
}

}  // namespace

// INV-1 — every spelling of one field reduces to the same word sequence.
TEST(CoChangeFamily, SplitWordsFormsAgree) {
    EXPECT_EQ(words("claude.mcp_enabled"), kFamily);
    EXPECT_EQ(words("claudeMcpEnabled"), kFamily);
    EXPECT_EQ(words("CLAUDE_MCP_ENABLED"), kFamily);

    EXPECT_EQ(words("m_claudeMcpEnabled"),
              (QStringList{QStringLiteral("m"), QStringLiteral("claude"),
                           QStringLiteral("mcp"), QStringLiteral("enabled")}));
    EXPECT_EQ(words("setMcpEnabled"),
              (QStringList{QStringLiteral("set"), QStringLiteral("mcp"),
                           QStringLiteral("enabled")}));
    EXPECT_EQ(words("MCP_ENABLED"),
              (QStringList{QStringLiteral("mcp"), QStringLiteral("enabled")}));

    EXPECT_TRUE(splitWords(QString()).isEmpty());
    EXPECT_TRUE(splitWords(QStringLiteral("___")).isEmpty());
}

// INV-2 — the pattern is the spec's regex block at min_run >= 2, and is
// STRICTLY wider at min_run == 1 (it widens the scan, not just the filter).
TEST(CoChangeFamily, ScanPatternWidensWithMinRun) {
    const QString tight = scanPattern(kFamily, 2);
    EXPECT_EQ(tight,
              QStringLiteral("(?i)(?:claude[_.\\-]?mcp|mcp[_.\\-]?enabled)"));

    const QString wide = scanPattern(kFamily, 1);
    EXPECT_NE(wide, tight);
    // Every tight alternative survives, and the single words are added.
    EXPECT_TRUE(wide.contains(QStringLiteral("claude[_.\\-]?mcp")));
    EXPECT_TRUE(wide.contains(QStringLiteral("mcp[_.\\-]?enabled")));
    EXPECT_TRUE(wide.length() > tight.length());

    // The whole point: the tight pattern cannot see a one-word match, so a
    // scan that ignored min_run would make min_run:1 a no-op.
    const QRegularExpression tightRe(tight);
    const QRegularExpression wideRe(wide);
    ASSERT_TRUE(tightRe.isValid());
    ASSERT_TRUE(wideRe.isValid());
    EXPECT_FALSE(tightRe.match(QStringLiteral("audioClaude")).hasMatch());
    EXPECT_TRUE(wideRe.match(QStringLiteral("audioClaude")).hasMatch());

    // A one-word stem yields the bare word at either setting.
    EXPECT_EQ(scanPattern({QStringLiteral("lod")}, 2),
              QStringLiteral("(?i)(?:lod)"));
}

// INV-3 — min_run is per stem, defaults to min(2, words), clamps to range.
TEST(CoChangeFamily, MinRunIsPerStemAndClamps) {
    EXPECT_EQ(defaultMinRun(3), 2);
    EXPECT_EQ(defaultMinRun(2), 2);
    EXPECT_EQ(defaultMinRun(1), 1);

    EXPECT_EQ(clampMinRun(0, 3), 1);     // below range -> clamped, not refused
    EXPECT_EQ(clampMinRun(9, 3), 3);     // above range -> clamped
    EXPECT_EQ(clampMinRun(2, 3), 2);
    EXPECT_EQ(clampMinRun(2, 1), 1);     // a one-word stem cannot demand 2

    // The run must be contiguous in BOTH sequences.
    EXPECT_EQ(longestRun(kFamily, words("setClaudeMcpEnabled")).len, 3);
    EXPECT_EQ(longestRun(kFamily, words("setMcpEnabled")).len, 2);
    EXPECT_EQ(longestRun(kFamily, words("audioClaude")).len, 1);
    // mcpTraceEnabled shares [mcp] and [enabled] but not adjacently: the
    // loose reading would score 2 and admit a site the scan cannot find.
    EXPECT_EQ(longestRun(kFamily, words("mcpTraceEnabled")).len, 1);
    EXPECT_EQ(longestRun(kFamily, words("unrelatedThing")).len, 0);
}

// INV-4 — a run of nothing but stopwords carries no signal.
TEST(CoChangeFamily, StopwordOnlyRunsDropped) {
    EXPECT_TRUE(isStopword(QStringLiteral("enabled")));
    EXPECT_TRUE(isStopword(QStringLiteral("set")));
    EXPECT_TRUE(isStopword(QStringLiteral("m")));
    EXPECT_FALSE(isStopword(QStringLiteral("mcp")));
    EXPECT_FALSE(isStopword(QStringLiteral("lod")));

    EXPECT_TRUE(allStopwords(words("isEnabled")));
    EXPECT_TRUE(allStopwords(words("set_value")));
    EXPECT_FALSE(allStopwords(kFamily));
    EXPECT_FALSE(allStopwords(words("lodEnabled")));
}

// INV-5 — six roles, that precedence, no seventh value.
TEST(CoChangeFamily, RoleVocabularyIsClosed) {
    const auto role = [](const char *line, const char *name, bool inLiteral) {
        Candidate c;
        c.name      = QString::fromLatin1(name);
        c.inLiteral = inLiteral;
        return classifyRole(QString::fromLatin1(line), c);
    };

    EXPECT_EQ(role("  return v(\"claude.mcp_enabled\");", "claude.mcp_enabled",
                   true),
              Role::JsonKey);
    EXPECT_EQ(role("  QCheckBox *m_claudeMcpEnabled;", "m_claudeMcpEnabled",
                   false),
              Role::Member);
    EXPECT_EQ(role("  void setClaudeMcpEnabled(bool);", "setClaudeMcpEnabled",
                   false),
              Role::Mutator);
    EXPECT_EQ(role("  void mcpEnabledChanged();", "mcpEnabledChanged", false),
              Role::Signal);
    EXPECT_EQ(role("struct McpEnabledState {", "McpEnabledState", false),
              Role::Type);
    EXPECT_EQ(role("  if (!cfg.claudeMcpEnabled()) return;", "claudeMcpEnabled",
                   false),
              Role::Reference);

    // Precedence: a member that is also inside a literal is a json_key; a
    // member whose name also ends Changed is still a member.
    EXPECT_EQ(role("  x(\"m_claudeMcpEnabled\");", "m_claudeMcpEnabled", true),
              Role::JsonKey);
    EXPECT_EQ(role("  bool m_mcpEnabledChanged;", "m_mcpEnabledChanged", false),
              Role::Member);

    // The vocabulary is closed: every role spells to one of six strings.
    const QStringList spelled = {
        QString::fromLatin1(roleStr(Role::JsonKey)),
        QString::fromLatin1(roleStr(Role::Member)),
        QString::fromLatin1(roleStr(Role::Mutator)),
        QString::fromLatin1(roleStr(Role::Signal)),
        QString::fromLatin1(roleStr(Role::Type)),
        QString::fromLatin1(roleStr(Role::Reference)),
    };
    EXPECT_EQ(spelled,
              (QStringList{QStringLiteral("json_key"), QStringLiteral("member"),
                           QStringLiteral("mutator"), QStringLiteral("signal"),
                           QStringLiteral("type"),
                           QStringLiteral("reference")}));
}

// INV-6 — one row per (path, line) across overlapping stems, ordered by the
// file's max run_len desc, then path, then line.
TEST(CoChangeFamily, OrderingIsDeterministic) {
    const QVector<Stem> stems = {stemOf("claudeMcpEnabled", 2),
                                 stemOf("mcpEnabled", 2)};

    // src/config.h has an exact 3-word run; src/zzz.cpp only a 2-word one.
    // The config.h line matches BOTH stems and must appear once.
    const QVector<RawMatch> raw = {
        mk("src/zzz.cpp", 9, "  setMcpEnabled(true);", 2, 15),
        mk("src/config.h", 20, "  bool claudeMcpEnabled() const;", 7, 20),
        mk("src/config.h", 5, "  void setClaudeMcpEnabled(bool);", 7, 23),
    };

    const Result r = assemble(raw, stems);
    ASSERT_EQ(r.sites.size(), 3);
    EXPECT_FALSE(r.truncated);

    // config.h (max run 3) before zzz.cpp (max run 2); lines ascending.
    EXPECT_EQ(r.sites[0].path, QStringLiteral("src/config.h"));
    EXPECT_EQ(r.sites[0].line, 5);
    EXPECT_EQ(r.sites[1].path, QStringLiteral("src/config.h"));
    EXPECT_EQ(r.sites[1].line, 20);
    EXPECT_EQ(r.sites[2].path, QStringLiteral("src/zzz.cpp"));

    // Dedup: no (path, line) appears twice even though both stems match.
    for (int i = 1; i < r.sites.size(); ++i) {
        const bool same = r.sites[i].path == r.sites[i - 1].path &&
                          r.sites[i].line == r.sites[i - 1].line;
        EXPECT_FALSE(same) << "duplicate (path, line) row";
    }
    // The longest run owns the row.
    EXPECT_EQ(r.sites[0].stem, QStringLiteral("claudeMcpEnabled"));
    EXPECT_EQ(r.sites[0].runLen, 3);
}

// INV-7 — a capped answer says so, and keeps the strongest sites.
TEST(CoChangeFamily, PartialAnswersAreFlagged) {
    const QVector<Stem> stems = {stemOf("claudeMcpEnabled", 2)};

    QVector<RawMatch> raw;
    // Two weak (2-word run) sites, then one strong (3-word run).
    raw << mk("src/a.cpp", 1, "  setMcpEnabled(true);", 2, 15);
    raw << mk("src/b.cpp", 1, "  setMcpEnabled(false);", 2, 15);
    raw << mk("src/c.cpp", 1, "  claudeMcpEnabled();", 2, 18);

    Options opts;
    opts.maxSites = 1;
    const Result r = assemble(raw, stems, opts);

    EXPECT_TRUE(r.truncated) << "a dropped site must set truncated";
    ASSERT_EQ(r.sites.size(), 1);
    // Highest run_len is retained, NOT the first in scan order.
    EXPECT_EQ(r.sites[0].path, QStringLiteral("src/c.cpp"));
    EXPECT_EQ(r.sites[0].runLen, 3);

    // An uncapped run of the same input is complete.
    const Result full = assemble(raw, stems);
    EXPECT_FALSE(full.truncated);
    EXPECT_EQ(full.sites.size(), 3);

    EXPECT_EQ(clampMaxSites(0), 1);
    EXPECT_EQ(clampMaxSites(99999), 1000);
    EXPECT_EQ(clampMaxSites(200), 200);
}

// INV-9 — the refusal gates, and the handler emits the documented codes.
TEST(CoChangeFamily, RefusalCodes) {
    EXPECT_TRUE(isValidStem(QStringLiteral("claudeMcpEnabled")));
    EXPECT_TRUE(isValidStem(QStringLiteral("claude.mcp_enabled")));
    EXPECT_TRUE(isValidStem(QStringLiteral("lod-enabled")));
    EXPECT_FALSE(isValidStem(QString()));
    EXPECT_FALSE(isValidStem(QStringLiteral("a.*b")));
    EXPECT_FALSE(isValidStem(QStringLiteral("foo(bar)")));
    EXPECT_FALSE(isValidStem(QStringLiteral("a b")));

    // A stem of nothing but stopwords would have every run dropped by
    // INV-4, so it refuses rather than returning a silent empty result.
    EXPECT_TRUE(allStopwords(splitWords(QStringLiteral("isEnabled"))));

    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdCoChangeFamily");
    ASSERT_FALSE(body.empty()) << "cmdCoChangeFamily not found in any RC TU";
    EXPECT_TRUE(has(body, "bad_args"));
    EXPECT_TRUE(has(body, "rg_failed"));
}

// INV-10 — the seam TU is pure, so it links into test_core alone.
TEST(CoChangeFamily, SeamTuHasNoChromeSymbols) {
    const std::string seam = ants_test::stripComments(
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/cochangefamily.cpp"));
    ASSERT_FALSE(seam.empty());

    EXPECT_FALSE(has(seam, "RemoteControl"));
    EXPECT_FALSE(has(seam, "MainWindow"));
    EXPECT_FALSE(has(seam, "ClaudeIntegration"));
    EXPECT_FALSE(has(seam, "QProcess"));
    EXPECT_FALSE(has(seam, "QWidget"));
}

// INV-11 — registration, contract table and schema opt-ins.
TEST(CoChangeFamily, RegistrationAndSchema) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_TRUE(has(mw, "registerToolProvider(\"co_change_family\""));

    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_TRUE(has(ci, "\"co_change_family\""));

    const std::string desc =
        ants_test::mcpToolDescriptor(ci, "co_change_family");
    ASSERT_FALSE(desc.empty()) << "no tools/list descriptor";
    const std::string flat = ants_test::squashWhitespace(desc);
    EXPECT_TRUE(has(flat, "\"object\""));
    EXPECT_TRUE(has(flat, "additionalProperties"));
    EXPECT_TRUE(has(flat, "makeEtagMatchProp()"));
    EXPECT_TRUE(has(flat, "makeFieldsProp()"));
    EXPECT_TRUE(has(flat, "\"stem\""));
    EXPECT_TRUE(has(flat, "\"stems\""));
    EXPECT_TRUE(has(flat, "\"min_run\""));
    EXPECT_TRUE(has(flat, "\"max_sites\""));

    // Contract table + the two opt-in predicates name the verb.
    const std::string ciFlat = ants_test::squashWhitespace(ci);
    EXPECT_TRUE(has(ciFlat, "co_change_family"));
    const std::string contract =
        ants_test::slurpFunctionBody(ci, "ClaudeIntegration::callerCwdContractFor");
    ASSERT_FALSE(contract.empty());
    EXPECT_TRUE(has(contract, "co_change_family"));
}

// INV-12 — a stem's words cannot inject pattern syntax into the rg argv.
TEST(CoChangeFamily, StemCannotInjectPattern) {
    // Drive the assembler directly with a word carrying metacharacters.
    const QString pat = scanPattern({QStringLiteral("a.*b")}, 1);
    const QRegularExpression re(pat);
    ASSERT_TRUE(re.isValid()) << qPrintable(pat);

    EXPECT_TRUE(re.match(QStringLiteral("xxa.*bxx")).hasMatch())
        << "the literal sequence must match";
    EXPECT_FALSE(re.match(QStringLiteral("axxxb")).hasMatch())
        << "'.*' must not act as a wildcard";

    // The separator class the assembler adds is still a real class, so the
    // escaping is of the WORDS only.
    const QRegularExpression pair(scanPattern(
        {QStringLiteral("lod"), QStringLiteral("enabled")}, 2));
    ASSERT_TRUE(pair.isValid());
    EXPECT_TRUE(pair.match(QStringLiteral("lod_enabled")).hasMatch());
    EXPECT_TRUE(pair.match(QStringLiteral("lodEnabled")).hasMatch());
}

// INV-13 — an rg match widens to the candidate the filter actually reads.
TEST(CoChangeFamily, MatchWidensToCandidate) {
    const QString jsonLine =
        QStringLiteral("    return m_data.value(\"claude.mcp_enabled\").toBool(true);");
    const int jsonStart = jsonLine.indexOf(QStringLiteral("claude.mcp"));
    ASSERT_GT(jsonStart, 0);
    const Candidate key =
        widenToCandidate(jsonLine, jsonStart, jsonStart + 10);
    EXPECT_TRUE(key.inLiteral);
    EXPECT_EQ(key.name, QStringLiteral("claude.mcp_enabled"));
    EXPECT_EQ(longestRun(kFamily, splitWords(key.name)).len, 3);
    EXPECT_EQ(classifyRole(jsonLine, key), Role::JsonKey);

    const QString memberLine = QStringLiteral("    bool m_claudeMcpEnabled;");
    const int memStart = memberLine.indexOf(QStringLiteral("claudeMcp"));
    ASSERT_GT(memStart, 0);
    const Candidate mem =
        widenToCandidate(memberLine, memStart, memStart + 9);
    EXPECT_FALSE(mem.inLiteral);
    EXPECT_EQ(mem.name, QStringLiteral("m_claudeMcpEnabled"));
    EXPECT_EQ(longestRun(kFamily, splitWords(mem.name)).len, 3);
    EXPECT_EQ(classifyRole(memberLine, mem), Role::Member);
}

// INV-14 — the scan is repo-wide; declared source roots are not consulted.
TEST(CoChangeFamily, ScanIgnoresDeclaredSourceRoots) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body = ants_test::stripComments(
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdCoChangeFamily"));
    ASSERT_FALSE(body.empty());

    // The whole point of the divergence from find_sources: a config key's
    // docs/ and CLAUDE.md mentions are co-change sites, so the handler must
    // not narrow the walk to the declared roots.
    EXPECT_FALSE(has(body, "ProjectSettings"));
    EXPECT_FALSE(has(body, "sourceRoots"));
    EXPECT_FALSE(has(body, "testRoots"));
    EXPECT_FALSE(has(body, "collectCandidates"));
}
