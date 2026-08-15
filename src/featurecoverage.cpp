// Implementation: see featurecoverage.h for the contract.

#include "featurecoverage.h"

#include "auditengine.h"
#include "markdownscan.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <functional>

namespace FeatureCoverage {

namespace {

// Tokens that routinely appear inside backticks in spec prose but would
// never usefully "drift" — either language keywords/types (exist in
// every C++ file), trivial literals, or generic meta-words. Keeping the
// list tight: the goal is to avoid *false positives*, not to classify.
//
// File-local accessor preserved as `stopwords()`; the public accessor
// `FeatureCoverage::specStopwords()` (defined at end of file) wraps it
// so downstream callers (DebtSweepEngine) consume one canonical set.
const QSet<QString> &stopwords() {
    static const QSet<QString> kSpecStopwords = {
        "true",    "false",   "null",     "nullptr",  "NULL",
        "TRUE",    "FALSE",
        "void",    "bool",    "char",     "auto",     "size_t",
        "std",     "const",   "static",   "virtual",  "override",
        "public",  "private", "protected","signals",  "slots",
        "return",  "class",   "struct",   "enum",     "using",
        "inline",  "explicit","typename", "template", "namespace",
        "QString", "QList",   "QByteArray","QObject", "Q_OBJECT",
        "QStringList", "QSet", "QHash", "QMap",
    };
    return kSpecStopwords;
}

// English prose stopwords — short-function words that would otherwise
// inflate the bullet↔title match heuristic. Confined to ≥4 chars since
// shorter words are already filtered out by length.
const QSet<QString> &englishStopwords() {
    static const QSet<QString> kStop = {
        "with", "from", "that", "this", "when", "then",  "where",
        "what", "have", "been", "into", "some", "such",  "also",
        "only", "onto", "does", "will", "more", "than",  "each",
        "same", "most", "just", "upon", "make", "made",  "over",
        "here", "they", "them", "theirs","these","those","there",
        "feature","features","fixes","fixed","adds","added",
        "change", "changed","changes","remove","removed","removes",
        "support","properly","instead","allow","allows","allowed",
        "about", "after","before","because","under","while","which",
        "would","could","should","might","must","shall",
    };
    return kStop;
}

} // anonymous

// ---------------------------------------------------------------------------
// Lane 1
// ---------------------------------------------------------------------------

// Shape filters for extractSpecTokens. These catch token classes that
// routinely appear in specs but aren't drift candidates:
//
//   • Qt/Q-prefixed scoped API names (`QStyle::SH_Widget_Animation_Duration`,
//     `Qt::WA_ShowModal`, `Qt6::DBus`) — external API references in prose,
//     not project-internal symbols. These always exist (in the Qt headers)
//     but wouldn't be in our src/ as plain text.
//
//   • Lint/rule codes (`S101`, `F401`, `B007`) — a capital letter plus
//     digits is the shape of ruff / flake8 / bandit rule IDs. They're
//     *discussed* in specs, not defined in src/.
//
//   • Placeholder tokens (`X.Y.Z`) — all-caps single-letter dotted
//     templates that sphinx-style specs use for version placeholders.
static bool isPseudoToken(const QString &tok) {
    // Qt API: Q-prefixed followed by any CamelCase, then "::"
    static const QRegularExpression qtApi(R"(^(Qt\d*|Q[A-Z][A-Za-z_0-9]*)::)");
    if (qtApi.match(tok).hasMatch()) return true;
    // Lint code: single uppercase letter + 2-4 digits, exact.
    static const QRegularExpression lintCode(R"(^[A-Z]\d{2,4}$)");
    if (lintCode.match(tok).hasMatch()) return true;
    // Version placeholder like `X.Y.Z` or `A.B` — single capital + dot + …
    static const QRegularExpression versionPlaceholder(R"(^[A-Z](\.[A-Z])+$)");
    if (versionPlaceholder.match(tok).hasMatch()) return true;
    return false;
}

QList<SpecToken> extractSpecTokens(const QString &specText) {
    // Identifier-shaped token between backticks. First char alpha/underscore;
    // remainder alphanumeric/underscore/colon/hyphen/dot — covers CamelCase,
    // snake_case, scoped::names, dotted.ids, and kebab-case command names.
    // Minimum length 4 (one leading alpha + {3,} more) — short tokens are
    // noise-prone (file extensions, single-char operators).
    static const QRegularExpression tokenRe(
        R"(`([A-Za-z_][A-Za-z0-9_:\-\.]{3,})`)");

    const QStringList lines = specText.split('\n');
    QList<SpecToken> out;
    QSet<QString> seen;
    const QSet<QString> &stop = stopwords();
    for (int i = 0; i < lines.size(); ++i) {
        QRegularExpressionMatchIterator it = tokenRe.globalMatch(lines[i]);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString tok = m.captured(1);
            if (stop.contains(tok)) continue;
            if (isPseudoToken(tok)) continue;
            // Pure numeric tail → noise (e.g. `1024`). The regex already
            // requires alpha-leading, but a token like `a123` could slip
            // through if alpha-then-all-digits; we keep those — they're
            // legitimate identifiers.
            if (seen.contains(tok)) continue;
            seen.insert(tok);
            out.append({tok, i + 1});
        }
    }
    return out;
}

