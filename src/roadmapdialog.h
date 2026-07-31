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
// Geometry: persists the user's resize SIZE only (dialogs.md D3) via
// DialogChrome's "RoadmapDialog" sizeKey; the dialog re-centers over
// the parent on every open (D4) rather than restoring a window
// position. Default 1200x800 on first open, minimum 720x480.
// (ANTS-2012 migrated this off the legacy saveGeometry blob.)
//
// Renderer: the dialog's rebuild() chain uses `renderCardsHtml` (the
// v2 card renderer) to compose signal-set discovery + parsing + HTML
// emission into the QTextBrowser. `RoadmapDialog::renderHtml` is the
// v1 markdown→HTML helper; it has NO production callers (ANTS-1747 —
// the `roadmap-query` IPC verb uses parseBullets + RoadmapIndex, not
// this renderer). It is retained as a static pure helper because its
// test suite locks the shared filter/sort/anchor/TOC semantics that
// renderCardsHtml also depends on; tests drive it without a widget
// tree.

#include "roadmapparse.h"   // ANTS-3764 — BulletRecord + the reader
#include <QDialog>
#include <QFileSystemWatcher>
#include <QHash>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <memory>

class Config;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QTabBar;
class QTextBrowser;
class RoadmapShortcutsDialog;

// ANTS-1236 — accessor for the file-scope `kRoadmapShortcuts[]` table
// living in `roadmapdialog.cpp`. Returns one (keys, action) pair per
// row in declaration order. The action column is resolved through
// `RoadmapDialog::tr()` so a future translation pass works; the keys
// column is pass-through (key glyphs are layout-invariant). See spec
// `docs/specs/ANTS-1236.md` § 3.c.
QVector<QPair<QString, QString>> roadmapShortcutRows();

// ANTS-1237 — render an age in seconds as a "today" / "yesterday" /
// "Nd ago" / "Nw ago" / "Nmo ago" / "Ny ago" string. Lives at file
// scope in `roadmapdialog.cpp` (outside the anonymous namespace) so
// the feature test can link against it. See spec § 2.f.5.
QString humanAge(qint64 ageSeconds);

class RoadmapDialog : public QDialog {
    Q_OBJECT

public:
    // ANTS-1276 — anchor-target validator (exposed static so the
    // feature test can exercise it without a dialog instance). The
    // expanded-state sets persisted to Config only ever hold a roadmap
    // item ID or a section slug; reject anything outside [A-Za-z0-9_-]
    // or implausibly long so a hostile ROADMAP.md anchor can't inject
    // arbitrary strings into the user's config.
    static bool isValidAnchorTarget(const QString &target);

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

    // ANTS-1238: card-density tier. Selects the per-CSS-class px
    // values + vertical-padding scale that `renderCardsHtml` emits.
    // Cozy preserves the pre-1238 byte-equal output. Spec:
    // docs/specs/ANTS-1238.md § 2.f.
    enum class Density {
        // Per-tier px live in kDensityTable (roadmapdialog.cpp:157);
        // ANTS-2211 floored the Compact/Cozy meta+label rows at 11/12px
        // (was a 9px floor). Values below are body/h1 landmarks only.
        Compact,      // -2px tier; body 11 / h1 14; meta+label floor 11px
        Cozy,         // current default; body 13 / h1 16; meta+label 12px
        Comfortable,  // +2px tier; body 15 / h1 18; label held at 12px
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
    // dialog's persisted UI state (expanded items/sections, table
    // sections, filters); the persisted SIZE is handled by
    // DialogChrome via the "RoadmapDialog" sizeKey (ANTS-2012).
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

