// ANTS-1301 — recent_errors scrollback parser. See scrollbackerrors.h.

#include "scrollbackerrors.h"

#include <QRegularExpression>

namespace ScrollbackErrors {

namespace {

constexpr int kDefaultMaxResults = 50;
constexpr int kMaxResultsCap     = 1000;

// Patterns are symbol-free, so process-static is sound (unlike
// ANTS-1303's per-call symbol splice). Each is built + optimised once.
const QRegularExpression &rxCompiler() {
    static const QRegularExpression rx = [] {
        QRegularExpression r(QStringLiteral(
            R"(^\s*(\S.*?):(\d+):(?:(\d+):)?\s+(?:fatal\s+)?error:\s*(.+)$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxLint() {
    static const QRegularExpression rx = [] {
        QRegularExpression r(QStringLiteral(
            R"(^\s*(\S.*?):(\d+):(\d+):\s+([A-Z]{1,5}\d{1,4})\b\s*(.*)$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxLua() {
    static const QRegularExpression rx = [] {
        QRegularExpression r(QStringLiteral(
            R"(^\s*lua:\s+(\S.*?):(\d+):\s*(.+)$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxTestBlock() {
    static const QRegularExpression rx = [] {
        QRegularExpression r(QStringLiteral(
            R"(^\s*\d+\s*-\s*(\S.+?)\s+\((Failed|Timeout|Exception|Not Run|Subprocess aborted)\)\s*$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxPyFrame() {
    static const QRegularExpression rx = [] {
        QRegularExpression r(QStringLiteral(
            R"RX(^\s+File "(.+?)", line (\d+)(?:, in .+)?$)RX"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxPyException() {
    static const QRegularExpression rx = [] {
        QRegularExpression r(QStringLiteral(
            R"(^([A-Za-z_][\w.]*)(?::\s*(.*))?$)"));
        r.optimize();
        return r;
    }();
    return rx;
}

bool hasTestMarker(const QString &line) {
    return line.contains(QStringLiteral("***Failed")) ||
           line.contains(QStringLiteral("***Timeout")) ||
           line.contains(QStringLiteral("***Exception"));
}

// True for an indented line that carries non-whitespace (a Python
// source-echo / caret line inside a traceback).
bool isIndentedNonEmpty(const QString &line) {
    if (line.isEmpty()) return false;
    const QChar c0 = line.at(0);
    if (c0 != QLatin1Char(' ') && c0 != QLatin1Char('\t')) return false;
    return !line.trimmed().isEmpty();
}

}  // namespace

Result parse(const QString &scrollback, const Options &opts) {
    Result r;
    int cap = opts.maxResults;
    if (cap <= 0)             cap = kDefaultMaxResults;
    if (cap > kMaxResultsCap) cap = kMaxResultsCap;

    QStringList lines = scrollback.split(QLatin1Char('\n'));
    r.linesScanned = lines.size();

    // Ring that keeps the most-recent `cap` entries; errorsTotal counts
    // every match regardless of the cap (§ 3.4 keep-last semantics).
    // NB: not named `emit` — that's a Qt keyword macro (expands to
    // nothing), which would silently erase every call site.
    auto pushEntry = [&](ErrorEntry e) {
        ++r.errorsTotal;
        r.errors.append(std::move(e));
        if (r.errors.size() > cap) r.errors.removeFirst();
    };

    // Python-traceback state.
    bool inTrace = false;
    bool haveFrame = false;
    QString frameFile;
    int     frameLine = 0;
    QString frameText;

    auto resetTrace = [&] {
        inTrace = false; haveFrame = false;
        frameFile.clear(); frameLine = 0; frameText.clear();
    };
    auto flushTraceNoExc = [&] {
        if (inTrace && haveFrame) {
            ErrorEntry e;
            e.category = QStringLiteral("python");
            e.file = frameFile;
            e.line = frameLine;
            e.message = frameText;
            e.text = frameText;
            pushEntry(std::move(e));
        }
        resetTrace();
    };

    static const QString kAnchor =
        QStringLiteral("Traceback (most recent call last):");

    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        const QString trimmed = line.trimmed();

        // (a) Traceback anchor — opens a block; a fresh anchor while
        //     already in a block flushes the previous (unterminated) one.
        if (trimmed == kAnchor) {
            if (inTrace) flushTraceNoExc();
            inTrace = true;
            continue;
        }

        // (b) Inside a traceback block.
        if (inTrace) {
            const auto fm = rxPyFrame().match(line);
            if (fm.hasMatch()) {
                frameFile = fm.captured(1);
                frameLine = fm.captured(2).toInt();
                frameText = trimmed;
                haveFrame = true;
                continue;
            }
            if (isIndentedNonEmpty(line)) {
                continue;  // source-echo / caret line
            }
            // First non-indented line ends the block.
            const auto xm = rxPyException().match(trimmed);
            if (!trimmed.isEmpty() && xm.hasMatch()) {
                ErrorEntry e;
                e.category = QStringLiteral("python");
                e.file = frameFile;
                e.line = frameLine;
                e.message = trimmed;
                e.text = trimmed;
                pushEntry(std::move(e));
                resetTrace();
                continue;  // exception line consumed
            }
            // Not an exception → flush with frame text, then fall
            // through to classify this line as a normal single-liner.
            flushTraceNoExc();
        }

        // (c) Single-line matchers, first-match-wins in spec order.
        const auto cm = rxCompiler().match(line);
        if (cm.hasMatch()) {
            ErrorEntry e;
            e.category = QStringLiteral("compiler");
            e.file = cm.captured(1);
            e.line = cm.captured(2).toInt();
            if (!cm.captured(3).isEmpty()) e.column = cm.captured(3).toInt();
            e.message = cm.captured(4).trimmed();
            e.text = trimmed;
            pushEntry(std::move(e));
            continue;
        }
        const auto lm = rxLint().match(line);
        if (lm.hasMatch()) {
            ErrorEntry e;
            e.category = QStringLiteral("lint");
            e.file = lm.captured(1);
            e.line = lm.captured(2).toInt();
            e.column = lm.captured(3).toInt();
            const QString code = lm.captured(4);
            const QString rest = lm.captured(5).trimmed();
            e.message = rest.isEmpty() ? code : (code + QLatin1Char(' ') + rest);
            e.text = trimmed;
            pushEntry(std::move(e));
            continue;
        }
        const auto um = rxLua().match(line);
        if (um.hasMatch()) {
            ErrorEntry e;
            e.category = QStringLiteral("lua");
            e.file = um.captured(1);
            e.line = um.captured(2).toInt();
            e.message = um.captured(3).trimmed();
            e.text = trimmed;
            pushEntry(std::move(e));
            continue;
        }
        const auto tm = rxTestBlock().match(line);
        if (tm.hasMatch()) {
            ErrorEntry e;
            e.category = QStringLiteral("test");
            e.message = tm.captured(1).trimmed();
            e.text = trimmed;
            pushEntry(std::move(e));
            continue;
        }
        if (hasTestMarker(line)) {
            ErrorEntry e;
            e.category = QStringLiteral("test");
            e.message = trimmed;
            e.text = trimmed;
            pushEntry(std::move(e));
            continue;
        }
    }

    // Trailing block ran to end-of-input without a terminating line.
    if (inTrace) flushTraceNoExc();

    r.truncated = r.errorsTotal > r.errors.size();
    return r;
}

}  // namespace ScrollbackErrors
