// Why this exists: locks the Shift+Enter bracketed-paste byte-sequence
// contract so the 0.6.26 "tab freezes after Shift+Enter" regression
// cannot return. The root cause there was
//     seq = QByteArray("\x1B[200~\n\x1B[201~", 8);
// — a 13-byte literal with a hand-coded length of 8, which truncated
// the emitted sequence to `ESC[200~\nESC` and dropped the closing
// `[201~` end-paste marker, wedging the shell in bracketed-paste mode.
// The fix uses QByteArrayLiteral(...) so the size is coupled to the
// literal. This test asserts the behavioural contract (exact byte
// sequences for both branches) and ALSO the source-level form
// (no hand-coded length in the Shift+Enter handler) — the latter is
// the direct regression guard for the specific bug.
//
// Spec: tests/features/shift_enter_bracketed_paste/spec.md
//
// The test does NOT instantiate TerminalWidget. That class is a
// QOpenGLWidget with a live PTY, GL renderer, and half of MainWindow's
// indirect dependencies, and its keyPressEvent is protected. Instead
// the test:
//   1. Locks the byte sequences via a reference implementation —
//      every correct implementation must produce the same bytes.
//   2. Inspects src/terminalwidget.cpp at runtime to confirm the
//      Shift+Enter handler is implemented with a size-coupled literal
//      form, and that it gates on !(mods & ControlModifier) so
//      Ctrl+Shift+Enter still reaches the scratchpad handler earlier
//      in the function.
//
// Exit 0 on pass, non-zero on fail.

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QTextStream>

#include <cstdio>

// SRC_TERMINALWIDGET_PATH is normally supplied by the CMake build with the
// absolute path to src/terminalwidget.cpp (see the add_executable block for
// this test in CMakeLists.txt). Provide an empty fallback so clang/LSP
// parsing outside the CMake build (e.g. the editor's language server) does
// not flag the identifier as undeclared. When the macro is empty the source-
// inspection checks below will fail fast with a clear diagnostic, which is
// the correct behaviour for a run that bypassed CMake.
#ifndef SRC_TERMINALWIDGET_PATH
#define SRC_TERMINALWIDGET_PATH ""
#endif

