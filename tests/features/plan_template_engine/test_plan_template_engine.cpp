// Feature-conformance test for ANTS-1290 PlanTemplateEngine.
// Drives PlanTemplateEngine::buildPlan against synthetic project
// trees under QTemporaryDir. Spec: tests/features/plan_template_engine/spec.md.

#include "plantemplateengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

namespace {

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

PlanTemplateEngine::PlanOptions defaultOpts(const QString &name) {
    PlanTemplateEngine::PlanOptions o;
    o.featureName = name;
    o.antsId = QStringLiteral("ANTS-9999");  // skip counter mutation
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// INV-1 — feature-name validation.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv1RejectsBadFeatureNames) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const char *bad[] = {
        "Live Search",   // space + uppercase
        "foo bar",       // space
        "Foo",           // uppercase
        "foo.bar",       // dot
        "foo/bar",       // slash
        "",              // empty
    };
    for (const char *name : bad) {
        auto opts = defaultOpts(QString::fromUtf8(name));
        opts.featureName = QString::fromUtf8(name);
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_FALSE(r.ok) << "expected reject for: " << name;
        EXPECT_EQ(r.errorCode, QStringLiteral("bad_feature_name"))
            << "name: " << name;
    }

    // Length cap: 65 chars rejected, 64 chars accepted.
    {
        auto opts = defaultOpts(QString(65, QLatin1Char('a')));
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_FALSE(r.ok) << "65-char feature name should reject";
    }
    {
        auto opts = defaultOpts(QString(64, QLatin1Char('a')));
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_TRUE(r.ok) << "64-char feature name should accept; err="
                          << r.errorCode.toStdString();
    }

    const char *good[] = { "live-search", "foo_bar", "a", "abc123", "a-b_c" };
    for (const char *name : good) {
        auto opts = defaultOpts(QString::fromUtf8(name));
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_TRUE(r.ok) << "expected accept for: " << name
                          << " err=" << r.errorCode.toStdString();
    }
}

// ---------------------------------------------------------------------------
// INV-10 (ANTS-1838) — ants_id validation at the trust boundary.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv10RejectsBadAntsId) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const char *bad[] = {
        "../../etc/passwd",   // traversal
        "ANTS-1234/../x",     // embedded slash + traversal
        "ants-1234",          // lowercase prefix
        "ANTS_1234",          // underscore separator
        "ANTS-",              // no number
        "-1234",              // no prefix
        "ANTS 1234",          // space
        "ANTS-12.34",         // dot
    };
    for (const char *id : bad) {
        auto opts = defaultOpts(QStringLiteral("foo"));
        opts.antsId = QString::fromUtf8(id);
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_FALSE(r.ok) << "expected reject for ants_id: " << id;
        EXPECT_EQ(r.errorCode, QStringLiteral("bad_args"))
            << "ants_id: " << id;
    }

    // Well-formed project-prefixed ids accept (dry-run, no counter).
    const char *good[] = { "ANTS-1234", "RETRO-42", "VEST-7", "A-1" };
    for (const char *id : good) {
        auto opts = defaultOpts(QStringLiteral("foo"));
        opts.antsId = QString::fromUtf8(id);
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_TRUE(r.ok) << "expected accept for ants_id: " << id
                          << " err=" << r.errorCode.toStdString();
    }

    // Empty ants_id is still allowed (means "allocate"); with no
    // .roadmap-counter present it falls through to AntsIdSource::None.
    {
        PlanTemplateEngine::PlanOptions opts;
        opts.featureName = QStringLiteral("foo");
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        EXPECT_TRUE(r.ok) << "empty ants_id must remain valid; err="
                          << r.errorCode.toStdString();
    }
}

// ---------------------------------------------------------------------------
// INV-2 — deterministic output.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv2DeterministicOutput) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    auto opts = defaultOpts(QStringLiteral("live-search"));
    opts.goal = QStringLiteral("Build a live-search filter.");
    opts.architecture = QStringLiteral("Debounce input; query Redis.");
    opts.techStack = QStringLiteral("Qt6, Redis");
    opts.taskCountHint = 3;

    const auto r1 = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    const auto r2 = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    ASSERT_TRUE(r1.ok);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r1.planMarkdown, r2.planMarkdown)
        << "buildPlan must be deterministic for identical inputs";
    EXPECT_EQ(r1.planPath, r2.planPath);
    EXPECT_EQ(r1.antsId, r2.antsId);
}

