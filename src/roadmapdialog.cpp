#include "roadmapdialog.h"

#include "coloredtabbar.h"     // for ClaudeTabIndicator::color (ToolUse yellow)
#include "themes.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStringBuilder>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

// Status emojis the ROADMAP legend documents. Keep in lock-step with
// the legend block at the top of ROADMAP.md and with INV-3 of the
// roadmap_viewer feature test.
constexpr const char *kEmojiDone        = "✅";
constexpr const char *kEmojiPlanned     = "📋";
constexpr const char *kEmojiInProgress  = "🚧";
constexpr const char *kEmojiConsidered  = "💭";

QString htmlEscape(QString s) {
    s.replace('&', QStringLiteral("&amp;"));
    s.replace('<', QStringLiteral("&lt;"));
    s.replace('>', QStringLiteral("&gt;"));
    return s;
}

// 1..4 if `raw` is a Markdown ATX heading, else 0. On a hit, `text`
// receives the heading content with the `# ` prefix stripped. Walk
// shape is shared between extractToc and renderHtml so the indices
// line up — anchor `roadmap-toc-N` refers to the N-th hit.
int headingLevel(const QString &raw, QString *text) {
    if (raw.startsWith(QStringLiteral("#### "))) {
        if (text) *text = raw.mid(5);
        return 4;
    }
    if (raw.startsWith(QStringLiteral("### "))) {
        if (text) *text = raw.mid(4);
        return 3;
    }
    if (raw.startsWith(QStringLiteral("## "))) {
        if (text) *text = raw.mid(3);
        return 2;
    }
    if (raw.startsWith(QStringLiteral("# "))) {
        if (text) *text = raw.mid(2);
        return 1;
    }
    return 0;
}

QString tocAnchorAt(int index) {
    return QStringLiteral("roadmap-toc-%1").arg(index);
}

// Backtick → <code>…</code>. Pure passthrough on everything else; the
// ROADMAP doesn't use other inline markdown shapes outside list bullets.
QString applyInline(const QString &line) {
    QString s = htmlEscape(line);
    static const QRegularExpression rxCode(QStringLiteral("`([^`]+)`"));
    s.replace(rxCode,
              QStringLiteral("<code style=\"font-family:monospace\">\\1</code>"));
    return s;
}

// Normalise a string for fuzzy matching: lowercase, hyphens/underscores
// → space, runs of whitespace collapsed, trailing punctuation trimmed.
QString fuzzy(const QString &in) {
    QString out;
    out.reserve(in.size());
    QChar last;
    for (QChar c : in) {
        QChar n = c.toLower();
        if (n == '-' || n == '_' || n == '/' || n == '\\') n = ' ';
        if (!n.isLetterOrNumber() && n != ' ') continue;
        if (n == ' ' && last == ' ') continue;
        out.append(n);
        last = n;
    }
    return out.trimmed();
}

// Strip a leading status emoji + bold marker from a bullet body. Used
// before the substring match for current-work signals so a bullet that
// starts with `✅ **State-dot palette**` matches a CHANGELOG line that
// reads "State-dot palette".
QString bulletPayload(QString body) {
    auto stripPrefix = [&](const char *needle) {
        if (body.startsWith(QString::fromUtf8(needle))) {
            body.remove(0, QString::fromUtf8(needle).size());
            while (!body.isEmpty() && body.front().isSpace()) body.remove(0, 1);
        }
    };
    stripPrefix(kEmojiDone);
    stripPrefix(kEmojiPlanned);
    stripPrefix(kEmojiInProgress);
    stripPrefix(kEmojiConsidered);
    if (body.startsWith(QStringLiteral("**"))) body.remove(0, 2);
    int closeBold = body.indexOf(QStringLiteral("**"));
    if (closeBold > 0 && closeBold < 80) body.truncate(closeBold);
    if (body.size() > 80) body.truncate(80);
    return body;
}

// Read CHANGELOG.md `[Unreleased]` block bullets — first 80 characters
// of each `^- ` or `^  - ` line, no leading whitespace, no leading
// markers. Stops at the next `^## ` heading.
QStringList readUnreleasedBullets(const QString &changelogPath) {
    QStringList out;
    QFile f(changelogPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    bool inBlock = false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QStringLiteral("## "))) {
            const bool isUnreleased = line.contains(
                QStringLiteral("[Unreleased]"), Qt::CaseInsensitive);
            inBlock = isUnreleased;
            continue;
        }
        if (!inBlock) continue;
        if (!line.startsWith(QStringLiteral("- ")) &&
            !line.startsWith(QStringLiteral("* "))) continue;
        QString body = line.mid(2).trimmed();
        // Drop common emphasis markers.
        body.remove(QStringLiteral("**"));
        if (body.size() > 80) body.truncate(80);
        if (!body.isEmpty()) out.append(body);
    }
    return out;
}

