// ANTS-3663 — doc_lint engine. See doclint.h for the design and the spec.

#include "doclint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

namespace DocLint {

const QStringList &checkNames() {
    static const QStringList v = {
        QStringLiteral("doc_integrity"),
        QStringLiteral("doc_citations"),
        QStringLiteral("doc_dedup"),
        QStringLiteral("doc_symbols"),
        QStringLiteral("spec_lint"),
    };
    return v;
}

namespace {

const QString kIntegrity = QStringLiteral("doc_integrity");
const QString kCitations = QStringLiteral("doc_citations");
const QString kDedup     = QStringLiteral("doc_dedup");
const QString kSymbols   = QStringLiteral("doc_symbols");
const QString kSpecLint  = QStringLiteral("spec_lint");

bool selected(const Options &o, const QString &name) {
    return o.checks.isEmpty() || o.checks.contains(name);
}

// spec_lint is the only checker narrower than "every enumerated *.md"
// (spec § 2.1's eligibility table). A filtered-out document is NOT a skip:
// nothing failed.
bool eligible(const QString &verb, const QString &rel, const Options &o) {
    if (verb != kSpecLint) return true;
    if (o.specsDirRel.isEmpty()) return false;
    const QString dir = o.specsDirRel.endsWith(QLatin1Char('/'))
                            ? o.specsDirRel
                            : o.specsDirRel + QLatin1Char('/');
    return rel.startsWith(dir);
}

// The wire strings docIntegrityBuildResponse already uses — one vocabulary, so
// a caller filtering doc_lint findings by a kind it learned from doc_integrity
// cannot miss.
QString integrityKind(DocIntegrity::Kind k) {
    switch (k) {
    case DocIntegrity::Kind::DeadAnchor:      return QStringLiteral("dead_anchor");
    case DocIntegrity::Kind::BrokenLink:      return QStringLiteral("broken_link");
    case DocIntegrity::Kind::TocGap:          return QStringLiteral("toc_gap");
    case DocIntegrity::Kind::HeadingSequence: return QStringLiteral("heading_sequence");
    case DocIntegrity::Kind::UngrantedTool:   return QStringLiteral("ungranted_tool");
    }
    return QString();
}

// Spec § 2.2's reading convention: the adapter sets autoFixable from a small
// per-adapter kind list, because DocIntegrity::Finding predates the flag.
bool integrityAutoFixable(DocIntegrity::Kind k) {
    return k == DocIntegrity::Kind::TocGap;
}

// One line, no trailing period, no file:line prefix — DocFinding's contract.
// The TARGET is named here and nowhere else: `file` holds the document the
// finding was found IN, never the thing it points at (INV-20).
QString citationMessage(const QJsonObject &e, const QString &status) {
    if (status == QStringLiteral("ambiguous")) {
        const QJsonArray cand = e.value(QStringLiteral("candidates")).toArray();
        if (!cand.isEmpty()) {
            QStringList names;
            for (const QJsonValue &v : cand) names << v.toString();
            return QStringLiteral("ambiguous citation: ") + names.join(QStringLiteral(", "));
        }
    }
    const QString path = e.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) return status + QStringLiteral(": ") + path;
    // A continuation citation carries no `path` by construction, so the raw
    // locus is the only thing left that names what failed.
    return status + QStringLiteral(": ") + e.value(QStringLiteral("raw")).toString();
}

}  // namespace

