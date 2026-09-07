// ANTS-3663 — doc_lint engine. See doclint.h for the design and the spec.

#include "doclint.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QVector>

#include <algorithm>

// fsyncParentDir — the durability half of the project's atomic-write idiom.
#include "secureio.h"

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

// ===== ANTS-3669 — the fix path (spec § 2.3) =============================
//
// Everything below writes, or decides whether to. The read half returns a wrong
// string when it is wrong; this rewrites a document the user has not opened, so
// every uncertainty REFUSES. There is no branch here that guesses.

// A `## heading` or a `[t](#s)` inside a code fence is neither.
QVector<bool> fenceMap(const QStringList &lines) {
    static const QRegularExpression fenceRe(QStringLiteral(R"(^\s{0,3}(?:```|~~~))"));
    QVector<bool> in(lines.size(), false);
    bool open = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (fenceRe.match(lines.at(i)).hasMatch()) { in[i] = true; open = !open; continue; }
        in[i] = open;
    }
    return in;
}

struct Hd {
    QString text;
    QString slug;
    int level = 0;
    int     line = 0;   // 1-based
};

// Slugs are assigned in document order through one `seen` map, which is how
// DocIntegrity::gfmSlug disambiguates repeats. Computing them independently
// per heading would give two headings of the same text the same slug and make
// the entry->heading lookup below ambiguous.
QVector<Hd> headingsOf(const QStringList &lines, const QVector<bool> &fence) {
    static const QRegularExpression re(QStringLiteral(R"(^ {0,3}(#{1,6})\s+(\S.*?)\s*$)"));
    QVector<Hd> out;
    QHash<QString, int> seen;
    for (int i = 0; i < lines.size(); ++i) {
        if (fence.at(i)) continue;
        const auto m = re.match(lines.at(i));
        if (!m.hasMatch()) continue;
        const QString text = m.captured(2);
        out.append({text, DocIntegrity::gfmSlug(text, seen),
                    static_cast<int>(m.captured(1).size()), i + 1});
    }
    return out;
}

bool isTocListItem(const QString &s) {
    static const QRegularExpression re(QStringLiteral(R"(^\s{0,8}(?:[-*+]|\d+[.)])\s)"));
    return re.match(s).hasMatch();
}

struct TocEntry {
    QString slug;
    int     line = 0;   // 1-based
};

struct TocRegion {
    bool              found     = false;
    int               firstLine = -1;
    QVector<TocEntry> entries;
};

// MIRRORS DocIntegrity's detectToc trigger and run rule — the `Contents` /
// `Table of Contents` heading, then the run of list items under it, with an
// indented non-item counted as a wrapped continuation rather than the end.
//
// It is a deliberate second copy of the smallest part the patch needs, because
// spec § 2.2 freezes that engine and its locator is file-static over types
// (`DocData`, `Heading`) that are not exported. What bounds the drift is that
// this function's only failure mode is returning `found:false`, which the
// caller turns into `no_template` and writes nothing: the two disagreeing costs
// a declined repair, never an edit to the wrong region.
TocRegion detectTocRegion(const QStringList &lines, const QVector<bool> &fence,
                          const QVector<Hd> &heads) {
    TocRegion t;
    int contentsLine = -1;
    for (const Hd &h : heads) {
        if (h.slug == QLatin1String("contents") ||
            h.slug == QLatin1String("table-of-contents")) {
            contentsLine = h.line;
            break;
        }
    }
    if (contentsLine < 0) return t;

    const int n = lines.size();
    int idx = contentsLine;   // 0-based index of the line AFTER the heading
    while (idx < n && lines.at(idx).trimmed().isEmpty()) ++idx;
    if (idx >= n || !isTocListItem(lines.at(idx))) return t;

    static const QRegularExpression linkRe(QStringLiteral(R"(\[[^\]]*\]\(([^)]*)\))"));
    int lastItem = -1;
    for (int j = idx; j < n; ++j) {
        if (fence.at(j)) break;
        if (lines.at(j).trimmed().isEmpty()) continue;
        const bool continuation =
            lastItem >= 0 && !lines.at(j).isEmpty() && lines.at(j).at(0).isSpace();
        if (!isTocListItem(lines.at(j)) && !continuation) break;
        if (!continuation) lastItem = j + 1;
        auto it = linkRe.globalMatch(lines.at(j));
        while (it.hasNext()) {
            const QString target = it.next().captured(1).trimmed();
            if (target.startsWith(QLatin1Char('#')))
                t.entries.append({target.mid(1), j + 1});
        }
    }
    if (lastItem < 0) return t;
    t.found     = true;
    t.firstLine = idx + 1;
    return t;
}