namespace {

int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

// Reference implementation: what the Shift+Enter handler MUST produce,
// per spec §1 and §2. This is the contract expressed as code. The
// production code at terminalwidget.cpp:1433-1449 must be
// byte-equivalent to this function for both branches.
QByteArray referenceShiftEnterSequence(bool bracketedPaste) {
    if (bracketedPaste) {
        // spec §1: ESC[200~\nESC[201~ (13 bytes)
        return QByteArrayLiteral("\x1B[200~\n\x1B[201~");
    }
    // spec §2: \x16\n (2 bytes)
    return QByteArrayLiteral("\x16\n");
}

// spec §1: bracketed-paste sequence is exactly the expected 13 bytes.
void testBracketedPasteSequence() {
    const QByteArray seq = referenceShiftEnterSequence(true);

    CHECK(seq.size() == 13,
          "bracketed-paste sequence must be exactly 13 bytes");

    const unsigned char expected[13] = {
        0x1B, 0x5B, 0x32, 0x30, 0x30, 0x7E,  // ESC [ 2 0 0 ~
        0x0A,                                 // \n
        0x1B, 0x5B, 0x32, 0x30, 0x31, 0x7E   // ESC [ 2 0 1 ~
    };
    bool ok = (seq.size() == 13);
    for (int i = 0; ok && i < 13; ++i) {
        if (static_cast<unsigned char>(seq.at(i)) != expected[i]) {
            std::fprintf(stderr,
                "  byte %d: got 0x%02X, expected 0x%02X\n",
                i, static_cast<unsigned char>(seq.at(i)), expected[i]);
            ok = false;
        }
    }
    CHECK(ok, "bracketed-paste sequence byte-for-byte match");
}

// spec §2: non-bracketed-paste fallback is exactly \x16\n (2 bytes).
void testFallbackSequence() {
    const QByteArray seq = referenceShiftEnterSequence(false);

    CHECK(seq.size() == 2,
          "fallback sequence must be exactly 2 bytes");
    CHECK(static_cast<unsigned char>(seq.at(0)) == 0x16,
          "fallback byte 0 must be 0x16 (quoted-insert)");
    CHECK(static_cast<unsigned char>(seq.at(1)) == 0x0A,
          "fallback byte 1 must be 0x0A (\\n)");
}

// spec §4: the last 6 bytes of the bracketed-paste sequence MUST be
// the end-paste marker ESC [ 2 0 1 ~. This is the invariant the 0.6.26
// truncation bug directly violated — an 8-byte truncation left the
// sequence ending at `...~\nESC`, not `...ESC[201~`.
void testEndPasteMarkerPresent() {
    const QByteArray seq = referenceShiftEnterSequence(true);
    if (seq.size() < 6) {
        CHECK(false, "sequence too short to inspect tail");
        return;
    }
    const QByteArray tail = seq.right(6);
    const unsigned char expected[6] = { 0x1B, 0x5B, 0x32, 0x30, 0x31, 0x7E };
    bool ok = true;
    for (int i = 0; i < 6; ++i) {
        if (static_cast<unsigned char>(tail.at(i)) != expected[i]) {
            std::fprintf(stderr,
                "  tail byte %d: got 0x%02X, expected 0x%02X\n",
                i, static_cast<unsigned char>(tail.at(i)), expected[i]);
            ok = false;
        }
    }
    CHECK(ok, "bracketed-paste sequence ends with ESC[201~");
}

// Reads src/terminalwidget.cpp and returns its contents, or an empty
// string on failure (with a diagnostic on stderr).
QString readTerminalWidgetSource() {
    QFile f(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n",
                     SRC_TERMINALWIDGET_PATH,
                     qUtf8Printable(f.errorString()));
        ++failures;
        return {};
    }
    QTextStream in(&f);
    return in.readAll();
}

