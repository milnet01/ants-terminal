// Feature-conformance test for tests/features/audit_run_allowlist/spec.md.
//
// ANTS-3615 — the headless `audit_run` engine now applies
// `.audit_allowlist.json` (previously GUI-only) and honours the
// `suppressions` request field (previously a silent no-op).

#include "auditengine.h"
#include "auditrunner.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

// One cppcheck-shaped finding line; the line-based parser keys its check id
// on the tool name, so an allowlist entry with "rule":"cppcheck" targets it.
const char *kRaw =
    "src/foo.cpp:42:7: warning: bogus thing here [uselessAssignment]\n";

bool writeAllowlist(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(body.toUtf8());
    f.close();
    return true;
}

QString entryJson(const QString &rule, const QString &glob,
                  const QString &lineRegex) {
    return QStringLiteral(
        "{\"version\":1,\"allowlist\":[{\"rule\":\"%1\","
        "\"path_glob\":\"%2\",\"line_regex\":\"%3\","
        "\"reason\":\"fixture\"}]}").arg(rule, glob, lineRegex);
}

}  // namespace

// INV-1 — a matching entry drops the finding from afterFilterCount while
// rawCount keeps the tool's true raw total.
TEST(AuditRunAllowlist, Inv1MatchingEntrySuppresses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/.audit_allowlist.json";
    ASSERT_TRUE(writeAllowlist(
        path, entryJson("cppcheck", "src/**", "bogus thing")));

    const AuditRunner::internal::ParsedCounts before =
        AuditRunner::internal::parseWithSuppression("cppcheck", kRaw, 10, {});
    ASSERT_EQ(before.rawCount, 1) << "fixture must parse as one finding";
    EXPECT_EQ(before.afterFilterCount, 1) << "no allowlist → not suppressed";

    const AuditRunner::internal::ParsedCounts after =
        AuditRunner::internal::parseWithSuppression(
            "cppcheck", kRaw, 10, {}, path);
    EXPECT_EQ(after.rawCount, 1) << "INV-1: rawCount keeps the raw total";
    EXPECT_EQ(after.afterFilterCount, 0) << "INV-1: allowlisted finding drops";
    EXPECT_EQ(after.sampleCount, 0) << "INV-1: suppressed → not sampled";
    EXPECT_EQ(after.findingsCount, 0) << "INV-1: suppressed → not in SARIF set";
}

// INV-2 — rule identity is exact; a non-matching rule never suppresses even
// when path and message patterns both match.
TEST(AuditRunAllowlist, Inv2RuleIdentityIsExact) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/.audit_allowlist.json";
    ASSERT_TRUE(writeAllowlist(
        path, entryJson("clazy", "src/**", "bogus thing")));

    const AuditRunner::internal::ParsedCounts c =
        AuditRunner::internal::parseWithSuppression(
            "cppcheck", kRaw, 10, {}, path);
    EXPECT_EQ(c.afterFilterCount, 1)
        << "INV-2: an entry for a different rule must not suppress";
}

// INV-2b — a non-matching path_glob does not suppress either.
TEST(AuditRunAllowlist, Inv2bPathGlobMustMatch) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/.audit_allowlist.json";
    ASSERT_TRUE(writeAllowlist(
        path, entryJson("cppcheck", "tests/**", "bogus thing")));

    const AuditRunner::internal::ParsedCounts c =
        AuditRunner::internal::parseWithSuppression(
            "cppcheck", kRaw, 10, {}, path);
    EXPECT_EQ(c.afterFilterCount, 1)
        << "INV-2: src/foo.cpp is not under tests/**";
}

// INV-3 — a catastrophic line_regex is dropped by the loader, so it can
// neither suppress nor pin the parser; the file stays usable.
TEST(AuditRunAllowlist, Inv3CatastrophicRegexDropped) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/.audit_allowlist.json";
    // (a+)+ — quantifier inside a quantified group, the canonical
    // catastrophic-backtracking shape isCatastrophicRegex rejects.
    ASSERT_TRUE(writeAllowlist(
        path, entryJson("cppcheck", "src/**", "(a+)+")));

    EXPECT_TRUE(AuditEngine::loadAllowlist(path).isEmpty())
        << "INV-3: shape-DoS entry rejected at load";

    const AuditRunner::internal::ParsedCounts c =
        AuditRunner::internal::parseWithSuppression(
            "cppcheck", kRaw, 10, {}, path);
    EXPECT_EQ(c.afterFilterCount, 1)
        << "INV-3: a rejected entry suppresses nothing";
}

// INV-3b — missing and malformed files yield an empty allowlist, not an error.
TEST(AuditRunAllowlist, Inv3bMissingAndMalformedAreEmpty) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_TRUE(AuditEngine::loadAllowlist(
        dir.path() + "/.audit_allowlist.json").isEmpty())
        << "INV-3: missing file → empty";

    const QString bad = dir.path() + "/bad.json";
    ASSERT_TRUE(writeAllowlist(bad, QStringLiteral("{not json")));
    EXPECT_TRUE(AuditEngine::loadAllowlist(bad).isEmpty())
        << "INV-3: malformed file → empty";
}

