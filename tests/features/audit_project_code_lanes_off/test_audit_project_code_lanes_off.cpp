// ANTS-6446 — audit lanes that run project code start off. Contract:
// spec.md beside this file.

#include "auditdialog.h"
#include "config.h"

#include <gtest/gtest.h>
#include "../../_support/xdg_guard.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes, bool exec = false) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) return false;
    if (exec) f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

const AuditCheck *findCheck(const QList<AuditCheck> &checks, const QString &id) {
    for (const AuditCheck &c : checks)
        if (c.id == id) return &c;
    return nullptr;
}

}  // namespace

TEST(AuditProjectCodeLanesOff, ProjectCodeLanesAreNotSelected) {
    QTemporaryDir project, bin;
    ASSERT_TRUE(project.isValid() && bin.isValid());
    ASSERT_TRUE(writeFile(project.filePath("Cargo.toml"), "[package]\nname=\"x\"\n"));
    ASSERT_TRUE(writeFile(project.filePath("main.py"), "print(1)\n"));
    for (const char *tool : {"cargo", "cargo-audit", "pylint"})
        ASSERT_TRUE(writeFile(bin.filePath(tool), "#!/bin/sh\nexit 0\n", true));

    ants_test::XdgGuard env;
    env.setEnv("PATH", (bin.path() + ":" + qEnvironmentVariable("PATH")).toUtf8());

    Config cfg;
    AuditDialog dlg(project.path(), nullptr, &cfg);
    const QList<AuditCheck> checks = dlg.checksForTest();

    // INV-1
    for (const char *id : {"cargo_clippy", "pylint"}) {
        const AuditCheck *c = findCheck(checks, id);
        ASSERT_NE(c, nullptr) << id << " lane missing";
        EXPECT_TRUE(c->available) << id << " not available with its tool on PATH";
        EXPECT_FALSE(c->autoSelect) << id << " runs project code but starts selected";
    }
    // INV-2
    const AuditCheck *audit = findCheck(checks, "cargo_audit");
    ASSERT_NE(audit, nullptr);
    EXPECT_TRUE(audit->autoSelect) << "cargo_audit should still start selected";
}