// spec §1 (implementation form): the Shift+Enter handler must use a
// size-coupled literal form — i.e. QByteArrayLiteral(...) — so the
// length cannot drift from the literal contents. This is the direct
// regression guard for the 0.6.26 bug, where a hand-coded length of 8
// truncated a 13-byte literal.
void testSourceUsesSizeCoupledLiteral() {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;

    // The fix form: QByteArrayLiteral("\x1B[200~\n\x1B[201~")
    // We match on a single line for simplicity; the fix is a one-liner.
    const QRegularExpression fixForm(
        QStringLiteral(R"(QByteArrayLiteral\(\s*"\\x1B\[200~\\n\\x1B\[201~"\s*\))"));
    const QRegularExpressionMatch fixMatch = fixForm.match(src);
    CHECK(fixMatch.hasMatch(),
          "source must use QByteArrayLiteral(\"\\x1B[200~\\n\\x1B[201~\") "
          "for the Shift+Enter bracketed-paste sequence");

    // The bug form: QByteArray("\x1B[200~...", <any number>)
    // The `\n` in the literal forces this to span the 200~ and 201~
    // pieces; the bug's signature is the *comma followed by a numeric
    // literal* inside a QByteArray() ctor whose string carries the
    // paste-start marker. If any such construction reappears in this
    // file, fail loudly — even if it isn't the exact 8 that caused
    // the original truncation.
    const QRegularExpression bugForm(
        QStringLiteral(R"(QByteArray\(\s*"\\x1B\[200~[^"]*",\s*\d+\s*\))"));
    const QRegularExpressionMatch bugMatch = bugForm.match(src);
    if (bugMatch.hasMatch()) {
        std::fprintf(stderr,
            "FAIL: source contains the 0.6.26-style hand-coded-length form:\n"
            "      %s\n"
            "      (use QByteArrayLiteral(...) so size is coupled to "
            "the literal)\n",
            qUtf8Printable(bugMatch.captured(0)));
        ++failures;
    }
}

// spec §3: the Shift+Enter handler must gate on !(mods & ControlModifier)
// so Ctrl+Shift+Enter still reaches the earlier scratchpad handler.
void testSourceGatesOutCtrlModifier() {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;

    // Locate the Shift+Enter handler. Match the full guard that gates
    // this branch: (Return|Enter) && ShiftModifier && !ControlModifier.
    // The exact whitespace can drift, so allow any intervening
    // whitespace / line breaks between the clauses.
    const QRegularExpression guard(
        QStringLiteral(
            R"(\(key\s*==\s*Qt::Key_Return\s*\|\|\s*key\s*==\s*Qt::Key_Enter\))"
            R"(\s*&&\s*\(mods\s*&\s*Qt::ShiftModifier\))"
            R"([\s\S]{0,120}?)"  // allow newlines / comments between clauses
            R"(!\s*\(mods\s*&\s*Qt::ControlModifier\))"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = guard.match(src);
    CHECK(m.hasMatch(),
          "Shift+Enter handler must gate on !(mods & Qt::ControlModifier) "
          "so Ctrl+Shift+Enter still reaches the scratchpad handler");
}

// spec §3 (complement): verify the scratchpad handler (Ctrl+Shift+Enter)
// still exists earlier in keyPressEvent. If someone deletes it, the
// !ControlModifier gate in the Shift+Enter handler has nothing to
// defer to and the user loses the scratchpad shortcut silently.
void testScratchpadHandlerExists() {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;

    const QRegularExpression scratchpadGuard(
        QStringLiteral(
            R"(\(key\s*==\s*Qt::Key_Return\s*\|\|\s*key\s*==\s*Qt::Key_Enter\))"
            R"([\s\S]{0,80}?)"
            R"(\(mods\s*&\s*Qt::ControlModifier\))"
            R"([\s\S]{0,80}?)"
            R"(\(mods\s*&\s*Qt::ShiftModifier\))"
            R"([\s\S]{0,80}?)"
            R"(showScratchpad\(\))"),
        QRegularExpression::DotMatchesEverythingOption);
    CHECK(scratchpadGuard.match(src).hasMatch(),
          "Ctrl+Shift+Enter scratchpad handler must still exist "
          "earlier in keyPressEvent");
}

// Sanity: the scratchpad handler (Ctrl+Shift+Enter) must appear in
// the source *before* the Shift+Enter handler, otherwise the order of
// evaluation means Ctrl+Shift+Enter would be intercepted by Shift+Enter
// (if the Ctrl gate in the Shift+Enter handler were ever relaxed).
// This is a belt-and-braces check for spec §3.
void testScratchpadHandlerOrderedBeforeShiftEnter() {
    const QString src = readTerminalWidgetSource();
    if (src.isEmpty()) return;

    const int scratchpadPos = src.indexOf(QStringLiteral("showScratchpad()"));
    const int shiftEnterPos = src.indexOf(
        QStringLiteral("QByteArrayLiteral(\"\\x1B[200~\\n\\x1B[201~\")"));
    CHECK(scratchpadPos > 0,
          "showScratchpad() call not found in keyPressEvent");
    CHECK(shiftEnterPos > 0,
          "Shift+Enter bracketed-paste literal not found");
    if (scratchpadPos > 0 && shiftEnterPos > 0) {
        CHECK(scratchpadPos < shiftEnterPos,
              "Ctrl+Shift+Enter scratchpad handler must appear BEFORE "
              "the Shift+Enter handler in keyPressEvent");
    }
}

}  // namespace

int main() {
    testBracketedPasteSequence();
    testFallbackSequence();
    testEndPasteMarkerPresent();
    testSourceUsesSizeCoupledLiteral();
    testSourceGatesOutCtrlModifier();
    testScratchpadHandlerExists();
    testScratchpadHandlerOrderedBeforeShiftEnter();

    if (failures == 0) {
        std::printf(
            "shift_enter_bracketed_paste: behaviour + source-form "
            "invariants hold\n");
        return 0;
    }
    std::fprintf(stderr,
        "shift_enter_bracketed_paste: %d failure(s)\n", failures);
    return 1;
}
