// Feature-conformance test for ANTS-6801 — NASM, CMake and YAML in the
// code-reading verbs. See spec.md.

#include "codebaseindex.h"
#include "commentclaims.h"
#include "fileoutline.h"
#include "functionsizes.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

const char *kAsm =
    "; Copy engine.\n"                    // 1
    "BLOCK_SIZE equ 512\n"                // 2
    "\n"                                  // 3
    "%macro SAVE_REGS 0\n"                // 4
    "    push rbx\n"                      // 5
    "%endmacro\n"                         // 6
    "\n"                                  // 7
    "copy_block:\n"                       // 8
    "    mov rcx, BLOCK_SIZE\n"           // 9
    ".loop:\n"                            // 10
    "    dec rcx\n"                       // 11
    "    jnz .loop\n"                     // 12
    "    ret\n"                           // 13
    "\n"                                  // 14
    "start: mov rax, msg ; entry point\n" // 15
    "    ret\n"                           // 16
    "msg db \"a;b\", 0\n";                // 17

const char *kCmake =
    "cmake_minimum_required(VERSION 3.20)\n"  // 1
    "project(defragr ASM_NASM)\n"             // 2
    "FUNCTION(add_engine name)\n"             // 3
    "    add_library(${name} STATIC)\n"       // 4
    "endfunction()\n"                         // 5
    "macro(warn msg)\n"                       // 6
    "    message(WARNING ${msg})\n"           // 7
    "endmacro()\n"                            // 8
    "add_executable(defragr main.asm)\n";     // 9

const char *kYaml =
    "name: build\n"
    "on:\n"
    "  push:\n"
    "jobs:\n"
    "  test:\n"
    "    runs-on: ubuntu-latest\n";

QString writeFile(const QTemporaryDir &dir, const QString &name, const char *body) {
    const QString path = dir.path() + QLatin1Char('/') + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(body);
    return path;
}

QJsonObject outline(const QString &path) {
    return FileOutline::compute(path, FileOutline::Mode::Auto, false, 200);
}

// name → kind for every symbol, so a missing one and a wrong kind both show.
QMap<QString, QString> kinds(const QJsonObject &o) {
    QMap<QString, QString> m;
    for (const QJsonValue v : o.value("symbols").toArray())
        m.insert(v.toObject().value("name").toString(), v.toObject().value("kind").toString());
    return m;
}

QStringList lines(const char *body) {
    QStringList l = QString::fromUtf8(body).split(QLatin1Char('\n'));
    if (!l.isEmpty() && l.last().isEmpty()) l.removeLast();
    return l;
}

}  // namespace

TEST(FileOutlineAsmCmakeYaml, Inv1NasmOutline) {
    QTemporaryDir dir;
    for (const char *name : {"engine.asm", "engine.nasm", "engine.inc"}) {
        const QJsonObject o = outline(writeFile(dir, QString::fromLatin1(name), kAsm));
        EXPECT_EQ(o.value("language").toString().toStdString(), "asm") << name;
        const auto k = kinds(o);
        EXPECT_EQ(k.value("copy_block").toStdString(), "func") << name;
        EXPECT_EQ(k.value("start").toStdString(), "func") << name;
        EXPECT_EQ(k.value("SAVE_REGS").toStdString(), "func") << name;
        EXPECT_EQ(k.value("BLOCK_SIZE").toStdString(), "const") << name;
        EXPECT_FALSE(k.contains(".loop")) << name;
        EXPECT_FALSE(k.contains("loop")) << name;
        EXPECT_EQ(k.size(), 4) << name;
    }
}

TEST(FileOutlineAsmCmakeYaml, Inv2CmakeOutline) {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("sub");
    for (const char *name : {"CMakeLists.txt", "sub/Engine.cmake"}) {
        const QJsonObject o = outline(writeFile(dir, QString::fromLatin1(name), kCmake));
        EXPECT_EQ(o.value("language").toString().toStdString(), "cmake") << name;
        const auto k = kinds(o);
        EXPECT_EQ(k.value("add_engine").toStdString(), "func") << name;
        EXPECT_EQ(k.value("warn").toStdString(), "func") << name;
        EXPECT_EQ(k.value("defragr").toStdString(), "type") << name;
        EXPECT_FALSE(k.contains("${name}")) << name;
    }
}