    // ANTS-1154 v2 card-renderer state, packed into one struct so the
    // new `renderCardsHtml` signature stays manageable. Default-
    // constructed values produce a "first open, no persisted state"
    // render — all sections collapsed, no expanded cards, no table
    // sections, no shipped-date cache. Declared as a forward-friendly
    // nested type without default member initializers (clang's nested-
    // class rules forbid initializing `Preset activePreset = Preset::Full`
    // inline before the enclosing class definition closes — set the
    // value at construction or via explicit assignment instead).
    struct CardRenderOptions {
        Preset activePreset;
        QSet<QString> expandedItems;
        QSet<QString> expandedSections;
        QSet<QString> tableSections;
        // ANTS-NNNN → shipped date (parsed from CHANGELOG ## [X.Y.Z] —
        // YYYY-MM-DD blocks). Populated by the dialog before render;
        // renderCardsHtml just looks up.
        QHash<QString, QString> shippedDates;
        // ANTS-1237 — ANTS-NNNN → Unix author-time of the most
        // recent touch to that bullet's block in ROADMAP.md, per
        // `git blame`. Populated by the dialog before render;
        // renderCardsHtml renders the human-readable string on
        // 🚧 cards only.
        QHash<QString, qint64> lastTouchDates;

        // ANTS-1238 — selects the per-CSS-class px values
        // emitted by renderCardsHtml. Default-constructed
        // CardRenderOptions{} preserves the pre-1238 byte-equal
        // cozy output (INV-1).
        Density density;

        CardRenderOptions() : activePreset(Preset::Full),
                              density(Density::Cozy) {}
    };

    // ANTS-1154 — new card-style renderer. Walks `markdownText` and
    // emits an HTML document where each top-level status-emoji bullet
    // becomes a `<div class="rm-card" id="rm-ANTS-NNNN">` block with
    // state icon, type chip, layman/headline summary, and meta row
    // (ID + shipped date for ✅ items). Section headings (`##`/`###`)
    // emit collapsible headers with `<span class="rm-section-counts">`
    // chips. Click-to-toggle anchors use the `ants://expand/` /
    // `ants://collapse/` / `ants://expand-section/` /
    // `ants://collapse-section/` / `ants://table/` URL schemes,
    // handled by the dialog's anchorClicked slot.
    //
    // Tab-relevance: on every preset except `Full`, prose narration
    // bullets (no status emoji) and section-intro paragraphs are
    // dropped (INV-11); section headers with zero visible bullets
    // under the active filter are suppressed (INV-12).
    //
    // Defaults preserve the parser's existing filter/search/Kind
    // semantics — pass identical arguments to renderCardsHtml as
    // renderHtml plus a CardRenderOptions value.
    static QString renderCardsHtml(const QString &markdownText,
                                   unsigned filter,
                                   const QStringList &currentBullets,
                                   const QString &themeName,
                                   SortOrder sortOrder = SortOrder::Document,
                                   const QString &searchPredicate = QString(),
                                   const QSet<QString> &kindFilter = {},
                                   const CardRenderOptions &opts = {});

    // ANTS-1154 — build the `shippedDates` map from a CHANGELOG.md
    // file by walking `^## [X.Y.Z] — YYYY-MM-DD` headings and
    // attributing every `[ANTS-NNNN]` token in the section body to
    // that date. Returns empty map on read failure (graceful — cards
    // just don't show a date row). Cache-keyed by file mtime is the
    // caller's responsibility.
    static QHash<QString, QString>
    parseShippedDates(const QString &changelogPath);

    // ANTS-1237 — return ANTS-NNNN → MAX(author-time) over the
    // bullet's block (the `- 🚧 [ANTS-NNNN]` line + every
    // continuation line until blank/next-bullet/EOF). Spawns
    // `git blame --line-porcelain` once in roadmapPath's dir;
    // returns an empty hash on any failure (not a git repo, file
    // not tracked, git not on PATH, file does not exist, empty
    // blame output). See spec `docs/specs/ANTS-1237.md` § 3.b.2.
    static QHash<QString, qint64>
    parseLastTouchDates(const QString &roadmapPath);

