// Feature-conformance test for tests/features/local_subagent_framework/spec.md.
//
// Behavioural test of `AntsHelper::driftCheck` against synthetic
// tree fixtures, plus a source-grep INV that the CMake option +
// gated add_executable wiring is in place.
//
// Exit 0 = all 8 invariants hold.

#include "antshelper.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

bool writeScript(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    f.write(body.toUtf8());
    f.close();
    QFile::setPermissions(path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::ExeOther);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // INV-2: clean drift script → ok:true, clean:true, exit 0.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-2", "could not make tmp dir");
        QDir(tmp.path()).mkpath(QStringLiteral("packaging"));
        const QString script = tmp.path() +
            QStringLiteral("/packaging/check-version-drift.sh");
        if (!writeScript(script,
                QStringLiteral("#!/usr/bin/env bash\nexit 0\n")))
            return fail("INV-2", "could not write fake drift script");

        int exitCode = -1;
        const QJsonObject resp =
            AntsHelper::driftCheck({}, tmp.path(), &exitCode);
        if (!resp.value("ok").toBool())
            return fail("INV-2", "expected ok:true on clean exit");
        const QJsonObject data = resp.value("data").toObject();
        if (!data.value("clean").toBool())
            return fail("INV-2", "expected data.clean = true");
        if (exitCode != 0)
            return fail("INV-2", "expected *exitCode = 0");
    }

    // INV-3 + INV-6: drift script with violations → ok:true,
    //   clean:false, violations[] populated, raw + exit_code set,
    //   *exitCode = 3.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-3", "could not make tmp dir");
        QDir(tmp.path()).mkpath(QStringLiteral("packaging"));
        const QString script = tmp.path() +
            QStringLiteral("/packaging/check-version-drift.sh");
        const QString body = QStringLiteral(
            "#!/usr/bin/env bash\n"
            "echo 'foo.spec:7: rpm version 0.7.1 drifts from CMake 0.7.58'\n"
            "echo 'metainfo.xml:42: appstream version 0.7.0 drifts from CMake 0.7.58'\n"
            "exit 7\n");
        if (!writeScript(script, body))
            return fail("INV-3", "could not write fake drift script");

        int exitCode = -1;
        const QJsonObject resp =
            AntsHelper::driftCheck({}, tmp.path(), &exitCode);
        if (!resp.value("ok").toBool())
            return fail("INV-3",
                        "drift detected ≠ handler error — ok must remain true");
        const QJsonObject data = resp.value("data").toObject();
        if (data.value("clean").toBool())
            return fail("INV-3", "data.clean must be false");
        const QJsonArray viol = data.value("violations").toArray();
        if (viol.size() != 2)
            return fail("INV-3", "expected exactly 2 parsed violations");
        const QJsonObject v0 = viol[0].toObject();
        if (v0.value("file").toString() != QStringLiteral("foo.spec") ||
            v0.value("line").toInt() != 7)
            return fail("INV-3", "violation[0] file/line mismatch");
        if (!data.contains(QStringLiteral("raw")))
            return fail("INV-6", "raw field missing");
        if (!data.contains(QStringLiteral("exit_code")))
            return fail("INV-6", "exit_code field missing");
        if (data.value("exit_code").toInt() != 7)
            return fail("INV-6", "exit_code must mirror the script's exit");
        if (exitCode != 3)
            return fail("INV-3", "expected *exitCode = 3 on drift");
    }

    // INV-4: missing script → ok:false, code:missing_script, exit 1.
    {
        QTemporaryDir tmp;
        if (!tmp.isValid()) return fail("INV-4", "could not make tmp dir");
        // No packaging/check-version-drift.sh.
        int exitCode = -1;
        const QJsonObject resp =
            AntsHelper::driftCheck({}, tmp.path(), &exitCode);
        if (resp.value("ok").toBool())
            return fail("INV-4", "expected ok:false");
        if (resp.value("code").toString() != QStringLiteral("missing_script"))
            return fail("INV-4",
                        "expected code = missing_script");
        if (exitCode != 1)
            return fail("INV-4", "expected *exitCode = 1");
    }

    // INV-5: bogus repo root → ok:false, code:missing_repo_root.
    {
        int exitCode = -1;
        const QJsonObject resp = AntsHelper::driftCheck(
            {}, QStringLiteral("/this/path/does/not/exist/xyzzy"),
            &exitCode);
        if (resp.value("ok").toBool())
            return fail("INV-5", "expected ok:false");
        if (resp.value("code").toString() !=
            QStringLiteral("missing_repo_root"))
            return fail("INV-5",
                        "expected code = missing_repo_root");
        if (exitCode != 1)
            return fail("INV-5", "expected *exitCode = 1");
    }

    // INV-7: jsonToCompactString produces single-line UTF-8.
    {
        QJsonObject probe;
        probe.insert(QStringLiteral("ok"), true);
        QJsonObject data;
        data.insert(QStringLiteral("clean"), true);
        probe.insert(QStringLiteral("data"), data);
        const QString s = AntsHelper::jsonToCompactString(probe);
        if (s.contains('\n'))
            return fail("INV-7",
                        "compact JSON output must not embed literal newlines");
        if (s.startsWith(QChar(0xFEFF)))
            return fail("INV-7", "compact JSON output must not start with BOM");
    }

    // INV-1 / INV-8: CMake gates the helper binary correctly.
    const std::string cmake = slurp(SRC_CMAKELISTS);
    if (cmake.empty())
        return fail("INV-1", "CMakeLists.txt not readable");
    if (!contains(cmake, "option(ANTS_ENABLE_HELPER_CLI"))
        return fail("INV-1",
                    "CMakeLists.txt missing ANTS_ENABLE_HELPER_CLI option");
    if (!contains(cmake, "add_executable(ants-helper"))
        return fail("INV-8",
                    "CMakeLists.txt missing add_executable(ants-helper)");
    if (!contains(cmake, "if(ANTS_ENABLE_HELPER_CLI)"))
        return fail("INV-8",
                    "ants-helper target must be gated on the option");

    std::puts("OK local_subagent_framework: 8/8 invariants");
    return 0;
}
