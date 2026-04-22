// Implementation: see featurecoverage.h for the contract.

#include "featurecoverage.h"

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

// ---------------------------------------------------------------------------
// Lane 2
// ---------------------------------------------------------------------------

QList<ChangelogBullet> extractTopVersionBullets(const QString &text) {
    const QStringList lines = text.split('\n');
    int start = -1;
    int end = lines.size();
    for (int i = 0; i < lines.size(); ++i) {
        // Header shape: `## ` at column 0. Keep-a-Changelog uses
        // `## [Unreleased]` and `## [0.7.0] - 2026-04-21`; anything
        // starting with `## ` is treated as a version header.
        if (lines[i].startsWith("## ")) {
            if (start < 0) {
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

QString runSpecDriftCheck(const QString &projectPath) {
    // Bail cleanly when the project doesn't use the convention. The audit
    // framework treats empty output as "no findings" — the whole lane
    // becomes a silent no-op, which is what we want for projects without
    // a tests/features/ corpus or without a src/ tree.
    QDir projectDir(projectPath);
    if (!projectDir.exists("src")) return {};
    if (!projectDir.exists("tests/features")) return {};

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
    QString sourceBlob;
    {
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
        // Excluded top-level dirs — build artifacts and VCS/IDE noise
        // that dilute the index without adding signal. Keep the list
        // in sync with kFindExcl / kGrepExcl from auditdialog.cpp.
        static const QSet<QString> kSkipTopDirs = {
            ".git", ".svn", ".hg", "build", "dist", "node_modules",
            ".cache", ".audit_cache", ".pytest_cache", ".mypy_cache",
            ".tox", ".venv", "venv", "target", ".claude", "__pycache__",
            "vendor", "third_party", "external", ".ccls-cache",
        };
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
                    walk(fi.filePath());
                    continue;
                }
                // File — check extension matches.
                const QString name = fi.fileName();
                // Exclude spec.md files themselves — they're the *source*
                // of the tokens we're looking up, so including them in
                // the blob would make every spec token match itself and
                // silently neuter the whole lane.
                if (name == "spec.md") continue;
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
                QFile f(fi.filePath());
                if (f.open(QIODevice::ReadOnly)) {
                    sourceBlob += QString::fromUtf8(f.readAll());
                    sourceBlob += '\n';
                }
            }
        };
        walk(projectPath);
    }
    if (sourceBlob.isEmpty()) return {};

    auto existsInSource = [&sourceBlob](const QString &tok) {
        if (sourceBlob.contains(tok, Qt::CaseSensitive)) return true;
        // Scoped fallback — `ClassName::method` and `file.cpp::member`
        // are documentation shorthand. The compound string rarely
        // appears literally (the declaration puts the class at the
        // file/class header and the member further down), so fall
        // back to looking for the last path component alone. Same
        // treatment for `.`-separated compounds (`module.func`).
        const int scopeIdx = tok.lastIndexOf(QStringLiteral("::"));
        if (scopeIdx > 0) {
            const QString tail = tok.mid(scopeIdx + 2);
            if (tail.size() >= 3 &&
                sourceBlob.contains(tail, Qt::CaseSensitive))
                return true;
        }
        const int dotIdx = tok.lastIndexOf('.');
        if (dotIdx > 0 && dotIdx < tok.size() - 1) {
            const QString tail = tok.mid(dotIdx + 1);
            // Only use this fallback when the tail is still
            // identifier-shaped (4+ alpha chars) — otherwise we'd
            // match file extensions (`*.cpp` → `cpp`) and get false
            // non-drift passes on literal filename tokens.
            if (tail.size() >= 4 && tail[0].isLetter() &&
                sourceBlob.contains(tail, Qt::CaseSensitive))
                return true;
        }
        return false;
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

        const QList<SpecToken> drift = findDriftTokens(specText, existsInSource);
        const QString relPath = projectDir.relativeFilePath(specPath);
        for (const SpecToken &d : drift) {
            out += QString("%1:%2: spec references `%3` but no match in src/\n")
                       .arg(relPath).arg(d.line).arg(d.token);
        }
    }
    return out.trimmed();
}

QString runChangelogCoverageCheck(const QString &projectPath) {
    QDir projectDir(projectPath);
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

    const QList<ChangelogBullet> bullets = extractTopVersionBullets(clogText);

    QString out;
    for (const ChangelogBullet &b : bullets) {
        // Only Added and Fixed claim a feature-level behavior that should
        // have a locking test. Changed/Removed/Deprecated/Security are
        // often internal or intentionally untested at the feature level.
        if (b.section != "Added" && b.section != "Fixed") continue;
        if (bulletMatchesAnyTitle(b.text, titles)) continue;
        QString preview = b.text;
        if (preview.size() > 90) preview = preview.left(87) + "…";
        out += QString("CHANGELOG.md:%1: [%2] %3 — no matching tests/features/*/spec.md\n")
                   .arg(b.line).arg(b.section, preview);
    }
    return out.trimmed();
}

} // namespace FeatureCoverage
