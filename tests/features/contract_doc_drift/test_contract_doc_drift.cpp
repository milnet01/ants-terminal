// ANTS-3600 — contract-doc ↔ code literal-drift lane, conformance test.
// See spec.md for the full contract. Headless: drives
// FeatureCoverage::runContractDocDriftCheck directly over a temp fixture
// tree (no QtWidgets, no AuditDialog), asserting INV-1 … INV-10.

#include "featurecoverage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#ifndef SRC_AUDIT_CPP_PATH
#error "SRC_AUDIT_CPP_PATH compile definition required (INV-10 source-grep)"
#endif

namespace {

// Write `content` to `path`, creating parent dirs. Returns false on I/O error.
bool writeFile(const QString &path, const QString &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(content.toUtf8());
    return true;
}

// How many finding lines mention `needle`.
int countMentions(const QString &out, const QString &needle) {
    int n = 0;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &l : lines)
        if (l.contains(needle)) ++n;
    return n;
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1 (unknown symbol flags), INV-3 (doc-B prose does not resolve),
// INV-5 (slash-path literal flags). A real symbol in src/ does NOT flag.
TEST(ContractDocDrift, DriftAndResolve) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    ASSERT_TRUE(writeFile(root + "/src/code.cpp",
                          "void realSymbol() {}\n// key: x-checker-data\n"));
    ASSERT_TRUE(writeFile(root + "/docs/standards/doc.md",
                          "Cites `realSymbol` (resolves).\n"
                          "Cites `nonexistent_symbol_xyz` (INV-1).\n"
                          "Cites `archived/unknown` (INV-5 slash-path).\n"
                          "Cites `madeUpToken` (INV-3, only in doc B prose).\n"));
    // doc B mentions madeUpToken as PROSE (no backticks) — it is a *.md body,
    // excluded from the blob, so it must not resolve doc A's citation.
    ASSERT_TRUE(writeFile(root + "/docs/standards/docB.md",
                          "Here madeUpToken appears only as prose.\n"));

    const QString out = FeatureCoverage::runContractDocDriftCheck(root);

    EXPECT_EQ(countMentions(out, "nonexistent_symbol_xyz"), 1);
    EXPECT_EQ(countMentions(out, "archived/unknown"), 1);
    EXPECT_EQ(countMentions(out, "madeUpToken"), 1);
    EXPECT_EQ(countMentions(out, "realSymbol"), 0);
    EXPECT_EQ(countMentions(out, "x-checker-data"), 0);  // config key resolves
}

// INV-2 — a literal naming a real file is not flagged, even for the
// name-excluded ROADMAP.md/CHANGELOG.md and an extensionless LICENSE
// (the manifest append precedes every walk gate). Deleting the files
// makes all three flag — proving the manifest, not doc prose, resolves them.
TEST(ContractDocDrift, RealFilenamesResolveViaManifest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    ASSERT_TRUE(writeFile(root + "/src/code.cpp", "int x;\n"));
    ASSERT_TRUE(writeFile(root + "/ROADMAP.md", "# roadmap\n"));
    ASSERT_TRUE(writeFile(root + "/CHANGELOG.md", "# changelog\n"));
    ASSERT_TRUE(writeFile(root + "/LICENSE", "MIT\n"));
    ASSERT_TRUE(writeFile(root + "/docs/standards/doc.md",
                          "See `ROADMAP.md`, `CHANGELOG.md` and `LICENSE`.\n"));

    QString out = FeatureCoverage::runContractDocDriftCheck(root);
    EXPECT_EQ(countMentions(out, "ROADMAP.md"), 0);
    EXPECT_EQ(countMentions(out, "CHANGELOG.md"), 0);
    EXPECT_EQ(countMentions(out, "LICENSE"), 0);

    // Remove the three real files; the citations now drift.
    ASSERT_TRUE(QFile::remove(root + "/ROADMAP.md"));
    ASSERT_TRUE(QFile::remove(root + "/CHANGELOG.md"));
    ASSERT_TRUE(QFile::remove(root + "/LICENSE"));
    out = FeatureCoverage::runContractDocDriftCheck(root);
    EXPECT_EQ(countMentions(out, "ROADMAP.md"), 1);
    EXPECT_EQ(countMentions(out, "CHANGELOG.md"), 1);
    EXPECT_EQ(countMentions(out, "LICENSE"), 1);
}

// INV-4 — a literal inside a fenced code block is not extracted; the same
// token inline in prose is.
TEST(ContractDocDrift, FencedTokensSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    ASSERT_TRUE(writeFile(root + "/src/code.cpp", "int x;\n"));
    ASSERT_TRUE(writeFile(root + "/docs/standards/doc.md",
                          "Inline `driftInline` reference.\n"
                          "\n"
                          "```bash\n"
                          "echo `driftFenced`\n"
                          "```\n"));

    const QString out = FeatureCoverage::runContractDocDriftCheck(root);
    EXPECT_EQ(countMentions(out, "driftInline"), 1);
    EXPECT_EQ(countMentions(out, "driftFenced"), 0);
}

