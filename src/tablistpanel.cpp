#include "tablistpanel.h"

#include <QCollator>
#include <QHeaderView>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kPageRole     = Qt::UserRole + 1;
constexpr int kGlyphRole    = Qt::UserRole + 2;  // tab rows only
constexpr int kTabColorRole = Qt::UserRole + 3;  // tab rows only

QColor inkOn(const QColor &fill) {
    const QColor black(Qt::black), white(Qt::white);
    return ClaudeTabIndicator::contrastRatio(black, fill)
                   >= ClaudeTabIndicator::contrastRatio(white, fill)
               ? black
               : white;
}

// ANTS-5650 — paints a tab row whole, so the app style sheet's item rules
// (a hovered row's fill, a selected row's text colour) never reach it.
// Group headers keep the default painting.
class TabRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setBackground(const QColor &background) { m_bg = background; }

    void paint(QPainter *p, const QStyleOptionViewItem &opt,
               const QModelIndex &index) const override {
        if (!index.data(kGlyphRole).isValid()) {
            QStyledItemDelegate::paint(p, opt, index);
            return;
        }
        const auto glyph = static_cast<ClaudeTabIndicator::Glyph>(index.data(kGlyphRole).toInt());
        const QColor bg = m_bg.isValid() ? m_bg : opt.palette.color(QPalette::Base);
        const bool current = opt.state & QStyle::State_Selected;
        const auto rp = TabListPanel::rowPaint(glyph, bg, opt.state & QStyle::State_MouseOver);
        const QRect r = opt.rect;
        // ANTS-5658 — 3px wider than the ANTS-5650 stripe.
        const int stripe = qMax(5, r.height() / 4) + 3;
        const int pad = qMax(6, r.height() / 4);

        QFont f = opt.font;
        f.setBold(current);
        const QRect textRect = r.adjusted(stripe + pad, 0, -pad, 0);
        const QFontMetrics fm(f);
        const QString name = fm.elidedText(index.data(Qt::DisplayRole).toString(),
                                           Qt::ElideRight, textRect.width());

        p->save();
        if (rp.fade == rp.fill) {
            p->fillRect(r, rp.fill);
        } else {
            // Left to right: the state colour under the name, then fading
            // into the background, so the name never sits on the fade.
            const int nameEnd = textRect.left() + fm.horizontalAdvance(name);
            QLinearGradient g(qMin(nameEnd, r.right() - 1), 0, r.right(), 0);
            g.setColorAt(0, rp.fill);
            g.setColorAt(1, rp.fade);
            p->fillRect(r, g);
        }
        const QColor tab = index.data(kTabColorRole).value<QColor>();
        if (tab.isValid())
            p->fillRect(QRect(r.left(), r.top(), stripe, r.height()), tab);
        p->setFont(f);
        p->setPen(rp.ink);
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, name);
        if (current) {
            // The current tab: outlined, so no highlight hides its colour.
            QPen pen(rp.ink, 2);
            pen.setJoinStyle(Qt::MiterJoin);
            p->setPen(pen);
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(r).adjusted(stripe + 1, 1, -1, -1));
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override {
        QSize s = QStyledItemDelegate::sizeHint(opt, index);
        if (index.data(kGlyphRole).isValid())
            s.setHeight(qMax(s.height(), QFontMetrics(opt.font).height() + 10));
        return s;
    }

private:
    QColor m_bg;
};