// INV-4 — an unrecognised `suppressions` value refuses bad_args rather than
// being silently ignored. Refusal lands before any tool is resolved.
TEST(AuditRunAllowlist, Inv4UnknownSuppressionsModeRefuses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    AuditRunner::RunRequest req;
    req.projectRoot      = dir.path();
    req.suppressionsMode = QStringLiteral("sometimes");

    const AuditRunner::RunResult r = AuditRunner::runAudit(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_args"))
        << "INV-4: unrecognised suppressions must refuse, not no-op";
}

// INV-4b — the three recognised forms are accepted by the validator (they do
// not refuse with bad_args / bad_path on a well-formed request).
TEST(AuditRunAllowlist, Inv4bRecognisedModesAccepted) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/.audit_allowlist.json";
    ASSERT_TRUE(writeAllowlist(
        path, entryJson("cppcheck", "src/**", "bogus thing")));

    for (const QString &mode : {QStringLiteral(""), QStringLiteral("auto"),
                                QStringLiteral("none"),
                                QStringLiteral("path:.audit_allowlist.json")}) {
        AuditRunner::RunRequest req;
        req.projectRoot      = dir.path();
        req.suppressionsMode = mode;
        // Name a tool that cannot resolve, so the run stops at tool
        // resolution — well past the suppressions gate — without spawning.
        req.tools            = {QStringLiteral("cppcheck")};
        const AuditRunner::RunResult r = AuditRunner::runAudit(req);
        EXPECT_NE(r.code, QStringLiteral("bad_args"))
            << "INV-4: \"" << mode.toStdString() << "\" must be accepted";
        EXPECT_NE(r.code, QStringLiteral("bad_path"))
            << "INV-5: \"" << mode.toStdString() << "\" is inside the root";
    }
}

// INV-5 — a `path:` target outside the project root refuses bad_path.
TEST(AuditRunAllowlist, Inv5PathEscapeRefused) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    AuditRunner::RunRequest req;
    req.projectRoot      = dir.path();
    req.suppressionsMode = QStringLiteral("path:/etc/passwd");

    const AuditRunner::RunResult r = AuditRunner::runAudit(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_path"))
        << "INV-5: a suppressions path outside the root must refuse";

    // ...and so does a traversal that starts relative.
    req.suppressionsMode = QStringLiteral("path:../../etc/passwd");
    const AuditRunner::RunResult r2 = AuditRunner::runAudit(req);
    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(r2.code, QStringLiteral("bad_path"))
        << "INV-5: relative traversal must refuse too";
}

// INV-6 — one implementation: the engine's glob compiler is what the dialog's
// allowlist globs go through, and the matcher is a pure free function.
TEST(AuditRunAllowlist, Inv6SharedEngineImplementation) {
    const QRegularExpression rx = AuditEngine::globToRegex("src/**");
    EXPECT_TRUE(rx.match("src/foo.cpp").hasMatch());
    EXPECT_TRUE(rx.match("src/a/b/foo.cpp").hasMatch()) << "** spans slashes";
    EXPECT_FALSE(rx.match("tests/foo.cpp").hasMatch());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + "/.audit_allowlist.json";
    ASSERT_TRUE(writeAllowlist(
        path, entryJson("cppcheck", "src/**", "bogus thing")));
    const QList<AuditEngine::AllowlistEntry> al =
        AuditEngine::loadAllowlist(path);
    ASSERT_EQ(al.size(), 1);
    EXPECT_TRUE(AuditEngine::allowlisted(
        al, "cppcheck", "src/foo.cpp", "warning: bogus thing here"));
    EXPECT_FALSE(AuditEngine::allowlisted(
        al, "cppcheck", "src/foo.cpp", "warning: some other message"));
    EXPECT_FALSE(AuditEngine::allowlisted({}, "cppcheck", "src/foo.cpp",
                                          "warning: bogus thing here"))
        << "empty allowlist never matches";
}

// INV-7 (ANTS-3626) — `formats` is validated, the same silent-no-op class
// INV-4 closed for `suppressions`. An unrecognised value used to be
// simultaneously non-empty (suppressing the ["sarif"] default) and matched
// by neither the sarif nor the html branch, so the run wrote no artifact,
// omitted sarif_path/cache_path, and still returned ok:true — discarding a
// potentially 15-minute sweep on a typo.
TEST(AuditRunAllowlist, Inv7UnknownFormatRefuses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    AuditRunner::RunRequest req;
    req.projectRoot = dir.path();
    req.formats     = {QStringLiteral("json")};

    const AuditRunner::RunResult r = AuditRunner::runAudit(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_args"))
        << "INV-7: an unrecognised format must refuse, not silently "
           "produce no artifact";

    // A mixed list refuses on the bad member — a valid sibling does not
    // launder it through.
    req.formats = {QStringLiteral("sarif"), QStringLiteral("pdf")};
    const AuditRunner::RunResult r2 = AuditRunner::runAudit(req);
    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(r2.code, QStringLiteral("bad_args"));
}

// INV-7b — the accepted set, and an EMPTY array, are not refused. Empty is
// valid and keeps the ["sarif"] default; pinning it here stops a future
// tightening from turning the default path into a refusal.
TEST(AuditRunAllowlist, Inv7bAcceptedFormatsNotRefused) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QList<QStringList> ok = {
        {},
        {QStringLiteral("sarif")},
        {QStringLiteral("html")},
        {QStringLiteral("sarif"), QStringLiteral("html")},
    };
    for (const QStringList &f : ok) {
        AuditRunner::RunRequest req;
        req.projectRoot = dir.path();
        req.formats     = f;
        // Name a tool that cannot resolve, so the run stops well past the
        // formats gate without spawning anything.
        req.tools       = {QStringLiteral("cppcheck")};
        const AuditRunner::RunResult r = AuditRunner::runAudit(req);
        EXPECT_NE(r.code, QStringLiteral("bad_args"))
            << "INV-7b: formats [" << f.join(QLatin1Char(',')).toStdString()
            << "] must be accepted";
    }
}