// `git log -n 5 --format=%s` from `repoRoot`. Best-effort — returns an
// empty list on any failure (no git in PATH, not a repo, etc.).
QStringList readRecentCommitSubjects(const QString &repoRoot) {
    QStringList out;
    QProcess git;
    git.setWorkingDirectory(repoRoot);
    git.start(QStringLiteral("git"),
              {QStringLiteral("log"), QStringLiteral("-n"), QStringLiteral("5"),
               QStringLiteral("--format=%s")});
    if (!git.waitForFinished(1500)) return out;
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0)
        return out;
    const QString stdoutStr = QString::fromUtf8(git.readAllStandardOutput());
    for (const QString &raw : stdoutStr.split('\n', Qt::SkipEmptyParts)) {
        const QString s = raw.trimmed();
        // Skip mechanical commits (release bumps, merges, reverts).
        static const QRegularExpression rxRelease(QStringLiteral("^\\d+\\.\\d+\\.\\d+:"));
        if (rxRelease.match(s).hasMatch()) continue;
        if (s.startsWith(QStringLiteral("Merge "))) continue;
        if (s.startsWith(QStringLiteral("Revert "))) continue;
        // Trim trailing parens/citations and take the meaningful slice.
        QString slice = s;
        const int dashPos = slice.indexOf(QStringLiteral(" — "));
        const int colonPos = slice.indexOf(QStringLiteral(": "));
        int cut = -1;
        if (dashPos > 0 && (colonPos < 0 || dashPos < colonPos)) cut = dashPos + 3;
        else if (colonPos > 0) cut = colonPos + 2;
        if (cut > 0 && cut < slice.size()) slice = slice.mid(cut);
        if (slice.size() > 80) slice.truncate(80);
        if (!slice.isEmpty()) out.append(slice);
    }
    return out;
}

}  // namespace

QStringList RoadmapDialog::collectCurrentBullets() const {
    QStringList out;
    if (!m_changelogPath.isEmpty()) out += readUnreleasedBullets(m_changelogPath);
    const QFileInfo fi(m_roadmapPath);
    out += readRecentCommitSubjects(fi.absolutePath());
    return out;
}

QVector<RoadmapDialog::TocEntry>
RoadmapDialog::extractToc(const QString &markdownText) {
    QVector<TocEntry> out;
    const QStringList lines = markdownText.split('\n');
    int idx = 0;
    for (const QString &raw : lines) {
        QString text;
        const int level = headingLevel(raw, &text);
        if (level <= 0) continue;
        TocEntry e;
        e.level = level;
        e.text = text;
        e.anchor = tocAnchorAt(idx++);
        out.push_back(e);
    }
    return out;
}