// ANTS-5656 — A–Z order: case ignored, digit runs compared as numbers.
// QCollator's numeric mode needs ICU, which Qt builds such as CI's lack, so
// the digit runs are compared here and only text runs go to the collator.
int compareTitles(const QString &a, const QString &b) {
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    auto runEnd = [](const QString &s, int i) {
        const bool digit = s.at(i).isDigit();
        while (i < s.size() && s.at(i).isDigit() == digit) ++i;
        return i;
    };
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const int ie = runEnd(a, i), je = runEnd(b, j);
        const QStringView ra = QStringView(a).mid(i, ie - i);
        const QStringView rb = QStringView(b).mid(j, je - j);
        int c = 0;
        if (ra.front().isDigit() && rb.front().isDigit()) {
            // Leading zeros aside, the longer run is the bigger number.
            auto strip = [](QStringView r) {
                while (r.size() > 1 && r.front() == u'0') r = r.mid(1);
                return r;
            };
            const QStringView na = strip(ra), nb = strip(rb);
            c = na.size() != nb.size() ? (na.size() < nb.size() ? -1 : 1)
                                       : na.compare(nb);
        } else {
            c = collator.compare(ra, rb);
        }
        if (c != 0) return c;
        i = ie;
        j = je;
    }
    // One title ran out: the shorter one sorts first.
    const qsizetype restA = a.size() - i, restB = b.size() - j;
    return restA < restB ? -1 : (restA > restB ? 1 : 0);
}

}  // namespace

TabListPanel::TabListPanel(QWidget *parent) : QWidget(parent) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(0);
    m_tree->setUniformRowHeights(false);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setFocusPolicy(Qt::StrongFocus);
    m_tree->setAccessibleName(QStringLiteral("Open tabs"));
    m_tree->setItemDelegate(new TabRowDelegate(m_tree));
    m_tree->setMouseTracking(true);  // hover state for TabRowDelegate
    lay->addWidget(m_tree);

    // A click and Enter both switch; activation alone would need a
    // double-click on most styles.
    connect(m_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) { onItemActivated(item); });
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { onItemActivated(item); });

    // Right-click opens the same tab menu as the tab bar, which may be
    // hidden while this panel shows.
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_tree->itemAt(pos);
        auto *page = item ? qobject_cast<QWidget *>(item->data(0, kPageRole).value<QObject *>())
                          : nullptr;
        if (page) emit tabContextMenuRequested(page);
    });
    // Enter and a click away submit; Escape reverts. commitData runs first,
    // so the row already holds the typed name here.
    connect(m_tree->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
            [this](QWidget *, QAbstractItemDelegate::EndEditHint hint) {
                onEditorClosed(hint != QAbstractItemDelegate::RevertModelCache);
            });
}

bool TabListPanel::startRename(QWidget *page) {
    if (!page || m_renaming) return false;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        QTreeWidgetItem *item = *it;
        if (item->data(0, kPageRole).value<QObject *>() != page) continue;
        m_renaming = item;
        m_renameFrom = item->text(0);
        m_renamePage = page;
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_tree->scrollToItem(item);
        m_tree->editItem(item, 0);
        return true;
    }
    return false;
}

void TabListPanel::onEditorClosed(bool submitted) {
    if (!m_renaming) return;
    const QString name = m_renaming->text(0);
    m_renaming = nullptr;
    if (submitted && m_renamePage && name != m_renameFrom) emit tabRenamed(m_renamePage, name);
    // Rebuild either way: it drops the editable flag, restores a reverted
    // title, and applies entries that arrived while the editor was open.
    rebuild();
}

void TabListPanel::setEntries(const QVector<TabListEntry> &entries, QWidget *current) {
    if (entries == m_entries && current == m_current && m_tree->topLevelItemCount() > 0)
        return;
    m_entries = entries;
    m_current = current;
    // A rebuild would delete the row under the open editor; closing it
    // rebuilds.
    if (!m_renaming) rebuild();
}

void TabListPanel::setFontPointSize(int points) {
    if (points <= 0 || points == m_points) return;
    m_points = points;
    QFont f = m_tree->font();
    f.setPointSize(points);
    m_tree->setFont(f);
    if (!m_renaming) rebuild();
}

void TabListPanel::setSortMode(SortMode mode) {
    if (mode == m_sort) return;
    m_sort = mode;
    if (!m_renaming) rebuild();
}

QString TabListPanel::sortModeKey(SortMode mode) {
    switch (mode) {
        case SortMode::ByColour:   return QStringLiteral("colour");
        case SortMode::ByColourAZ: return QStringLiteral("colour_az");
        case SortMode::AZ:         return QStringLiteral("az");
        case SortMode::TabOrder:   return QStringLiteral("tab_order");
    }
    return QStringLiteral("colour");
}

