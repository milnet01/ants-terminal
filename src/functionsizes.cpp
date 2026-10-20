// function_sizes (ANTS-5787) — the longest functions in the given paths.

#include "functionsizes.h"

#include "codewalk.h"
#include "fileoutline.h"
#include "readregion.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>

#include <algorithm>

namespace FunctionSizes {

namespace {

int indentOf(const QString &line) {
    int n = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' ')) ++n;
        else if (c == QLatin1Char('\t')) n += 8;
        else break;
    }
    return n;
}

bool isBlankOrComment(const QString &line) {
    const QString t = line.trimmed();
    return t.isEmpty() || t.startsWith(QLatin1Char('#'));
}

// Python has no closing brace: a body ends at its last non-blank line before
// the next line indented no deeper than the `def`. Nesting counts the
// distinct deeper indentation levels open at once.
int pythonEnd(const QStringList &lines, int defLine, int *maxDepth) {
    const int defIndent = indentOf(lines.at(defLine - 1));
    int end = 0;
    QList<int> open;
    int deepest = 0;
    for (int n = defLine + 1; n <= lines.size(); ++n) {
        const QString &line = lines.at(n - 1);
        if (isBlankOrComment(line)) continue;
        const int indent = indentOf(line);
        if (indent <= defIndent) break;
        while (!open.isEmpty() && open.last() >= indent) open.removeLast();
        open.append(indent);
        deepest = qMax(deepest, int(open.size()));
        end = n;
    }
    *maxDepth = deepest;
    return end;
}

} // namespace

QList<Function> measure(const QString &absPath, const QStringList &lines,
                        int *unmeasured) {
    QList<Function> out;
    const QJsonObject outline = FileOutline::compute(
        absPath, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        FileOutline::kMaxSymbolsCap);
    const QString language = outline.value(QStringLiteral("language")).toString();
    const bool python = language == QLatin1String("py");
    // Ruby closes a body with `end`, which the brace scan cannot see.
    const bool keywordDelimited = language == QLatin1String("ruby");
    const QJsonArray symbols = outline.value(QStringLiteral("symbols")).toArray();
    for (qsizetype i = 0; i < symbols.size(); ++i) {
        const QJsonObject s = symbols.at(i).toObject();
        if (s.value(QStringLiteral("kind")).toString() != QLatin1String("func")) continue;
        if (s.value(QStringLiteral("signature")).toString().trimmed().endsWith(QLatin1Char(';')))
            continue;
        Function f;
        f.symbol = s.value(QStringLiteral("name")).toString();
        f.bodyStart = s.value(QStringLiteral("line")).toInt();
        if (f.bodyStart < 1 || f.bodyStart > lines.size()) continue;
        if (keywordDelimited) {
            ++*unmeasured;
            continue;
        }
        if (python) {
            f.bodyEnd = pythonEnd(lines, f.bodyStart, &f.nestingDepth);
        } else {
            // A body's opening brace comes before the next symbol starts. An
            // expression-bodied function (`fun f() = x`) has none, and the
            // scan would otherwise take the next function's braces as its own.
            int nextSymbol = int(lines.size()) + 1;
            for (const QJsonValue other : symbols) {
                const int line = other.toObject().value(QStringLiteral("line")).toInt();
                if (line > f.bodyStart) nextSymbol = qMin(nextSymbol, line);
            }
            int openLine = 0;
            f.bodyEnd = ReadRegion::braceBalancedEndLine(lines, f.bodyStart,
                                                         &f.nestingDepth, &openLine);
            if (openLine >= nextSymbol) f.bodyEnd = 0;
        }
        if (f.bodyEnd < f.bodyStart) {
            ++*unmeasured;
            continue;
        }
        out.append(f);
    }
    return out;
}

QJsonObject run(const QString &rootCanonical, const QJsonObject &args) {
    const QString verb = QStringLiteral("function_sizes");
    int top = kDefaultTop;
    if (args.contains(QStringLiteral("top"))) {
        top = args.value(QStringLiteral("top")).toInt(0);
        if (top < 1 || top > kMaxTop) {
            QJsonObject e;
            e[QStringLiteral("ok")] = false;
            e[QStringLiteral("code")] = QStringLiteral("bad_args");
            e[QStringLiteral("error")] =
                QStringLiteral("function_sizes: \"top\" must be 1 to %1").arg(kMaxTop);
            return e;
        }
    }
    CodeWalk::Options walkOpts;
    walkOpts.includeTests = args.value(QStringLiteral("include_tests")).toBool(false);
    walkOpts.includeGenerated = args.value(QStringLiteral("include_generated")).toBool(false);
    const CodeWalk::Result files = CodeWalk::collect(
        rootCanonical, args.value(QStringLiteral("paths")).toArray(), walkOpts, verb);
    if (!files.refusal.isEmpty()) return files.refusal;

    struct Row {
        QString file;
        Function f;
        int lines() const { return f.bodyEnd - f.bodyStart + 1; }
    };
    QList<Row> rows;
    int unmeasured = 0;
    const QDir root(rootCanonical);
    for (const QString &abs : files.files) {
        QFile in(abs);
        if (!in.open(QIODevice::ReadOnly)) continue;
        QStringList lines;
        while (!in.atEnd()) lines.append(QString::fromUtf8(in.readLine()));
        const QString rel = root.relativeFilePath(abs);
        for (const Function &f : measure(abs, lines, &unmeasured))
            rows.append({rel, f});
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.lines() != b.lines()) return a.lines() > b.lines();
        if (a.file != b.file) return a.file < b.file;
        return a.f.bodyStart < b.f.bodyStart;
    });

    QJsonArray functions;
    for (int i = 0; i < rows.size() && i < top; ++i) {
        const Row &r = rows.at(i);
        QJsonObject o;
        o[QStringLiteral("id")] =
            QStringLiteral("%1:%2:%3").arg(r.file).arg(r.f.bodyStart).arg(r.f.symbol);
        o[QStringLiteral("file")] = r.file;
        o[QStringLiteral("symbol")] = r.f.symbol;
        o[QStringLiteral("lines")] = r.lines();
        o[QStringLiteral("body_start")] = r.f.bodyStart;
        o[QStringLiteral("body_end")] = r.f.bodyEnd;
        o[QStringLiteral("nesting_depth")] = r.f.nestingDepth;
        functions.append(o);
    }

    QJsonObject scanned = CodeWalk::scannedSummary(files);
    scanned[QStringLiteral("functions_measured")] = int(rows.size());
    scanned[QStringLiteral("unmeasured")] = unmeasured;
    QJsonObject env;
    env[QStringLiteral("ok")] = true;
    env[QStringLiteral("functions")] = functions;
    env[QStringLiteral("top")] = top;
    env[QStringLiteral("scanned")] = scanned;
    return env;
}

} // namespace FunctionSizes
