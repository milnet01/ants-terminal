// ANTS-3833 TU 7/14 — Docs and project layout verbs.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "codebaseindex.h"
#include "docsindex.h"
#include "speclint.h"            // ANTS-3662 — spec_lint verb
#include "specconformance.h"     // ANTS-4108 — spec_conformance verb
#include "pathvalidation.h"
#include "projectsettings.h"    // ANTS-2160 — .ants/project.json overrides
#include "falseposledger.h"
#include "resolvedroot.h"
#include "secureio.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QDirIterator>

using namespace rcdetail;  // ANTS-3833

// ANTS-2139 — docs_index: serve a pre-computed project documentation map.
// caller_cwd Required. A `doc_path` selector routes through PathValidation
// (bad_path on escape, incl. an in-root symlink resolving outside root). The
// disk cache + lazy refresh live in DocsIndex::serve.
QJsonDocument RemoteControl::cmdDocsIndex(const QJsonObject &req) {
    const QString callerRaw = req.value(QStringLiteral("caller_cwd")).toString();
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(callerRaw);
    const QString rootCanonical =
        !sentinelRoot.isEmpty() ? sentinelRoot
                                : resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o["ok"]    = false;
        o["error"] = QStringLiteral("docs_index: no focused project");
        o["code"]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }

    DocsIndex::QueryParams params;
    params.topic = req.value(QStringLiteral("topic")).toString();
    params.id    = req.value(QStringLiteral("id")).toString();

    const QString rawDocPath = req.value(QStringLiteral("doc_path")).toString();
    if (!rawDocPath.isEmpty()) {
        const auto check = PathValidation::validatePath(
            rawDocPath, rootCanonical,
            QStringLiteral("docs_index"), QStringLiteral("doc_path"));
        if (check.bad) return QJsonDocument(check.err);
        // Project-relative form for the index lookup (empty .resolved = a
        // not-yet-existing in-root path, still a valid soft-miss query).
        QString rel = check.resolved.isEmpty() ? rawDocPath : check.resolved;
        if (rel.startsWith(rootCanonical))
            rel = rel.mid(rootCanonical.size()).startsWith(QLatin1Char('/'))
                      ? rel.mid(rootCanonical.size() + 1)
                      : rel.mid(rootCanonical.size());
        params.docPath = rel;
    }

    return QJsonDocument(
        DocsIndex::serve(rootCanonical, QDateTime::currentMSecsSinceEpoch(),
                         params));
}

// ANTS-3737 — a content fingerprint of the doc set a run actually checked,
// folded into the envelope so the CENTRAL ETag becomes content-sensitive.
//
// The ETag is a sha256 of the response body (ClaudeIntegration::etagFor). For
// most verbs that is content-sensitive because the content IS the body —
// cmdFeedbackQuery says exactly that of its own delta. The three doc verbs are
// the exception: they report FINDINGS, not content. Edit three specs without
// changing a finding and the body is byte-identical, so `etag_match` answers
// 304 "unchanged" and the caller skips the re-check. That is the worst moment
// for a false 304 — the reason to re-run after a fix batch is to catch a NEW
// finding the fix introduced, and zero-findings is the COMMON state, so two
// different document states collide precisely where it matters. Fin Break
// feedback, 2026-07-30 (observed: same etag 77ce38c85a1b9ab4 across a
// substantive three-file edit, while spec_query saw the new mtime/size).
//
// (relpath, size, mtime_ms) per checked file. The run just read every one of
// them, so the stat is cheap. mtime granularity errs conservative: a touch
// with no content change busts the cache (one wasted re-run) rather than
// hiding a real change — the safe direction.
QString RemoteControl::docSetDigest(const QString &rootCanonical,
                                    const QStringList &checkedDocs) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (const QString &rel : checkedDocs) {
        const QFileInfo fi(QDir(rootCanonical).filePath(rel));
        h.addData(rel.toUtf8());
        h.addData(QByteArray::number(static_cast<qint64>(fi.size())));
        h.addData(QByteArray::number(fi.lastModified().toMSecsSinceEpoch()));
    }
    return QString::fromUtf8(h.result().toHex().left(16));
}

