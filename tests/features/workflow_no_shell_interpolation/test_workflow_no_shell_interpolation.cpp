// ANTS-4792 — GitHub's template engine substitutes ${{ }} into a `run:` body
// before any shell parses it, so an expanded value carrying $(...), a backtick
// or a `;` becomes script rather than data. ANTS-4772 closed the direct
// `inputs.tag` path and left eight sites reading the same value back out of
// $GITHUB_OUTPUT into the script body; nothing local or remote ran a workflow
// linter, so the survivors appeared in no run at all. See spec.md.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

struct Hit {
    QString file;
    int     line = 0;
    QString text;
};

int indentOf(const QString &l) {
    int i = 0;
    while (i < l.size() && (l.at(i) == QLatin1Char(' ')
                            || l.at(i) == QLatin1Char('\t'))) ++i;
    return i;
}

// A line-oriented scan, not a YAML parse -- the project ships no YAML parser
// (audit_rules.json is JSON for exactly that reason) and does not need one
// here: what matters is whether a line sits inside a `run:` body, which block
// indentation alone decides. Anything ambiguous enough to defeat this is
// ambiguous enough that it should not be in a release workflow.
void scanWorkflow(const QString &path, QList<Hit> *hits, int *runBlocks) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);
    const QString base = QFileInfo(path).fileName();

    bool inRun = false;
    int  runIndent = 0;
    int  lineNo = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        ++lineNo;
        const QString trimmed = line.trimmed();

        if (inRun) {
            if (trimmed.isEmpty()) continue;          // blank lines stay inside
            if (indentOf(line) > runIndent) {
                if (line.contains(QStringLiteral("${{")))
                    hits->append({base, lineNo, trimmed});
                continue;
            }
            inRun = false;                            // dedented out of the body
        }

        // `run:` as a mapping key, with or without a leading `- ` step dash.
        const QString key = trimmed.startsWith(QStringLiteral("- "))
                                ? trimmed.mid(2).trimmed() : trimmed;
        if (!key.startsWith(QStringLiteral("run:"))) continue;

        const QString rest = key.mid(4).trimmed();
        // Block scalar: `|`, `>`, and their chomping/keep variants.
        if (!rest.isEmpty() && (rest.at(0) == QLatin1Char('|')
                                || rest.at(0) == QLatin1Char('>'))) {
            inRun = true;
            runIndent = indentOf(line);
            ++(*runBlocks);
        } else {
            ++(*runBlocks);                           // single-line `run: cmd`
            if (rest.contains(QStringLiteral("${{")))
                hits->append({base, lineNo, trimmed});
        }
    }
}

QString workflowsDir() {
    return QString::fromUtf8(ANTS_SOURCE_DIR)
           + QStringLiteral("/.github/workflows");
}

}  // namespace

// INV-1 — no workflow expands ${{ }} into a shell script body. Enumerates the
// DIRECTORY: guarding a named list is what turns a class defect into a queue
// of surfaces, each found by a later build (ANTS-4717).
TEST(WorkflowNoShellInterpolation, NoTemplateExpansionReachesAShellBody) {
    const QDir dir(workflowsDir());
    ASSERT_TRUE(dir.exists()) << workflowsDir().toStdString() << " not found";

    const QStringList files =
        dir.entryList({QStringLiteral("*.yml"), QStringLiteral("*.yaml")},
                      QDir::Files, QDir::Name);
    ASSERT_FALSE(files.isEmpty())
        << "no workflow files under " << workflowsDir().toStdString();

    QList<Hit> hits;
    int runBlocks = 0;
    for (const QString &f : files)
        scanWorkflow(dir.filePath(f), &hits, &runBlocks);

    // Guard the guard: a scanner that recognises no `run:` at all would report
    // a clean sweep of nothing. Both workflows that matter are dense with
    // them, so any collapse of the block detection trips here first.
    ASSERT_GT(runBlocks, 10)
        << "only " << runBlocks << " run: block(s) recognised across "
        << files.size() << " workflow file(s) -- the SCANNER is what is broken "
           "here, not the workflows";

    QString detail;
    for (const Hit &h : hits)
        detail += QStringLiteral("\n  %1:%2  %3").arg(h.file)
                      .arg(h.line).arg(h.text.left(90));

    EXPECT_TRUE(hits.isEmpty())
        << hits.size() << " template expansion(s) reach a shell body:"
        << detail.toStdString()
        << "\n\nPass the value through the step's `env:` instead and reference "
           "it as a shell variable. ${{ }} is substituted into the script TEXT "
           "before any shell parses it, so a value carrying $(...), a backtick "
           "or a `;` is executed -- and release.yml runs with contents: write. "
           "A `#` comment in the body is not a safe home for one either: the "
           "expansion happens first, and a newline in the value puts what "
           "follows it back on a live line.";
}