// ---------------------------------------------------------------------------
// INV-3 — `.roadmap-counter` atomic mutation via RoadmapFoldIn.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv3AtomicCounterIncrement) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".roadmap-counter", "5\n");

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName = QStringLiteral("foo");
    // No antsId — should allocate.

    const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    ASSERT_TRUE(r.ok) << "err=" << r.errorCode.toStdString();
    EXPECT_EQ(r.antsId, QStringLiteral("ANTS-6"));
    EXPECT_EQ(r.antsIdSource, PlanTemplateEngine::AntsIdSource::Allocated);

    // Counter post-call should be `6\n`, single line.
    QFile c(QDir(tmp.path()).filePath(QStringLiteral(".roadmap-counter")));
    ASSERT_TRUE(c.open(QIODevice::ReadOnly));
    EXPECT_EQ(c.readAll(), QByteArray("6\n"));
    // No tmpfile residue.
    EXPECT_FALSE(QFileInfo::exists(
        QDir(tmp.path()).filePath(QStringLiteral(".roadmap-counter.tmp"))));
}

TEST(PlanTemplateEngine, Inv3MissingCounterFallsThroughToNone) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName = QStringLiteral("foo");
    // No antsId, no counter file → AntsIdSource::None.

    const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    EXPECT_TRUE(r.ok) << "err=" << r.errorCode.toStdString();
    EXPECT_TRUE(r.antsId.isEmpty());
    EXPECT_EQ(r.antsIdSource, PlanTemplateEngine::AntsIdSource::None);
    EXPECT_TRUE(r.planMarkdown.contains(QStringLiteral("<ANTS-ID>")))
        << "skeleton must carry the literal placeholder when allocation fails";
    EXPECT_EQ(r.planPath, QStringLiteral("docs/plans/foo.md"));
}

// ---------------------------------------------------------------------------
// INV-4 — strict-below path-traversal guard.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv4SymlinkOutsideRejected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Make docs/plans/ AND an external target.
    QDir().mkpath(QDir(tmp.path()).filePath(QStringLiteral("docs/plans")));
    QTemporaryDir externalTmp;
    ASSERT_TRUE(externalTmp.isValid());
    const QString externalTarget = QDir(externalTmp.path()).filePath(
        QStringLiteral("escape-target.md"));
    {
        QFile f(externalTarget);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("pre-existing target\n");
    }

    // Pre-create a symlink: docs/plans/ANTS-9999-escape.md ->
    // /tmp/<externalTmp>/escape-target.md
    const QString symlinkPath = QDir(tmp.path()).filePath(
        QStringLiteral("docs/plans/ANTS-9999-escape.md"));
    ASSERT_TRUE(QFile::link(externalTarget, symlinkPath));

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName = QStringLiteral("escape");
    opts.antsId = QStringLiteral("ANTS-9999");
    opts.save = true;

    const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.errorCode == QStringLiteral("bad_path") ||
                r.errorCode == QStringLiteral("plan_exists"))
        << "symlink-outside-root should be rejected by INV-4 (bad_path) "
           "or INV-5 (plan_exists, since the symlink already exists). "
           "errorCode: " << r.errorCode.toStdString();

    // External target must remain untouched.
    QFile ext(externalTarget);
    ASSERT_TRUE(ext.open(QIODevice::ReadOnly));
    EXPECT_EQ(ext.readAll(), QByteArray("pre-existing target\n"));
}

