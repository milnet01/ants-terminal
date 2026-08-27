// ANTS-3636 — doc_citations verb conformance. Behaviour drives the two pure
// statics (docCitationsValidate / docCitationsClampOptions); the wiring is
// source-scraped, because the handler needs a live MainWindow. See
// tests/features/doc_citations_verb/spec.md.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "doccitations.h"
#include "mcpprojection.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <unistd.h>

ANTS_TEST_SCOPE();

namespace {

QString slurp(const char *path) { return QString::fromStdString(ants_test::slurpFile(path)); }

bool writeFile(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(content);
    return true;
}

QString codeOf(const QJsonObject &o) { return o.value(QStringLiteral("code")).toString(); }

// The source region that authors one tool descriptor: from its name assignment
// to the tools.append that closes it. INV-22's subject is what is written here,
// not what tools/list emits.
QString descriptorRegion(const QString &src, const QString &verb) {
    const int start = src.indexOf(QStringLiteral("\"name\"] = \"%1\"").arg(verb));
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("tools.append("), start);
    if (end < 0) return QString();
    return src.mid(start, end - start);
}

// Join the string literals of one `= QStringLiteral( "a" "b" );` assignment, so
// a length rule can be applied to the value rather than to the source text.
QString authoredString(const QString &region, const QString &key) {
    const int at = region.indexOf(QStringLiteral("\"%1\"] = QStringLiteral(").arg(key));
    if (at < 0) return QString();
    const int end = region.indexOf(QStringLiteral(");"), at);
    if (end < 0) return QString();
    const QString span = region.mid(at, end - at);
    QString out;
    int i = span.indexOf(QLatin1Char('('));
    while ((i = span.indexOf(QLatin1Char('"'), i)) >= 0) {
        int j = i + 1;
        while (j < span.size() && !(span.at(j) == QLatin1Char('"')
                                    && span.at(j - 1) != QLatin1Char('\\')))
            ++j;
        if (j >= span.size()) break;
        out += span.mid(i + 1, j - i - 1);
        i = j + 1;
    }
    out.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    return out;
}

}  // namespace

// INV-18 — exactly five refusals, and an empty envelope for accepted input.
TEST(DocCitationsVerb, Inv18ValidateRefusalSet) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/a.md"), "# H\n"));
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/docs/sub")));

    const auto call = [&](const QString &r, const QJsonObject &req) {
        return RemoteControl::docCitationsValidate(r, req);
    };
    const QJsonObject good{{QStringLiteral("path"), QStringLiteral("docs/a.md")}};

    expect(codeOf(call(QString(), good)) == QStringLiteral("bad_path"),
           "INV-18: no focused project refuses bad_path");
    expect(codeOf(call(root, {{QStringLiteral("path"), QStringLiteral("../../etc/passwd")}}))
               == QStringLiteral("bad_path"),
           "INV-18: a root-escaping path refuses bad_path");
    expect(codeOf(call(root, {{QStringLiteral("path"), QStringLiteral("docs/sub")}}))
               == QStringLiteral("bad_args"),
           "INV-18: a directory refuses bad_args");
    expect(codeOf(call(root, QJsonObject{})) == QStringLiteral("missing_field"),
           "INV-18: an absent path refuses missing_field");
    expect(codeOf(call(root, {{QStringLiteral("path"), QString()}}))
               == QStringLiteral("missing_field"),
           "INV-18: an empty path refuses missing_field");

    const QString locked = root + QStringLiteral("/docs/locked.md");
    ASSERT_TRUE(writeFile(locked, "# H\n"));
    if (::geteuid() != 0 && QFile::setPermissions(locked, QFile::Permissions{})) {
        expect(codeOf(call(root, {{QStringLiteral("path"), QStringLiteral("docs/locked.md")}}))
                   == QStringLiteral("read_failed"),
               "INV-18: an unreadable doc refuses read_failed");
        QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner);
    }

    expect(call(root, good).isEmpty(), "INV-18: accepted input returns an empty envelope");
    // A well-formed in-root path that does not exist is accepted here and
    // answered ok:true by the engine — the companion-verb convergence.
    expect(call(root, {{QStringLiteral("path"), QStringLiteral("docs/nope.md")}}).isEmpty(),
           "INV-18: a non-existent in-root path is not a refusal");
    EXPECT_EQ(0, expect_failures());
}

