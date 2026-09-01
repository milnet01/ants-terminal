// ANTS-1636 — find_sources implementation. See header for contract.

#include "findsources.h"
#include "projectsettings.h"   // ANTS-3489 — source_roots / test_roots override

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace FindSources {

namespace {

// ANTS-3444 — ASCII-lowercase a byte buffer in place. find_sources
// matches identifier/keyword needles that are ASCII, so folding the
// haystack bytes (A-Z -> a-z) and searching with QByteArray::indexOf
// avoids the full UTF-16 decode + lowercased-copy that dominated the
// per-file cost. UTF-8 continuation bytes are 0x80-0xBF, so an ASCII
// needle can never straddle a multibyte boundary; the byte search is
// exact for the ASCII domain.
void asciiLowerInPlace(QByteArray &b) {
    char *const d = b.data();
    const int n = b.size();
    for (int i = 0; i < n; ++i) {
        const char c = d[i];
        if (c >= 'A' && c <= 'Z') d[i] = static_cast<char>(c - 'A' + 'a');
    }
}

bool looksLikeRoadmapId(const QString &t) {
    // INV-1 — drop bare ANTS-NNNN tokens. Including them in the regex
    // matches roadmap-id text that almost never lives in source code,
    // wasting the per-file scan budget on a guaranteed miss.
    static const QRegularExpression rx(QStringLiteral("^[A-Z]{2,8}-\\d+$"));
    return rx.match(t).hasMatch();
}

QString toSnakeCase(const QString &t) {
    // CamelCase / camelCase / mixedCase → camel_case. Conservative —
    // only injects an underscore between [a-z][A-Z] boundaries to avoid
    // mangling all-caps acronyms (`MCPServer` → `mcp_server`).
    QString out;
    out.reserve(t.size() + 4);
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (i > 0 && c.isUpper()
            && t.at(i - 1).isLower()) {
            out += QLatin1Char('_');
        }
        out += c.toLower();
    }
    return out;
}

QString toCamelFromSnake(const QString &t) {
    // foo_bar_baz → fooBarBaz. Leaves an all-lowercase token unchanged.
    QString out;
    out.reserve(t.size());
    bool capNext = false;
    for (const QChar c : t) {
        if (c == QLatin1Char('_') || c == QLatin1Char('-')) {
            capNext = true;
            continue;
        }
        if (capNext) {
            out += c.toUpper();
            capNext = false;
        } else {
            out += c;
        }
    }
    return out;
}

QString dropSeparators(const QString &t) {
    QString out;
    out.reserve(t.size());
    for (const QChar c : t) {
        if (c != QLatin1Char('_') && c != QLatin1Char('-')) {
            out += c.toLower();
        }
    }
    return out;
}

QString classifyRole(const QString &relPath) {
    if (relPath.startsWith(QLatin1String("tests/"))
        || relPath.contains(QLatin1String("/tests/"))) {
        return QStringLiteral("test");
    }
    if (relPath.endsWith(QLatin1String(".h"))
        || relPath.endsWith(QLatin1String(".hpp"))) {
        return QStringLiteral("header");
    }
    return QStringLiteral("impl");
}

// Append `path` to `out` if it's a source file we want to scan
// (.cpp/.cc/.cxx/.h/.hpp). Skips obvious generated artefacts.
void maybeAddSource(QVector<QString> *out, const QString &relPath) {
    static const QStringList kExts = {
        QStringLiteral(".cpp"), QStringLiteral(".cc"),
        QStringLiteral(".cxx"), QStringLiteral(".h"),
        QStringLiteral(".hpp"),
    };
    bool ok = false;
    for (const QString &ext : kExts) {
        if (relPath.endsWith(ext)) { ok = true; break; }
    }
    if (!ok) return;
    // INV-6 — skip generated files (moc/ui/qrc/protobuf/anything
    // under a /generated/ path). Mirrors the audit-engine skip list.
    if (relPath.contains(QLatin1String("/generated/"))
        || relPath.contains(QLatin1String("moc_"))
        || relPath.contains(QLatin1String("ui_"))
        || relPath.contains(QLatin1String("qrc_"))
        || relPath.endsWith(QLatin1String(".pb.cc"))
        || relPath.endsWith(QLatin1String(".pb.h"))
        || relPath.endsWith(QLatin1String("_generated.cpp"))
        || relPath.endsWith(QLatin1String("_generated.h"))) {
        return;
    }
    out->append(relPath);
}

// Enumerate the source-file candidate set under a canonical project
// root, filtered by maybeAddSource. Shared by findSources() (the query)
// and prewarm() (the page-cache warm) so the two can never scan a
// different set (ANTS-3444a).
//
// ANTS-3489 — the walked roots honour .ants/project.json
// source_roots / test_roots (ANTS-2160), falling back to the src/ + tests/
// default. A C/C++ project laid out under a non-src/ root (declared in
// project.json) is now scanned instead of silently yielding zero candidates
// (files_scanned:0 → every token "unmatched", indistinguishable from
// "scanned but no hit"). Mirrors codebaseindex.cpp::candidates. Vendored /
// build-output / virtualenv trees are pruned via ProjectSettings::isNoiseDir
// so a flat-root source_roots=["."] can't drag a committed venv into the set.
QVector<QString> collectCandidates(const QString &rootCanonical) {
    QVector<QString> candidates;
    candidates.reserve(512);
    const ProjectSettings::Settings settings =
        ProjectSettings::load(rootCanonical);
    const auto resolve = [&](const std::optional<QStringList> &declared,
                             const QString &def) -> QStringList {
        if (!declared) return {def};
        QStringList dirs;
        for (const QString &r : *declared)
            if (QDir(rootCanonical + QLatin1Char('/') + r).exists()) dirs << r;
        return dirs.isEmpty() ? QStringList{def} : dirs;   // all dropped → default
    };
    QStringList roots = resolve(settings.sourceRoots, QStringLiteral("src"));
    roots += resolve(settings.testRoots, QStringLiteral("tests"));
    roots.removeDuplicates();

    for (const QString &subdir : roots) {
        QDir dir(rootCanonical + QLatin1Char('/') + subdir);
        if (!dir.exists()) continue;
        QDirIterator it(dir.absolutePath(),
                        QDir::Files | QDir::Readable,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString abs = it.next();
            QString rel = abs.mid(rootCanonical.size());
            if (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
            // Skip files under any vendored / build / virtualenv dir.
            bool noisy = false;
            for (const QString &comp : rel.split(QLatin1Char('/')))
                if (ProjectSettings::isNoiseDir(comp)) { noisy = true; break; }
            if (!noisy) maybeAddSource(&candidates, rel);
        }
    }
    // Declared roots may nest (source_roots=["."] contains the tests default);
    // dedup so a file isn't scanned twice.
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    return candidates;
}

}  // namespace

QStringList tokenise(const QString &topic) {
    // INV-1 — split on whitespace + common punctuation. Tokens shorter
    // than 3 chars are dropped (`a`, `to`, `in` are too noisy to scan).
    // ANTS-4776 — constant pattern: compile once, not once per query.
    static const QRegularExpression sep(
        QStringLiteral("[\\s_/\\.\\(\\),:;]+"));
    QStringList raw = topic.split(sep, Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(raw.size());
    for (QString &t : raw) {
        t = t.trimmed();
        if (t.size() < 3) continue;
        if (looksLikeRoadmapId(t)) continue;
        out.append(t);
    }
    return out;
}

QStringList variantsForToken(const QString &token) {
    QStringList v;
    const QString lower = token.toLower();
    v.append(lower);

    const QString snake = toSnakeCase(token);
    if (!snake.isEmpty() && !v.contains(snake)) v.append(snake);

    const QString camel = toCamelFromSnake(lower);
    if (!camel.isEmpty() && !v.contains(camel)) v.append(camel);
    // Strict-camel (snake → camelCase via TitleCase first letter only
    // when the token has separators — covers `model_auto_switch` →
    // `ModelAutoSwitch` matches in CamelCase class names).
    if (lower.contains(QLatin1Char('_')) || lower.contains(QLatin1Char('-'))) {
        QString pascal = camel;
        if (!pascal.isEmpty()) pascal[0] = pascal[0].toUpper();
        if (!v.contains(pascal)) v.append(pascal);
    }

    const QString flat = dropSeparators(lower);
    if (!flat.isEmpty() && !v.contains(flat)) v.append(flat);

    return v;
}

Result findSources(const QString &topic,
                   const QString &projectRoot,
                   const Options &optsIn) {
    Result r;

    QFileInfo rootInfo(projectRoot);
    if (!rootInfo.isDir()) return r;
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty()) return r;

    Options opts = optsIn;
    if (opts.maxResults <= 0) opts.maxResults = 20;
    if (opts.maxResults > opts.maxResultsHard) {
        opts.maxResults = opts.maxResultsHard;
    }
    if (opts.contentByteCap <= 0) opts.contentByteCap = 256 * 1024;

    const QStringList tokens = tokenise(topic);
    if (tokens.isEmpty()) return r;

    // Precompute variants per token. Variants are lowercase except
    // the optional PascalCase one; we'll match case-insensitively
    // throughout so the case detail is informational, not load-bearing.
    QVector<QStringList> variantsPerToken;
    variantsPerToken.reserve(tokens.size());
    for (const QString &t : tokens) {
        variantsPerToken.append(variantsForToken(t));
    }

    // ANTS-3444 — ASCII-lowercased needle bytes per variant, computed
    // once (was a per-file `v.toLower()` inside the content loop). Kept
    // index-aligned with variantsPerToken so the original variant string
    // is still available for the evidence lines. variantsForToken never
    // yields an empty variant, so the two arrays stay 1:1.
    QVector<QVector<QByteArray>> needlesPerToken;
    needlesPerToken.reserve(tokens.size());
    for (const QStringList &variants : variantsPerToken) {
        QVector<QByteArray> needles;
        needles.reserve(variants.size());
        for (const QString &v : variants) {
            QByteArray nb = v.toUtf8();
            asciiLowerInPlace(nb);
            needles.append(nb);
        }
        needlesPerToken.append(needles);
    }

    // Walk src/ + tests/ — find_sources is for source code.
    const QVector<QString> candidates = collectCandidates(rootCanonical);
    r.filesScanned = candidates.size();

    QVector<int> tokenHitCount(tokens.size(), 0);

    for (const QString &rel : candidates) {
        // Filename score: count distinct tokens whose ANY variant
        // appears in the filename (case-insensitive). One token = one
        // match (don't double-count two variants of the same token).
        const QString lowerName =
            QFileInfo(rel).fileName().toLower();
        int filenameTokenMatches = 0;
        QStringList filenameEvidence;
        for (int i = 0; i < tokens.size(); ++i) {
            for (const QString &v : variantsPerToken.at(i)) {
                if (lowerName.contains(v.toLower())) {
                    ++filenameTokenMatches;
                    ++tokenHitCount[i];
                    filenameEvidence.append(
                        QStringLiteral("filename matches \"%1\"").arg(v));
                    break;
                }
            }
        }

        // Content score: open the file (up to contentByteCap bytes),
        // count total variant-occurrence hits across all tokens. We
        // already know the filename matched; even files that didn't
        // match by name can rank in via content.
        int contentHits = 0;
        QStringList contentEvidence;
        QFile f(rootCanonical + QLatin1Char('/') + rel);
        if (f.open(QIODevice::ReadOnly)) {
            // ANTS-3444 — fold the raw bytes to lowercase ASCII in place
            // and search with QByteArray::indexOf, skipping the UTF-16
            // decode + lowercased QString copy that dominated the scan.
            QByteArray data = f.read(opts.contentByteCap);
            asciiLowerInPlace(data);
            for (int i = 0; i < tokens.size(); ++i) {
                int tokenLocalHits = 0;
                QString matchedVariant;
                const QVector<QByteArray> &needles = needlesPerToken.at(i);
                for (int vi = 0; vi < needles.size(); ++vi) {
                    const QByteArray &needle = needles.at(vi);
                    if (needle.isEmpty()) continue;
                    int from = 0;
                    while (true) {
                        const int at = data.indexOf(needle, from);
                        if (at < 0) break;
                        ++tokenLocalHits;
                        from = at + needle.size();
                        if (tokenLocalHits > 50) break;  // per-token cap
                    }
                    if (tokenLocalHits > 0 && matchedVariant.isEmpty()) {
                        matchedVariant = variantsPerToken.at(i).at(vi);
                    }
                }
                if (tokenLocalHits > 0) {
                    contentHits += tokenLocalHits;
                    if (filenameTokenMatches == 0) ++tokenHitCount[i];
                    if (contentEvidence.size() < 2) {
                        contentEvidence.append(
                            QStringLiteral("\"%1\" × %2 in body")
                                .arg(matchedVariant)
                                .arg(tokenLocalHits));
                    }
                }
            }
        }

        if (filenameTokenMatches == 0 && contentHits == 0) continue;

        FileHit hit;
        hit.path  = rel;
        hit.role  = classifyRole(rel);
        // Score: 50 per filename-matched-token (heavy bias toward
        // filename hits) + content count clamped to [0, 60].
        hit.score = filenameTokenMatches * 50
                  + std::min(contentHits, 60);
        hit.evidence = filenameEvidence;
        for (const QString &e : contentEvidence) {
            if (hit.evidence.size() >= 3) break;
            hit.evidence.append(e);
        }
        r.files.append(hit);
    }

    // Rank: score desc, then path asc for stable ordering.
    std::sort(r.files.begin(), r.files.end(),
        [](const FileHit &a, const FileHit &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.path < b.path;
        });

    if (r.files.size() > opts.maxResults) {
        r.files.resize(opts.maxResults);
        r.truncated = true;
    }

    // unmatchedTerms — the original input tokens with zero hits.
    for (int i = 0; i < tokens.size(); ++i) {
        if (tokenHitCount[i] == 0) r.unmatchedTerms.append(tokens.at(i));
    }

    return r;
}

int prewarm(const QString &projectRoot) {
    QFileInfo rootInfo(projectRoot);
    if (!rootInfo.isDir()) return 0;
    const QString rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical.isEmpty()) return 0;

    const QVector<QString> candidates = collectCandidates(rootCanonical);

    // Mirror Options::contentByteCap — findSources reads at most this many
    // bytes per file, so warming exactly that prefix (no more) is enough to
    // make its first read hit the page cache without wasting I/O.
    constexpr qint64 kContentByteCap = 256 * 1024;
    QByteArray scratch;  // reused; holds one file's prefix at a time
    for (const QString &rel : candidates) {
        QFile f(rootCanonical + QLatin1Char('/') + rel);
        if (f.open(QIODevice::ReadOnly)) {
            // The read pulls the file's leading pages into the OS cache;
            // the returned bytes are discarded on the next iteration.
            scratch = f.read(kContentByteCap);
        }
    }
    return candidates.size();
}

}  // namespace FindSources
