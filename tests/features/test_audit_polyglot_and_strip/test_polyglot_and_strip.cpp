// ANTS-1623 + ANTS-1627 — polyglot framework detection + Python
// literal/comment strip in test_audit_partition pre-pass.
// Asserts INV-A1..A5 (polyglot) and INV-B1..B7 (strip) from spec.md.

#include "testauditengine.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

void touch(const QString &dir, const QString &relPath,
           const QByteArray &content = QByteArrayLiteral("# fixture\n")) {
    const QFileInfo fi(QDir(dir).filePath(relPath));
    QDir().mkpath(fi.absolutePath());
    QFile f(fi.absoluteFilePath());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}

TestAuditEngine::PartitionResult partitionAt(const QString &root,
                                             const QString &scope = QStringLiteral("auto")) {
    TestAuditEngine::PartitionRequest req;
    req.callerCwd  = root;
    req.scope      = scope;
    req.dimensions = QStringLiteral("auto");
    return TestAuditEngine::partition(req);
}

// Count pre-pass findings with a given pattern_id across all chunks.
int prePassHits(const TestAuditEngine::PartitionResult &r,
                const QString &patternId) {
    int n = 0;
    for (auto it = r.prePassFindingsByChunk.constBegin();
         it != r.prePassFindingsByChunk.constEnd(); ++it) {
        for (const auto &v : it.value()) {
            if (v.toObject().value(QStringLiteral("pattern_id")).toString()
                == patternId) {
                ++n;
            }
        }
    }
    return n;
}

// Return the line numbers where pattern_id fired.
QList<int> prePassLines(const TestAuditEngine::PartitionResult &r,
                        const QString &patternId) {
    QList<int> out;
    for (auto it = r.prePassFindingsByChunk.constBegin();
         it != r.prePassFindingsByChunk.constEnd(); ++it) {
        for (const auto &v : it.value()) {
            const auto o = v.toObject();
            if (o.value(QStringLiteral("pattern_id")).toString() == patternId) {
                out.append(o.value(QStringLiteral("line")).toInt());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool additionalFrameworksContains(const TestAuditEngine::PartitionResult &r,
                                  const QString &name,
                                  const QString &rootRel) {
    for (const auto &v : r.additionalFrameworks) {
        const auto o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == name &&
            o.value(QStringLiteral("root_rel")).toString() == rootRel) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ────────────────────────── ANTS-1623 polyglot ──

TEST(TestAuditPolyglot, InvA1ScopedFrontendPicksJest) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    touch(tmp.path(), "tests/test_root.py",
          QByteArrayLiteral("def test_x(): pass\n"));
    touch(tmp.path(), "frontend/package.json",
          QByteArrayLiteral("{}\n"));
    touch(tmp.path(), "frontend/src/a.test.ts",
          QByteArrayLiteral("test('x', () => {});\n"));
    const auto r = partitionAt(tmp.path(),
                               QStringLiteral("path:frontend"));
    EXPECT_TRUE(r.ok) << "scoped partition must succeed; code="
                      << r.code.toStdString() << ", error="
                      << r.error.toStdString();
    EXPECT_EQ("jest", r.framework.toStdString())
        << "INV-A1: scope:path:frontend must pick jest from "
           "frontend/package.json, not pytest from root pyproject.toml";
    EXPECT_EQ(1, r.totalFiles);
}

TEST(TestAuditPolyglot, InvA2ScopedSubdirNoSignalFallsBackToRoot) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    touch(tmp.path(), "tests/test_a.py",
          QByteArrayLiteral("def test_x(): pass\n"));
    touch(tmp.path(), "tests/test_b.py",
          QByteArrayLiteral("def test_y(): pass\n"));
    const auto r = partitionAt(tmp.path(),
                               QStringLiteral("path:tests"));
    EXPECT_TRUE(r.ok) << "code=" << r.code.toStdString()
                      << ", error=" << r.error.toStdString();
    EXPECT_EQ("pytest", r.framework.toStdString())
        << "INV-A2: tests/ has no signal file, fallback to root pytest";
}

TEST(TestAuditPolyglot, InvA3AdditionalFrameworksListsRootPeers) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    // Both pytest and jest signals at the root.
    touch(tmp.path(), "pyproject.toml");
    touch(tmp.path(), "package.json", QByteArrayLiteral("{}\n"));
    touch(tmp.path(), "tests/test_a.py",
          QByteArrayLiteral("def t(): pass\n"));
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("pytest", r.framework.toStdString());
    EXPECT_TRUE(additionalFrameworksContains(r, QStringLiteral("jest"),
                                             QString()))
        << "INV-A3: jest should be listed as a root-level additional "
           "framework";
}

TEST(TestAuditPolyglot, InvA3SubdirAdditionalFrameworks) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    touch(tmp.path(), "tests/test_a.py",
          QByteArrayLiteral("def t(): pass\n"));
    touch(tmp.path(), "frontend/package.json",
          QByteArrayLiteral("{}\n"));
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("pytest", r.framework.toStdString());
    EXPECT_TRUE(additionalFrameworksContains(r, QStringLiteral("jest"),
                                             QStringLiteral("frontend")))
        << "INV-A3: jest should be listed with root_rel='frontend'";
}

TEST(TestAuditPolyglot, InvA4ExcludesPrimaryFrameworkFromExtras) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    touch(tmp.path(), "tests/test_a.py",
          QByteArrayLiteral("def t(): pass\n"));
    touch(tmp.path(), "backend/pyproject.toml");  // same fw at subdir
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("pytest", r.framework.toStdString());
    EXPECT_FALSE(additionalFrameworksContains(r, QStringLiteral("pytest"),
                                              QStringLiteral("backend")))
        << "INV-A4: primary framework must not appear in additional list";
}

