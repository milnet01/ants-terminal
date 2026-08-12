// ANTS-4108 — feature-conformance test for SpecConformance::run.
// See tests/features/spec_conformance/spec.md.
//
// The parent spec is accepted-with-caveat (§ 2.3-2.6 provisional), so these
// fixtures are the arbiter: where one disagrees with the prose, the spec is
// amended, not the test.

#include "specconformance.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

// Write a fixture .md and return its absolute path.
QString writeSpec(const QTemporaryDir &d, const char *name, const QByteArray &body) {
    const QString p = d.filePath(QLatin1String(name));
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(body);
    f.close();
    return p;
}

QJsonArray arr(const QJsonObject &o, const char *key) {
    return o.value(QLatin1String(key)).toArray();
}
QJsonObject at(const QJsonObject &o, const char *key, int i) {
    return arr(o, key).at(i).toObject();
}

// A three-backtick regex fence plus a GFM expectation table.
QByteArray fence(const char *info, const char *pattern) {
    return QByteArray("```") + info + "\n" + pattern + "\n```\n\n";
}

}  // namespace

// INV-1 — one case per table row; only TOP-LEVEL fences are scanned.
TEST(spec_conformance, Inv1OneCasePerRowTopLevelOnly) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p = writeSpec(d, "a.md",
        fence("regex pcre2", "^(?:const|let)\\s++([A-Za-z_$][\\w$]*)\\s*=") +
        "| input | expected |\n|---|---|\n"
        "| `const CSS = ` | `CSS` |\n"
        "| `let x = ` | `x` |\n"
        "| `  const local = ` | no match |\n");
    const QJsonObject env = SpecConformance::run(p);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("cases_run").toInt(), 3)
        << "one case per expectation row";
    EXPECT_TRUE(arr(env, "findings").isEmpty())
        << "all three rows are correct — no finding expected";
}

TEST(spec_conformance, Inv1NestedFenceIsNotScanned) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    // A four-backtick markdown fence CONTAINING a regex fence + table — the
    // shape of the parent spec's own § 2.4 illustration. A naive line scan
    // extracts a case from it and reports the deliberately-wrong row as real.
    const QString p = writeSpec(d, "b.md",
        "````markdown\n"
        "```regex pcre2\n^zzz\n```\n\n"
        "| input | expected |\n|---|---|\n| `qqq` | `zzz` |\n"
        "````\n");
    const QJsonObject env = SpecConformance::run(p);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("cases_run").toInt(), 0)
        << "a fence nested inside a longer-delimiter fence is not a case";
    EXPECT_TRUE(arr(env, "findings").isEmpty());
    EXPECT_TRUE(arr(env, "candidates").isEmpty());
}

// INV-1 — an INDENTED fence is not a fence, and the silence is total.
// This is the natural GFM rendering of a pattern invariant attached to an
// `- **INV-N**` bullet, so it is the mistake an author is most likely to make;
// docs/standards/specs.md § 3.5.1 warns about it precisely because nothing
// here reports it.
TEST(spec_conformance, Inv1IndentedFenceIsNotScanned) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p = writeSpec(d, "ind.md",
        "- **INV-1** — the bullet this block is attached to.\n\n"
        "  ```regex pcre2\n  ^(a)\n  ```\n\n"
        "  | input | expected |\n  |---|---|\n  | `ab` | `a` |\n");
    const QJsonObject env = SpecConformance::run(p);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("cases_run").toInt(), 0)
        << "fenceDelimLen counts from index 0 — an indented fence is not one";
    EXPECT_TRUE(arr(env, "findings").isEmpty());
    EXPECT_TRUE(arr(env, "candidates").isEmpty())
        << "invisible, NOT a candidate — this failure is silent by design";
    EXPECT_TRUE(arr(env, "refusals").isEmpty());
}