// Pure renderer. See spec for the parsing rules. Returns a self-
// contained HTML fragment ready for QTextBrowser::setHtml.
QString RoadmapDialog::renderHtml(const QString &markdownText,
                                  unsigned filter,
                                  const QStringList &currentBullets,
                                  const QString &themeName) {
    const Theme &th = Themes::byName(themeName);
    const QString currentColor =
        ClaudeTabIndicator::color(ClaudeTabIndicator::Glyph::ToolUse).name();
    const bool wantDone = (filter & ShowDone) != 0;
    const bool wantPlanned = (filter & ShowPlanned) != 0;
    const bool wantInProgress = (filter & ShowInProgress) != 0;
    const bool wantConsidered = (filter & ShowConsidered) != 0;
    const bool wantCurrent = (filter & ShowCurrent) != 0;

    // Pre-fuzzied signal phrases for substring matching.
    QStringList signalsFuzzy;
    signalsFuzzy.reserve(currentBullets.size());
    for (const QString &s : currentBullets) {
        const QString f = fuzzy(s);
        if (f.size() >= 6) signalsFuzzy.append(f);
    }

    auto isCurrent = [&](const QString &bulletBody) {
        if (signalsFuzzy.isEmpty()) return false;
        const QString fHay = fuzzy(bulletPayload(bulletBody));
        if (fHay.size() < 6) return false;
        for (const QString &s : signalsFuzzy) {
            if (fHay.contains(s) || s.contains(fHay)) return true;
        }
        return false;
    };

    QString html;
    html.reserve(markdownText.size() * 2);
    html += QStringLiteral(
        "<html><head><style>"
        "body{font-family:sans-serif;color:%1;}"
        "h1,h2,h3,h4{color:%2;font-weight:bold;}"
        "h1{font-size:18px;} h2{font-size:16px;}"
        "h3{font-size:14px;} h4{font-size:13px;}"
        "code{background:%3;padding:0 4px;border-radius:3px;}"
        "ul{margin-top:2px;margin-bottom:2px;}"
        "li{margin-bottom:4px;}"
        ".cur{border-left:4px solid %4;padding-left:8px;background:rgba(229,194,74,0.08);}"
        "table{border-collapse:collapse;}"
        "td,th{border:1px solid %5;padding:2px 6px;}"
        "</style></head><body>")
        .arg(th.textPrimary.name(),
             th.textPrimary.name(),
             th.bgSecondary.name(),
             currentColor,
             th.border.name());

    enum class BulletKind { Other, Done, Planned, InProgress, Considered };
    auto classify = [](const QString &body) {
        if (body.startsWith(QString::fromUtf8(kEmojiDone))) return BulletKind::Done;
        if (body.startsWith(QString::fromUtf8(kEmojiPlanned))) return BulletKind::Planned;
        if (body.startsWith(QString::fromUtf8(kEmojiInProgress))) return BulletKind::InProgress;
        if (body.startsWith(QString::fromUtf8(kEmojiConsidered))) return BulletKind::Considered;
        return BulletKind::Other;
    };

    const QStringList lines = markdownText.split('\n');
    bool inList = false;
    bool skipBlock = false;       // dropping a filtered-out bullet's continuation
    int headingIdx = 0;           // increments per emitted heading; matches extractToc

    auto closeListIfOpen = [&]() {
        if (inList) {
            html += QStringLiteral("</ul>");
            inList = false;
        }
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString &raw = lines[i];

        // Headings — always rendered, regardless of filters. Each
        // gets a `<a name="roadmap-toc-N">` anchor so the TOC sidebar
        // can scroll to it via QTextBrowser::scrollToAnchor.
        QString hText;
        if (const int level = headingLevel(raw, &hText); level > 0) {
            closeListIfOpen();
            skipBlock = false;
            const QString anchor = tocAnchorAt(headingIdx++);
            html += QStringLiteral("<a name=\"%1\"></a>").arg(anchor);
            html += QStringLiteral("<h%1>").arg(level)
                  + applyInline(hText)
                  + QStringLiteral("</h%1>").arg(level);
            continue;
        }

        // Markdown table rows — render as a <pre> block so the columns
        // line up. Coalesce consecutive `|` lines into one block.
        if (raw.startsWith(QStringLiteral("|"))) {
            closeListIfOpen();
            skipBlock = false;
            html += QStringLiteral("<pre style=\"font-family:monospace;\">");
            html += htmlEscape(raw);
            html += '\n';
            while (i + 1 < lines.size() && lines[i + 1].startsWith(QStringLiteral("|"))) {
                ++i;
                html += htmlEscape(lines[i]);
                html += '\n';
            }
            html += QStringLiteral("</pre>");
            continue;
        }

        // Top-level bullet.
        if (raw.startsWith(QStringLiteral("- ")) ||
            raw.startsWith(QStringLiteral("* "))) {
            const QString body = raw.mid(2);
            const BulletKind kind = classify(body);
            const bool current = isCurrent(body);
            // Inclusive-OR over enabled categories. Plain narration
            // bullets (Other) always render — they carry document
            // context, not status.
            const bool keep =
                (kind == BulletKind::Other) ||
                (kind == BulletKind::Done && wantDone) ||
                (kind == BulletKind::Planned && wantPlanned) ||
                (kind == BulletKind::InProgress && wantInProgress) ||
                (kind == BulletKind::Considered && wantConsidered) ||
                (current && wantCurrent);
            if (!keep) {
                skipBlock = true;
                continue;
            }
            skipBlock = false;
            if (!inList) {
                html += QStringLiteral("<ul>");
                inList = true;
            }
            const QString cls = current
                ? QStringLiteral(" class=\"cur\"") : QString();
            html += QStringLiteral("<li") + cls + QStringLiteral(">")
                  + applyInline(body);
            // The </li> is closed when we leave the bullet (next non-
            // continuation line). A continuation appends inline.
            continue;
        }

        // Continuation of a bullet (two-space indent or blank line).
        if (raw.startsWith(QStringLiteral("  ")) && inList) {
            if (skipBlock) continue;
            html += '\n' + applyInline(raw.trimmed());
            continue;
        }

        // Blank line — terminate the current bullet item / list.
        if (raw.trimmed().isEmpty()) {
            if (inList) html += QStringLiteral("</li>");
            closeListIfOpen();
            skipBlock = false;
            continue;
        }

        // Other prose — rendered as a paragraph.
        closeListIfOpen();
        skipBlock = false;
        html += QStringLiteral("<p>") + applyInline(raw) + QStringLiteral("</p>");
    }
    closeListIfOpen();

    html += QStringLiteral("</body></html>");
    return html;
}

