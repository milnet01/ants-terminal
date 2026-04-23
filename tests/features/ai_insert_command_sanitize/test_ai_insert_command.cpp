// Feature-conformance test for spec.md — exercises
// AiDialog::extractAndSanitizeCommand (pure, no UI needed) and
// source-greps the click handler to confirm the confirmation-dialog
// guard is still wired before the insertCommand signal emit.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "aidialog.h"

#include <QCoreApplication>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_AI_CPP
#error "SRC_AI_CPP compile definition required"
#endif

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const QString &detail = QString()) {
    std::fprintf(stderr, "[%-58s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 detail.isEmpty() ? "" : " — ",
                 qUtf8Printable(detail));
    if (!ok) ++g_failures;
}

void testExtraction() {
    // Fenced block with language hint
    {
        int n = 0;
        QString cmd = AiDialog::extractAndSanitizeCommand(
            "Here:\n```bash\ngit status\n```\nAll done.", &n);
        expect(cmd == "git status", "extract: fenced block with bash hint",
               QString("got: '%1'").arg(cmd));
        expect(n == 0, "extract: nothing stripped from clean input");
    }
    // Fenced block without language
    {
        QString cmd = AiDialog::extractAndSanitizeCommand(
            "```\nls -la\n```");
        expect(cmd == "ls -la", "extract: bare fenced block");
    }
    // No fenced block → last non-empty line
    {
        QString cmd = AiDialog::extractAndSanitizeCommand(
            "Try this:\n\nls\n");
        expect(cmd == "ls", "extract: fallback to last non-empty line");
    }
    // Empty response
    {
        QString cmd = AiDialog::extractAndSanitizeCommand("");
        expect(cmd.isEmpty(), "extract: empty input → empty output");
    }
}

void testSanitization() {
    // ESC injection via fenced block — the headline attack.
    {
        QString resp;
        resp += "```\n";
        resp += "rm -rf ~";
        resp += QChar(0x1B);   // ESC
        resp += "[2J";
        resp += "\n```";
        int n = 0;
        QString cmd = AiDialog::extractAndSanitizeCommand(resp, &n);
        expect(!cmd.contains(QChar(0x1B)),
               "sanitize: ESC (0x1B) stripped from fenced block");
        expect(n >= 1,
               "sanitize: stripped count reports >=1",
               QString("n=%1").arg(n));
    }

    // NUL byte — truncates downstream C-string handling
    {
        QString resp = QStringLiteral("```\nfoo");
        resp += QChar(0x00);
        resp += QStringLiteral("bar\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(cmd == "foobar", "sanitize: NUL stripped, remainder joined");
    }

    // DEL + BS
    {
        QString resp = QStringLiteral("```\ncmd");
        resp += QChar(0x08);   // BS
        resp += QChar(0x7F);   // DEL
        resp += QStringLiteral("\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(cmd == "cmd", "sanitize: BS + DEL stripped");
    }

    // HT preserved — use a realistic fenced block with language hint
    // on the first line so the pre-existing "strip language identifier
    // if first line < 10 chars" heuristic doesn't eat our payload.
    {
        QString resp = QStringLiteral("```bash\ncol1\tcol2\nline2\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(cmd.contains('\t'), "sanitize: HT preserved",
               QString("got: '%1'").arg(cmd));
        expect(cmd.contains('\n'), "sanitize: LF preserved");
    }

    // UTF-8 multi-byte preserved
    {
        QString resp = QStringLiteral("```\necho \"café\"\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(cmd.contains(QStringLiteral("café")),
               "sanitize: UTF-8 multi-byte preserved");
    }

    // Expanded strip set (0.7.12 re-review): C1 controls, line/para
    // separators, bidi overrides, zero-width codepoints.
    // "Trojan Source" (CVE-2021-42574): bidi overrides can make the
    // confirmation preview display text different from what executes.
    // All of these must be stripped so the preview is truthful.
    {
        QString resp = QStringLiteral("```bash\ncmd");
        resp += QChar(0x0085);  // NEL
        resp += QStringLiteral("next\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(!cmd.contains(QChar(0x0085)),
               "sanitize: NEL (U+0085) stripped");
    }
    {
        QString resp = QStringLiteral("```bash\nrm");
        resp += QChar(0x2028);  // LINE SEPARATOR
        resp += QStringLiteral("rf ~\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(!cmd.contains(QChar(0x2028)),
               "sanitize: LINE SEPARATOR (U+2028) stripped");
    }
    {
        QString resp = QStringLiteral("```bash\nsafe");
        resp += QChar(0x202E);  // RIGHT-TO-LEFT OVERRIDE
        resp += QStringLiteral("evil\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(!cmd.contains(QChar(0x202E)),
               "sanitize: RTL OVERRIDE (U+202E) stripped (Trojan Source)");
    }
    {
        QString resp = QStringLiteral("```bash\nrm");
        resp += QChar(0x200B);  // ZERO WIDTH SPACE
        resp += QStringLiteral("alias-kill\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(!cmd.contains(QChar(0x200B)),
               "sanitize: ZERO WIDTH SPACE (U+200B) stripped");
    }
    {
        QString resp = QStringLiteral("```bash\ncafe");
        resp += QChar(0xFEFF);  // ZWNBSP / BOM-as-ZWSP
        resp += QStringLiteral("\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(!cmd.contains(QChar(0xFEFF)),
               "sanitize: ZWNBSP (U+FEFF) stripped");
    }
    // Sanity: a legitimate accented character near the strip range is
    // preserved. U+00E9 (é) is > 0x9F so not in the C1 range.
    {
        QString resp = QStringLiteral("```bash\necho \"passé\"\n```");
        QString cmd = AiDialog::extractAndSanitizeCommand(resp);
        expect(cmd.contains(QChar(0x00E9)),
               "sanitize: regular Latin-1 accent (é, U+00E9) preserved");
    }
}

void testLengthCap() {
    // 5000 bytes of a fenced block → truncated to 4096 after strip
    QString big = QStringLiteral("```\n");
    big.append(QString('x').repeated(5000));
    big.append(QStringLiteral("\n```"));

    int n = 0;
    QString cmd = AiDialog::extractAndSanitizeCommand(big, &n);
    expect(cmd.size() <= AiDialog::kInsertCommandMaxBytes,
           "cap: output <= 4 KiB",
           QString("size=%1 cap=%2").arg(cmd.size())
                                     .arg(AiDialog::kInsertCommandMaxBytes));
    expect(n >= 5000 - AiDialog::kInsertCommandMaxBytes,
           "cap: stripped count includes truncation amount");
}

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

void testSourceInvariants() {
    const std::string src = slurp(SRC_AI_CPP);

    // The click handler calls extractAndSanitizeCommand.
    expect(src.find("extractAndSanitizeCommand(") != std::string::npos,
           "source: click handler uses extractAndSanitizeCommand");

    // And shows a QMessageBox::question BEFORE emitting insertCommand.
    const size_t qboxPos = src.find("QMessageBox::question");
    const size_t emitPos = src.find("emit insertCommand(cmd)");
    expect(qboxPos != std::string::npos,
           "source: click handler shows QMessageBox::question");
    expect(emitPos != std::string::npos,
           "source: click handler emits insertCommand");
    expect(qboxPos < emitPos,
           "source: question appears BEFORE emit (confirmation gate)");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    testExtraction();
    testSanitization();
    testLengthCap();
    testSourceInvariants();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    return 0;
}