TabListPanel::SortMode TabListPanel::sortModeFromKey(const QString &key) {
    for (SortMode m : {SortMode::ByColourAZ, SortMode::AZ, SortMode::TabOrder})
        if (key == sortModeKey(m)) return m;
    return SortMode::ByColour;
}

void TabListPanel::setThemeColors(const QColor &background, const QColor &text) {
    QPalette pal = m_tree->palette();
    for (auto role : {QPalette::Base, QPalette::Window, QPalette::AlternateBase})
        pal.setColor(role, background);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::WindowText, text);
    m_tree->setPalette(pal);
    m_tree->viewport()->setAutoFillBackground(true);
    setPalette(pal);
    setAutoFillBackground(true);
    static_cast<TabRowDelegate *>(m_tree->itemDelegate())->setBackground(background);
    m_tree->viewport()->update();
}

QString TabListPanel::groupName(const QColor &color) {
    if (!color.isValid()) return QStringLiteral("No colour");
    for (const ColoredTabBar::PaletteEntry &p : ColoredTabBar::palette())
        if (p.color.rgb() == color.rgb()) return p.name;
    return QStringLiteral("Custom colour");
}

QString TabListPanel::stateLabel(ClaudeTabIndicator::Glyph glyph) {
    using G = ClaudeTabIndicator::Glyph;
    switch (glyph) {
        case G::None:          return QString();
        case G::Idle:          return QStringLiteral("Claude idle");
        case G::Thinking:      return QStringLiteral("Claude thinking");
        case G::ToolUse:       return QStringLiteral("Claude using a tool");
        case G::Bash:          return QStringLiteral("Claude running a command");
        case G::Planning:      return QStringLiteral("Claude planning");
        case G::Auditing:      return QStringLiteral("Claude auditing");
        case G::Compacting:    return QStringLiteral("Claude compacting");
        case G::AwaitingInput: return QStringLiteral("Claude waiting for you");
    }
    return QString();
}