// INV-21 — clamps at both ends, and coercion rather than refusal for
// out-of-domain input.
TEST(DocCitationsVerb, Inv21ClampAndCoerce) {
    expect_reset();
    const auto clamp = [](const QJsonObject &req) {
        return RemoteControl::docCitationsClampOptions(req);
    };
    const DocCitations::Options def;

    expect(clamp({{QStringLiteral("max_range_lines"), 0}}).maxRangeLines == 1,
           "INV-21: max_range_lines floor");
    expect(clamp({{QStringLiteral("max_range_lines"), 999}}).maxRangeLines == 20,
           "INV-21: max_range_lines ceiling");
    expect(clamp({{QStringLiteral("max_doc_lines"), 1}}).maxDocLines == 1000,
           "INV-21: max_doc_lines floor");
    expect(clamp({{QStringLiteral("max_doc_lines"), 999999}}).maxDocLines == 50000,
           "INV-21: max_doc_lines ceiling");
    expect(clamp({{QStringLiteral("max_bytes"), 10}}).maxBytes == 64 * 1024,
           "INV-21: max_bytes floor");
    expect(clamp({{QStringLiteral("max_bytes"), 99 * 1024 * 1024}}).maxBytes == 4 * 1024 * 1024,
           "INV-21: max_bytes ceiling");
    expect(clamp({{QStringLiteral("offset"), -5}}).offset == 0, "INV-21: offset floor");
    expect(clamp({{QStringLiteral("offset"), 999999}}).offset == 999999,
           "INV-21: offset has no upper clamp — that bound is count");

    expect(clamp({{QStringLiteral("only"), QStringLiteral("stail")}}).only == DocCitations::Only::All,
           "INV-21: an unrecognised only coerces to all, it does not refuse");
    expect(clamp({{QStringLiteral("only"), QStringLiteral("stale")}}).only
               == DocCitations::Only::Stale,
           "INV-21: stale is recognised");
    // The wrong-typed case must use max_range_lines: offset's default and clamp
    // floor are both 0, so it cannot tell fall-back-to-default from
    // fall-back-to-clamp-endpoint.
    expect(clamp({{QStringLiteral("max_range_lines"), QStringLiteral("x")}}).maxRangeLines
               == def.maxRangeLines,
           "INV-21: a wrong-typed numeric falls back to its DEFAULT, not to a clamp endpoint");
    EXPECT_EQ(0, expect_failures());
}

