// Feature-conformance test for ANTS-4826 — the file_outline shell lane. See
// spec.md.
//
// The defect: file_outline's mode enum was auto|cpp|py|md|json|generic|glsl|
// html, so a shell script returned {language:"unknown"} with no symbols —
// while SymbolQuery::langForExt has mapped `.sh` and `.bash` to Lang::Sh all
// along, so find_definition and workspace_search both advertise `sh`. Two
// answers about whether shell is a supported language, which is the same
// drift ANTS-4096 fixed for shaders and ANTS-4425 for HTML.
//
// Reported independently by three projects. Shell is where a project's
// release and launcher logic lives, and those are exactly the files a session
// needs to orient in without paying for a full Read; all three fell back to
// grep or cat.

#include "../../_support/expect.h"
#include "codebaseindex.h"
#include "fileoutline.h"

#include <gtest/gtest.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

// A script in the shape these projects actually write: a shebang, top-level
// configuration constants, both function spellings, and a local inside a body
// that must NOT be mistaken for a top-level binding.
const char *kScript = R"(#!/usr/bin/env bash
# Release helper.
set -euo pipefail

BUILD_DIR=build
readonly VERSION_FILE=CMakeLists.txt
export ANTS_LOG_LEVEL=warn

say() {
    local message="$1"
    printf '%s\n' "$message"
}

function die {
    say "$1"
    exit 1
}

run_engine() {
    say "running"
}

main() {
    run_engine
}

main "$@"
)";

QString writeScript(const QTemporaryDir &dir, const QString &name,
                    const char *body = kScript) {
    const QString path =
        QFileInfo(dir.path()).canonicalFilePath() + QLatin1Char('/') + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
    f.close();
    return path;
}

QStringList symbolNames(const QJsonObject &out) {
    QStringList names;
    for (const QJsonValue &v : out.value(QStringLiteral("symbols")).toArray())
        names << v.toObject().value(QStringLiteral("name")).toString();
    return names;
}

QString kindOf(const QJsonObject &out, const QString &name) {
    for (const QJsonValue &v : out.value(QStringLiteral("symbols")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == name)
            return o.value(QStringLiteral("kind")).toString();
    }
    return QString();
}

}  // namespace

// INV-1 — a shell script is detected by extension and yields BOTH function
// spellings. Breaks when: pickModeByExt has no shell branch, so the file falls
// through to Mode::Auto and compute() reports "unknown" with no symbols.
TEST(FileOutlineSh, DetectsShellExtensionsAndExtractsFunctions) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    for (const QString &name : {QStringLiteral("release.sh"),
                                QStringLiteral("gate.bash")}) {
        const QString path = writeScript(dir, name);
        const QJsonObject out =
            FileOutline::compute(path, FileOutline::Mode::Auto, true, 1000);
        EXPECT_EQ(out.value(QStringLiteral("language")).toString(),
                  QStringLiteral("sh"))
            << "not detected as shell: " << name.toStdString();
        const QStringList syms = symbolNames(out);
        // `name() {` — the form every reporter's script used.
        EXPECT_TRUE(syms.contains(QStringLiteral("say")))
            << name.toStdString() << " symbols: "
            << syms.join(QStringLiteral(",")).toStdString();
        EXPECT_TRUE(syms.contains(QStringLiteral("run_engine")));
        // `function name {` — the ksh/bash spelling, with no parens.
        EXPECT_TRUE(syms.contains(QStringLiteral("die")))
            << "the `function name` spelling must be found too: "
            << syms.join(QStringLiteral(",")).toStdString();
        EXPECT_EQ(kindOf(out, QStringLiteral("say")), QStringLiteral("func"));
    }
}

