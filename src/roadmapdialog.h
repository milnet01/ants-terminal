#pragma once

// Status-bar Roadmap viewer.
// User request 2026-04-27 (initial); ANTS-1100 redesign 2026-04-30.
// See `tests/features/roadmap_viewer/spec.md` (original) +
// `tests/features/roadmap_viewer_tabs/spec.md` (ANTS-1100).
//
// Surface: a status-bar button labelled "Roadmap" appears whenever the
// active terminal tab's working directory contains a ROADMAP.md
// (case-insensitive). Clicking opens this dialog. The header has, in
// vertical order: a faceted tab strip (Full / History / Current /
// Next / Far Future / Custom — each a (filter, sort) preset), a
// debounced search box (substring + `id:NNNN` shorthand), then the
// peer category checkboxes (✅ Done / 📋 Planned / 🚧 In progress /
// 💭 Considered / Currently being tackled). The body splits a TOC
// sidebar from the rendered viewer. Current-work bullets get a
// border-left highlight derived from the local CHANGELOG.md
// [Unreleased] block + recent git commit subjects.
//
// Live updates: a QFileSystemWatcher on the canonical roadmap path and
// the local CHANGELOG.md re-render the body on change events. Update
// path mirrors the 0.7.37 / 0.7.38 scroll-preservation pattern shared
// with ReviewChangesDialog and ClaudeBgTasksDialog (capture vbar
// before setHtml, restore with qMin clamp; was-at-bottom pin).
//
// Geometry: persists user resize via `Config::roadmapDialogGeometry`
// (saveGeometry → base64 → restoreGeometry round-trip); default
// 1200x800 on first open, minimum 720x480.
//
// Renderer: `RoadmapDialog::renderHtml` is a static pure helper so
// tests can drive it without spinning a Qt widget tree. The dialog's
// rebuild() chain composes signal-set discovery + parsing + HTML
// emission into the QTextBrowser.

#include <QDialog>
#include <QFileSystemWatcher>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <memory>

class Config;
class QCheckBox;
class QLineEdit;
class QListWidget;
class QTabBar;
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

    // ANTS-1100: Faceted preset tabs above the existing checkbox row.
    // Each preset is a (filter, sort) tuple. `Custom` is the implicit
    // tab the dialog switches to when the user diverges from any
    // named preset via the checkbox row.
    enum class Preset {
        Full,        // every Show* bit; document order
        History,     // ShowDone only; descending chronological
        Current,     // ShowInProgress | ShowCurrent; document order
        Next,        // ShowPlanned only; document order
        FarFuture,   // ShowConsidered only; document order
        Custom,      // user-tuned via checkboxes — no preset match
    };

    enum class SortOrder {
        Document,                 // top-to-bottom as authored
        DescendingChronological,  // top-level `## ` sections reversed
    };

    // Pure helpers — preset → mask + preset → sort. `presetMatching`
    // is the inverse: given a (filter, sort) pair, return the preset
    // that produced it, or `Custom` if no named preset matches.
    static unsigned filterFor(Preset p);
    static SortOrder sortFor(Preset p);
    static Preset presetMatching(unsigned filter, SortOrder sort);

    // `roadmapPath` is the canonical absolute path to the file. The
    // owning project root for CHANGELOG / git lookups is the directory
    // containing `roadmapPath`. `cfg` (optional) is consulted for the
    // dialog's persisted geometry (`Config::roadmapDialogGeometry`).
    RoadmapDialog(const QString &roadmapPath,
                  const QString &themeName,
                  QWidget *parent = nullptr,
                  Config *cfg = nullptr);
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
    //
    // `sortOrder` controls section ordering before render — only
    // `DescendingChronological` reorders, otherwise pass-through.
    // `searchPredicate` is a case-insensitive substring filter
    // applied to each top-level bullet's body; empty disables
    // filtering. The `id:NNNN` shorthand matches a bullet whose
    // body contains `[ANTS-NNNN]` regardless of headline content.
    // `kindFilter` (ANTS-1106) narrows by `Kind:` line value.
    // Empty set = no filter (current behaviour). Non-empty set =
    // only bullets whose Kind: matches one of the entries render;
    // bullets with no Kind: line are excluded under a non-empty
    // filter.
    static QString renderHtml(const QString &markdownText,
                              unsigned filter,
                              const QStringList &currentBullets,
                              const QString &themeName,
                              SortOrder sortOrder = SortOrder::Document,
                              const QString &searchPredicate = QString(),
                              const QSet<QString> &kindFilter = {});

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

    // Bullet record surfaced via the `roadmap-query` IPC verb (ANTS-1117).
    // One entry per top-level status-emoji-prefixed bullet in document
    // order; plain narration bullets without an emoji are omitted (they
    // don't have stable IDs and so are out of contract). See
    // `docs/specs/ANTS-1117.md` § Acceptance criteria.
    struct BulletRecord {
        QString id;          // ANTS-NNNN; empty if no `[ANTS-NNNN]` token
        QString status;      // "✅" | "🚧" | "📋" | "💭"
        QString headline;    // first **bold** chunk after the emoji (≤ 120 chars)
        QString kind;        // value from `Kind:` line; "" if absent
        QStringList lanes;   // values from `Lanes:` line; [] if absent
    };

    // Pure helper: parse `markdownText` into top-level status-emoji
    // bullets. Mirrors the renderHtml top-level-bullet detection so the
    // two stay in lock-step. Result is read-only; used by the
    // `roadmap-query` IPC verb to feed Claude a structured snapshot
    // without re-burning the file content as tokens.
    static QVector<BulletRecord> parseBullets(const QString &markdownText);

    // ANTS-1125: split-by-version archive helpers. Public-static so the
    // feature test (`tests/features/roadmap_viewer_archive/`) can drive
    // them without instantiating a dialog.
    //
    // `archiveDirFor(roadmapPath)` resolves the canonical
    // (symlink-followed) `m_roadmapPath`, then returns
    // `<dirname>/docs/roadmap/` iff that path exists, is a directory,
    // and is readable. Returns the empty string in every other case
    // (missing, regular file, broken symlink, symlink cycle,
    // unreadable, non-directory). Spec INVs 1, 1a.
    static QString archiveDirFor(const QString &roadmapPath);

    // `loadMarkdown(roadmapPath, includeArchive)` reads the file at
    // `roadmapPath` (capped at 8 MiB per `read()`) and, if
    // `includeArchive` is true and `archiveDirFor` returns non-empty,
    // appends each `*.md` archive matching `^[0-9]+\.[0-9]+\.md$`
    // (case-sensitive), sorted numerically descending by the
    // `(major, minor)` integer tuple parsed from the filename. Each
    // archive is preceded by a thematic-break + HTML-comment sentinel
    // separator. Total assembled-buffer cap is 64 MiB; once exceeded
    // the loader emits a single truncation sentinel and stops.
    // Spec INVs 2, 3, 3b, 4, 4a, 5, 5a, 11.
    static QString loadMarkdown(const QString &roadmapPath, bool includeArchive);

    // `shouldLoadHistory(activePreset, searchText)` returns true iff
    // the dialog should pull archives for the next render. Triggers:
    // `Preset::History` enumerator OR a non-empty trimmed search
    // predicate. Spec INV-6.
    static bool shouldLoadHistory(Preset activePreset,
                                  const QString &searchText);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void rebuild();