// INV-19 — seven registration sites across two files, plus the two enrolments
// § 2.1 declines on contract grounds.
TEST(DocCitationsVerb, Inv19RegisteredAtSevenSites) {
    expect_reset();
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(mw.isEmpty());
    ASSERT_FALSE(ci.isEmpty());
    ASSERT_FALSE(rc.isEmpty());

    // 1 — registerToolProvider, with the contract as the second positional arg.
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"doc_citations\""));
    expect(reg >= 0, "INV-19 site 1: registered in mainwindow.cpp");
    expect(reg >= 0 && mw.mid(reg, 200).contains(QStringLiteral("CallerCwdContract::Required")),
           "INV-19 site 1: with CallerCwdContract::Required");

    const QString region = descriptorRegion(ci, QStringLiteral("doc_citations"));
    // 2 — the inputSchema.
    expect(region.contains(QStringLiteral("inputSchema")),
           "INV-19 site 2: the descriptor authors an inputSchema");
    // 3 — the selection_hint, under hook 3's undocumented format contract.
    const QString hint = authoredString(region, QStringLiteral("selection_hint"));
    expect(hint.startsWith(QStringLiteral("Use "), Qt::CaseInsensitive),
           "INV-19 site 3: the hint begins with \"Use \"", hint);
    expect(!hint.isEmpty() && hint.size() <= 240,
           "INV-19 site 3: the hint is at most 240 characters",
           QStringLiteral("len=%1").arg(hint.size()));
    // 4 — a kindForName bucket. Scoped to that lambda's own body: the name
    // appears again further down the file (the contract table), so an
    // indexOf-after-the-anchor test would pass with no bucket at all.
    const std::string kindBody =
        ants_test::slurpFunctionBody(ci.toStdString(), "kindForName");
    expect(!kindBody.empty()
               && ants_test::countOccurrences(kindBody, "\"doc_citations\"") >= 1,
           "INV-19 site 4: bucketed inside kindForName's own body");
    // 5 — the token-cost table.
    expect(ci.contains(QStringLiteral("{QStringLiteral(\"doc_citations\"),")),
           "INV-19 site 5: a token-cost entry");
    // 6a — callerCwdContractFor.
    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"doc_citations\")"));
    expect(cc >= 0 && ci.mid(cc, 120).contains(QStringLiteral("C::Required")),
           "INV-19 site 6a: callerCwdContractFor → Required");
    // 6b — isEtagSupportedTool.
    expect(ci.contains(QStringLiteral("|| toolName == QStringLiteral(\"doc_citations\")")),
           "INV-19 site 6b: enrolled in isEtagSupportedTool");
    // 7 — the handler itself.
    expect(rc.contains(QStringLiteral("RemoteControl::cmdDocCitations")),
           "INV-19 site 7: the handler exists");

    // Negative: enrolment in either would delete anchor_found:false and
    // truncated:false, which § 2.1 requires present when false.
    // ANTS-4524 — `fields=` is universal and harmless here (opt-in, and the
    // caller names what it wants). What would delete those flags is
    // COMPACTION, so that is the enrolment this asserts the absence of.
    expect(!mcp::isCompactArgTool(QStringLiteral("doc_citations")),
           "INV-19: declares no `compact` — it would drop its false flags");
    expect(!mcp::isRawEligible(QStringLiteral("doc_citations")),
           "INV-19: NOT raw-eligible — file text is one field among many");
    EXPECT_EQ(0, expect_failures());
}

// INV-22 — the authored descriptor's shape, including the two step-11
// prohibitions that no existing test covers.
TEST(DocCitationsVerb, Inv22DescriptorShape) {
    expect_reset();
    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    const QString region = descriptorRegion(ci, QStringLiteral("doc_citations"));
    ASSERT_FALSE(region.isEmpty());

    expect(region.contains(QStringLiteral("schema[\"type\"] = \"object\"")),
           "INV-22: type is object");
    expect(region.contains(QStringLiteral("schema[\"additionalProperties\"] = false")),
           "INV-22: additionalProperties is false");
    // Anchored on the assignment, not on the bare word: the description ends
    // "caller_cwd required." and would match first.
    const int req = region.indexOf(QStringLiteral("schema[\"required\"]"));
    expect(req >= 0 && region.mid(req, 160).contains(QStringLiteral("caller_cwd"))
               && region.mid(req, 160).contains(QStringLiteral("path")),
           "INV-22: an explicit required[] naming caller_cwd and path");

    // All eleven properties. The optional request args are load-bearing:
    // under additionalProperties:false an undeclared one is client-rejected,
    // which would leave INV-21's whole clamp path unreachable — and the exact
    // count is what catches a property added to the handler but not here.
    // 9 → 11 by ANTS-4386, which added the opt-in quotation check
    // (`quotes` + `quote_min_chars`).
    for (const char *p : {"caller_cwd", "path", "only", "offset", "max_range_lines",
                          "max_bytes", "max_doc_lines", "etag_match", "encoding",
                          "quotes", "quote_min_chars"})
        expect(region.contains(QStringLiteral("props[\"%1\"]").arg(QLatin1String(p))),
               "INV-22: property declared", QLatin1String(p));
    expect(ants_test::countOccurrences(region.toStdString(), "props[\"") == 11,
           "INV-22: exactly eleven properties, no more");
    expect(region.contains(QStringLiteral("makeEtagMatchProp()"))
               && region.contains(QStringLiteral("makeEncodingProp()")),
           "INV-22: the shared etag_match / encoding makers are used");

    // Prohibition 1 — a comment containing props[" makes the
    // mcp_dispatch_forward_completeness scraper miscount properties.
    for (const QString &line : region.split(QLatin1Char('\n')))
        expect(!(line.trimmed().startsWith(QStringLiteral("//"))
                 && line.contains(QStringLiteral("props[\""))),
               "INV-22: no descriptor comment contains the literal props[\"", line);

    // Prohibition 2 — the tools/list loop prepends a [<kind>] tag only when the
    // description does not already start with `[`, so a hand-written bracket
    // costs the verb its kind tag rather than doubling it.
    const QString desc = authoredString(region, QStringLiteral("description"));
    expect(!desc.isEmpty(), "INV-22: the descriptor authors a description");
    expect(!desc.startsWith(QLatin1Char('[')),
           "INV-22: the authored description does not begin with [", desc.left(60));
    EXPECT_EQ(0, expect_failures());
}