TEST(TestAuditPolyglot, InvA5SkipsBuildAndNodeModules) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    touch(tmp.path(), "tests/test_a.py",
          QByteArrayLiteral("def t(): pass\n"));
    // These signal files exist in dirs that MUST be excluded.
    touch(tmp.path(), "build/package.json", QByteArrayLiteral("{}\n"));
    touch(tmp.path(), "node_modules/package.json",
          QByteArrayLiteral("{}\n"));
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(additionalFrameworksContains(r, QStringLiteral("jest"),
                                              QStringLiteral("build")))
        << "INV-A5: build/ must not contribute additional frameworks";
    EXPECT_FALSE(additionalFrameworksContains(r, QStringLiteral("jest"),
                                              QStringLiteral("node_modules")))
        << "INV-A5: node_modules/ must not contribute additional frameworks";
}

// ────────────────────────── ANTS-1627 Python strip ──

TEST(TestAuditPyStrip, InvB1SleepInSingleQuotedStringSuppressed) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    // Test that holds the needle as a string and asserts absence.
    // The literal `time.sleep(` is inside a single-quoted string —
    // the strip should replace it with spaces, so no sleep_call hit.
    QByteArray src =
        "def test_no_bare_time_sleep_in_jobs():\n"
        "    needle = 'time.sleep('\n"
        "    src = open('jobs.py').read()\n"
        "    assert needle not in src\n";
    touch(tmp.path(), "tests/test_negative_grep.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(0, prePassHits(r, QStringLiteral("sleep_call")))
        << "INV-B1: sleep_call inside 'time.sleep(' string literal "
           "must not fire";
}

TEST(TestAuditPyStrip, InvB2SleepInTripleQuotedStringSuppressed) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    QByteArray src =
        "\"\"\"Module docstring describing a bug where time.sleep(0.1)\n"
        "was called inside a tight loop.\n"
        "\"\"\"\n"
        "def test_x():\n"
        "    pass\n";
    touch(tmp.path(), "tests/test_docstring.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(0, prePassHits(r, QStringLiteral("sleep_call")))
        << "INV-B2: sleep_call inside module docstring must not fire";
}

TEST(TestAuditPyStrip, InvB3SleepInLineCommentSuppressed) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    QByteArray src =
        "def test_x():\n"
        "    # historical bug: time.sleep(0.5) was called here\n"
        "    pass\n";
    touch(tmp.path(), "tests/test_comment.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(0, prePassHits(r, QStringLiteral("sleep_call")))
        << "INV-B3: sleep_call inside `#` comment must not fire";
}

TEST(TestAuditPyStrip, InvB4PasswordInStringSuppressed) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    // Negative-grep test asserting a string is NOT present.
    QByteArray src =
        "def test_no_admin_password_in_config():\n"
        "    needle = 'password = \"admin\"'\n"
        "    assert needle not in open('config.py').read()\n";
    touch(tmp.path(), "tests/test_negative_password.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(0, prePassHits(r, QStringLiteral("hardcoded_password")))
        << "INV-B4: hardcoded_password inside string literal must not "
           "fire";
}

TEST(TestAuditPyStrip, InvB5RealSleepCallStillFires) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    QByteArray src =
        "import time\n"
        "def test_x():\n"
        "    time.sleep(0.1)\n"
        "    assert True\n";
    touch(tmp.path(), "tests/test_real_sleep.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(1, prePassHits(r, QStringLiteral("sleep_call")))
        << "INV-B5: a real time.sleep() call must still fire";
}

TEST(TestAuditPyStrip, InvB6LineNumberPreservedAfterStrip) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    QByteArray src =
        "# line 1 comment\n"
        "\"\"\"line 2 docstring\n"
        "describing time.sleep on this line — must be suppressed\n"
        "\"\"\"\n"
        "import time\n"
        "def test_x():\n"
        "    time.sleep(0.1)   # this is the real call, line 7\n"
        "    pass\n";
    touch(tmp.path(), "tests/test_line_preservation.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    const auto lines = prePassLines(r, QStringLiteral("sleep_call"));
    ASSERT_EQ(1, lines.size())
        << "INV-B6: exactly one sleep_call should fire";
    EXPECT_EQ(7, lines.first())
        << "INV-B6: line number must report the real call site (line 7)";
}

TEST(TestAuditPyStrip, InvB7StringPrefixesHandled) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "pyproject.toml");
    QByteArray src =
        "def test_x():\n"
        "    a = r'time.sleep(0.1)'   # raw-string\n"
        "    b = b'time.sleep(0.1)'   # bytes\n"
        "    c = f'time.sleep({0.1})' # f-string\n"
        "    d = rb'time.sleep(0.1)'  # combined prefix\n"
        "    pass\n";
    touch(tmp.path(), "tests/test_string_prefixes.py", src);
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(0, prePassHits(r, QStringLiteral("sleep_call")))
        << "INV-B7: prefixed strings (r/b/f/rb) must also strip";
}