private:
    void scheduleRebuild();
    void applyPreset(Preset p);
    void onCheckboxToggled();
    QStringList collectCurrentBullets() const;

    // ANTS-1150: persist the active preset to Config. Called from
    // BOTH applyPreset (named-preset path) and onCheckboxToggled
    // (Custom-divergence path) so m_activePreset's two write sites
    // both round-trip through disk. Switch-on-enum guarantees
    // compiler -Wswitch-enum coverage of future Preset additions.
    void persistActivePreset(Preset p);

    // ANTS-1125 instance wrappers — bind the active dialog state
    // (`m_roadmapPath`, `m_activePreset`, `m_searchBox->text()`) to
    // the public-static helpers above so `rebuild()` can call them
    // without threading the state through manually.
    QString historyArchiveDir() const { return archiveDirFor(m_roadmapPath); }
    QString loadRoadmapMarkdown(bool includeArchive) const {
        return loadMarkdown(m_roadmapPath, includeArchive);
    }
    bool wantsHistoryLoad() const;

    QString m_roadmapPath;
    QString m_changelogPath;
    QString m_themeName;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_searchDebounce;
    QPointer<QTextBrowser> m_viewer;
    QPointer<QListWidget> m_toc;
    QPointer<QTabBar> m_tabs;
    QPointer<QLineEdit> m_searchBox;
    QPointer<QCheckBox> m_filterDone;
    QPointer<QCheckBox> m_filterPlanned;
    QPointer<QCheckBox> m_filterInProgress;
    QPointer<QCheckBox> m_filterConsidered;
    QPointer<QCheckBox> m_filterCurrent;
    // ANTS-1106 — Kind-faceted secondary filter. Empty set = no
    // narrowing (current behaviour). Populated from the Kind row's
    // checkboxes; passed through to renderHtml on every refresh().
    QSet<QString> m_kindFilter;
    // ANTS-1150 — keyed by KindEntry value (matches m_kindFilter
    // entries). Populated during ctor's Kind-row build loop;
    // re-iterated during the persisted-Kind-filter restore so we
    // don't have to walk findChildren<QCheckBox*>() and parse
    // objectName prefixes. Same-life as the dialog.
    QHash<QString, QCheckBox *> m_kindCheckboxes;
    std::shared_ptr<QString> m_lastHtml;
    Config *m_config = nullptr;
    SortOrder m_sortOrder = SortOrder::Document;
    // ANTS-1125 INV-6: track the active preset by enumerator so
    // wantsHistoryLoad() doesn't depend on a tab-index literal.
    // Set in applyPreset(); read in wantsHistoryLoad().
    Preset m_activePreset = Preset::Full;
    bool m_suppressCheckboxSignal = false;
    bool m_suppressTabSignal = false;
};