// INV-48 — the Required contract, which is what makes the dispatcher answer
// caller_cwd_required before the handler runs.
// ANTS-4728 - a bare basename naming a markdown document resolves where the
// codebase index cannot answer.
//
// Reported after measuring a skill tree: a skill cites a standard as
// `commits.md`, the file lives at standards/<name>, and none of the resolver's
// three routes reaches it - not the citing document's own directory, not the
// repo root, and not the basename map, which is EMPTY in a tree with no
// codebase index. Every such quotation came back target_unresolved. That is
// honest, but in an aggregate it is indistinguishable from a clean pass, and
// one measured case was a skill quoting its own superseded wording, where
// not_found was the right answer and never came.
TEST(DocCitationsVerb, Ants4728MdScanSupplementsAnEmptyBasenameIndex) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/standards/commits.md"),
                          QByteArrayLiteral("# Commits\n")));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/guide.md"),
                          QByteArrayLiteral("# Guide\n")));
    // A dot-directory must contribute nothing. It holds no document anyone
    // cites, and here it would also manufacture a false ambiguity on the very
    // basename the fix exists to resolve.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/.git/hooks/commits.md"),
                          QByteArrayLiteral("decoy\n")));

    QHash<QString, QStringList> index;
    const int seen = RemoteControl::docCitationsMdScan(root, &index);

    EXPECT_EQ(2, seen) << "two markdown files sit outside dot-directories";
    ASSERT_TRUE(index.contains(QStringLiteral("commits.md")))
        << "ANTS-4728: a standard cited by bare basename must be reachable";
    EXPECT_EQ(index.value(QStringLiteral("commits.md")),
              QStringList{QStringLiteral("standards/commits.md")});
    EXPECT_TRUE(index.contains(QStringLiteral("guide.md")));
    EXPECT_EQ(index.value(QStringLiteral("commits.md")).size(), 1)
        << "the decoy under .git must not create a false ambiguity";
}

// ANTS-4728 - and a basename with two real matches stays AMBIGUOUS.
TEST(DocCitationsVerb, Ants4728DuplicateBasenameIsLeftAmbiguous) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(writeFile(root + QStringLiteral("/a/notes.md"),
                          QByteArrayLiteral("a\n")));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/b/notes.md"),
                          QByteArrayLiteral("b\n")));

    QHash<QString, QStringList> index;
    RemoteControl::docCitationsMdScan(root, &index);
    EXPECT_EQ(index.value(QStringLiteral("notes.md")).size(), 2)
        << "both paths must be recorded so the engine takes its existing "
           "ambiguous arm; picking one here is how a check like this starts "
           "producing confident wrong answers";
}

TEST(DocCitationsVerb, Inv48CallerCwdRequired) {
    expect_reset();
    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    ASSERT_FALSE(mw.isEmpty());

    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"doc_citations\")"));
    expect(cc >= 0 && ci.mid(cc, 120).contains(QStringLiteral("C::Required")),
           "INV-48: the contract table says Required");
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"doc_citations\""));
    expect(reg >= 0 && mw.mid(reg, 200).contains(QStringLiteral("CallerCwdContract::Required")),
           "INV-48: and so does the registration — drift between them is refused at runtime");
    // The refusal itself is unreachable from here: docCitationsValidate never
    // sees a request without caller_cwd, because the dispatcher answered first.
    // That is exactly why caller_cwd is not in INV-18's set.
    expect(!RemoteControl::docCitationsValidate(QStringLiteral("/tmp"),
                                                {{QStringLiteral("path"), QStringLiteral("x.md")}})
                .contains(QStringLiteral("caller_cwd")),
           "INV-48: the handler has no caller_cwd branch to duplicate it");
    EXPECT_EQ(0, expect_failures());
}