RoadmapDialog::RoadmapDialog(const QString &roadmapPath,
                             const QString &themeName,
                             QWidget *parent)
    : QDialog(parent),
      m_roadmapPath(roadmapPath),
      m_themeName(themeName),
      m_lastHtml(std::make_shared<QString>()) {
    setWindowTitle(tr("Roadmap — %1").arg(QFileInfo(roadmapPath).fileName()));
    resize(900, 700);

    // Find a sibling CHANGELOG.md (case-insensitive) for the
    // current-work signal set.
    const QDir dir = QFileInfo(roadmapPath).absoluteDir();
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
        if (fi.fileName().compare(QStringLiteral("CHANGELOG.md"),
                                  Qt::CaseInsensitive) == 0) {
            m_changelogPath = fi.absoluteFilePath();
            break;
        }
    }

    auto *root = new QVBoxLayout(this);

    auto *filterRow = new QHBoxLayout();
    m_filterDone = new QCheckBox(tr("✅ Done"), this);
    m_filterDone->setObjectName(QStringLiteral("roadmap-filter-done"));
    m_filterDone->setChecked(true);
    m_filterPlanned = new QCheckBox(tr("📋 Planned"), this);
    m_filterPlanned->setObjectName(QStringLiteral("roadmap-filter-planned"));
    m_filterPlanned->setChecked(true);
    m_filterInProgress = new QCheckBox(tr("🚧 In progress"), this);
    m_filterInProgress->setObjectName(QStringLiteral("roadmap-filter-in-progress"));
    m_filterInProgress->setChecked(true);
    m_filterConsidered = new QCheckBox(tr("💭 Considered"), this);
    m_filterConsidered->setObjectName(QStringLiteral("roadmap-filter-considered"));
    m_filterConsidered->setChecked(true);
    m_filterCurrent = new QCheckBox(tr("Currently being tackled"), this);
    m_filterCurrent->setObjectName(QStringLiteral("roadmap-filter-current"));
    m_filterCurrent->setChecked(true);
    filterRow->addWidget(m_filterDone);
    filterRow->addWidget(m_filterPlanned);
    filterRow->addWidget(m_filterInProgress);
    filterRow->addWidget(m_filterConsidered);
    filterRow->addWidget(m_filterCurrent);
    filterRow->addStretch(1);
    root->addLayout(filterRow);

    // Body: TOC list (left) + rendered viewer (right) inside a
    // QSplitter so the user can resize the sidebar. QTextBrowser
    // (vs plain QTextEdit) for `scrollToAnchor` support — the TOC
    // entries jump to `<a name="roadmap-toc-N">` anchors emitted by
    // renderHtml.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("roadmap-splitter"));

    m_toc = new QListWidget(splitter);
    m_toc->setObjectName(QStringLiteral("roadmap-toc"));
    m_toc->setUniformItemSizes(false);
    m_toc->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_toc->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toc->setMinimumWidth(180);
    splitter->addWidget(m_toc);

    m_viewer = new QTextBrowser(splitter);
    m_viewer->setReadOnly(true);
    // Internal anchors only — disable navigation on `<a href>` so a
    // stray markdown link can't replace the document.
    m_viewer->setOpenLinks(false);
    m_viewer->setOpenExternalLinks(false);
    splitter->addWidget(m_viewer);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 680});
    root->addWidget(splitter, 1);

    connect(m_toc, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) {
                if (!item || !m_viewer) return;
                const QString anchor =
                    item->data(Qt::UserRole).toString();
                if (anchor.isEmpty()) return;
                m_viewer->scrollToAnchor(anchor);
            });
    connect(m_toc, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                if (!item || !m_viewer) return;
                const QString anchor =
                    item->data(Qt::UserRole).toString();
                if (anchor.isEmpty()) return;
                m_viewer->scrollToAnchor(anchor);
            });

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
    btns->setObjectName(QStringLiteral("roadmap-buttons"));
    // Connect the Close button two ways. The role-based `rejected`
    // signal is the documented path, but in practice user reports
    // showed the standard "Close" button doing nothing on click in
    // some Qt 6 builds — connecting `clicked` directly to the
    // QPushButton bypasses any role-dispatch surprise.
    connect(btns, &QDialogButtonBox::rejected, this, [this]() { close(); });
    if (auto *closeBtn = btns->button(QDialogButtonBox::Close)) {
        closeBtn->setObjectName(QStringLiteral("roadmap-close-button"));
        connect(closeBtn, &QAbstractButton::clicked,
                this, [this]() { close(); });
    }
    root->addWidget(btns);

    // Live-update plumbing: 200 ms debounce shared with sibling
    // dialogs (review-changes, bg-tasks).
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(200);
    connect(&m_debounce, &QTimer::timeout, this, &RoadmapDialog::rebuild);

    m_watcher.addPath(roadmapPath);
    if (!m_changelogPath.isEmpty()) m_watcher.addPath(m_changelogPath);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged,
            this, &RoadmapDialog::scheduleRebuild);

    connect(m_filterDone, &QCheckBox::toggled, this, &RoadmapDialog::rebuild);
    connect(m_filterPlanned, &QCheckBox::toggled, this, &RoadmapDialog::rebuild);
    connect(m_filterInProgress, &QCheckBox::toggled, this, &RoadmapDialog::rebuild);
    connect(m_filterConsidered, &QCheckBox::toggled, this, &RoadmapDialog::rebuild);
    connect(m_filterCurrent, &QCheckBox::toggled, this, &RoadmapDialog::rebuild);

    rebuild();
}