// DocIntegrity emits toc_gap for exactly two causes; the message is the only
// place the cause survives into DocFinding::Finding, which carries no cause
// field. Parsed rather than re-derived so the fixer stays a consumer of
// findings[] and never an independent scanner (spec § 5).
bool isDuplicateEntryGap(const QString &message, QString *slug) {
    static const QRegularExpression re(QStringLiteral(R"(^duplicate TOC entry '(.*)'$)"));
    const auto m = re.match(message);
    if (!m.hasMatch()) return false;
    if (slug) *slug = m.captured(1);
    return true;
}

bool isMissingSectionGap(const QString &message, QString *heading) {
    static const QRegularExpression re(QStringLiteral(R"(^section '(.*)' missing from TOC$)"));
    const auto m = re.match(message);
    if (!m.hasMatch()) return false;
    if (heading) *heading = m.captured(1);
    return true;
}

// A PATCH, NEVER A REGENERATION (INV-18). Every line the two causes do not name
// is copied byte-identical — H3 and deeper entries, free-text rows, blank lines,
// and any entry whose slug matches no heading. That last one looks like a defect
// and is deliberately left: doc_integrity does not report it (coverage runs
// heading->entry, never entry->heading), so removing it would be the fixer
// acting on a judgement no checker made.
//
// Rebuilding the region from the H2 set passes a fixture whose TOC is H2-only
// and silently deletes a hand-written sub-entry on the first real document it
// meets, which is exactly what INV-18 exists to catch.
bool patchTocRegion(QStringList &lines, const QList<DocFinding::Finding> &gaps,
                    QString *reason) {
    const QVector<bool> fence = fenceMap(lines);
    const QVector<Hd>   heads = headingsOf(lines, fence);
    const TocRegion     toc   = detectTocRegion(lines, fence, heads);
    if (!toc.found) { *reason = QStringLiteral("no_template"); return false; }

    QHash<QString, const Hd *> bySlug;
    for (const Hd &h : heads) bySlug.insert(h.slug, &h);

    // The template is an existing entry pointing at an H2 — its indent, list
    // marker and link form are what an inserted entry copies. A region with
    // none is not repaired: the file's convention is unknown, and guessing it
    // is how a fixer starts writing markdown the author did not choose.
    static const QRegularExpression prefixRe(QStringLiteral(R"(^(\s*(?:[-*+]|\d+[.)])\s+))"));
    QVector<TocEntry> h2Entries;
    for (const TocEntry &e : toc.entries) {
        const Hd *const h = bySlug.value(e.slug, nullptr);
        if (h && h->level == 2 && prefixRe.match(lines.at(e.line - 1)).hasMatch())
            h2Entries.append(e);
    }
    if (h2Entries.isEmpty()) { *reason = QStringLiteral("no_template"); return false; }

    QSet<QString> entrySlugs;
    for (const TocEntry &e : toc.entries) entrySlugs.insert(e.slug);

    QSet<int>              deletions;   // 1-based entry lines
    // Deliberately NOT a QMultiMap: its values(key) returns most-recently
    // inserted first, which silently reverses two sections inserted after the
    // same anchor — document order is the one thing this repair is for.
    QMap<int, QStringList> insertions;  // after 1-based line -> rendered entries

    for (const DocFinding::Finding &g : gaps) {
        QString slug, headingText;
        if (isDuplicateEntryGap(g.message, &slug)) {
            if (g.line < 1 || g.line > lines.size()) {
                *reason = QStringLiteral("stale");
                return false;
            }
            deletions.insert(g.line);
            continue;
        }
        if (!isMissingSectionGap(g.message, &headingText)) {
            // A toc_gap whose cause this cannot name is not repaired blind.
            *reason = QStringLiteral("stale");
            return false;
        }

        // Located by TEXT, never by the finding's line. The walk finishes before
        // any of this runs, so a save in between shifts every line below it
        // while leaving the gap the same gap — and § 2.3 requires those to still
        // repair, which is why fixed[] reports the walk-time location. The
        // heading must still be UNCOVERED, the condition that produced the
        // finding; two uncovered H2s sharing one text are ambiguous and refuse.
        const Hd *target = nullptr;
        int matches = 0;
        for (const Hd &h : heads) {
            if (h.level != 2 || h.text != headingText) continue;
            if (entrySlugs.contains(h.slug)) continue;
            target = &h;
            ++matches;
        }
        if (matches != 1) { *reason = QStringLiteral("stale"); return false; }

        // Document order: the last existing H2 entry whose own heading sits
        // above this one. None means this section precedes every entry, so the
        // new row goes at the head of the region.
        const TocEntry *anchor = nullptr;
        for (const TocEntry &e : h2Entries) {
            const Hd *const h = bySlug.value(e.slug, nullptr);
            if (h && h->line < target->line) anchor = &e;
        }
        const TocEntry &tmpl  = anchor ? *anchor : h2Entries.first();
        const QString   prefix = prefixRe.match(lines.at(tmpl.line - 1)).captured(1);
        const QString   row    = prefix + QStringLiteral("[%1](#%2)")
                                              .arg(target->text, target->slug);
        insertions[anchor ? anchor->line : toc.firstLine - 1] << row;
    }

    QStringList out;
    out.reserve(lines.size() + insertions.size());
    for (int i = 0; i < lines.size(); ++i) {
        const int lineNo = i + 1;
        if (!deletions.contains(lineNo)) out << lines.at(i);
        const auto it = insertions.constFind(lineNo);
        if (it != insertions.constEnd()) out << *it;
    }
    lines = out;
    return true;
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

    // The walk is over and nothing has been opened for writing. INV-21's seam
    // sits exactly here because this is the window the precondition is about.
    if (opts.afterWalkHook) opts.afterWalkHook();

    // ---- Phase 3: the fix path (spec § 2.3) ---------------------------------
    // Report-only unless asked (INV-6). Runs AFTER the sort, so fixed[] inherits
    // INV-7's total order from the array it is drawn from.
    if (!opts.fix) return r;

    // THE GATE IS THE FLAG, NEVER THE KIND. An implementation testing
    // kind == "toc_gap" passes every fixture here — toc_gap is the only fixable
    // kind today, so the two gates are behaviourally identical — and breaks the
    // moment a producer marks something else fixable. INV-5 carries a
    // source-scrape arm precisely because no fixture can separate them.
    QStringList                               fixOrder;
    QMap<QString, QList<DocFinding::Finding>> byFile;
    for (const DocFinding::Finding &f : std::as_const(r.findings)) {
        if (!f.autoFixable) continue;
        if (!byFile.contains(f.file)) fixOrder << f.file;
        byFile[f.file].append(f);
    }

    for (const QString &rel : std::as_const(fixOrder)) {
        const QList<DocFinding::Finding> gaps = byFile.value(rel);
        const QString abs = QDir::cleanPath(rootDir.filePath(rel));

        // Re-derive from the bytes as they are NOW, and re-read them to patch.
        // The whole walk finished before this loop began, so the user may have
        // saved in between; patching the text the walk read would apply an edit
        // derived from a document that no longer exists (INV-21).
        const QList<DocIntegrity::Finding> now =
            DocIntegrity::check(opts.rootCanonical, {rel}, opts.walk);

        QFile in(abs);
        if (!in.open(QIODevice::ReadOnly)) {
            // Same name skipped[] uses for an unreadable document: the same
            // event, one phase later.
            r.fixErrors.append({rel, QStringLiteral("read_failed")});
            continue;
        }
        const QString text = QString::fromUtf8(in.readAll());
        in.close();

        // Only a gap present in BOTH the walk and the re-derivation is
        // repaired. Matched on kind and message, not on line: a shifted line is
        // still the same gap, and § 2.3 requires it to repair.
        QSet<QString> current;
        for (const DocIntegrity::Finding &g : now)
            current.insert(integrityKind(g.kind) + QLatin1Char('\n') + g.message);
        bool stale = false;
        for (const DocFinding::Finding &g : gaps) {
            if (!current.contains(g.kind + QLatin1Char('\n') + g.message)) { stale = true; break; }
        }
        if (stale) {
            // The whole document, not the one gap: divergence means the file
            // moved under us, and nothing is written for it.
            r.fixErrors.append({rel, QStringLiteral("stale")});
            continue;
        }

        QStringList lines = text.split(QLatin1Char('\n'));
        QString     reason;
        if (!patchTocRegion(lines, gaps, &reason)) {
            r.fixErrors.append({rel, reason});
            continue;
        }

        // The project's full atomic idiom, the sequence cmdApplyEdits uses:
        // open, write, CHECK THE WRITE WAS NOT SHORT, commit, then fsync the
        // parent. fsyncParentDir runs after commit(), so a failure there cannot
        // leave a half-written file — the replacement already happened and only
        // its durability is at stake, which is why its return is not read.
        bool wrote = opts.dryRun;   // dry_run: same path, disk untouched
        if (!opts.dryRun) {
            QSaveFile sf(abs);
            if (sf.open(QIODevice::WriteOnly)) {
                const QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8();
                if (sf.write(bytes) == bytes.size() && sf.commit()) {
                    fsyncParentDir(abs);
                    wrote = true;
                }
            }
        }
        if (!wrote) {
            // QSaveFile leaves the original byte-identical on any of these.
            r.fixErrors.append({rel, QStringLiteral("write_failed")});
            continue;
        }

        // Appended on SUCCESS, never on intent — one document is one write
        // however many gaps it had, so these two numbers differ by design.
        r.fixed.append(gaps);
        ++r.filesWritten;
    }

    return r;
}

}  // namespace DocLint