// INV-1 — a fence whose info string's FIRST word is not exactly `regex` is not
// ours at all. Distinct from INV-5's `regex python`, which IS ours and refuses:
// here the engine check never runs, so there is no refusal to report.
TEST(spec_conformance, Inv1NonRegexFirstWordIsInvisible) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p = writeSpec(d, "tw.md",
        fence("regexp pcre2", "^(a)") +
        "| input | expected |\n|---|---|\n| `ab` | `a` |\n");
    const QJsonObject env = SpecConformance::run(p);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("cases_run").toInt(), 0);
    EXPECT_TRUE(arr(env, "findings").isEmpty());
    EXPECT_TRUE(arr(env, "candidates").isEmpty())
        << "not engine_not_declared — a `regexp` fence is not ours to judge";
    EXPECT_TRUE(arr(env, "refusals").isEmpty())
        << "not unsupported_engine — that is only for `regex <other>`";
}

// INV-2 — the reporter's own defect is the canonical finding.
TEST(spec_conformance, Inv2MismatchIsAFinding) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p = writeSpec(d, "c.md",
        fence("regex pcre2", "\\d{1,5}(?![0-9])") +
        "| input | expected |\n|---|---|\n| ` 123456` | no match |\n");
    const QJsonObject env = SpecConformance::run(p);
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_EQ(arr(env, "findings").size(), 1)
        << "a lookahead does not reject a longer number under SEARCH";
    const QJsonObject f = at(env, "findings", 0);
    EXPECT_EQ(f.value("kind").toString().toStdString(), "mismatch");
    EXPECT_EQ(f.value("expected").toString().toStdString(), "no match");
    EXPECT_EQ(f.value("actual").toString().toStdString(), "23456");
    EXPECT_FALSE(f.value("pattern").toString().isEmpty());
    EXPECT_GT(f.value("line").toInt(), 0) << "line points at the row";
}

// INV-3 — no match / no capture / empty capture stay three distinct outcomes.
TEST(spec_conformance, Inv3ThreeOutcomesStayDistinct) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    // a(b*) on "a": group 1 PARTICIPATES and captures the empty string.
    const QString p1 = writeSpec(d, "d1.md",
        fence("regex pcre2", "a(b*)") +
        "| input | expected |\n|---|---|\n| `a` | `` |\n");
    EXPECT_TRUE(arr(SpecConformance::run(p1), "findings").isEmpty())
        << "an empty capture must satisfy the empty-backtick encoding";

    const QString p2 = writeSpec(d, "d2.md",
        fence("regex pcre2", "a(b*)") +
        "| input | expected |\n|---|---|\n| `a` | no match |\n");
    EXPECT_EQ(arr(SpecConformance::run(p2), "findings").size(), 1)
        << "an empty capture must NOT satisfy `no match`";

    // a(b)? on "a": group 1 does NOT participate.
    const QString p3 = writeSpec(d, "d3.md",
        fence("regex pcre2", "a(b)?") +
        "| input | expected |\n|---|---|\n| `a` | no capture |\n");
    EXPECT_TRUE(arr(SpecConformance::run(p3), "findings").isEmpty())
        << "a non-participating group must satisfy `no capture`";

    const QString p4 = writeSpec(d, "d4.md",
        fence("regex pcre2", "a(b)?") +
        "| input | expected |\n|---|---|\n| `a` | `` |\n");
    EXPECT_EQ(arr(SpecConformance::run(p4), "findings").size(), 1)
        << "a non-participating group is NOT an empty capture — QString() == "
           "QString(\"\") is true, so this fails for a build using ==";
}