RoadmapDialog::~RoadmapDialog() = default;

void RoadmapDialog::scheduleRebuild() {
    // Re-add the watch — QFileSystemWatcher drops watches after atomic
    // editor saves. Same dance as the review-changes / bg-tasks
    // dialogs.
    if (!m_watcher.files().contains(m_roadmapPath))
        m_watcher.addPath(m_roadmapPath);
    if (!m_changelogPath.isEmpty() &&
        !m_watcher.files().contains(m_changelogPath)) {
        m_watcher.addPath(m_changelogPath);
    }
    m_debounce.start();
}

void RoadmapDialog::rebuild() {
    if (!m_viewer) return;

    QFile f(m_roadmapPath);
    QString markdown;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        markdown = QString::fromUtf8(f.readAll());
    }

    unsigned filter = 0;
    if (m_filterDone && m_filterDone->isChecked()) filter |= ShowDone;
    if (m_filterPlanned && m_filterPlanned->isChecked()) filter |= ShowPlanned;
    if (m_filterInProgress && m_filterInProgress->isChecked()) filter |= ShowInProgress;
    if (m_filterConsidered && m_filterConsidered->isChecked()) filter |= ShowConsidered;
    if (m_filterCurrent && m_filterCurrent->isChecked()) filter |= ShowCurrent;

    const QStringList signals_ = collectCurrentBullets();
    const QString html = renderHtml(markdown, filter, signals_, m_themeName);

    if (m_lastHtml && *m_lastHtml == html) return;  // skip identical re-render

    auto *vbar = m_viewer->verticalScrollBar();
    const int saved = vbar ? vbar->value() : 0;
    const int oldMax = vbar ? vbar->maximum() : 0;
    const bool wasAtBottom = vbar && (oldMax - saved <= 4);

    m_viewer->setHtml(html);
    if (m_lastHtml) *m_lastHtml = html;

    if (vbar) {
        if (wasAtBottom) vbar->setValue(vbar->maximum());
        else vbar->setValue(qMin(saved, vbar->maximum()));
    }

    // Refresh the TOC sidebar from the same markdown so the
    // anchor indices line up with what renderHtml just emitted.
    if (m_toc) {
        const QString prevAnchor =
            m_toc->currentItem()
                ? m_toc->currentItem()->data(Qt::UserRole).toString()
                : QString();
        m_toc->clear();
        const QVector<TocEntry> entries = extractToc(markdown);
        for (const TocEntry &e : entries) {
            // Indent by level — flat QListWidget shows hierarchy via
            // leading spaces (two per level above 1).
            QString prefix;
            for (int i = 1; i < e.level; ++i) prefix.append(QStringLiteral("  "));
            auto *item = new QListWidgetItem(prefix + e.text, m_toc);
            item->setData(Qt::UserRole, e.anchor);
            QFont itemFont = item->font();
            itemFont.setBold(e.level == 1);
            item->setFont(itemFont);
            if (e.anchor == prevAnchor) m_toc->setCurrentItem(item);
        }
    }
}
