// Feature-conformance test for ANTS-5685 — see spec.md.

#include "shellhistory.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

QString readSource(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Brace-balanced body from the first `{` after the signature.
QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int braceStart = src.indexOf(QChar('{'), start);
    if (braceStart < 0) return QString();
    int depth = 1;
    int i = braceStart + 1;
    while (i < src.size() && depth > 0) {
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(braceStart, i - braceStart);
}

}  // namespace

// INV-1 — a file over the cap is read from its tail, cut at a line start.
TEST(ShellHistoryTail, Inv1ReadsOnlyTheTail) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("hist");
    QByteArray bytes;
    for (int i = 0; i < 1000; ++i) bytes += "command-" + QByteArray::number(i) + '\n';
    writeFile(path, bytes);

    const QByteArray tail = shellhistory::readTail(path, 100);
    EXPECT_LE(tail.size(), 100);
    EXPECT_TRUE(tail.startsWith("command-")) << tail.left(20).constData();
    EXPECT_TRUE(tail.endsWith("command-999\n"));
    EXPECT_FALSE(tail.contains("command-0\n"));

    EXPECT_EQ(shellhistory::readTail(path, bytes.size()), bytes);
    EXPECT_EQ(shellhistory::readTail(path, bytes.size() + 10), bytes);
    EXPECT_TRUE(shellhistory::readTail(dir.filePath("absent"), 100).isEmpty());
}

// INV-2 — the parse is the one loadHistory did before ANTS-5685.
TEST(ShellHistoryTail, Inv2ParseIsUnchanged) {
    const QByteArray zsh =
        ": 1700000000:0;git status\n"
        ": 1700000001:0;make\n"
        ": no-semicolon\n"
        ": 1700000002:0;make\n"
        "\n"
        "  ls -la  \n";
    EXPECT_EQ(shellhistory::parse(zsh),
              (QStringList{"ls -la", "make", "git status"}));

    const QByteArray fish = "- cmd: cargo build\n- cmd: cargo test\n";
    EXPECT_EQ(shellhistory::parse(fish), (QStringList{"cargo test", "cargo build"}));
}

// INV-3 — zsh wins over bash; an empty zsh file falls through to bash.
TEST(ShellHistoryTail, Inv3ShellOrder) {
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    writeFile(home.filePath(".bash_history"), "from-bash\n");
    writeFile(home.filePath(".local/share/fish/fish_history"), "- cmd: from-fish\n");
    writeFile(home.filePath(".zsh_history"), "\n");
    EXPECT_EQ(shellhistory::load(home.path()), QStringList{"from-bash"});

    writeFile(home.filePath(".zsh_history"), ": 1:0;from-zsh\n");
    EXPECT_EQ(shellhistory::load(home.path()), QStringList{"from-zsh"});

    QFile::remove(home.filePath(".zsh_history"));
    QFile::remove(home.filePath(".bash_history"));
    EXPECT_EQ(shellhistory::load(home.path()), QStringList{"from-fish"});
}

// INV-5 (ANTS-6654) — a fish file's metadata lines are not commands.
TEST(ShellHistoryTail, Inv5FishKeepsOnlyCommands) {
    const QByteArray fish =
        "- cmd: cargo build\n"
        "  when: 1700000000\n"
        "- cmd: vim notes.md\n"
        "  when: 1700000001\n"
        "  paths:\n"
        "    - notes.md\n";
    EXPECT_EQ(shellhistory::parseFish(fish), (QStringList{"vim notes.md", "cargo build"}));

    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    writeFile(home.filePath(".local/share/fish/fish_history"), fish);
    EXPECT_EQ(shellhistory::load(home.path()), (QStringList{"vim notes.md", "cargo build"}));
}

// INV-4 — the widget hands the read to a worker.
TEST(ShellHistoryTail, Inv4WidgetLoadsOnAWorker) {
    const QString body = functionBody(readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH)),
                                      QStringLiteral("void TerminalWidget::loadHistory()"));
    ASSERT_FALSE(body.isEmpty()) << "loadHistory not found";
    EXPECT_TRUE(body.contains(QStringLiteral("QThread::create")))
        << "loadHistory does not start a worker";
    EXPECT_TRUE(body.contains(QStringLiteral("shellhistory::load(")))
        << "loadHistory does not read through shellhistory::load";
    EXPECT_FALSE(body.contains(QStringLiteral("readAll")))
        << "loadHistory still reads the history file on the GUI thread";
    EXPECT_FALSE(body.contains(QStringLiteral("QFile")))
        << "loadHistory still opens the history file itself";
}