// INV-4 — an unchecked pattern is a CANDIDATE, one per fence.
TEST(spec_conformance, Inv4CandidateKinds) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p1 = writeSpec(d, "e1.md", fence("regex pcre2", "^abc"));
    const QJsonObject e1 = SpecConformance::run(p1);
    ASSERT_EQ(arr(e1, "candidates").size(), 1);
    EXPECT_EQ(at(e1, "candidates", 0).value("kind").toString().toStdString(),
              "pattern_without_expectation");
    EXPECT_TRUE(arr(e1, "findings").isEmpty());

    // Bare `regex` with no engine AND no table: engine check wins, and a
    // fence yields at most ONE candidate.
    const QString p2 = writeSpec(d, "e2.md", fence("regex", "^abc"));
    const QJsonObject e2 = SpecConformance::run(p2);
    ASSERT_EQ(arr(e2, "candidates").size(), 1)
        << "at most one candidate per fence";
    EXPECT_EQ(at(e2, "candidates", 0).value("kind").toString().toStdString(),
              "engine_not_declared");
}

// INV-5 — unsupported engine refuses per case, never substitutes.
TEST(spec_conformance, Inv5UnsupportedEngineRefusesPerCase) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p1 = writeSpec(d, "f1.md", fence("regex python", "(?P<w>a)"));
    const QJsonObject e1 = SpecConformance::run(p1);
    ASSERT_TRUE(e1.value("ok").toBool()) << "a bad fence must not fail the call";
    ASSERT_EQ(arr(e1, "refusals").size(), 1);
    EXPECT_EQ(at(e1, "refusals", 0).value("code").toString().toStdString(),
              "unsupported_engine");
    EXPECT_EQ(e1.value("cases_run").toInt(), 0);
    EXPECT_TRUE(arr(e1, "candidates").isEmpty())
        << "engine check precedes the table check — not also a candidate";

    // Mixed file: the valid cases still run.
    const QString p2 = writeSpec(d, "f2.md",
        fence("regex python", "(?P<w>a)") +
        fence("regex pcre2", "^(a)") +
        "| input | expected |\n|---|---|\n| `ab` | `a` |\n| `ba` | no match |\n");
    const QJsonObject e2 = SpecConformance::run(p2);
    ASSERT_TRUE(e2.value("ok").toBool());
    EXPECT_EQ(e2.value("cases_run").toInt(), 2)
        << "one stray fence must not blind the file";
    EXPECT_EQ(arr(e2, "refusals").size(), 1);
}

// INV-6 — the verb writes nothing.
TEST(spec_conformance, Inv6WritesNothing) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p = writeSpec(d, "g.md",
        fence("regex pcre2", "\\d{1,5}(?![0-9])") +
        "| input | expected |\n|---|---|\n| ` 123456` | no match |\n");
    auto digest = [&] {
        QCryptographicHash h(QCryptographicHash::Sha256);
        QDirIterator it(d.path(), QDir::Files, QDirIterator::Subdirectories);
        QStringList files;
        while (it.hasNext()) files << it.next();
        files.sort();
        for (const QString &f : files) {
            QFile fh(f);
            if (fh.open(QIODevice::ReadOnly)) h.addData(fh.readAll());
            h.addData(f.toUtf8());
        }
        return h.result().toHex();
    };
    const QByteArray before = digest();
    SpecConformance::run(p);
    EXPECT_EQ(before, digest()) << "the engine must not write";
}