Result run(const QStringList &relDocs, const Options &opts) {
    Result r;
    const QDir rootDir(opts.rootCanonical);

    const bool wantIntegrity = selected(opts, kIntegrity);
    const bool wantCitations = selected(opts, kCitations);
    const bool wantDedup     = selected(opts, kDedup);
    const bool wantSymbols   = selected(opts, kSymbols);
    const bool wantSpecLint  = selected(opts, kSpecLint);

    DocDedup::Accumulator acc;
    bool dedupRan = false, symbolsRan = false, specLintRan = false;

    // The run-wide needle budget doc_symbols' per-document Options cannot hold:
    // the engine is per document, the bound is per run, so the caller debits it.
    int symbolBudget = opts.symbols.maxSymbolsPerRun;

    // ---- Phase 1: one enumeration, one read, three native checkers ----------
    for (int i = 0; i < relDocs.size(); ++i) {
        const QString &rel = relDocs.at(i);

        // maxDocsPerRun stops the WALK — DocIntegrity's own semantics, whose
        // comment is explicit that docs beyond it are skipped, not read.
        if (i >= opts.walk.maxDocsPerRun) {
            r.skipped.append({rel, QStringLiteral("doc_cap")});
            r.truncated = true;
            continue;
        }

        const QString abs = QDir::cleanPath(rootDir.filePath(rel));

        // maxDocBytes TRUNCATES in DocIntegrity; here it SKIPS, at every checker
        // and the two adapters included. Truncating would hand the three native
        // checkers a document cut off mid-sentence and report its findings as a
        // complete account of it — the silent partial coverage this envelope
        // exists to make visible.
        if (QFileInfo(abs).size() > opts.walk.maxDocBytes) {
            r.skipped.append({rel, QStringLiteral("too_large")});
            r.truncated = true;
            continue;
        }

        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) {
            r.skipped.append({rel, QStringLiteral("read_failed")});
            continue;  // one bad file must not fail a whole review pass
        }
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        if (opts.probe) ++opts.probe->opens;

        r.checkedDocs << rel;

        if (wantDedup && eligible(kDedup, rel, opts)) {
            dedupRan = true;
            if (!DocDedup::isExcludedPath(rel, opts.dedup))
                acc.add(text, rel, opts.dedup);
        }

        if (wantSymbols && eligible(kSymbols, rel, opts)) {
            symbolsRan = true;
            DocSymbols::Options so = opts.symbols;
            so.maxSymbolsPerRun = qMax(0, symbolBudget);
            const DocSymbols::ScanResult sr = DocSymbols::scan(text, rel, so);
            symbolBudget -= sr.needlesResolved;
            r.findings.append(sr.findings);
            r.stats.symbolsTotal      += sr.total;
            r.stats.symbolsResolved   += sr.resolved;
            r.stats.symbolsUnresolved += sr.unresolved;
            r.stats.symbolsNotChecked += sr.notChecked;
            r.stats.symbolsTruncated   = r.stats.symbolsTruncated || sr.truncated;
        }

        if (wantSpecLint && eligible(kSpecLint, rel, opts)) {
            specLintRan = true;
            const SpecLint::Result sl = SpecLint::check(text, rel, opts.spec);
            r.findings.append(sl.findings);
            // OR-ed: it describes the run's configuration, not a per-document
            // outcome, and is identical for every document anyway.
            r.stats.sectionsChecked   = r.stats.sectionsChecked || sl.sectionsChecked;
            r.stats.specLintTruncated = r.stats.specLintTruncated || sl.truncated;
            // MERGED by project-relative path. A composer that treats this as a
            // scalar overwrites it once per document and returns the last one.
            r.stats.lineCount.insert(rel, sl.lineCount);
        }
    }

    // ---- doc_dedup scores once, over the whole corpus -----------------------
    if (dedupRan) {
        const DocDedup::Result dr = acc.finish();
        r.findings.append(dr.findings);
        r.pairs    = dr.pairs;
        r.clusters = dr.clusters;
        // Run-scoped: taken verbatim, never summed. Summing would multiply each
        // by the document count.
        r.stats.passagesTotal    = dr.passagesTotal;
        r.stats.passagesCompared = dr.passagesCompared;
        r.stats.dedupTruncated   = dr.truncated;
    }

    // ---- Phase 2: the two frozen adapters ----------------------------------
    if (wantIntegrity && !r.checkedDocs.isEmpty()) {
        const QList<DocIntegrity::Finding> got =
            DocIntegrity::check(opts.rootCanonical, r.checkedDocs, opts.walk);
        int emission = 0;  // the adapter IS the producer: DocIntegrity::Finding
                           // has no such member, and INV-7's tiebreak needs one.
        for (const DocIntegrity::Finding &g : got) {
            DocFinding::Finding f;
            f.verb          = kIntegrity;
            f.kind          = integrityKind(g.kind);
            f.file          = g.file;
            f.line          = g.line;
            f.message       = g.message;
            f.autoFixable   = integrityAutoFixable(g.kind);
            f.emissionIndex = emission++;
            r.findings.append(f);
        }
    }

    if (wantCitations) {
        int emission = 0;
        for (const QString &rel : std::as_const(r.checkedDocs)) {
            const QString abs = QDir::cleanPath(rootDir.filePath(rel));
            const QJsonObject o =
                DocCitations::check(opts.rootCanonical, abs, opts.citations);
            if (!o.value(QStringLiteral("ok")).toBool()) {
                r.checkErrors.append({kCitations, rel, QStringLiteral("check_failed")});
                continue;
            }

            const auto add = [&](const QString &kind, int line, const QString &msg,
                                 bool fixable = false) {
                DocFinding::Finding f;
                f.verb          = kCitations;
                f.kind          = kind;
                f.file          = rel;   // the document, never the target
                f.line          = line;
                f.message       = msg;
                f.autoFixable   = fixable;
                f.emissionIndex = emission++;
                r.findings.append(f);
            };

            const QJsonArray cites = o.value(QStringLiteral("citations")).toArray();
            for (const QJsonValue &v : cites) {
                const QJsonObject e = v.toObject();
                const QString status = e.value(QStringLiteral("status")).toString();
                // An ok citation is never a finding, INCLUDING one whose
                // anchor_found is false: that flag is advisory — it says a
                // needle moved, not that the citation is wrong.
                if (status.isEmpty() || status == QStringLiteral("ok")) continue;
                add(status, e.value(QStringLiteral("doc_line")).toInt(),
                    citationMessage(e, status));
            }

            // unparsed[] entries are contained noise by design and never become
            // findings — but they are not lost either (INV-4's second half).
            r.stats.unparsedTotal +=
                o.value(QStringLiteral("unparsed_total")).toInt();
            r.stats.examplesSuppressed +=
                o.value(QStringLiteral("examples_suppressed")).toInt();

            // Defects by construction: an unterminated fence swallows the rest
            // of the document and an unterminated doc-examples region suppresses
            // every citation after it. Both are invisible to a reader, which is
            // why /cold-eyes reads the fence alarm from this field.
            if (o.contains(QStringLiteral("unterminated_fence")))
                add(QStringLiteral("unterminated_fence"),
                    o.value(QStringLiteral("unterminated_fence")).toInt(),
                    QStringLiteral("code fence opened here is never closed"));
            if (o.contains(QStringLiteral("unterminated_examples")))
                add(QStringLiteral("unterminated_examples"),
                    o.value(QStringLiteral("unterminated_examples")).toInt(),
                    QStringLiteral("doc-examples region opened here is never closed"));

            // The three incompletenesses. In all of them this document's
            // ok/not-ok split is a FLOOR rather than an answer, so they route to
            // check_errors[] and not to check_stats — a caller reading
            // findings[] as complete is reading it wrong. `truncated` is the one
            // an adapter written from a shipped envelope never sees: it reads as
            // pagination and means the citation set itself is partial.
            if (o.value(QStringLiteral("read_budget_exhausted")).toBool())
                r.checkErrors.append({kCitations, rel,
                                      QStringLiteral("read_budget_exhausted")});
            if (o.value(QStringLiteral("basename_index_truncated")).toBool())
                r.checkErrors.append({kCitations, rel,
                                      QStringLiteral("basename_index_truncated")});
            if (o.value(QStringLiteral("truncated")).toBool())
                r.checkErrors.append({kCitations, rel,
                                      QStringLiteral("citations_truncated")});
        }
    }

    // ---- checks_run: ran clean is not the same as did not run ---------------
    if (wantIntegrity && !r.checkedDocs.isEmpty()) r.checksRun << kIntegrity;
    if (wantCitations && !r.checkedDocs.isEmpty()) r.checksRun << kCitations;
    if (dedupRan)    r.checksRun << kDedup;
    if (symbolsRan)  r.checksRun << kSymbols;
    if (specLintRan) r.checksRun << kSpecLint;

    // ---- INV-7: a TOTAL order -----------------------------------------------
    // The first five keys are all properties of a finding, so two findings can
    // agree on every one of them; emissionIndex is the tiebreak of last resort.
    std::stable_sort(r.findings.begin(), r.findings.end(),
                     [](const DocFinding::Finding &a, const DocFinding::Finding &b) {
                         if (a.file != b.file)       return a.file < b.file;
                         if (a.line != b.line)       return a.line < b.line;
                         if (a.verb != b.verb)       return a.verb < b.verb;
                         if (a.kind != b.kind)       return a.kind < b.kind;
                         if (a.message != b.message) return a.message < b.message;
                         return a.emissionIndex < b.emissionIndex;
                     });
    return r;
}

}  // namespace DocLint