// ---------------------------------------------------------------------------
// INV-5 — refuse to overwrite.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv5NoOverwrite) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir().mkpath(QDir(tmp.path()).filePath(QStringLiteral("docs/plans")));

    const QString planFile = QDir(tmp.path()).filePath(
        QStringLiteral("docs/plans/ANTS-9999-foo.md"));
    {
        QFile f(planFile);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("pre-existing plan\n");
    }

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName = QStringLiteral("foo");
    opts.antsId = QStringLiteral("ANTS-9999");
    opts.save = true;

    const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errorCode, QStringLiteral("plan_exists"));
    EXPECT_FALSE(r.planMarkdown.isEmpty())
        << "skeleton must still be returned on plan_exists so the caller "
           "doesn't lose the rendered text";

    // Pre-existing content untouched.
    QFile f(planFile);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("pre-existing plan\n"));
}

// ---------------------------------------------------------------------------
// INV-6 — task-count clamp.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv6TaskCountClamp) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    struct Case { int input; int expected; };
    const Case cases[] = {
        {0, 1}, {1, 1}, {4, 4}, {12, 12}, {13, 12}, {-5, 1}, {100, 12},
    };
    for (const auto &c : cases) {
        auto opts = defaultOpts(QStringLiteral("foo"));
        opts.taskCountHint = c.input;
        const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
        ASSERT_TRUE(r.ok) << "input=" << c.input
                          << " err=" << r.errorCode.toStdString();
        EXPECT_EQ(r.taskCount, c.expected)
            << "input=" << c.input << " expected=" << c.expected
            << " got=" << r.taskCount;
    }
}

// ---------------------------------------------------------------------------
// INV-9 — pure-read dry-run when save:false AND ants_id is supplied.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, Inv9DryRunNoDiskWrites) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName = QStringLiteral("foo");
    opts.antsId = QStringLiteral("ANTS-9999");  // skip allocation
    opts.save = false;

    const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.saved);

    // docs/plans/ must NOT exist post-call (we didn't create it).
    EXPECT_FALSE(QFileInfo::exists(
        QDir(tmp.path()).filePath(QStringLiteral("docs/plans"))));
    // .roadmap-counter must NOT have been touched (we didn't allocate).
    EXPECT_FALSE(QFileInfo::exists(
        QDir(tmp.path()).filePath(QStringLiteral(".roadmap-counter"))));
}

// ---------------------------------------------------------------------------
// AntsIdSource → JSON key mapping.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, AntsIdSourceKeyMapping) {
    using PlanTemplateEngine::AntsIdSource;
    using PlanTemplateEngine::antsIdSourceKey;
    EXPECT_EQ(antsIdSourceKey(AntsIdSource::Provided),
              QStringLiteral("provided"));
    EXPECT_EQ(antsIdSourceKey(AntsIdSource::Allocated),
              QStringLiteral("allocated"));
    EXPECT_EQ(antsIdSourceKey(AntsIdSource::None),
              QStringLiteral("none"));
}

// ---------------------------------------------------------------------------
// Save-true happy path — writes the skeleton to disk.
// ---------------------------------------------------------------------------
TEST(PlanTemplateEngine, SaveTrueWritesAtomicallyToDisk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    PlanTemplateEngine::PlanOptions opts;
    opts.featureName = QStringLiteral("happy-path");
    opts.antsId = QStringLiteral("ANTS-9999");
    opts.save = true;

    const auto r = PlanTemplateEngine::buildPlan(tmp.path(), opts);
    ASSERT_TRUE(r.ok) << "err=" << r.errorCode.toStdString();
    EXPECT_TRUE(r.saved);
    EXPECT_EQ(r.planPath,
              QStringLiteral("docs/plans/ANTS-9999-happy-path.md"));

    const QString abs = QDir(tmp.path()).filePath(r.planPath);
    ASSERT_TRUE(QFileInfo::exists(abs));
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray disk = f.readAll();
    EXPECT_EQ(disk, r.planMarkdown.toUtf8())
        << "on-disk content must match the returned planMarkdown byte-for-byte";
    EXPECT_TRUE(disk.contains("Happy Path Implementation Plan"))
        << "h1 should be title-cased from the kebab feature_name";
    EXPECT_TRUE(disk.contains("ANTS-9999"))
        << "skeleton should embed the provided ANTS-ID";
}