// INV-7 — caps enforced and reported, never silently applied.
TEST(spec_conformance, Inv7CapsAreReported) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QByteArray big(600, 'a');
    const QString p1 = writeSpec(d, "h1.md",
        fence("regex pcre2", big.constData()) +
        "| input | expected |\n|---|---|\n| `a` | `a` |\n");
    const QJsonObject e1 = SpecConformance::run(p1);
    ASSERT_EQ(arr(e1, "refusals").size(), 1) << "over-cap pattern";
    EXPECT_EQ(at(e1, "refusals", 0).value("code").toString().toStdString(),
              "too_large");

    QByteArray rows = "| input | expected |\n|---|---|\n| `";
    rows += big;
    rows += "` | no match |\n";
    const QString p2 = writeSpec(d, "h2.md", fence("regex pcre2", "^zzz") + rows);
    const QJsonObject e2 = SpecConformance::run(p2);
    ASSERT_EQ(arr(e2, "refusals").size(), 1) << "over-cap input";
    EXPECT_EQ(at(e2, "refusals", 0).value("code").toString().toStdString(),
              "too_large");

    // max_cases outside its range refuses the CALL — it is not clamped.
    SpecConformance::Options bad;
    bad.maxCases = 5000;
    const QJsonObject e3 = SpecConformance::run(p1, bad);
    EXPECT_FALSE(e3.value("ok").toBool())
        << "an out-of-range max_cases refuses rather than clamping";
    EXPECT_EQ(e3.value("code").toString().toStdString(), "bad_args");

    // More rows than max_cases → truncated, with cases_run at the cap.
    QByteArray many = "| input | expected |\n|---|---|\n";
    for (int i = 0; i < 5; ++i) many += "| `abc` | `abc` |\n";
    const QString p4 = writeSpec(d, "h4.md", fence("regex pcre2", "abc") + many);
    SpecConformance::Options three;
    three.maxCases = 3;
    const QJsonObject e4 = SpecConformance::run(p4, three);
    ASSERT_TRUE(e4.value("ok").toBool());
    EXPECT_TRUE(e4.value("truncated").toBool());
    EXPECT_EQ(e4.value("cases_run").toInt(), 3);
}

// INV-8 — no subprocess, no interpreter. A source-scrape, because no running
// test can observe the ABSENCE of an interpreter. Scoped to the two files this
// change creates: the shared TUs the verb is wired into already carry 34 such
// lines for dozens of unrelated verbs, so a wider scrape would assert zero
// against 34 and be red for every possible implementation.
TEST(spec_conformance, Inv8NoSubprocessNoInterpreter) {
    const QRegularExpression forbidden(QStringLiteral(
        "\\bQProcess\\b|system\\(|popen|fork\\(|lua_newstate|luaL_newstate|"
        "Py_Initialize"));
    for (const char *rel : {"src/specconformance.h", "src/specconformance.cpp"}) {
        const QString path =
            QStringLiteral(ANTS_SOURCE_DIR) + QLatin1Char('/') +
            QLatin1String(rel);
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << rel << " must exist";
        const QString body = QString::fromUtf8(f.readAll());
        f.close();
        // The header's own prose names QProcess to explain the scope; strip
        // comment lines so the check is about CODE, not about what the file
        // says — the same mistake ANTS-4124's first guard made.
        QString code;
        for (const QString &line : body.split(QLatin1Char('\n'))) {
            const QString t = line.trimmed();
            if (t.startsWith(QLatin1String("//"))) continue;
            code += line;
            code += QLatin1Char('\n');
        }
        EXPECT_FALSE(code.contains(forbidden))
            << rel << " must not run a subprocess or load an interpreter";
    }
}

// INV-9 — stable across runs except for observations; etag stable.
TEST(spec_conformance, Inv9StableAcrossRuns) {
    QTemporaryDir d;
    ASSERT_TRUE(d.isValid());
    const QString p = writeSpec(d, "i.md",
        fence("regex pcre2", "\\d{1,5}(?![0-9])") +
        "| input | expected |\n|---|---|\n| ` 123456` | no match |\n");
    QJsonObject a = SpecConformance::run(p);
    QJsonObject b = SpecConformance::run(p);
    EXPECT_FALSE(a.value("etag").toString().isEmpty()) << "etag must be emitted";
    EXPECT_EQ(a.value("etag").toString(), b.value("etag").toString());
    a.remove(QStringLiteral("observations"));
    b.remove(QStringLiteral("observations"));
    EXPECT_EQ(QJsonDocument(a).toJson(QJsonDocument::Compact),
              QJsonDocument(b).toJson(QJsonDocument::Compact))
        << "envelopes must agree once timings are elided";
    EXPECT_FALSE(arr(SpecConformance::run(p), "observations").isEmpty())
        << "a timing observation is emitted per case";
}
