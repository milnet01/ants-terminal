// ANTS-3368 — feature-conformance test for co_change_family.
// One case per live invariant of docs/specs/ANTS-3368-co-change-family.md
// (INV-1..INV-7, INV-9..INV-14; INV-8 is withdrawn and has no case).
// Pure-seam cases drive CoChangeFamily::* directly; the wiring cases
// source-grep the registration, the schema and the handler.
//
// ANTS-5066 adds three cases pinning Site::text clipping — cost, surrogate
// safety and the ASCII-prefix guard (see spec.md's "ANTS-5066" section).
// These are this test file's own local invariants: clipUtf8() is a
// file-local implementation detail of assemble(), not part of the owner
// spec's numbered sequence above.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include "cochangefamily.h"

#include <QElapsedTimer>
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

// ANTS-5066 — the last two UTF-16 code units, as hex, for a failure message.
QString tailCodeUnitsHex(const QString &s) {
    QStringList out;
    for (int i = qMax(0, s.size() - 2); i < s.size(); ++i) {
        out << QStringLiteral("%1").arg(uint(s.at(i).unicode()), 4, 16, QLatin1Char('0'));
    }
    return out.join(QStringLiteral(","));
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

// ANTS-5066 — Site::text clipping must cost O(line_length), not
// O(line_length^2). Today's clipUtf8() (src/cochangefamily.cpp, file-local)
// calls out.chop(1) in a loop and re-encodes the WHOLE shrinking string to
// UTF-8 on every iteration, unbounded by anything before it runs, so one
// long raw-match line is quadratic in its own length.
//
// Sizing (reasoned, not measured — no build/run available to this writer):
// a several-hundred-thousand-ASCII-byte filler line with a small byte
// budget forces roughly (line_bytes - budget) chop+re-encode iterations,
// each re-encoding a string whose length shrinks from line_bytes down to
// budget — total work on the order of line_bytes^2 / 2 byte-conversions.
// Even at an optimistic ~2 GB/s sustained QString::toUtf8() throughput for
// this shrinking-buffer pattern that lands in the low tens of seconds;
// even at a pessimistic ~1 GB/s (a busy desktop) it stays comfortably under
// a minute. A correct linear pass does exactly one O(line_bytes) encode —
// well under a millisecond at any of those throughputs — plus a boundary
// scan, so it clears the bound below with orders-of-magnitude of margin.
// kCostBoundMs is chosen generously for the FIXED code, not tightly against
// the buggy one: the point is "fast" vs. "not remotely fast", not a precise
// threshold.
TEST(CoChangeFamily, ClipCostIsBoundedNotQuadratic) {
    // Measured on the first red run: an ASCII filler stayed inside the bound,
    // because Qt encodes ASCII to UTF-8 on a vectorised fast path. A 3-byte
    // character takes the per-character path, so the quadratic loop costs
    // seconds here while a linear clip costs microseconds.
    constexpr int kFillerChars   = 80000;   // ~240 KB of UTF-8 filler
    constexpr int kClipBudget    = 64;      // small budget -> almost all chopped
    constexpr qint64 kCostBoundMs = 2000;   // generous bound for the linear fix

    const QString identifier = QStringLiteral("claudeMcpEnabled");
    // Filler is U+2014 EM DASH (not a word char), so widenToCandidate widens
    // to exactly the identifier and no further — the huge tail plays no part
    // in matching, only in the clip that runs afterward.
    QString line = identifier + QString(kFillerChars, QChar(0x2014));

    RawMatch match;
    match.path       = QStringLiteral("src/long_line.cpp");
    match.line       = 1;
    match.text       = line;
    match.matchStart = 0;
    match.matchEnd   = identifier.size();

    const QVector<Stem> stems = {stemOf("claudeMcpEnabled", 2)};
    Options opts;
    opts.maxTextBytes = kClipBudget;

    QElapsedTimer timer;
    timer.start();
    const Result r = assemble({match}, stems, opts);
    const qint64 elapsedMs = timer.elapsed();

    ASSERT_EQ(r.sites.size(), 1) << "the long line must still be accepted as one site";
    const int textBytes = r.sites[0].text.toUtf8().size();

    EXPECT_LE(elapsedMs, kCostBoundMs)
        << "elapsed_ms=" << elapsedMs << " bound_ms=" << kCostBoundMs
        << " line_bytes=" << line.toUtf8().size()
        << " -- clipUtf8 must be linear in line length, not quadratic";
    EXPECT_LE(textBytes, kClipBudget)
        << "clipped_text_bytes=" << textBytes << " budget=" << kClipBudget;
}

// ANTS-5066 — clipUtf8's chop(1) removes one UTF-16 code unit at a time and
// re-measures the UTF-8 byte length after each chop. When the budget lands
// one replacement-character's width inside a surrogate pair, the re-measured
// length can fit BEFORE the trailing lone high surrogate is itself chopped
// away, leaving that dangling surrogate inside the returned QString — never
// splitting the pair is exactly what a correct clip must guarantee instead.
//
// Reasoned trigger for today's code (not run here): the line is
// "claudeMcpEnabled" (17 ASCII bytes) followed by ONE 4-byte emoji
// (U+1F600) as the very last content, so its low surrogate is the string's
// last code unit. A budget of identifier_bytes + 3 — 3 being the width of
// Qt's U+FFFD substitution for an unpaired surrogate — is exactly one chop
// too few for today's code: the first chop removes the low surrogate,
// leaving a lone high surrogate whose re-encoded length (17 ASCII bytes +
// a 3-byte replacement char = budget) no longer exceeds the budget, so the
// loop stops with that lone surrogate still in the string. Expected RED
// today; the linear fix must drop the whole emoji rather than split it, so
// the returned text is the 17-byte identifier alone.
TEST(CoChangeFamily, ClipNeverSplitsASurrogatePair) {
    const QString identifier = QStringLiteral("claudeMcpEnabled");
    QString line = identifier;
    line += QChar(0xD83D);  // high surrogate of U+1F600 (grinning face)
    line += QChar(0xDE00);  // low surrogate — the string's last code unit

    RawMatch match;
    match.path       = QStringLiteral("src/emoji_line.cpp");
    match.line       = 1;
    match.text       = line;
    match.matchStart = 0;
    match.matchEnd   = identifier.size();

    const QVector<Stem> stems = {stemOf("claudeMcpEnabled", 2)};
    Options opts;
    opts.maxTextBytes = identifier.toUtf8().size() + 3;  // lands inside the pair

    const Result r = assemble({match}, stems, opts);
    ASSERT_EQ(r.sites.size(), 1);
    const QString &text = r.sites[0].text;
    const int textBytes = text.toUtf8().size();

    const bool endsInLoneHighSurrogate =
        !text.isEmpty() && text.at(text.size() - 1).isHighSurrogate();
    EXPECT_FALSE(endsInLoneHighSurrogate)
        << "text ends in a lone (unpaired) high surrogate; tail_hex="
        << qPrintable(tailCodeUnitsHex(text)) << " byte_size=" << textBytes
        << " budget=" << opts.maxTextBytes;

    // A valid clip round-trips through UTF-8 unchanged; a dangling surrogate
    // re-encodes to U+FFFD and does not equal the original.
    EXPECT_EQ(QString::fromUtf8(text.toUtf8()), text)
        << "clipped text does not round-trip through UTF-8 unchanged; tail_hex="
        << qPrintable(tailCodeUnitsHex(text)) << " byte_size=" << textBytes;

    EXPECT_LE(textBytes, opts.maxTextBytes)
        << "byte_size=" << textBytes << " budget=" << opts.maxTextBytes;
}

// ANTS-5066 — guard: the fix must not change output for ordinary input. A
// line within budget returns unchanged; a line over budget is cut to the
// longest byte-fitting ASCII prefix, with no marker added — the same result
// today's chop loop gives for ASCII, since every char is exactly one byte.
TEST(CoChangeFamily, ClipIsAByteBudgetPrefix) {
    const QString identifier = QStringLiteral("claudeMcpEnabled");
    const QString line = identifier + QStringLiteral(" 0123456789abcdefghij");

    RawMatch match;
    match.path       = QStringLiteral("src/guard_line.cpp");
    match.line       = 1;
    match.text       = line;
    match.matchStart = 0;
    match.matchEnd   = identifier.size();

    const QVector<Stem> stems = {stemOf("claudeMcpEnabled", 2)};

    // Within budget: returned unchanged.
    {
        Options opts;
        opts.maxTextBytes = line.toUtf8().size() + 10;
        const Result r = assemble({match}, stems, opts);
        ASSERT_EQ(r.sites.size(), 1);
        EXPECT_EQ(r.sites[0].text, line)
            << "byte_size=" << r.sites[0].text.toUtf8().size()
            << " line_bytes=" << line.toUtf8().size();
    }
    // Over budget: cut to the longest fitting prefix, no ellipsis.
    {
        Options opts;
        const int budget  = identifier.size() + 5;  // "claudeMcpEnabled 0123"
        opts.maxTextBytes = budget;
        const Result r = assemble({match}, stems, opts);
        ASSERT_EQ(r.sites.size(), 1);
        const QString &text = r.sites[0].text;
        const int textBytes = text.toUtf8().size();

        EXPECT_EQ(text, line.left(budget))
            << "clipped=" << qPrintable(text)
            << " expected_prefix=" << qPrintable(line.left(budget))
            << " byte_size=" << textBytes << " budget=" << budget;
        EXPECT_LE(textBytes, budget)
            << "byte_size=" << textBytes << " budget=" << budget;
        EXPECT_FALSE(text.endsWith(QChar(0x2026)))
            << "no ellipsis marker expected; tail_hex=" << qPrintable(tailCodeUnitsHex(text));
    }
}
