#pragma once

// Status-bar Roadmap viewer.
// User request 2026-04-27. See `tests/features/roadmap_viewer/spec.md`.
//
// Surface: a status-bar button labelled "Roadmap" appears whenever the
// active terminal tab's working directory contains a ROADMAP.md
// (case-insensitive). Clicking opens this dialog rendering the file
// with filter checkboxes (Done / Planned / Current work) and a
// current-work highlight derived from the local CHANGELOG.md
// [Unreleased] block + recent git commit subjects.
//
// Live updates: a QFileSystemWatcher on the canonical roadmap path and
// the local CHANGELOG.md re-render the body on change events. Update
// path mirrors the 0.7.37 / 0.7.38 scroll-preservation pattern shared
// with ReviewChangesDialog and ClaudeBgTasksDialog (capture vbar
// before setHtml, restore with qMin clamp; was-at-bottom pin).
//
// Renderer: `RoadmapDialog::renderHtml` is a static pure helper so
// tests can drive it without spinning a Qt widget tree. The dialog's
// rebuild() chain composes signal-set discovery + parsing + HTML
// emission into the QTextEdit.

#include <QDialog>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <memory>

class QCheckBox;
class QListWidget;
class QTextBrowser;

class RoadmapDialog : public QDialog {
    Q_OBJECT

public:
    // Filter mask. Each bit is a *peer* category checkbox in the
    // dialog header. A bullet renders iff at least one of its
    // category memberships is enabled (inclusive OR). Plain
    // narration bullets (no status emoji, no current-work match)
    // always render — they carry document context, not status.
    //
    // ShowCurrent is an additive category: a bullet's "currently
    // being tackled" membership is derived from CHANGELOG.md
    // [Unreleased] + recent commit subjects. Checked alone (with
    // everything else off), the dialog shows just in-flight items.
    // Highlighting on matched bullets is independent of the
    // filter — see renderHtml.
    enum Filter : unsigned {
        ShowDone        = 1u << 0,  // ✅
        ShowPlanned     = 1u << 1,  // 📋
        ShowInProgress  = 1u << 2,  // 🚧
        ShowConsidered  = 1u << 3,  // 💭
        ShowCurrent     = 1u << 4,  // CHANGELOG/commits-derived
    };

    // `roadmapPath` is the canonical absolute path to the file. The
    // owning project root for CHANGELOG / git lookups is the directory
    // containing `roadmapPath`.
    RoadmapDialog(const QString &roadmapPath,
                  const QString &themeName,
                  QWidget *parent = nullptr);
    ~RoadmapDialog() override;

    // Pure helper: render `markdownText` to HTML respecting the
    // filter mask and current-work signal phrases. `currentBullets`
    // is a set of substrings; any bullet whose first 80 characters
    // (post-emoji-strip, normalised) contains one of them is treated
    // as current work. Each emitted heading is preceded by an HTML
    // anchor of the form `<a name="roadmap-toc-N"></a>` where N
    // matches the corresponding entry in `extractToc(markdownText)`
    // — that lets the TOC sidebar use `QTextBrowser::scrollToAnchor`
    // to jump to a heading without re-parsing the rendered document.
    static QString renderHtml(const QString &markdownText,
                              unsigned filter,
                              const QStringList &currentBullets,
                              const QString &themeName);

    // Heading entry surfaced in the TOC sidebar. `level` is 1..4,
    // `text` is the raw heading text post-`#` strip (no inline
    // expansion), `anchor` is the same name renderHtml emits.
    struct TocEntry {
        int level;
        QString text;
        QString anchor;
    };

    // Pure helper: walk `markdownText` and return its `# `..`#### `
    // headings in document order. Walk shape mirrors renderHtml's
    // heading detection so the indices line up.
    static QVector<TocEntry> extractToc(const QString &markdownText);

private slots:
    void rebuild();

private:
    void scheduleRebuild();
    QStringList collectCurrentBullets() const;

    QString m_roadmapPath;
    QString m_changelogPath;
    QString m_themeName;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QPointer<QTextBrowser> m_viewer;
    QPointer<QListWidget> m_toc;
    QPointer<QCheckBox> m_filterDone;
    QPointer<QCheckBox> m_filterPlanned;
    QPointer<QCheckBox> m_filterInProgress;
    QPointer<QCheckBox> m_filterConsidered;
    QPointer<QCheckBox> m_filterCurrent;
    std::shared_ptr<QString> m_lastHtml;
};