TEST(FileOutlineAsmCmakeYaml, Inv3YamlOutline) {
    QTemporaryDir dir;
    for (const char *name : {"ci.yml", "ci.yaml"}) {
        const QJsonObject o = outline(writeFile(dir, QString::fromLatin1(name), kYaml));
        EXPECT_EQ(o.value("language").toString().toStdString(), "yaml") << name;
        const auto k = kinds(o);
        EXPECT_EQ(k.value("name").toStdString(), "const") << name;
        EXPECT_EQ(k.value("on").toStdString(), "const") << name;
        EXPECT_EQ(k.value("jobs").toStdString(), "const") << name;
        EXPECT_FALSE(k.contains("push")) << name;
        EXPECT_FALSE(k.contains("runs-on")) << name;
    }
}

TEST(FileOutlineAsmCmakeYaml, Inv4FileFilter) {
    QTemporaryDir dir;
    for (const char *name : {"a.asm", "a.nasm", "a.inc", "a.cmake", "CMakeLists.txt",
                             "a.yml", "a.yaml"}) {
        writeFile(dir, QString::fromLatin1(name), "x\n");
        EXPECT_TRUE(CodebaseIndex::isIndexableFile(QFileInfo(dir.path() + "/" + name))) << name;
    }
    for (const char *name : {"Containerfile", "notes.txt"}) {
        writeFile(dir, QString::fromLatin1(name), "x\n");
        EXPECT_FALSE(CodebaseIndex::isIndexableFile(QFileInfo(dir.path() + "/" + name))) << name;
    }
}

TEST(FileOutlineAsmCmakeYaml, Inv5ExplicitMode) {
    for (const char *m : {"asm", "cmake", "yaml"})
        EXPECT_NE(FileOutline::parseMode(QString::fromLatin1(m)), FileOutline::Mode::Auto) << m;
}

TEST(FileOutlineAsmCmakeYaml, Inv6FunctionSizes) {
    QTemporaryDir dir;
    auto spans = [&](const QString &name, const char *body) {
        QList<FunctionSizes::Unmeasured> skipped;
        QMap<QString, QPair<int, int>> m;
        for (const auto &f : FunctionSizes::measure(writeFile(dir, name, body), lines(body), &skipped))
            m.insert(f.symbol, {f.bodyStart, f.bodyEnd});
        EXPECT_TRUE(skipped.isEmpty()) << name.toStdString();
        return m;
    };
    const auto a = spans("engine.asm", kAsm);
    EXPECT_EQ(a.value("SAVE_REGS"), qMakePair(4, 6));
    EXPECT_EQ(a.value("copy_block"), qMakePair(8, 13));
    EXPECT_EQ(a.value("start"), qMakePair(15, 17));
    const auto c = spans("CMakeLists.txt", kCmake);
    EXPECT_EQ(c.value("add_engine"), qMakePair(3, 5));
    EXPECT_EQ(c.value("warn"), qMakePair(6, 8));
}

TEST(FileOutlineAsmCmakeYaml, Inv7NasmComments) {
    QStringList texts;
    for (const CommentClaims::Block &b : CommentClaims::harvest("engine.asm", QString::fromUtf8(kAsm)))
        for (const CommentClaims::CommentLine &l : b) texts << l.text.trimmed();
    EXPECT_TRUE(texts.contains("Copy engine.")) << texts.join('|').toStdString();
    EXPECT_TRUE(texts.contains("entry point")) << texts.join('|').toStdString();
    EXPECT_FALSE(texts.join('|').contains("b\"")) << texts.join('|').toStdString();
}