    // ANTS-1237 — refresh m_lastTouchDates from m_roadmapPath when
    // its mtime has changed since the last call. Public (diverges
    // from `refreshShippedDatesIfStale` which is private) so the
    // feature test can drive it directly. Idempotent and cheap
    // when mtime is unchanged. See spec § 3.a.3.
    void refreshLastTouchDatesIfStale();

    // ANTS-1237 INV-5 test hook — returns the file mtime
    // (ms-resolution) that last triggered a parseLastTouchDates
    // refresh. -1 before the first refresh.
    qint64 lastTouchDatesMtime() const noexcept {
        return m_lastTouchDatesMtime;
    }

    // ANTS-1235 — return the screen-reader-readable label
    // ("shipped" / "in progress" / "planned" / "considered") for one
    // of the four status emoji constants. Returns an empty QString
    // if `emoji` is not a recognised status glyph. Lookup is a
    // four-entry linear scan over the file-scope `kStatusLabels`
    // table; tr()-resolved at call time so future translations work.
    static QString statusAccessibleLabel(const QString &emoji);

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

    // ANTS-3764 — the record and the parser moved to RoadmapParse
    // (src/roadmapparse.{h,cpp}, ants_core_lib) so the headless roadmap
    // migration can share the one reader instead of linking Widgets or
    // growing a second parser. The alias keeps every existing
    // `RoadmapDialog::BulletRecord` call site compiling unchanged; the field
    // documentation lives with the struct in roadmapparse.h.
    using BulletRecord = RoadmapParse::BulletRecord;

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

    // ANTS-1264 (spec ANTS-1154 §4.5, INV-13) — scroll-position
    // persistence. The dialog records the topmost visible card per tab on
    // close and restores it on the next open. The anchor is ID-keyed (not
    // a raw pixel offset) so it survives roadmap edits between sessions.
    struct ScrollAnchor {
        QString sectionSlug;  // section of the captured card (fallback key)
        QString id;           // captured card id (`<PREFIX>-NNNN`)
        int offsetPx = 0;     // pixels the card had scrolled above the top
    };

    // Outcome of resolving a saved ScrollAnchor against freshly-rendered
    // content. Card: the id still exists, restore to it + offset. Section:
    // the id is gone but its section survives, scroll to that section.
    // Top: neither survives, leave at the top.
    struct ScrollTarget {
        enum Kind { Top, Section, Card };
        Kind kind = Top;
        QString id;           // Card only
        QString sectionSlug;  // Section only
        int offsetPx = 0;     // Card only
    };

    // Pure + static (the feature test drives it without a dialog) — decide
    // what to scroll to from a saved anchor and the ids / section slugs
    // present in the freshly-rendered document. Implements the INV-13
    // three-case resolver: card → section → top. Empty saved fields are
    // treated as "not present".
    static ScrollTarget resolveScrollAnchor(const ScrollAnchor &saved,
                                            const QSet<QString> &presentIds,
                                            const QSet<QString> &presentSlugs);

protected:
    void closeEvent(QCloseEvent *event) override;
    // ANTS-1264 — restore the persisted scroll anchor on the first show,
    // once the viewport has a real size (a ctor-time restore would clamp
    // to a zero-height scrollbar). Deferred one event-loop turn so the
    // QTextBrowser layout has settled.
    void showEvent(QShowEvent *event) override;
    // ANTS-1236 — open / toggle the keyboard-shortcut cheatsheet on
    // `?` (Shift+/), gated by §3.d's search-box-focus guard so the
    // user can type `?` into the substring filter. Layout-robust:
    // matches `event->text() == "?"` rather than Key_Question so
    // AltGr-on-Polish / dead-key sequences still hit it.
    // ANTS-1234 — extends the override to also handle `/` (focus
    // search box) by the same layout-robust path.
    void keyPressEvent(QKeyEvent *event) override;
    // ANTS-1234 — installed as event filter on m_searchBox so Esc on
    // the focused search predicate clears + blurs instead of bubbling
    // to QDialog::reject. Falls through for every non-Escape key.
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void rebuild();
    // ANTS-1154: handle clicks on the card / section toggle anchors
    // emitted by renderCardsHtml. URL scheme `ants://<verb>/<slug>`
    // where verb is one of {expand, collapse, expand-section,
    // collapse-section, table}.
    void handleAnchorClicked(const QUrl &link);
    // ANTS-1154 — custom context menu on the QTextBrowser. The default
    // context menu Qt provides parents the popup to the viewer widget,
    // which on Wayland (+ frameless translucent parent on this stack)
    // can desync — the popup shows but Copy is a no-op and outside-
    // clicks don't dismiss it. Same family of bugs as QTBUG-79126. We
    // build the menu ourselves parented to the dialog top-level.
    void showViewerContextMenu(const QPoint &pos);

private:
    void scheduleRebuild();
    void applyPreset(Preset p);
    void onCheckboxToggled();
    // ANTS-1234 — focus m_searchBox and select any pre-existing text
    // (so the next keystroke replaces it). Called from keyPressEvent
    // when `/` arrives without the search box already focused.
    void focusSearchBox();
    QStringList collectCurrentBullets() const;
    // ANTS-1154 — refresh m_shippedDates from m_changelogPath when its
    // mtime has changed since the last call. No-op if path empty.
    void refreshShippedDatesIfStale();