// INV-6 — an allowlisted token is never reported; #-comment and blank lines
// are ignored and surrounding whitespace is trimmed. Removing the entry
// makes the token flag (proving the allowlist, not blob coincidence — the
// allowlist file's own content is excluded from the blob).
TEST(ContractDocDrift, AllowlistSuppresses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    ASSERT_TRUE(writeFile(root + "/src/code.cpp", "int x;\n"));
    ASSERT_TRUE(writeFile(root + "/docs/standards/doc.md",
                          "Cites `someDriftToken` here.\n"));
    ASSERT_TRUE(writeFile(root + "/.ants_doc_drift_allow.txt",
                          "# a whole-line comment\n"
                          "\n"
                          "  someDriftToken  \n"));

    QString out = FeatureCoverage::runContractDocDriftCheck(root);
    EXPECT_EQ(countMentions(out, "someDriftToken"), 0);

    // Drop the allowlist entirely — the token now drifts.
    ASSERT_TRUE(QFile::remove(root + "/.ants_doc_drift_allow.txt"));
    out = FeatureCoverage::runContractDocDriftCheck(root);
    EXPECT_EQ(countMentions(out, "someDriftToken"), 1);
}

// INV-9 — silent no-op with neither docs/standards nor docs/specs; scans
// whichever exists when only one is present.
TEST(ContractDocDrift, NoOpAndPartialDir) {
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        ASSERT_TRUE(writeFile(tmp.path() + "/src/code.cpp", "int x;\n"));
        EXPECT_TRUE(FeatureCoverage::runContractDocDriftCheck(tmp.path()).isEmpty());
    }
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString root = tmp.path();
        ASSERT_TRUE(writeFile(root + "/src/code.cpp", "int x;\n"));
        // Only docs/specs/ present (no docs/standards/).
        ASSERT_TRUE(writeFile(root + "/docs/specs/ANTS-9001.md",
                              "Cites `driftInSpecsOnly` token.\n"));
        const QString out = FeatureCoverage::runContractDocDriftCheck(root);
        EXPECT_EQ(countMentions(out, "driftInSpecsOnly"), 1);
    }
}

// INV-11 (ANTS-3849) — a `<head>:<line|json-literal>` citation is not
// extracted: source spells the head and the literal separately, so the joined
// form can never resolve. Path-shaped heads were 49.5% of this lane's findings
// on the Ants corpus; widening to any head took a further 14.9%, measured at
// 128 of 129 residual heads resolving in source. The bare path is still in
// scope (INV-5), and a head carrying a colon (`Qt::CaseSensitive`) is not a
// citation.
TEST(ContractDocDrift, PathLineCitationsSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    ASSERT_TRUE(writeFile(root + "/src/code.cpp", "int x;\n"));
    ASSERT_TRUE(writeFile(root + "/docs/standards/doc.md",
                          "Cites `src/gone.cpp:2540` (citation, skipped).\n"
                          "Cites `gone.cpp:39-49` (range citation, skipped).\n"
                          "Cites `applyGone:3118` (symbol:line, skipped).\n"
                          "Cites `gone_checked:false` (field:value, skipped).\n"
                          "Cites `src/gone.cpp` (bare path, still flags).\n"
                          "Cites `Gone::method` (scoped name, still flags).\n"));

    const QString out = FeatureCoverage::runContractDocDriftCheck(root);
    EXPECT_EQ(countMentions(out, "src/gone.cpp:2540"), 0);
    EXPECT_EQ(countMentions(out, "gone.cpp:39-49"), 0);
    EXPECT_EQ(countMentions(out, "applyGone:3118"), 0);
    EXPECT_EQ(countMentions(out, "gone_checked:false"), 0);
    EXPECT_EQ(countMentions(out, "`src/gone.cpp`"), 1);
    EXPECT_EQ(countMentions(out, "Gone::method"), 1);
}

// INV-10 — the registered audit check leaves the output filter uncapped
// (maxLines = 0), so no finding is dropped past the default 100-line cap.
// Source-grep over auditdialog.cpp's populateChecks (the feature test cannot
// exercise applyFilter, so this is a registration invariant).
TEST(ContractDocDrift, RegistrationUncapped) {
    const QString src = readAll(QStringLiteral(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(src.isEmpty());
    const int idx = src.indexOf(QStringLiteral("contract_doc_drift"));
    ASSERT_GE(idx, 0) << "contract_doc_drift not registered in auditdialog.cpp";
    // The maxLines = 0 line must appear within the registration block.
    const QString window = src.mid(idx, 800);
    EXPECT_TRUE(window.contains(QStringLiteral("maxLines = 0")))
        << "contract_doc_drift must set c.filter.maxLines = 0 (INV-10)";
}