// INV-2 — a top-level assignment is emitted as kind "const", mirroring what
// ANTS-4090 does for the brace family, and a `local` inside a function body is
// NOT. The second half is the control: without it a fix that emitted every
// `name=` line would pass, and a script's locals outnumber its constants.
TEST(FileOutlineSh, TopLevelBindingsOnly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QJsonObject out = FileOutline::compute(
        writeScript(dir, QStringLiteral("release.sh")),
        FileOutline::Mode::Auto, true, 1000);
    const QStringList syms = symbolNames(out);

    EXPECT_TRUE(syms.contains(QStringLiteral("BUILD_DIR")));
    EXPECT_EQ(kindOf(out, QStringLiteral("BUILD_DIR")),
              QStringLiteral("const"));
    // The declaration keywords a real script uses in front of a constant.
    EXPECT_TRUE(syms.contains(QStringLiteral("VERSION_FILE")))
        << "a `readonly NAME=` constant must be found";
    EXPECT_TRUE(syms.contains(QStringLiteral("ANTS_LOG_LEVEL")))
        << "an `export NAME=` constant must be found";

    EXPECT_FALSE(syms.contains(QStringLiteral("message")))
        << "an indented `local message=` is a body local, not a landmark: "
        << syms.join(QStringLiteral(",")).toStdString();
}

// INV-3 — an explicit mode:"sh" is honoured, so a caller can force the lane
// for a script with a project-specific extension. Breaks when: parseMode has
// no "sh" branch and silently returns Auto, which reads as "unknown language"
// rather than as a rejected argument.
TEST(FileOutlineSh, ExplicitModeIsHonoured) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeScript(dir, QStringLiteral("hook.zsh"));

    EXPECT_EQ(FileOutline::parseMode(QStringLiteral("sh")),
              FileOutline::Mode::Sh);
    const QJsonObject out =
        FileOutline::compute(path, FileOutline::Mode::Sh, true, 1000);
    EXPECT_EQ(out.value(QStringLiteral("language")).toString(),
              QStringLiteral("sh"));
    EXPECT_TRUE(symbolNames(out).contains(QStringLiteral("run_engine")));
}

// INV-4 — an EXTENSIONLESS script is detected from its shebang. Named by the
// reporters because it is the common case for the files that most want
// outlining: a git hook, a launcher, a CI gate. The extension is the only
// signal pickModeByExt has, and these carry none.
TEST(FileOutlineSh, ShebangDetectsAnExtensionlessScript) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QJsonObject out = FileOutline::compute(
        writeScript(dir, QStringLiteral("pre-push")),
        FileOutline::Mode::Auto, true, 1000);
    EXPECT_EQ(out.value(QStringLiteral("language")).toString(),
              QStringLiteral("sh"))
        << "an extensionless script must be recognised by its shebang";
    EXPECT_TRUE(symbolNames(out).contains(QStringLiteral("run_engine")));
}

// INV-5 — a file with NO shebang and no known extension stays unknown. The
// control for INV-4: a shebang peek that fired on anything would claim every
// extensionless file in the tree, and "unknown" is the honest answer there.
TEST(FileOutlineSh, NoShebangStaysUnknown) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QJsonObject out = FileOutline::compute(
        writeScript(dir, QStringLiteral("NOTES"),
                    "Just prose, no shebang.\nsay() {\n  :\n}\n"),
        FileOutline::Mode::Auto, true, 1000);
    EXPECT_EQ(out.value(QStringLiteral("language")).toString(),
              QStringLiteral("unknown"))
        << "only a shebang may promote an extensionless file to shell";
}

// INV-6 — codebase_index admits the same extensions. This file's own in-step
// rule is that count → outline → symbol query cover the same files, and it is
// the rule ANTS-4096 and ANTS-4425 were each filed to restore after exactly
// this drift. Adding the outline lane without this gate would repeat it.
TEST(FileOutlineSh, IndexAdmitsTheSameExtensions) {
    EXPECT_TRUE(CodebaseIndex::isIndexableSuffix(QStringLiteral("sh")));
    EXPECT_TRUE(CodebaseIndex::isIndexableSuffix(QStringLiteral("bash")));
}