    // ANTS-1264 — capture the topmost visible card to
    // Config::roadmapScrollAnchors (keyed by the active preset tab) on
    // close; restore it on first show. Both no-op without a Config or a
    // live viewer. See spec ANTS-1154 §4.5.
    void captureScrollAnchor();
    void restoreScrollAnchor();

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
    // ANTS-1242 — custom frameless title bar so the dialog's chrome
    // honours the active theme. KWin draws the standard title bar
    // from the system colour scheme and ignores Qt's QPalette, so a
    // plain QDialog title bar always looked grey against a navy /
    // dracula bgPrimary. Reuses the main-window TitleBar widget.
    QPointer<class TitleBar> m_titleBar;
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
    // ANTS-1264 — guard so the deferred scroll-anchor restore fires only
    // on the first show (the dialog is WA_DeleteOnClose, so this is
    // belt-and-braces against a re-show).
    bool m_scrollRestored = false;
    // ANTS-1154 v2 card-renderer in-memory state. Loaded from Config
    // in the ctor, mutated by handleAnchorClicked, persisted back to
    // Config in closeEvent.
    QSet<QString> m_expandedItems;
    QSet<QString> m_expandedSections;
    QSet<QString> m_tableSections;
    QHash<QString, QString> m_shippedDates;
    qint64 m_shippedDatesMtime = -1;
    // ANTS-1237 — last-touch (git author-time) per ANTS-NNNN.
    // Refreshed by refreshLastTouchDatesIfStale() when m_roadmapPath
    // mtime changes. Default-constructed empty; mtime -1 before
    // first refresh.
    QHash<QString, qint64> m_lastTouchDates;
    qint64 m_lastTouchDatesMtime = -1;
    // ANTS-2012 — collectCurrentBullets() shells out to `git log` (blocking).
    // rebuild() runs per search keystroke, so cache the external signals with
    // a short TTL to kill the per-keystroke git jank (mutable: filled from a
    // const getter).
    mutable QStringList m_externalSignalsCache;
    mutable qint64 m_externalSignalsCacheMs = 0;
    // ANTS-1238 — selected card-density tier. Default Cozy preserves
    // pre-1238 byte-equal rendering (INV-1). Loaded from
    // Config::roadmapDensity() in the ctor; written back on every
    // QComboBox::currentIndexChanged from the density combo.
    Density m_density = Density::Cozy;
    QPointer<QComboBox> m_densityCombo;
    // ANTS-1236 — lazily-constructed cheatsheet overlay. Held by
    // QPointer so the parent's child-destruction sets us back to
    // null on close. INV-6 contract: reuse the same instance across
    // re-opens.
    QPointer<RoadmapShortcutsDialog> m_shortcutsDialog;
};