// ANTS-3601 — doc_integrity: deterministic dead-anchor / broken-link / TOC
// checks over a project-relative doc set. caller_cwd Required (the dispatcher
// enforces caller_cwd_required before this runs). Optional `path` (a single
// doc or a dir, default the docs_dir override else `docs/`) routes through
// PathValidation; an optional `kinds` filter narrows findings + counts; the
// ETag-304 short-circuit is applied centrally (isEtagSupportedTool). Findings
// come from DocIntegrity::check (docintegrity.cpp). See docs/specs/ANTS-3601.md.
QJsonDocument RemoteControl::cmdDocIntegrity(const QJsonObject &req) {
    // ANTS-3719 — `~global` / `~claude-config` routes to ~/.claude, the same
    // sentinel workspace_search and file_outline already take (ANTS-1390).
    // Without it this verb could not reach the only corpus the ungranted_tool
    // check serves: a skill tree is not an Ants project and has no root a
    // per-call path would validate against, which is exactly why ANTS-3719 was
    // filed CONSIDERED until ANTS-3713 settled the second-allowed-root posture.
    // Re-rooting rather than widening keeps INV-6/INV-10 intact — every path
    // is still validated, against ~/.claude instead of the project.
    const QString sentinelRoot = ants::expandGlobalConfigSentinel(
        req.value(QStringLiteral("caller_cwd")).toString());
    const QString rootCanonical =
        sentinelRoot.isEmpty() ? resolveRootCanonical(m_main, req) : sentinelRoot;
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("doc_integrity: no focused project");
        o[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    // path → relDocs (validate a supplied path first, so a root-escape refuses
    // bad_path before any enumeration — INV-10).
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (!rawPath.isEmpty()) {
        const auto check = PathValidation::validatePath(
            rawPath, rootCanonical, QStringLiteral("doc_integrity"),
            QStringLiteral("path"));
        if (check.bad) return QJsonDocument(check.err);  // root-escape → bad_path
    }
    // ANTS-4106 — `paths:[…]`, the multi-file form file_outline already has.
    // /apply-fixes step 5 says to pass the files a run edited, precisely to
    // avoid a whole-tree walk; a real pass edits a handful spread across the
    // tree (CLAUDE.md + CHANGELOG.md + ROADMAP.md at the root, two under
    // docs/) and no single `path` covers that set, so the full walk was what
    // happened — 28 documents checked when 5 were edited, and a pre-existing
    // finding in an untouched file read as one this pass caused. Wins over
    // `path` when both are sent (file_outline's rule). docs_digest and the
    // ETag need no change: they already key off the walked set, which is now
    // the union.
    QStringList rawPaths;
    const QJsonArray pathsArr = req.value(QStringLiteral("paths")).toArray();
    for (const QJsonValue &v : pathsArr) {
        const QString p = v.toString().trimmed();
        if (!p.isEmpty()) rawPaths << p;
    }
    for (const QString &p : std::as_const(rawPaths)) {
        const auto check = PathValidation::validatePath(
            p, rootCanonical, QStringLiteral("doc_integrity"),
            QStringLiteral("paths"));
        if (check.bad) return QJsonDocument(check.err);
    }

    const QString docsDir =
        ProjectSettings::load(rootCanonical).docsDir.value_or(QStringLiteral("docs"));
    QStringList relDocs =
        rawPaths.isEmpty()
            ? docIntegrityEnumerate(rootCanonical, rawPath, docsDir)
            : docIntegrityEnumerateMany(rootCanonical, rawPaths, docsDir);

    DocIntegrity::Options opts;
    if (relDocs.size() > opts.maxDocsPerRun)
        relDocs = relDocs.mid(0, opts.maxDocsPerRun);

    QStringList checked;
    const QList<DocIntegrity::Finding> findings =
        DocIntegrity::check(rootCanonical, relDocs, opts, &checked);

    QSet<QString> kindFilter;
    for (const auto &v : req.value(QStringLiteral("kinds")).toArray())
        kindFilter.insert(v.toString());

    // etag injected centrally (isEtagSupportedTool) — over THIS envelope, so
    // docs_digest is what makes it content-sensitive (ANTS-3737).
    QJsonObject out = docIntegrityBuildResponse(findings, kindFilter, checked);
    out[QStringLiteral("docs_digest")] = docSetDigest(rootCanonical, checked);
    return QJsonDocument(out);
}

// ANTS-3601 — pure: a validated `rawPath` → sorted project-relative doc set.
QStringList RemoteControl::docIntegrityEnumerate(const QString &rootCanonical,
                                                 const QString &rawPath,
                                                 const QString &docsDirDefault) {
    const QDir rootDir(rootCanonical);
    const auto collectMd = [&](const QString &dirAbs) {
        QStringList out;
        QDirIterator it(dirAbs, {QStringLiteral("*.md")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) out << rootDir.relativeFilePath(it.next());
        out.sort();
        return out;
    };
    if (rawPath.isEmpty())  // default → the docs_dir override else docs/
        return collectMd(QDir::cleanPath(rootDir.filePath(docsDirDefault)));
    const QString abs = QDir::cleanPath(rootDir.filePath(rawPath));
    const QFileInfo fi(abs);
    if (fi.isDir())  return collectMd(abs);           // dir → recursive *.md
    if (fi.isFile()) return {rootDir.relativeFilePath(abs)};  // file → one doc
    return {};  // non-existent in-root path → INV-15 (empty checked_docs)
}

// ANTS-4106 — pure: several validated paths → their de-duplicated, sorted
// union. Overlapping entries (a dir and a file inside it) collapse to one
// doc, so a caller listing everything a fix pass touched cannot double-count
// a document into two findings.
QStringList RemoteControl::docIntegrityEnumerateMany(
    const QString &rootCanonical, const QStringList &rawPaths,
    const QString &docsDirDefault) {
    QSet<QString> seen;
    QStringList out;
    for (const QString &p : rawPaths) {
        const QStringList part =
            docIntegrityEnumerate(rootCanonical, p, docsDirDefault);
        for (const QString &d : part)
            if (!seen.contains(d)) { seen.insert(d); out << d; }
    }
    out.sort();
    return out;
}

// ANTS-3601 — pure: DocIntegrity findings → the response object, filtered by
// `kinds` (empty = all). Both findings[] and counts{} narrow together (INV-18).
QJsonObject RemoteControl::docIntegrityBuildResponse(
    const QList<DocIntegrity::Finding> &findings, const QSet<QString> &kinds,
    const QStringList &checkedDocs) {
    // Kind wire strings (snake_case) — the CamelCase enum never reaches the wire.
    const auto kindStr = [](DocIntegrity::Kind k) -> QString {
        switch (k) {
        case DocIntegrity::Kind::DeadAnchor: return QStringLiteral("dead_anchor");
        case DocIntegrity::Kind::BrokenLink: return QStringLiteral("broken_link");
        case DocIntegrity::Kind::TocGap:     return QStringLiteral("toc_gap");
        case DocIntegrity::Kind::HeadingSequence:   // ANTS-3700
            return QStringLiteral("heading_sequence");
        case DocIntegrity::Kind::UngrantedTool:     // ANTS-3719
            return QStringLiteral("ungranted_tool");
        }
        return QString();
    };
    const auto wanted = [&](const QString &k) {
        return kinds.isEmpty() || kinds.contains(k);
    };

    QJsonArray findingsArr;
    int deadAnchor = 0, brokenLink = 0, tocGap = 0, headingSeq = 0, ungranted = 0;
    for (const DocIntegrity::Finding &f : findings) {
        const QString ks = kindStr(f.kind);
        if (!wanted(ks)) continue;
        QJsonObject fo;
        fo[QStringLiteral("kind")]    = ks;
        fo[QStringLiteral("file")]    = f.file;
        fo[QStringLiteral("line")]    = f.line;
        fo[QStringLiteral("message")] = f.message;
        findingsArr.append(fo);
        // ANTS-3700 — switch, not an if/else chain ending in `else ++tocGap`:
        // the old shape counted any future kind as a toc_gap, silently.
        switch (f.kind) {
        case DocIntegrity::Kind::DeadAnchor:      ++deadAnchor; break;
        case DocIntegrity::Kind::BrokenLink:      ++brokenLink; break;
        case DocIntegrity::Kind::TocGap:          ++tocGap;     break;
        case DocIntegrity::Kind::HeadingSequence: ++headingSeq; break;
        case DocIntegrity::Kind::UngrantedTool:   ++ungranted;  break;
        }
    }

    QJsonObject counts;
    if (wanted(QStringLiteral("dead_anchor"))) counts[QStringLiteral("dead_anchor")] = deadAnchor;
    if (wanted(QStringLiteral("broken_link"))) counts[QStringLiteral("broken_link")] = brokenLink;
    if (wanted(QStringLiteral("toc_gap")))     counts[QStringLiteral("toc_gap")]     = tocGap;
    if (wanted(QStringLiteral("heading_sequence")))
        counts[QStringLiteral("heading_sequence")] = headingSeq;
    if (wanted(QStringLiteral("ungranted_tool")))
        counts[QStringLiteral("ungranted_tool")] = ungranted;

    QJsonObject o;
    o[QStringLiteral("ok")]           = true;
    o[QStringLiteral("findings")]     = findingsArr;
    o[QStringLiteral("counts")]       = counts;
    o[QStringLiteral("checked_docs")] = QJsonArray::fromStringList(checkedDocs);
    return o;
}

// ANTS-3661 — the refusal-code half of DocSymbols' `excludedNames`. Parsed from
// the project's own `docs/standards/mcp-error-codes.md` (CLAUDE.md names it the
// canonical taxonomy) rather than hardcoded, so a project that adds a code does
// not acquire a permanent false candidate. A project without that standard
// contributes nothing here — which is correct: it has no such vocabulary.
//
// Backticked snake_case runs only. That shape is what a refusal code is, and
// widening it would start excluding real symbols from a file this verb has no
// business trusting that far.
static QSet<QString> docSymbolsRefusalCodes(const QString &rootCanonical) {
    QSet<QString> out;
    QFile f(QDir(rootCanonical).filePath(
        QStringLiteral("docs/standards/mcp-error-codes.md")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    static const QRegularExpression re(QStringLiteral("`([a-z][a-z0-9]*(?:_[a-z0-9]+)+)`"));
    const QString text = QString::fromUtf8(f.readAll());
    auto it = re.globalMatch(text);
    while (it.hasNext()) out.insert(it.next().captured(1));
    return out;
}

// ANTS-3661 — doc_symbols: resolve the identifiers a doc asserts something
// about, and report — never judge. caller_cwd Required. `path` routes through
// PathValidation and then the SAME enumeration doc_integrity uses. ETag-304 is
// applied centrally (isEtagSupportedTool). Engine: DocSymbols::scan. See
// docs/specs/ANTS-3661.md.
QJsonDocument RemoteControl::cmdDocSymbols(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("doc_symbols: no focused project");
        o[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (!rawPath.isEmpty()) {
        const auto check = PathValidation::validatePath(
            rawPath, rootCanonical, QStringLiteral("doc_symbols"),
            QStringLiteral("path"));
        if (check.bad) return QJsonDocument(check.err);  // root-escape → bad_path
    }
    const QString docsDir =
        ProjectSettings::load(rootCanonical).docsDir.value_or(QStringLiteral("docs"));
    QStringList relDocs = docIntegrityEnumerate(rootCanonical, rawPath, docsDir);

    const DocIntegrity::Options walk;  // shared caps: the two doc walks cost the same
    if (relDocs.size() > walk.maxDocsPerRun)
        relDocs = relDocs.mid(0, walk.maxDocsPerRun);

    DocSymbols::Options opts;
    opts.rootCanonical = rootCanonical;
    opts.excludedNames = docSymbolsRefusalCodes(rootCanonical);
    if (m_mcpVerbVocabularyProvider)
        for (const QString &n : m_mcpVerbVocabularyProvider()) opts.excludedNames.insert(n);

    QVector<DocSymbols::Symbol> symbols;
    QList<DocFinding::Finding> findings;
    QStringList checked;
    bool truncated = false;
    // maxSymbolsPerRun is a RUN budget, so it is decremented across documents:
    // the engine is per-document, and a per-document cap would let a 100-doc
    // sweep spend 100× the walks § 4 costs.
    int budget = opts.maxSymbolsPerRun;
    for (const QString &rel : std::as_const(relDocs)) {
        QFile f(QDir(rootCanonical).filePath(rel));
        if (f.size() > walk.maxDocBytes) continue;
        if (!f.open(QIODevice::ReadOnly)) continue;  // unreadable → INV-15, silently out
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        checked << rel;

        opts.maxSymbolsPerRun = budget;
        const DocSymbols::ScanResult r = DocSymbols::scan(text, rel, opts);
        budget -= r.needlesResolved;
        symbols += r.symbols;
        for (DocFinding::Finding fnd : r.findings) {
            fnd.emissionIndex = findings.size();  // run-wide, not per-document
            findings.push_back(fnd);
        }
        truncated = truncated || r.truncated;
    }
    // etag injected centrally (isEtagSupportedTool); docs_digest keeps it
    // content-sensitive (ANTS-3737 — same shape as doc_integrity).
    QJsonObject out =
        docSymbolsBuildResponse(symbols, findings, truncated, checked);
    out[QStringLiteral("docs_digest")] = docSetDigest(rootCanonical, checked);
    return QJsonDocument(out);
}

// ANTS-3661 — pure: engine output → the response object.
QJsonObject RemoteControl::docSymbolsBuildResponse(
    const QVector<DocSymbols::Symbol> &symbols,
    const QList<DocFinding::Finding> &findings, bool truncated,
    const QStringList &checkedDocs) {
    QJsonArray symbolsArr;
    int resolved = 0, unresolved = 0, notChecked = 0;
    for (const DocSymbols::Symbol &s : symbols) {
        QJsonObject so;
        so[QStringLiteral("symbol")]     = s.symbol;
        so[QStringLiteral("doc_line")]   = s.docLine;
        so[QStringLiteral("doc_col")]    = s.docCol;
        so[QStringLiteral("resolution")] = DocSymbols::resolutionStr(s.resolution);
        if (s.resolution == DocSymbols::Resolution::Resolved) {
            QJsonArray defs;
            for (const SymbolQuery::DefMatch &d : s.definitions) {
                QJsonObject dobj;
                dobj[QStringLiteral("file")]      = d.file;
                dobj[QStringLiteral("line")]      = d.line;
                dobj[QStringLiteral("signature")] = d.signature;
                dobj[QStringLiteral("lang")]      = d.lang;
                dobj[QStringLiteral("kind")]      = d.kind;
                defs.append(dobj);
            }
            so[QStringLiteral("definitions")] = defs;
            ++resolved;
        } else if (s.resolution == DocSymbols::Resolution::Unresolved) {
            ++unresolved;
        } else {
            ++notChecked;
        }
        symbolsArr.append(so);
    }

    QJsonObject counts;
    counts[QStringLiteral("total")]       = symbols.size();
    counts[QStringLiteral("resolved")]    = resolved;
    counts[QStringLiteral("unresolved")]  = unresolved;
    counts[QStringLiteral("not_checked")] = notChecked;
    // ANTS-4359 — split the unresolved bucket by the SHAPE the document wrote,
    // so a reader starts with the class where "no such symbol" is a real
    // defect rather than reading two dozen names one by one. On one standard
    // that bucket was 24 of 58 and every name looked benign, so the whole of
    // it was logged into a cold-review brief as settled noise — with one
    // genuine defect inside it, which survived two full loops.
    //
    // A count, never a filter: nothing is dropped and nothing is judged
    // (§ 2.3's report-never-judge rule). Each finding also carries its own
    // `shape`, so the list can be sorted rather than only summarised.
    {
        int callN = 0, qualN = 0, bareN = 0;
        for (const auto &f : std::as_const(findings)) {
            if (f.kind != QLatin1String("unresolved_symbol")) continue;
            const QString sh =
                f.extra.value(QStringLiteral("shape")).toString();
            if (sh == QLatin1String("call"))           ++callN;
            else if (sh == QLatin1String("qualified")) ++qualN;
            else                                       ++bareN;
        }
        QJsonObject byShape;
        byShape[QStringLiteral("call")]      = callN;
        byShape[QStringLiteral("qualified")] = qualN;
        byShape[QStringLiteral("bare")]      = bareN;
        counts[QStringLiteral("unresolved_by_shape")] = byShape;
    }

    QJsonObject o;
    o[QStringLiteral("ok")]           = true;
    o[QStringLiteral("symbols")]      = symbolsArr;
    o[QStringLiteral("counts")]       = counts;
    o[QStringLiteral("findings")]     = DocFinding::toJson(findings);
    o[QStringLiteral("truncated")]    = truncated;
    o[QStringLiteral("checked_docs")] = QJsonArray::fromStringList(checkedDocs);
    return o;
}

// ANTS-3662 — the required-section list, read from the project's OWN format
// standard and never assumed. `spec-format.md` wins over `specs.md` where both
// exist, matching /write-spec's resolution order.
//
// NO `_shared` FALLBACK. The canonical ~/.claude/skills/_shared/spec-format.md
// lives outside the project root, which this verb's own bad_path contract
// forbids it from reading — and because that file always exists, a _shared tier
// would mean the skip arm never fires (spec § 2.1). In-project or skipped.
// ANTS-4390 — the candidate list, in resolution order. The `standards/`
// entries are NOT a guess at a second convention: the global standards repo
// (~/.claude) has no `docs/standards/` because it IS the standards set, so
// every candidate missed and `sections_checked` came back false on the one
// repository that owns the canonical block. One extra stat makes it
// self-checkable.
//
// ANTS-4373 — `whichOut` reports the path that was actually consulted. A
// boolean says a check did not run; it does not say what to fix, so without
// this every caller re-derives the cause.
static const QStringList &specLintStandardCandidates() {
    static const QStringList v = {
        QStringLiteral("docs/standards/spec-format.md"),
        QStringLiteral("docs/standards/specs.md"),
        QStringLiteral("standards/spec-format.md"),
        QStringLiteral("standards/specs.md"),
    };
    return v;
}

// ANTS-4080 — the GLOBAL tier, and it is not the `_shared` fallback the two
// paragraphs above reject. That one resolved
// `~/.claude/skills/_shared/spec-format.md`, a file which always exists, so the
// skip arm could never fire and INV-1's second half was untestable. Three
// things are different here. The path is `~/.claude/standards/`, which became
// the authoritative home of the spec-format standard on 2026-08-08 with
// projects carrying deltas — so a project with no local copy is currently
// linted against NOTHING, which is the defect. It resolves through
// `expandGlobalConfigSentinel`, the same `~global` re-rooting ANTS-3719 gave
// doc_integrity: one well-known read-only root, every path still validated, no
// widening of `bad_path`. And it is testable BY CONSTRUCTION rather than by
// argument — `QDir::homePath()` follows `$HOME`, so a fixture pointing HOME at
// a directory with no `.claude/` still exercises the skip arm. `none` therefore
// stays a real outcome, which is the whole reason the old tier was refused.
//
// `whichOut` gets a `~global/` prefix rather than a `project|global|none`
// enum: ANTS-4373 already made this field the PATH consulted, which answers the
// enum's question and the next one too.
static QStringList specLintRequiredSections(const QString &rootCanonical,
                                            QString *whichOut = nullptr) {
    const auto tryRoot = [&](const QDir &root, const QString &prefix) {
        QStringList got;
        for (const QString &rel : specLintStandardCandidates()) {
            QFile f(root.filePath(rel));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            got = SpecLint::parseRequiredSections(QString::fromUtf8(f.readAll()));
            if (!got.isEmpty()) {
                if (whichOut) *whichOut = prefix + rel;
                return got;
            }
        }
        return QStringList();
    };
    const QStringList local = tryRoot(QDir(rootCanonical), QString());
    if (!local.isEmpty()) return local;

    const QString globalRoot =
        ants::expandGlobalConfigSentinel(QStringLiteral("~global"));
    if (globalRoot.isEmpty() || globalRoot == rootCanonical) return {};
    return tryRoot(QDir(globalRoot), QStringLiteral("~global/"));
}

// ANTS-4127 — the two filesystem facts the spec_lint engine cannot gather for
// itself: the `tests/features/<name>` directories that EXIST, and the subset
// holding a `test_*.cpp` named in `CMakeLists.txt`. Both keyed by the BARE
// `<name>`. Read ONCE per run and shared across every document in the walk, like
// specLintRequiredSections above — the cost is one directory scan plus one file
// read whatever the walk's size.
//
// An absent directory or an unreadable CMakeLists.txt returns an EMPTY set,
// which the engine reads as "skip this check". That is the whole failure
// posture: a check that cannot see the disk must decline, never condemn.
static QSet<QString> specLintExistingTestDirs(const QString &rootCanonical) {
    QDir features(QDir(rootCanonical).filePath(QStringLiteral("tests/features")));
    if (!features.exists()) return {};
    const QStringList names =
        features.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    return QSet<QString>(names.begin(), names.end());
}

// The wiring half. Deliberately a SUBSET of what it is handed rather than an
// independent scrape: a name CMakeLists.txt mentions but no directory backs is
// not a wired test, and letting it into the set would break the subset invariant
// the engine's header states.
static QSet<QString> specLintWiredTestDirs(const QString &rootCanonical,
                                           const QSet<QString> &existing) {
    if (existing.isEmpty()) return {};
    QFile f(QDir(rootCanonical).filePath(QStringLiteral("CMakeLists.txt")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString text = QString::fromUtf8(f.readAll());
    f.close();

    static const QRegularExpression re(
        QStringLiteral(R"(tests/features/([a-z0-9_]+)/test_[a-z0-9_]+\.cpp)"));
    QSet<QString> wired;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (existing.contains(name)) wired.insert(name);
    }
    return wired;
}

// ANTS-3662 — spec_lint: the deterministic half of /cold-eyes § 1e's spec
// pre-pass. caller_cwd Required. `path` routes through PathValidation and then
// the SAME enumeration doc_integrity uses, defaulted to `specs_dir` rather than
// `docs_dir`. ETag-304 is applied centrally (isEtagSupportedTool). Engine:
// SpecLint::check. See docs/specs/ANTS-3662.md.
QJsonDocument RemoteControl::cmdSpecLint(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("spec_lint: no focused project");
        o[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (!rawPath.isEmpty()) {
        const auto check = PathValidation::validatePath(
            rawPath, rootCanonical, QStringLiteral("spec_lint"),
            QStringLiteral("path"));
        if (check.bad) return QJsonDocument(check.err);  // root-escape → bad_path
    }
    const QString specsDir =
        ProjectSettings::load(rootCanonical).specsDir.value_or(
            QStringLiteral("docs/specs"));
    QStringList relDocs = docIntegrityEnumerate(rootCanonical, rawPath, specsDir);

    const DocIntegrity::Options walk;  // shared caps: the doc walks cost the same
    if (relDocs.size() > walk.maxDocsPerRun)
        relDocs = relDocs.mid(0, walk.maxDocsPerRun);

    // Read ONCE per run, not once per spec: it is the same file for every
    // document in the walk (spec § 4). Absent, or present with no marked block,
    // means the check is skipped — which is this verb's shipping default, since
    // no standard in this project carries the block yet.
    SpecLint::Options opts;
    QString sectionsSource;
    opts.requiredSections =
        specLintRequiredSections(rootCanonical, &sectionsSource);
    opts.maxFindings      = qBound(1, req.value(QStringLiteral("max_findings"))
                                          .toInt(500), 5000);
    // ANTS-4127 — same once-per-run rule, same reason (spec § 2.8).
    opts.existingTestDirs = specLintExistingTestDirs(rootCanonical);
    opts.wiredTestDirs    = specLintWiredTestDirs(rootCanonical,
                                                  opts.existingTestDirs);

    // ANTS-4110 — decide the project's INV NUMBERING SCHEME once per run, then
    // hand the engine the numbers its siblings own. The discriminator is
    // ownership, not contiguity: under per-document numbering nearly every spec
    // opens at INV-1, so numbers are shared; under project-global numbering each
    // number belongs to exactly one document. Requiring that NO number is shared
    // (across at least two specs that have any) is therefore the property that
    // makes a sibling lookup mean anything, and it fails toward the old
    // behaviour — a corpus that shares even one number keeps every gap finding.
    // Anchored on the specs dir alone, and independent of `path`, so linting one
    // file answers the same way linting the tree does.
    {
        QStringList corpus =
            docIntegrityEnumerate(rootCanonical, QString(), specsDir);
        if (corpus.size() > walk.maxDocsPerRun)
            corpus = corpus.mid(0, walk.maxDocsPerRun);
        QHash<int, int> ownersByNumber;
        int specsWithInvariants = 0;
        for (const QString &rel : std::as_const(corpus)) {
            if (!rel.startsWith(specsDir)) continue;
            QFile sf(QDir(rootCanonical).filePath(rel));
            if (sf.size() > walk.maxDocBytes) continue;
            if (!sf.open(QIODevice::ReadOnly)) continue;
            const QSet<int> ns =
                SpecLint::invariantNumbers(QString::fromUtf8(sf.readAll()));
            sf.close();
            if (ns.isEmpty()) continue;
            ++specsWithInvariants;
            for (const int n : ns) ownersByNumber[n]++;
        }
        bool projectGlobal = specsWithInvariants >= 2;
        for (auto it = ownersByNumber.constBegin();
             projectGlobal && it != ownersByNumber.constEnd(); ++it)
            if (it.value() > 1) projectGlobal = false;
        if (projectGlobal)
            opts.siblingInvNumbers =
                QSet<int>(ownersByNumber.keyBegin(), ownersByNumber.keyEnd());
    }

    QList<DocFinding::Finding> findings;
    QJsonObject lineCounts;
    QStringList checked;
    bool truncated = false, sectionsChecked = false;
    int idGapsSuppressed = 0;
    // ANTS-4127 — the walk's total is the SUM over documents (§ 2.3), so a
    // directory cited by three specs contributes three. `surfacesChecked` is a
    // property of the injected set rather than of any one document, so it is
    // seeded from the set and not accumulated: a walk that read no document
    // still reports honestly whether resolution was on.
    int  surfacesResolved = 0;
    bool surfacesChecked  = !opts.existingTestDirs.isEmpty();
    int budget = opts.maxFindings;
    for (const QString &rel : std::as_const(relDocs)) {
        QFile f(QDir(rootCanonical).filePath(rel));
        if (f.size() > walk.maxDocBytes) continue;
        if (!f.open(QIODevice::ReadOnly)) continue;  // unreadable → INV-15, silently out
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        checked << rel;

        // max_findings is a RUN cap, decremented across documents for the same
        // reason doc_symbols decrements its needle budget: the engine is
        // per-document, and a per-document cap bounds nothing run-wide.
        opts.maxFindings = qMax(1, budget);
        const SpecLint::Result r = SpecLint::check(text, rel, opts);
        lineCounts[rel] = r.lineCount;
        sectionsChecked = sectionsChecked || r.sectionsChecked;
        idGapsSuppressed += r.idGapsSuppressed;
        surfacesResolved += r.surfacesResolved;
        if (budget <= 0) {
            truncated = true;
            continue;
        }
        budget -= r.findings.size();
        for (DocFinding::Finding fnd : r.findings) {
            fnd.emissionIndex = findings.size();  // run-wide, not per-document
            findings.push_back(fnd);
        }
        truncated = truncated || r.truncated;
    }
    // etag injected centrally (isEtagSupportedTool); docs_digest keeps it
    // content-sensitive (ANTS-3737 — same shape as doc_integrity).
    QJsonObject out = specLintBuildResponse(findings, sectionsChecked,
                                            lineCounts, truncated, checked,
                                            surfacesResolved, surfacesChecked,
                                            sectionsSource);
    out[QStringLiteral("docs_digest")] = docSetDigest(rootCanonical, checked);
    // ANTS-4110 — say that gaps were suppressed and how many. Emitted only when
    // non-zero: a caller reading a short findings list is entitled to know a
    // check declined to fire, and a constant 0 on every other project is noise.
    if (idGapsSuppressed > 0)
        out[QStringLiteral("id_gaps_suppressed")] = idGapsSuppressed;
    return QJsonDocument(out);
}

// ANTS-3662 — pure: engine output → the response object.
QJsonObject RemoteControl::specLintBuildResponse(
    const QList<DocFinding::Finding> &findings, bool sectionsChecked,
    const QJsonObject &lineCounts, bool truncated,
    const QStringList &checkedDocs, int surfacesResolved,
    bool surfacesChecked, const QString &sectionsSource) {
    QJsonObject o;
    o[QStringLiteral("ok")]       = true;
    o[QStringLiteral("findings")] = DocFinding::toJson(findings);
    // Counts over the WHOLE list, before any cap or filter (ANTS-3664).
    o[QStringLiteral("counts")] =
        DocFinding::countsByVerbAndKind(findings)
            .value(QStringLiteral("spec_lint")).toObject();
    // Never omitted when false — see the header.
    o[QStringLiteral("sections_checked")] = sectionsChecked;
    // ANTS-4373 — the shape AROUND the skip, which is a separate defect from
    // the check not running. `ok:true` with an empty `findings[]` is the same
    // envelope a genuinely clean run produces, and nothing said which standard
    // was consulted, that a check was skipped, or what would make it run.
    //
    // It launders: /write-spec Step 4 reads this envelope, and
    // review-contract Phase 1d feeds the mechanical results to cold lanes as
    // SETTLED FACTS they are forbidden to question — so an unrun check
    // reported as clean becomes an unchallengeable false fact one layer
    // downstream. An ARRAY is read where a `false` is not, which is why the
    // skip is reported as one.
    o[QStringLiteral("sections_source")] =
        sectionsSource.isEmpty() ? QJsonValue(QJsonValue::Null)
                                 : QJsonValue(sectionsSource);
    // ANTS-4393 — `surfaces_checked` has a DIFFERENT cause from
    // `sections_checked`, measured on a project where adding the format
    // standard flipped the first to true and left the second false. So the
    // skip list must name EVERY gated check that did not run, not just the
    // one with a documented switch: there is a documented input for section
    // structure and NONE for test surfaces, which means `ok:true,
    // findings:[]` still says nothing about surfaces on every project.
    QJsonArray skipped;
    if (!surfacesChecked) skipped.append(QStringLiteral("invariant_no_test"));
    if (!sectionsChecked) skipped.append(QStringLiteral("missing_section"));
    if (!skipped.isEmpty()) o[QStringLiteral("skipped")] = skipped;
    if (!surfacesChecked) {
        o[QStringLiteral("surfaces_skipped_hint")] = QStringLiteral(
            "the test-surface check did NOT run, and unlike the "
            "required-section check there is NO documented input that turns "
            "it on — this is a limitation of the verb, not something the "
            "project can configure. Treat `findings:[]` as silent about test "
            "surfaces, not as a pass. Tracked as ANTS-4393.");
    }
    if (!sectionsChecked) {
        // Two causes, and the hint must name the right one. An empty walk
        // checked no document, so NO check ran and the format standard is
        // irrelevant — reporting "no block was found" there states a false
        // cause for a true skip, the class ANTS-4373 exists to close.
        o[QStringLiteral("skipped_hint")] =
            checkedDocs.isEmpty()
                ? QStringLiteral(
                      "no document was checked, so NO check ran: `path` matched "
                      "nothing under the specs dir. `checked_docs` is empty — "
                      "this says nothing about the project's format standard.")
                : QStringLiteral(
                      "the required-section check did NOT run: no `<!-- "
                      "required-sections -->` fenced block was found in any of "
                      "%1 (relative to the project root), nor in the same four "
                      "relative to ~/.claude/ (ANTS-4080's global tier). "
                      "Findings are silent about section structure — this is "
                      "not a clean structural result. Add the block to "
                      "whichever of those files is this project's spec-format "
                      "standard.")
                      .arg(specLintStandardCandidates().join(QStringLiteral(", ")));
    }
    // ANTS-4127 — the same contract, for the same reason: the count is the
    // denominator without which two zero-finding runs are indistinguishable, and
    // the flag is what stops a zero being read as "checked and clean" when it
    // means "nobody checked". Both always emitted; neither inferred from the
    // other.
    o[QStringLiteral("surfaces_resolved")] = surfacesResolved;
    o[QStringLiteral("surfaces_checked")]  = surfacesChecked;
    o[QStringLiteral("line_count")]       = lineCounts;
    o[QStringLiteral("checked_docs")]     = QJsonArray::fromStringList(checkedDocs);
    if (truncated) o[QStringLiteral("truncated")] = true;
    return o;
}

// ANTS-4108 — spec_conformance: RUN the patterns a spec prescribes against the
// expectation table beside them, instead of reading them. caller_cwd Required;
// `path` REQUIRED and validated under the project root — this verb reads ONE
// document, so unlike its spec_lint sibling there is no tree walk to default to.
// Engine: SpecConformance::run. See docs/specs/ANTS-4108-spec-conformance-verb.md.
QJsonDocument RemoteControl::cmdSpecConformance(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] =
            QStringLiteral("spec_conformance: no focused project");
        o[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (rawPath.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] =
            QStringLiteral("spec_conformance: path is required");
        o[QStringLiteral("code")]  = QStringLiteral("bad_args");
        return QJsonDocument(o);
    }
    const auto check = PathValidation::validatePath(
        rawPath, rootCanonical, QStringLiteral("spec_conformance"),
        QStringLiteral("path"));
    if (check.bad) return QJsonDocument(check.err);  // root-escape → bad_path
    // `resolved` is empty when the path does not canonicalise (absent file);
    // hand the joined form to the engine so IT reports not_found, rather than
    // minting a second absence code here.
    const QString abs = check.resolved.isEmpty()
        ? QDir(rootCanonical).filePath(rawPath)
        : check.resolved;

    SpecConformance::Options opt;
    // NOT clamped. An out-of-range max_cases REFUSES the call (engine INV-7):
    // a clamp here would make a partial run read as a complete one, which is
    // the failure mode the whole verb exists to catch.
    opt.maxCases = req.value(QStringLiteral("max_cases")).toInt(opt.maxCases);

    return QJsonDocument(specConformanceBuildResponse(
        SpecConformance::run(abs, opt),
        QDir(rootCanonical).relativeFilePath(abs),
        req.value(QStringLiteral("etag_match")).toString()));
}

// ANTS-4108 — pure: engine envelope → the verb's response.
//
// The ETag 304 is HANDLER-LOCAL, deliberately against mcp-tools.md step 7. The
// envelope carries one measured-microsecond `observations[]` row per case, so
// the dispatcher's etag — a hash of the whole response text — would differ on
// every run: the 304 could never fire. The engine hashes the envelope MINUS
// observations (spec INV-9) and this compares that. `spec_conformance` is
// therefore absent from isEtagSupportedTool, and a test asserts that absence.
//
// The etag covers the engine's ABSOLUTE path while the response carries the
// project-relative one. That makes it strictly more discriminating, never less:
// same root + same content → same etag, and a different root is a different
// file. The rewrite is here because the envelope crosses the wire, where an
// absolute path leaks the caller's home directory and differs per machine.
QJsonObject RemoteControl::specConformanceBuildResponse(
    const QJsonObject &engineOut, const QString &relPath,
    const QString &etagMatch) {
    // A refusal passes through untouched: it carries no etag, and reporting
    // "unchanged" for a call that never ran would hide it from the caller.
    if (!engineOut.value(QStringLiteral("ok")).toBool()) return engineOut;

    QJsonObject out = engineOut;
    out[QStringLiteral("path")] = relPath;

    const QString etag = out.value(QStringLiteral("etag")).toString();
    if (!etagMatch.isEmpty() && etagMatch == etag) {
        QJsonObject env;
        env[QStringLiteral("ok")]        = true;
        env[QStringLiteral("unchanged")] = true;
        env[QStringLiteral("etag")]      = etag;
        return env;
    }
    return out;
}

// ANTS-3660 — doc_dedup: the same passage written twice, across a doc set.
// caller_cwd Required. Walks `docs_dir` like doc_integrity (a duplicated fact
// is not confined to specs), reusing docIntegrityEnumerate for the walk.
//
// Structurally unlike its two siblings: the engine is CORPUS-scoped, so this
// accumulates every document and scores once at the end rather than building a
// per-document result. See docdedup.h.
QJsonDocument RemoteControl::cmdDocDedup(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    if (rootCanonical.isEmpty()) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("error")] = QStringLiteral("doc_dedup: no focused project");
        o[QStringLiteral("code")]  = QStringLiteral("bad_path");
        return QJsonDocument(o);
    }
    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (!rawPath.isEmpty()) {
        const auto check = PathValidation::validatePath(
            rawPath, rootCanonical, QStringLiteral("doc_dedup"),
            QStringLiteral("path"));
        if (check.bad) return QJsonDocument(check.err);  // root-escape → bad_path
    }
    const QString docsDir =
        ProjectSettings::load(rootCanonical).docsDir.value_or(QStringLiteral("docs"));
    QStringList relDocs = docIntegrityEnumerate(rootCanonical, rawPath, docsDir);

    const DocIntegrity::Options walk;  // shared caps: the doc walks cost the same
    if (relDocs.size() > walk.maxDocsPerRun)
        relDocs = relDocs.mid(0, walk.maxDocsPerRun);

    DocDedup::Options opts;
    opts.minSimilarity = qBound(0.0, req.value(QStringLiteral("min_similarity"))
                                         .toDouble(0.40), 1.0);
    opts.minWords      = qBound(1, req.value(QStringLiteral("min_words"))
                                       .toInt(15), 1000);
    opts.shingleSize   = qBound(2, req.value(QStringLiteral("shingle_size"))
                                       .toInt(3), 10);
    // Spec § 2.4: generated and templated artifacts. Measured, not chosen —
    // they supply more than half of this corpus's exact-duplicate pairs, which
    // is what those files ARE.
    opts.excludedPathGlobs = QStringList{
        QStringLiteral("*AUTOMATED_AUDIT_REPORT*"),
        QStringLiteral("*superpowers/*"),
    };

    DocDedup::Accumulator acc;
    QStringList checked;
    for (const QString &rel : std::as_const(relDocs)) {
        // Tested here rather than left to the engine so an excluded file is
        // never READ, and `checked_docs` lists what was actually compared.
        if (DocDedup::isExcludedPath(rel, opts)) continue;
        QFile f(QDir(rootCanonical).filePath(rel));
        if (f.size() > walk.maxDocBytes) continue;
        if (!f.open(QIODevice::ReadOnly)) continue;  // unreadable → INV-15, silently out
        const QString text = QString::fromUtf8(f.readAll());
        f.close();
        checked << rel;
        acc.add(text, rel, opts);
    }
    // etag injected centrally (isEtagSupportedTool).
    return QJsonDocument(docDedupBuildResponse(acc.finish(), checked));
}

QJsonObject RemoteControl::docDedupBuildResponse(const DocDedup::Result &result,
                                                 const QStringList &checkedDocs) {
    const auto passageObj = [](const DocDedup::Passage &p) {
        QJsonObject o;
        o[QStringLiteral("file")] = p.file;
        o[QStringLiteral("line")] = p.line;
        return o;
    };
    // 3 dp on the wire: the score is a triage signal, and full double precision
    // makes an otherwise-identical response differ in its last bits.
    const auto sim = [](double v) { return qRound(v * 1000.0) / 1000.0; };

    QJsonObject o;
    o[QStringLiteral("ok")]       = true;
    o[QStringLiteral("findings")] = DocFinding::toJson(result.findings);
    // Counts over the WHOLE list, before any cap or filter (ANTS-3664).
    o[QStringLiteral("counts")] =
        DocFinding::countsByVerbAndKind(result.findings)
            .value(QStringLiteral("doc_dedup")).toObject();

    QJsonArray pairs;
    for (const DocDedup::Pair &p : result.pairs) {
        QJsonObject e;
        e[QStringLiteral("a")]          = passageObj(p.a);
        e[QStringLiteral("b")]          = passageObj(p.b);
        e[QStringLiteral("similarity")] = sim(p.similarity);
        pairs.append(e);
    }
    o[QStringLiteral("pairs")] = pairs;

    QJsonArray clusters;
    for (const DocDedup::Cluster &c : result.clusters) {
        QJsonArray ps;
        for (const DocDedup::Passage &p : c.passages) ps.append(passageObj(p));
        QJsonObject e;
        e[QStringLiteral("passages")]       = ps;
        e[QStringLiteral("size")]           = ps.size();
        e[QStringLiteral("max_similarity")] = sim(c.maxSimilarity);
        clusters.append(e);
    }
    o[QStringLiteral("clusters")] = clusters;

    o[QStringLiteral("passages_total")]    = result.passagesTotal;
    o[QStringLiteral("passages_compared")] = result.passagesCompared;
    o[QStringLiteral("checked_docs")] = QJsonArray::fromStringList(checkedDocs);
    if (result.truncated) o[QStringLiteral("truncated")] = true;
    return o;
}

// ANTS-3636 — doc_citations: resolve a doc's `path:line` citations against the
// files and return the cited line TEXT. caller_cwd Required (the dispatcher
// enforces caller_cwd_required before this runs). `path` names a single FILE —
// unlike doc_integrity, which accepts a directory and walks it, because this
// verb's payload is per-citation text and the whole docs/ corpus would bury it;
// the sweep shape is ANTS-3643. Engine: DocCitations::check. See
// docs/specs/ANTS-3636.md.
QJsonDocument RemoteControl::cmdDocCitations(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    const QJsonObject refusal = docCitationsValidate(rootCanonical, req);
    if (!refusal.isEmpty()) return QJsonDocument(refusal);

    DocCitations::Options opts = docCitationsClampOptions(req);

    // The basename map, in the five steps § 2.7 spells out. Step 4 is the one an
    // implementer omits: CodebaseIndex::fromJson copies `version` and
    // `root_canonical` into the struct and parses `files` unconditionally — it
    // validates neither. The reject-on-mismatch lives one level up in
    // CodebaseIndex::serve, which this deliberately bypasses because serve may
    // BUILD the index and a read verb must not spend multiple seconds doing so.
    // Skipping the check silently mines a stale-schema or wrong-project cache
    // and reports every health field green.
    QFile cacheFile(CodebaseIndex::cachePathFor(rootCanonical));
    if (cacheFile.open(QIODevice::ReadOnly)) {
        QJsonParseError perr{};
        const QJsonDocument jd = QJsonDocument::fromJson(cacheFile.readAll(), &perr);
        if (perr.error == QJsonParseError::NoError && jd.isObject()) {
            const CodebaseIndex::Index idx = CodebaseIndex::fromJson(jd.object());
            if (idx.version == CodebaseIndex::kIndexVersion
                && idx.rootCanonical == rootCanonical) {
                for (const CodebaseIndex::FileEntry &fe : idx.files)
                    opts.basenameIndex[fe.path.section(QLatin1Char('/'), -1)].append(fe.path);
                // A truncated index looks large and healthy from the map alone,
                // so the flag has to travel separately (INV-37).
                opts.basenameIndexTruncated = idx.filesTruncated;
            }
        }
    }

    const QString rawPath = req.value(QStringLiteral("path")).toString();
    const QString abs = QDir::cleanPath(QDir(rootCanonical).filePath(rawPath));
    QJsonObject out = DocCitations::check(rootCanonical, abs, opts);
    // The engine reports the doc project-relative; the caller gets its own
    // string back, which is what makes a stored page self-describing.
    if (out.value(QStringLiteral("ok")).toBool())
        out[QStringLiteral("path")] = rawPath;
    // etag injected centrally (isEtagSupportedTool).
    return QJsonDocument(out);
}

// ANTS-3636 — pure: the five refusals, in the order § 2.1's table gives them.
// Everything else coerces (docCitationsClampOptions), which is what keeps this
// set at exactly five and errs toward the unfiltered view — the safe direction
// for a verb whose failure mode is under-reporting drift.
QJsonObject RemoteControl::docCitationsValidate(const QString &rootCanonical,
                                                const QJsonObject &req) {
    const auto refuse = [](const QString &code, const QString &msg) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("code"), code},
                           {QStringLiteral("error"), QStringLiteral("doc_citations: ") + msg}};
    };
    if (rootCanonical.isEmpty())
        return refuse(QStringLiteral("bad_path"), QStringLiteral("no focused project"));

    const QString rawPath = req.value(QStringLiteral("path")).toString();
    if (rawPath.isEmpty())
        return refuse(QStringLiteral("missing_field"),
                      QStringLiteral("path is required and names a single file"));

    const auto checked = PathValidation::validatePath(
        rawPath, rootCanonical, QStringLiteral("doc_citations"), QStringLiteral("path"));
    if (checked.bad) return checked.err;   // root-escaping → bad_path

    const QFileInfo fi(QDir::cleanPath(QDir(rootCanonical).filePath(rawPath)));
    if (fi.isDir())
        return refuse(QStringLiteral("bad_args"),
                      QStringLiteral("path names a directory; this verb reads one file at a "
                                     "time (a corpus sweep is ANTS-3643)"));
    // A non-existent in-root path is NOT a refusal: it answers ok:true with an
    // empty result, so a typo'd path gets the same answer doc_integrity gives.
    if (!fi.exists()) return {};
    if (!fi.isReadable() || fi.size() > DocCitations::Options{}.maxDocBytes)
        return refuse(QStringLiteral("read_failed"),
                      QStringLiteral("doc is unreadable or too large to hold"));
    return {};
}

// ANTS-3636 — pure: request args → Options. Out-of-domain values coerce rather
// than refuse, and a wrong-typed numeric falls back to its DEFAULT rather than
// to a clamp endpoint — a caller who sends a string got the argument wrong, and
// the default is the answer they would have had by omitting it.
DocCitations::Options RemoteControl::docCitationsClampOptions(const QJsonObject &req) {
    DocCitations::Options o;
    const auto clamped = [&req](const char *key, int def, int lo, int hi) {
        const QJsonValue v = req.value(QLatin1String(key));
        if (!v.isDouble()) return def;
        return qBound(lo, v.toInt(def), hi);
    };
    o.maxRangeLines = clamped("max_range_lines", o.maxRangeLines, 1, 20);
    o.maxDocLines   = clamped("max_doc_lines", o.maxDocLines, 1000, 50000);
    o.maxBytes      = clamped("max_bytes", o.maxBytes, 64 * 1024, 4 * 1024 * 1024);
    // No upper clamp on offset: that bound is `count`, which the handler has not
    // computed. Paging past the end is an empty page, not an error (INV-38).
    const QJsonValue off = req.value(QStringLiteral("offset"));
    o.offset = off.isDouble() ? qMax(0, off.toInt(0)) : 0;
    o.only   = req.value(QStringLiteral("only")).toString() == QLatin1String("stale")
                   ? DocCitations::Only::Stale
                   : DocCitations::Only::All;
    // ANTS-4386 — opt-in quotation check. Off by default so no existing
    // caller's envelope moves and nobody pays for a pass they did not ask
    // for; the floor keeps it off every quoted word in the corpus.
    o.quotes        = req.value(QStringLiteral("quotes")).toBool(false);
    o.quoteMinChars = clamped("quote_min_chars", o.quoteMinChars, 10, 500);
    return o;
}

// ANTS-2161 — project_settings: detect a misplaced layout + create/update
// <root>/.ants/project.json. Ops detect (read-only preview) / init (write
// the detected or explicit block, refuse settings_exists, no clobber) / set
// (create-or-update; raw-JSON merge preserving unknown keys). caller_cwd
// Required. The file is written world-readable (0644) by design — it holds
// only non-secret path declarations (contrast the 0600 global config). See
// docs/specs/ANTS-2161.md.
QJsonDocument RemoteControl::cmdProjectSettings(const QJsonObject &req) {
    const QString rootCanonical = resolveRootCanonical(m_main, req);
    const auto err = [](const QString &code, const QString &msg) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("code")]  = code;
        o[QStringLiteral("error")] = msg;
        return QJsonDocument(o);
    };
    if (rootCanonical.isEmpty())
        return err(QStringLiteral("bad_path"),
                   QStringLiteral("project_settings: no focused project"));

    const QString op = req.value(QStringLiteral("op")).toString();
    if (op != QLatin1String("detect") && op != QLatin1String("init")
        && op != QLatin1String("set"))
        return err(QStringLiteral("bad_args"),
                   QStringLiteral("project_settings: op must be detect|init|set"));

    const QString settingsPath =
        rootCanonical + QStringLiteral("/.ants/project.json");

    // ANTS-2227 — dry_run preview for the write ops (init/set). `detect` is
    // already read-only, so the flag is a no-op there.
    const bool dryRun = req.value(QStringLiteral("dry_run")).toBool();

    // The six recognised layout keys. Declared here (ahead of op:detect)
    // because detect's ANTS-3705 `declared` echo walks the same set that
    // op:set turns into a `changes` object below.
    static const QStringList kKeys = {
        QStringLiteral("source_roots"), QStringLiteral("test_roots"),
        QStringLiteral("docs_dir"), QStringLiteral("specs_dir"),
        QStringLiteral("roadmap"), QStringLiteral("changelog")};

    // ---- detect (read-only) ----
    if (op == QLatin1String("detect")) {
        const ProjectSettings::Suggestion sug =
            ProjectSettings::detect(rootCanonical);
        QJsonObject s;
        if (sug.sourceRoots)
            s[QStringLiteral("source_roots")] =
                QJsonArray::fromStringList(*sug.sourceRoots);
        s[QStringLiteral("reason")]               = sug.reason;
        s[QStringLiteral("default_source_count")] = sug.defaultSourceCount;
        s[QStringLiteral("total_source_count")]   = sug.totalSourceCount;
        // ANTS-3369 — echo what's already in effect / what was discounted,
        // emitted only when non-empty (1:1 snake_case mapping like above).
        if (sug.wouldUseRoots)
            s[QStringLiteral("would_use_roots")] =
                QJsonArray::fromStringList(*sug.wouldUseRoots);
        if (!sug.excluded.isEmpty())
            s[QStringLiteral("excluded")] =
                QJsonArray::fromStringList(sug.excluded);
        // ANTS-3588 (INV-19) — conventional aux layout keys, emitted only when
        // detect proposed them (i.e. alongside a source_roots suggestion).
        if (sug.testRoots)
            s[QStringLiteral("test_roots")] =
                QJsonArray::fromStringList(*sug.testRoots);
        if (sug.docsDir)   s[QStringLiteral("docs_dir")]  = *sug.docsDir;
        if (sug.specsDir)  s[QStringLiteral("specs_dir")] = *sug.specsDir;
        if (sug.roadmap)   s[QStringLiteral("roadmap")]   = *sug.roadmap;
        if (sug.changelog) s[QStringLiteral("changelog")] = *sug.changelog;
        // ANTS-3705 — echo the CURRENT declaration alongside the suggestion, so
        // "which of the six keys are declared, and are they still correct?" is
        // one call instead of a native Read of .ants/project.json. Reads the
        // stored file rather than ProjectSettings::load(), because load()
        // silently DROPS an entry whose path no longer resolves — which is
        // precisely the state worth surfacing: to every consumer a dropped key
        // is indistinguishable from an absent one. Each such entry is named in
        // `declared_missing`.
        QJsonObject declared;
        QJsonArray  declaredMissing;
        QFile sf(settingsPath);
        if (sf.open(QIODevice::ReadOnly)) {
            const QJsonObject stored =
                QJsonDocument::fromJson(sf.readAll()).object();
            sf.close();
            for (const QString &k : kKeys) {
                if (!stored.contains(k)) continue;
                const QJsonValue v = stored.value(k);
                declared[k] = v;
                QStringList vals;
                if (v.isString()) {
                    vals << v.toString();
                } else if (v.isArray()) {
                    const QJsonArray arr = v.toArray();
                    for (const QJsonValue &e : arr)
                        if (e.isString()) vals << e.toString();
                }
                for (int vi = 0; vi < vals.size(); ++vi) {
                    const QString rel = vals.at(vi).trimmed();
                    const QString abs =
                        rootCanonical + QLatin1Char('/') + rel;
                    if (rel.isEmpty()
                        || !PathValidation::isInsideProject(rootCanonical, abs))
                        declaredMissing.append(
                            QStringLiteral("%1: %2").arg(k, vals.at(vi)));
                }
            }
        }
        // ANTS-4093 — name the keys that are NOT declared, always. `declared`
        // alone answers "what is set"; the question a caller actually acts on
        // is "what is still unset", and reading a reply that says only
        // "no override needed" as "nothing to configure" is how a project
        // stays unconfigured. Derived from the same kKeys set, so the two
        // fields can't disagree about what the recognised keys are.
        QJsonArray undeclared;
        for (const QString &k : kKeys)
            if (!declared.contains(k)) undeclared.append(k);

        QJsonObject o;
        o[QStringLiteral("ok")]         = true;
        o[QStringLiteral("present")]    = sug.present;
        o[QStringLiteral("suggestion")] = s;
        if (!declared.isEmpty())
            o[QStringLiteral("declared")] = declared;
        if (!undeclared.isEmpty())
            o[QStringLiteral("undeclared")] = undeclared;
        if (!declaredMissing.isEmpty())
            o[QStringLiteral("declared_missing")] = declaredMissing;
        return QJsonDocument(o);
    }

    // Recognised flat keys → a `changes` object (a present JSON-null is
    // retained so set can clear a key — INV-8).
    QJsonObject changes;
    for (const QString &k : kKeys)
        if (req.contains(k)) changes[k] = req.value(k);

    // Shared atomic writer: world-readable 0644 (NOT setOwnerOnlyPerms).
    // ANTS-2227 — under dry_run, return the would-be settings + landing path
    // without creating .ants/ or writing the file (shares the merged `obj` the
    // real write would persist, so the preview can't drift).
    const auto writeOut = [&](const QJsonObject &obj) -> QJsonDocument {
        if (dryRun) {
            QJsonObject o;
            o[QStringLiteral("ok")]          = true;
            o[QStringLiteral("dry_run")]     = true;
            o[QStringLiteral("written")]     = false;
            o[QStringLiteral("would_write")] = true;
            o[QStringLiteral("path")]        = settingsPath;
            o[QStringLiteral("settings")]    = obj;
            return QJsonDocument(o);
        }
        QDir().mkpath(rootCanonical + QStringLiteral("/.ants"));
        QSaveFile sf(settingsPath);
        if (!sf.open(QIODevice::WriteOnly))
            return err(QStringLiteral("write_failed"),
                       QStringLiteral("project_settings: cannot open %1")
                           .arg(settingsPath));
        sf.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        if (!sf.commit())
            return err(QStringLiteral("write_failed"),
                       QStringLiteral("project_settings: commit failed for %1")
                           .arg(settingsPath));
        QFile::setPermissions(settingsPath,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ReadGroup
                                  | QFileDevice::ReadOther);
        fsyncParentDir(settingsPath);
        QJsonObject o;
        o[QStringLiteral("ok")]       = true;
        o[QStringLiteral("written")]  = true;
        o[QStringLiteral("path")]     = settingsPath;
        o[QStringLiteral("settings")] = obj;
        return QJsonDocument(o);
    };

    // ---- init (create only, no clobber) ----
    if (op == QLatin1String("init")) {
        if (QFileInfo::exists(settingsPath)) {
            QJsonObject o;
            o[QStringLiteral("ok")]    = false;
            o[QStringLiteral("code")]  = QStringLiteral("settings_exists");
            o[QStringLiteral("error")] = QStringLiteral(
                "project_settings: .ants/project.json already exists (use op:set to update)");
            o[QStringLiteral("path")]  = settingsPath;
            return QJsonDocument(o);
        }
        QJsonObject toWrite;
        if (!changes.isEmpty()) {                 // explicit keys suppress the detector
            QString ec, ek, ev;
            const auto merged = ProjectSettings::applyWrite(
                QJsonObject{}, changes, rootCanonical, &ec, &ek, &ev);
            if (!merged)
                return err(ec, QStringLiteral("project_settings: invalid %1=%2")
                                   .arg(ek, ev));
            toWrite = *merged;
        } else {
            const ProjectSettings::Suggestion sug =
                ProjectSettings::detect(rootCanonical);
            QJsonObject detected;
            if (sug.sourceRoots)
                detected[QStringLiteral("source_roots")] =
                    QJsonArray::fromStringList(*sug.sourceRoots);
            // ANTS-3588 (INV-19) — the conventional aux layout keys ride along a
            // source_roots suggestion, so one init writes the whole block.
            if (sug.testRoots)
                detected[QStringLiteral("test_roots")] =
                    QJsonArray::fromStringList(*sug.testRoots);
            if (sug.docsDir)   detected[QStringLiteral("docs_dir")]  = *sug.docsDir;
            if (sug.specsDir)  detected[QStringLiteral("specs_dir")] = *sug.specsDir;
            if (sug.roadmap)   detected[QStringLiteral("roadmap")]   = *sug.roadmap;
            if (sug.changelog) detected[QStringLiteral("changelog")] = *sug.changelog;
            if (detected.isEmpty()) {             // nothing to do is not an error
                QJsonObject o;
                o[QStringLiteral("ok")]       = true;
                o[QStringLiteral("written")]  = false;
                o[QStringLiteral("reason")]   = sug.reason.isEmpty()
                    ? QStringLiteral("layout already standard; nothing to write")
                    : sug.reason;
                return QJsonDocument(o);
            }
            // ANTS-3588 — validate the detector-derived block exactly as explicit
            // keys (§2.3): each path was existence-checked in detect, so this is a
            // formality unless a path vanished (TOCTOU) → bad_path.
            QString ec, ek, ev;
            const auto merged = ProjectSettings::applyWrite(
                QJsonObject{}, detected, rootCanonical, &ec, &ek, &ev);
            if (!merged)
                return err(ec, QStringLiteral("project_settings: invalid %1=%2")
                                   .arg(ek, ev));
            toWrite = *merged;
        }
        return writeOut(toWrite);
    }

    // ---- set (create-or-update; merge preserves unknown keys) ----
    QJsonObject existing;
    if (QFileInfo::exists(settingsPath)) {
        QFile f(settingsPath);
        if (!f.open(QIODevice::ReadOnly))
            return err(QStringLiteral("read_failed"),
                       QStringLiteral("project_settings: cannot read %1")
                           .arg(settingsPath));
        const QByteArray raw = f.readAll();
        f.close();
        QJsonParseError pe{};
        const QJsonDocument d = QJsonDocument::fromJson(raw, &pe);
        if (pe.error != QJsonParseError::NoError || !d.isObject()) {
            QJsonObject o;
            o[QStringLiteral("ok")]              = false;
            o[QStringLiteral("code")]            = QStringLiteral("unrecognised_format");
            o[QStringLiteral("expected_format")] =
                QJsonArray{QStringLiteral("json-object")};
            o[QStringLiteral("hint")] = QStringLiteral(
                "existing .ants/project.json is not a JSON object — hand-edit or delete it");
            o[QStringLiteral("error")] = QStringLiteral(
                "project_settings: existing .ants/project.json is not a JSON object");
            return QJsonDocument(o);
        }
        existing = d.object();
    }
    if (changes.isEmpty())
        return err(QStringLiteral("bad_args"), QStringLiteral(
            "project_settings set: supply at least one of "
            "source_roots/test_roots/docs_dir/specs_dir/roadmap/changelog"));
    QString ec, ek, ev;
    const auto merged = ProjectSettings::applyWrite(
        existing, changes, rootCanonical, &ec, &ek, &ev);
    if (!merged)
        return err(ec,
                   QStringLiteral("project_settings: invalid %1=%2").arg(ek, ev));
    return writeOut(*merged);
}