QList<SpecToken> findDriftTokens(
    const QString &specText,
    const std::function<bool(const QString &)> &existsInSource) {
    const QList<SpecToken> candidates = extractSpecTokens(specText);
    QList<SpecToken> drift;
    for (const SpecToken &t : candidates) {
        if (!existsInSource(t.token))
            drift.append(t);
    }
    return drift;
}

QList<SpecToken> extractDocLiteralTokens(const QString &docText) {
    // Like extractSpecTokens but path-widened (`/` added to the body charset,
    // so slash-paths like `archived/unknown` are captured) and fence-aware.
    // See docs/specs/ANTS-3600.md § 2.4.
    static const QRegularExpression tokenRe(
        R"(`([A-Za-z_][A-Za-z0-9_:\-\./]{3,})`)");
    // URL guard — the widened charset admits `:` and `/`, so a back-ticked URL
    // (`https://example.com/x`) would become a token that never matches source
    // and floods the lane. Skip any token with a scheme prefix, matched
    // case-insensitively (so `HTTPS://…` is also skipped).
    static const QRegularExpression urlScheme(
        R"(^[a-z][a-z0-9+.-]*://)", QRegularExpression::CaseInsensitiveOption);
    // ANTS-3849 — citation guard. A `<head>:<literal>` token names a LOCATION
    // or a FIELD'S VALUE, not a literal any source could contain:
    // `remotecontrol.cpp:2540`, `applyTheme:3118`, `sections_checked:false`.
    // Source spells the head and the literal as separate tokens, so the joined
    // form can never resolve and is false by construction. Citation staleness
    // has its own owner — `doc_citations` (ANTS-3636) resolves the anchor;
    // this lane checks literals.
    //
    // Shipped 2026-08-13 for a path-shaped head only (1,146 of 2,315
    // findings, 49.5%), deliberately keeping a bare `symbol:123` and a JSON
    // fragment (`limit:10`) in scope. Widened 2026-08-15 after measuring that
    // decision on the residual corpus: of the 129 distinct heads still
    // reaching the lane, 128 resolve in source, so the finding carried no
    // information about its head — 175 of 1,175 findings, 14.9%. The one
    // exception (`no_truncate:true`) sits in an open design question, so
    // reporting the head instead was measured and rejected as complexity for
    // a single false positive. The head holds no colon, so `Qt::CaseSensitive`
    // is not a citation; INV-5's bare slash-path carries no tail and is
    // untouched.
    static const QRegularExpression citationToken(
        R"(^[^\s:]+:(?:\d+(?:-\d+)?|true|false|null)$)");

    const QStringList lines = docText.split('\n');
    // Fenced code blocks are illustrations, not contract claims — skip them.
    // Fence state comes from the shared ANTS-3603 scanner, not a local copy.
    const QVector<bool> inFence = MarkdownScan::fenceMask(lines);

    QList<SpecToken> out;
    QSet<QString> seen;
    const QSet<QString> &stop = stopwords();
    for (int i = 0; i < lines.size(); ++i) {
        if (inFence.value(i)) continue;
        QRegularExpressionMatchIterator it = tokenRe.globalMatch(lines[i]);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString tok = m.captured(1);
            if (stop.contains(tok)) continue;
            if (isPseudoToken(tok)) continue;
            if (urlScheme.match(tok).hasMatch()) continue;
            if (citationToken.match(tok).hasMatch()) continue;
            if (seen.contains(tok)) continue;
            seen.insert(tok);
            out.append({tok, i + 1});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Lane 2
// ---------------------------------------------------------------------------

QList<ChangelogBullet> extractTopVersionBullets(const QString &text,
                                                bool skipUnreleased) {
    const QStringList lines = text.split('\n');
    int start = -1;
    int end = lines.size();
    for (int i = 0; i < lines.size(); ++i) {
        // Header shape: `## ` at column 0. Keep-a-Changelog uses
        // `## [Unreleased]` and `## [0.7.0] - 2026-04-21`; anything
        // starting with `## ` is treated as a version header.
        if (lines[i].startsWith("## ")) {
            if (start < 0) {
                // ANTS-2007 — when asked, skip `## [Unreleased]`: its items are
                // in-progress, not yet specced or released, so coverage-checking
                // them flags a missing test for every WIP entry. Anchor on the
                // first RELEASED version section instead.
                if (skipUnreleased &&
                    lines[i].contains(QStringLiteral("[Unreleased]"),
                                      Qt::CaseInsensitive))
                    continue;
                start = i;
            } else {
                end = i;
                break;
            }
        }
    }
    if (start < 0) return {};

    QString section;
    QList<ChangelogBullet> out;
    for (int i = start + 1; i < end; ++i) {
        const QString &raw = lines[i];
        const QString s = raw.trimmed();
        if (s.startsWith("### ")) {
            section = s.mid(4).trimmed();
            continue;
        }
        if (s.startsWith("- ")) {
            ChangelogBullet b;
            b.section = section;
            b.text = s.mid(2).trimmed();
            b.line = i + 1;
            out.append(b);
        }
    }
    return out;
}

QList<QString> extractBacktickTokens(const QString &s) {
    // Looser than `extractSpecTokens` — we want any backtick-fenced
    // identifier-shaped content, not just ones long enough to be
    // meaningful drift candidates. The bullet↔title matcher uses this
    // to cross-reference short command names like `ls`, `ps`, etc.
    static const QRegularExpression re(R"(`([^`\s]{2,80})`)");
    QList<QString> out;
    QRegularExpressionMatchIterator it = re.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString t = m.captured(1);
        // Filter out pure-punctuation fragments (`\n`, `{}`, etc.) — the
        // matcher would never find those in a spec title anyway and
        // including them dilutes the signal.
        bool hasAlpha = false;
        for (QChar c : t) if (c.isLetter()) { hasAlpha = true; break; }
        if (!hasAlpha) continue;
        out << t;
    }
    return out;
}

QStringList significantWords(const QString &text) {
    const QSet<QString> &stop = englishStopwords();
    QStringList out;
    QString buf;
    auto flush = [&] {
        if (buf.size() >= 4 && !stop.contains(buf)) out << buf;
        buf.clear();
    };
    for (QChar c : text) {
        if (c.isLetterOrNumber() || c == '_' || c == '-') {
            buf += c.toLower();
        } else {
            flush();
        }
    }
    flush();
    return out;
}

QString extractChangelogEntryId(const QString &bulletText) {
    // Anchored at end: `changelog_log` renders `- **summary** (ID)`, so the
    // bullet's own key is always trailing. An id quoted mid-sentence is a
    // cross-reference to some other entry and must not be read as this
    // bullet's key.
    static const QRegularExpression idRe(
        R"(\(([A-Za-z][A-Za-z0-9]{1,15}-\d+)\)[\s.]*$)");
    const QRegularExpressionMatch m = idRe.match(bulletText.trimmed());
    return m.hasMatch() ? m.captured(1) : QString();
}

bool bulletMatchesAnyTitle(const QString &bulletText,
                           const QStringList &titles) {
    // Strong match — shared backtick token with any title.
    const QList<QString> bulletBt = extractBacktickTokens(bulletText);
    if (!bulletBt.isEmpty()) {
        QSet<QString> bbSet(bulletBt.begin(), bulletBt.end());
        for (const QString &title : titles) {
            const QList<QString> tb = extractBacktickTokens(title);
            for (const QString &t : tb) {
                if (bbSet.contains(t)) return true;
            }
        }
    }

    // Fallback — ≥2 significant words in common with any title. Bullet
    // scope limited to its first 120 chars so trailing prose (caveats,
    // rationale) doesn't inflate the overlap into a false match.
    const QStringList bw = significantWords(bulletText.left(120));
    if (bw.size() < 2) return false;
    const QSet<QString> bs(bw.begin(), bw.end());
    for (const QString &title : titles) {
        const QStringList tw = significantWords(title);
        int hits = 0;
        for (const QString &w : tw) if (bs.contains(w)) ++hits;
        if (hits >= 2) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// File-I/O runners
// ---------------------------------------------------------------------------

QString buildProjectSourceBlob(const QString &projectPath,
                               const BlobOptions &opts) {
    // Build the existence index ONCE. The blob spans the WHOLE project
    // tree (minus build/VCS/cache dirs) — not just src/ — so spec
    // references to filenames (`remotecontrol.cpp`), package names
    // (`lua54-devel`), YAML keys (`x-checker-data`), and test-file
    // titles (`test_vtparser_simd.cpp`) all resolve naturally.
    //
    // Using substring-containment across a concatenated blob rather
    // than per-token grep: O(N) per lookup instead of O(files × tokens),
    // and correct for scoped names like `RemoteControl::dispatch` that
    // word-boundary grep fails on (the `::` breaks \b).
    static const QStringList kExts = {
        // Source code
        "*.cpp", "*.h", "*.c", "*.hpp", "*.cc", "*.cxx", "*.hxx",
        "*.py", "*.js", "*.ts", "*.tsx", "*.jsx",
        "*.go", "*.rs", "*.lua", "*.java", "*.kt",
        "*.qml", "*.sh", "*.bash", "*.zsh", "*.ps1",
        // Build / config / docs — carries filenames, package names,
        // CMake targets, JSON/YAML keys that specs routinely cite.
        "*.txt",        // CMakeLists.txt, requirements.txt
        "*.md",         // CHANGELOG.md, README.md, ROADMAP.md, PLUGINS.md
        "*.json",       // flathub manifests, VSCode launch
        "*.yml", "*.yaml",
        "*.toml",       // pyproject.toml, Cargo.toml
        "*.ini", "*.cfg",
        "*.xml",        // AppStream metainfo, .desktop-style
        "*.cmake",
        "*.in",         // CMake .in templates
        "*.pro", "*.pri",
    };
    // Excluded dirs — build artifacts and VCS/IDE noise that dilute the
    // index without adding signal. The core set is the shared audit
    // exclusion set (AuditEngine::excludedDirNames — ANTS-1709, the single
    // source of truth) plus a few VCS/IDE caches that only matter to this
    // spec-drift walk. `build` and the `build-*` glob are handled by the
    // literal + startsWith() check below.
    static const QSet<QString> kSkipTopDirs = [] {
        QSet<QString> s(AuditEngine::excludedDirNames().cbegin(),
                        AuditEngine::excludedDirNames().cend());
        s += {QStringLiteral("build"), QStringLiteral(".svn"),
              QStringLiteral(".hg"), QStringLiteral(".ccls-cache"),
              // ANTS-2007 — review-artifact cache: a stale symbol lingering in
              // a cached report would mask real spec-drift in the source.
              QStringLiteral(".indie-review")};
        return s;
    }();
    QString sourceBlob;
    // ANTS-3600 — when opts.appendPathManifest, record every walked file's
    // project-relative path here and append it to the blob after the walk, so
    // a doc-cited *filename* resolves even when the file's content is
    // name-excluded / off-kExts / a markdown body. rootDir anchors the
    // relativeFilePath() computation.
    QString pathManifest;
    const QDir rootDir(projectPath);
    // Walk the project tree manually so we can skip heavy dirs
    // without relying on QDirIterator's limited filtering.
    std::function<void(const QString &)> walk = [&](const QString &dir) {
        QDir d(dir);
        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo &fi : entries) {
            if (fi.isDir()) {
                if (kSkipTopDirs.contains(fi.fileName())) continue;
                // Also skip build-* variants (build-debug, build-ci, …).
                if (fi.fileName().startsWith("build-")) continue;
                // ANTS-2007 — don't follow directory symlinks: a cyclic link
                // would recurse unboundedly and crash the walk.
                if (fi.isSymLink()) continue;
                walk(fi.filePath());
                continue;
            }
            // File.
            const QString name = fi.fileName();
            // ANTS-3600 phase 1 — record the rel-path UNCONDITIONALLY, before
            // any content gate below, so a doc-cited filename resolves via the
            // manifest even when the file is name-excluded / off-kExts / a
            // markdown body (docs/specs/ANTS-3600.md § 2.3).
            if (opts.appendPathManifest) {
                pathManifest += rootDir.relativeFilePath(fi.filePath());
                pathManifest += '\n';
            }
            // Phase 2 — content-concatenation gates.
            // Exclude spec.md files themselves — they're the *source*
            // of the tokens we're looking up, so including them in
            // the blob would make every spec token match itself and
            // silently neuter the whole lane.
            if (name == "spec.md") continue;
            // ANTS-2007 — the changelog/roadmap carry historical symbol names;
            // a token deleted from src/ but still cited there would mask the
            // drift it's meant to catch (the finding even says "no match in
            // src/"). Exclude them from the source blob.
            if (name == "ROADMAP.md" || name == "CHANGELOG.md") continue;
            // ANTS-3600 — the drift lane's own allowlist is a hidden *.txt at
            // root; keep its BODY out of the blob (else existsInSource would
            // self-resolve any token it lists, incl. in `#` comments, masking
            // real drift). Its path may still enter the manifest above
            // (harmless) — § 2.6.
            if (name == QStringLiteral(".ants_doc_drift_allow.txt")) continue;
            bool match = false;
            for (const QString &pat : kExts) {
                // pat is "*.ext"; compare suffix.
                if (pat.size() > 1 && name.endsWith(pat.mid(1),
                                                    Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
            // ANTS-3600 — exclude *.md bodies when the caller asks (the
            // contract-doc drift lane): doc prose must not self-satisfy a
            // quoted literal. Non-`.md` code/config extensions are unaffected.
            if (!opts.includeMarkdownContents &&
                name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
                continue;
            QFile f(fi.filePath());
            if (f.open(QIODevice::ReadOnly)) {
                sourceBlob += QString::fromUtf8(f.readAll());
                sourceBlob += '\n';
            }
        }
    };
    walk(projectPath);
    // ANTS-3600 — the manifest (every walked rel-path) is appended AFTER the
    // content half so a quoted filename literal resolves via existsInSource's
    // substring containment without pulling any doc's prose into the blob.
    if (opts.appendPathManifest) sourceBlob += pathManifest;
    return sourceBlob;
}

bool existsInSource(const QString &blob, const QString &token) {
    if (blob.contains(token, Qt::CaseSensitive)) return true;
    // Scoped fallback — `ClassName::method` and `file.cpp::member`
    // are documentation shorthand. The compound string rarely
    // appears literally (the declaration puts the class at the
    // file/class header and the member further down), so fall
    // back to looking for the last path component alone. Same
    // treatment for `.`-separated compounds (`module.func`).
    const int scopeIdx = token.lastIndexOf(QStringLiteral("::"));
    if (scopeIdx > 0) {
        const QString tail = token.mid(scopeIdx + 2);
        if (tail.size() >= 3 && blob.contains(tail, Qt::CaseSensitive))
            return true;
    }
    const int dotIdx = token.lastIndexOf('.');
    if (dotIdx > 0 && dotIdx < token.size() - 1) {
        const QString tail = token.mid(dotIdx + 1);
        // Only use this fallback when the tail is still
        // identifier-shaped (4+ alpha chars) — otherwise we'd
        // match file extensions (`*.cpp` → `cpp`) and get false
        // non-drift passes on literal filename tokens.
        if (tail.size() >= 4 && tail[0].isLetter() &&
            blob.contains(tail, Qt::CaseSensitive))
            return true;
    }
    return false;
}

const QSet<QString> &specStopwords() {
    return stopwords();
}

QString runSpecDriftCheck(const QString &projectPath) {
    // Bail cleanly when the project doesn't use the convention. The audit
    // framework treats empty output as "no findings" — the whole lane
    // becomes a silent no-op, which is what we want for projects without
    // a tests/features/ corpus or without a src/ tree.
    QDir projectDir(projectPath);
    if (!projectDir.exists("src")) return {};
    if (!projectDir.exists("tests/features")) return {};

    // ANTS-4098 — append the path manifest. A feature spec cites FILES as
    // often as symbols (its own sibling test, a fixture, a config it drives),
    // and a filename is not present in its own contents. With contents alone
    // such a citation resolved only when some *other* file's text happened to
    // name it — true here, where CMakeLists.txt lists every test source, and
    // false on a project whose tests are collected by convention (pytest and
    // friends), where every cited test file read as drift. The manifest makes
    // the file's existence the evidence, which is what the citation claims.
    // A symbol that exists nowhere is unaffected: it is in no path either.
    const QString sourceBlob = buildProjectSourceBlob(
        projectPath, BlobOptions{.appendPathManifest = true});
    if (sourceBlob.isEmpty()) return {};

    auto resolves = [&sourceBlob](const QString &tok) {
        return existsInSource(sourceBlob, tok);
    };

    QString out;
    const QDir featuresDir(projectPath + "/tests/features");
    const QFileInfoList featureDirs =
        featuresDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &fi : featureDirs) {
        const QString specPath = fi.filePath() + "/spec.md";
        QFile specFile(specPath);
        if (!specFile.open(QIODevice::ReadOnly)) continue;
        const QString specText = QString::fromUtf8(specFile.readAll());
        specFile.close();

        const QList<SpecToken> drift = findDriftTokens(specText, resolves);
        const QString relPath = projectDir.relativeFilePath(specPath);
        for (const SpecToken &d : drift) {
            // ANTS-2113 H1 — the search is whole-tree (buildProjectSourceBlob
            // walks the project, not just src/) so the test corpus can resolve
            // a spec's own gtest case names; the old "no match in src/" message
            // lied about that scope. Empirically, excluding tests/ to make the
            // "src/" claim true floods the lane with false drift on every
            // spec-cited test-case name, so whole-tree is the correct design —
            // the message is what gets fixed. (ANTS-2113 H2, identifier-
            // boundary matching, deferred: it surfaces imprecise-but-harmless
            // spec wording, not real drift — see ROADMAP.)
            out += QString("%1:%2: spec references `%3` but no match in "
                           "project sources\n")
                       .arg(relPath).arg(d.line).arg(d.token);
        }
    }
    return out.trimmed();
}

QSet<QString> loadAllowlist(const QString &projectPath) {
    QSet<QString> allow;
    QFile f(projectPath + "/.ants_doc_drift_allow.txt");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return allow;  // absent
    const QString text = QString::fromUtf8(f.readAll());
    const QStringList lines = text.split('\n');
    for (const QString &raw : lines) {
        // trimmed() strips surrounding whitespace AND a trailing \r (CRLF).
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith('#')) continue;   // whole-line comment only
        allow.insert(line);
    }
    return allow;
}

QString runContractDocDriftCheck(const QString &projectPath) {
    // Silent no-op ("" == "no findings") when neither contract-doc dir exists;
    // scans whichever of the two is present (INV-9).
    const QDir projectDir(projectPath);
    const bool hasStandards = projectDir.exists(QStringLiteral("docs/standards"));
    const bool hasSpecs     = projectDir.exists(QStringLiteral("docs/specs"));
    if (!hasStandards && !hasSpecs) return {};

    // *.md bodies excluded so doc prose can't self-satisfy a literal; a path
    // manifest so quoted filenames still resolve (§ 2.3). Designated
    // initializers guard against a silent field-swap (CLAUDE.md § 5 idiom).
    const QString blob = buildProjectSourceBlob(
        projectPath,
        BlobOptions{.includeMarkdownContents = false, .appendPathManifest = true});
    if (blob.isEmpty()) return {};

    const QSet<QString> allow = loadAllowlist(projectPath);

    QString out;
    const QStringList subdirs = {QStringLiteral("docs/standards"),
                                 QStringLiteral("docs/specs")};
    for (const QString &sub : subdirs) {
        const QString dirPath = projectPath + '/' + sub;
        if (!QDir(dirPath).exists()) continue;
        // Every *.md under the dir (recursive), sorted for stable output order.
        QStringList docPaths;
        QDirIterator it(dirPath, {QStringLiteral("*.md")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) docPaths << it.next();
        docPaths.sort();
        for (const QString &docPath : docPaths) {
            QFile docFile(docPath);
            if (!docFile.open(QIODevice::ReadOnly)) continue;
            const QString docText = QString::fromUtf8(docFile.readAll());
            docFile.close();
            const QString relPath = projectDir.relativeFilePath(docPath);
            for (const SpecToken &t : extractDocLiteralTokens(docText)) {
                if (allow.contains(t.token)) continue;
                if (existsInSource(blob, t.token)) continue;
                out += QString("%1:%2: doc references `%3` but no match in "
                               "project sources\n")
                           .arg(relPath).arg(t.line).arg(t.token);
            }
        }
    }
    return out.trimmed();
}

QString runChangelogCoverageCheck(const QString &projectPath) {
    QFile clog(projectPath + "/CHANGELOG.md");
    if (!clog.open(QIODevice::ReadOnly)) return {};
    const QString clogText = QString::fromUtf8(clog.readAll());
    clog.close();

    // Collect all feature spec titles. Title is the first `# ` line of
    // each spec.md (the markdown H1). Specs without an H1 header
    // contribute nothing — they'd never match anyway.
    QStringList titles;
    const QDir featuresDir(projectPath + "/tests/features");
    if (featuresDir.exists()) {
        const QFileInfoList featureDirs = featuresDir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &fi : featureDirs) {
            QFile specFile(fi.filePath() + "/spec.md");
            if (!specFile.open(QIODevice::ReadOnly)) continue;
            const QString firstLine =
                QString::fromUtf8(specFile.readLine()).trimmed();
            specFile.close();
            if (firstLine.startsWith("# "))
                titles << firstLine.mid(2).trimmed();
        }
    }
    // No specs at all → this project doesn't use the feature-conformance
    // convention. Don't flood it with coverage warnings; silently skip.
    if (titles.isEmpty()) return {};

    // ANTS-4099 — id-keyed coverage, checked before the prose match below.
    // An entry written by `changelog_log` carries its ticket id in a trailing
    // `(PROJ-NNNN)`, and the feature locking it cites the same id in its spec
    // or test. That id is an exact join key; prose is not, because a
    // changelog headline is written for users and a spec title for
    // developers, so the better the headline reads the worse it matches.
    // Scanned lazily and once — a project with no ids pays nothing.
    QSet<QString> corpusIds;
    bool corpusIdsLoaded = false;
    auto idIsCovered = [&](const QString &id) {
        if (!corpusIdsLoaded) {
            corpusIdsLoaded = true;
            static const QRegularExpression anyIdRe(
                R"(\b[A-Za-z][A-Za-z0-9]{1,15}-\d+\b)");
            const QFileInfoList dirs = featuresDir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo &fd : dirs) {
                const QFileInfoList files =
                    QDir(fd.filePath()).entryInfoList(QDir::Files);
                for (const QFileInfo &ff : files) {
                    QFile f(ff.filePath());
                    if (!f.open(QIODevice::ReadOnly)) continue;
                    const QString text = QString::fromUtf8(f.readAll());
                    f.close();
                    QRegularExpressionMatchIterator it =
                        anyIdRe.globalMatch(text);
                    while (it.hasNext())
                        corpusIds.insert(it.next().captured(0));
                }
            }
        }
        return corpusIds.contains(id);
    };

    const QList<ChangelogBullet> bullets =
        extractTopVersionBullets(clogText, /*skipUnreleased=*/true);

    QString out;
    for (const ChangelogBullet &b : bullets) {
        // Only Added and Fixed claim a feature-level behavior that should
        // have a locking test. Changed/Removed/Deprecated/Security are
        // often internal or intentionally untested at the feature level.
        if (b.section != "Added" && b.section != "Fixed") continue;
        const QString entryId = extractChangelogEntryId(b.text);
        if (!entryId.isEmpty() && idIsCovered(entryId)) continue;
        if (bulletMatchesAnyTitle(b.text, titles)) continue;
        QString preview = b.text;
        if (preview.size() > 90) preview = preview.left(87) + "…";
        out += QString("CHANGELOG.md:%1: [%2] %3 — no matching tests/features/*/spec.md\n")
                   .arg(b.line).arg(b.section, preview);
    }
    return out.trimmed();
}

} // namespace FeatureCoverage