void TabListPanel::rebuild() {
    const int scroll = m_tree->verticalScrollBar() ? m_tree->verticalScrollBar()->value() : 0;
    m_tree->clear();
    if (m_entries.isEmpty()) return;

    const int iconSide = QFontMetrics(m_tree->font()).height();
    m_tree->setIconSize(QSize(iconSide, iconSide));  // the header swatches

    // TabRowDelegate paints the stripe and the state colour from these roles.
    auto makeRow = [](const TabListEntry &e) {
        auto *item = new QTreeWidgetItem(QStringList{e.title});
        item->setData(0, kPageRole, QVariant::fromValue(static_cast<QObject *>(e.page)));
        item->setData(0, kGlyphRole, static_cast<int>(e.glyph));
        item->setData(0, kTabColorRole, e.color);
        const QString state = stateLabel(e.glyph);
        item->setToolTip(0, state.isEmpty() ? e.title : e.title + QStringLiteral(" — ") + state);
        item->setData(0, Qt::AccessibleTextRole,
                      state.isEmpty() ? e.title : e.title + QStringLiteral(", ") + state);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        return item;
    };

    // ANTS-5656 — the A–Z modes order rows by name: case ignored, numbers
    // in number order, tab order kept between equal names.
    QVector<TabListEntry> ordered = m_entries;
    if (m_sort == SortMode::AZ || m_sort == SortMode::ByColourAZ) {
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const TabListEntry &a, const TabListEntry &b) {
                             return compareTitles(a.title, b.title) < 0;
                         });
    }

    QTreeWidgetItem *currentItem = nullptr;
    bool anyColour = false;
    for (const TabListEntry &e : std::as_const(m_entries))
        anyColour = anyColour || e.color.isValid();
    const bool grouped =
        anyColour && (m_sort == SortMode::ByColour || m_sort == SortMode::ByColourAZ);

    if (!grouped) {
        for (const TabListEntry &e : std::as_const(ordered)) {
            QTreeWidgetItem *row = makeRow(e);
            m_tree->addTopLevelItem(row);
            if (e.page == m_current) currentItem = row;
        }
    } else {
        // One header per colour, in the order of its first tab; no colour last.
        QVector<QRgb> order;
        for (const TabListEntry &e : std::as_const(m_entries))
            if (e.color.isValid() && !order.contains(e.color.rgb())) order << e.color.rgb();
        auto addGroup = [&](const QColor &colour, bool coloured) {
            auto *header = new QTreeWidgetItem(QStringList{groupName(colour)});
            QFont bold = m_tree->font();
            bold.setBold(true);
            header->setFont(0, bold);
            header->setFlags(Qt::ItemIsEnabled);
            if (coloured) {
                QPixmap swatch(iconSide, iconSide);
                swatch.fill(colour);
                header->setIcon(0, QIcon(swatch));
            }
            m_tree->addTopLevelItem(header);
            for (const TabListEntry &e : std::as_const(ordered)) {
                const bool match = coloured ? (e.color.isValid() && e.color.rgb() == colour.rgb())
                                            : !e.color.isValid();
                if (!match) continue;
                QTreeWidgetItem *row = makeRow(e);
                header->addChild(row);
                if (e.page == m_current) currentItem = row;
            }
            if (header->childCount() == 0)
                delete m_tree->takeTopLevelItem(m_tree->indexOfTopLevelItem(header));
            else
                header->setExpanded(true);
        };
        for (QRgb rgb : std::as_const(order)) addGroup(QColor::fromRgb(rgb), true);
        addGroup(QColor(), false);
    }

    if (currentItem) m_tree->setCurrentItem(currentItem);
    if (m_tree->verticalScrollBar()) m_tree->verticalScrollBar()->setValue(scroll);
}

void TabListPanel::onItemActivated(QTreeWidgetItem *item) {
    if (!item) return;
    auto *page = qobject_cast<QWidget *>(item->data(0, kPageRole).value<QObject *>());
    if (page) emit tabActivated(page);
}

TabListPanel::RowPaint TabListPanel::rowPaint(ClaudeTabIndicator::Glyph glyph,
                                              const QColor &background, bool hovered) {
    QColor fill = glyph == ClaudeTabIndicator::Glyph::None ? background
                                                           : ClaudeTabIndicator::color(glyph);
    if (hovered || glyph == ClaudeTabIndicator::Glyph::None) {
        if (hovered) {
            // A quarter of the way towards the ink: visibly different, same hue.
            const QColor toward = inkOn(fill);
            const auto mix = [](int a, int b) { return a + (b - a) / 4; };
            fill = QColor(mix(fill.red(), toward.red()), mix(fill.green(), toward.green()),
                          mix(fill.blue(), toward.blue()));
        }
        return {fill, fill, inkOn(fill)};
    }

    // ANTS-5658 — at rest the state colour fades into the background. The
    // delegate starts the fade after the name, so the name always sits on
    // the colour its ink was chosen for.
    return {fill, background, inkOn(fill)};
}

QIcon TabListPanel::tabIcon(const TabListEntry &e, int h, const QColor &bg, qreal dpr) {
    // A colour stripe on the left, then the Claude dot, sized to the text.
    QPixmap pm(QSize(h * 2, h) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const int stripe = qMax(4, h / 4);
    if (e.color.isValid())
        p.fillRect(QRect(0, 0, stripe, h), e.color);
    if (e.glyph != ClaudeTabIndicator::Glyph::None) {
        const qreal r = h * 0.28;
        const QPointF c(stripe + h * 0.6, h / 2.0);
        p.setPen(QPen(ClaudeTabIndicator::ringColor(bg), 1));
        p.setBrush(ClaudeTabIndicator::contrastColor(e.glyph, bg));
        p.drawEllipse(c, r, r);
    }
    return QIcon(pm);
}
