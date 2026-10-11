#include "tablistpanel.h"

#include <QHeaderView>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kPageRole = Qt::UserRole + 1;

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
    lay->addWidget(m_tree);

    // A click and Enter both switch; activation alone would need a
    // double-click on most styles.
    connect(m_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) { onItemActivated(item); });
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { onItemActivated(item); });
}

void TabListPanel::setEntries(const QVector<TabListEntry> &entries, QWidget *current) {
    if (entries == m_entries && current == m_current && m_tree->topLevelItemCount() > 0)
        return;
    m_entries = entries;
    m_current = current;
    rebuild();
}

void TabListPanel::setFontPointSize(int points) {
    if (points <= 0 || points == m_points) return;
    m_points = points;
    QFont f = m_tree->font();
    f.setPointSize(points);
    m_tree->setFont(f);
    rebuild();
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
    rebuild();  // the dot's contrast colour depends on the background
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
    m_tree->setIconSize(QSize(iconSide * 2, iconSide));
    // No stripe or dot on any row: draw no icons, so names start at the edge.
    bool anyMark = false;
    for (const TabListEntry &e : std::as_const(m_entries))
        anyMark = anyMark || e.color.isValid() || e.glyph != ClaudeTabIndicator::Glyph::None;

    auto makeRow = [this, anyMark, iconSide](const TabListEntry &e) {
        auto *item = new QTreeWidgetItem(QStringList{e.title});
        item->setData(0, kPageRole, QVariant::fromValue(static_cast<QObject *>(e.page)));
        if (anyMark)
            item->setIcon(0, tabIcon(e, iconSide, m_tree->palette().color(QPalette::Base),
                                     devicePixelRatioF()));
        const QString state = stateLabel(e.glyph);
        item->setToolTip(0, state.isEmpty() ? e.title : e.title + QStringLiteral(" — ") + state);
        item->setData(0, Qt::AccessibleTextRole,
                      state.isEmpty() ? e.title : e.title + QStringLiteral(", ") + state);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        return item;
    };

    QTreeWidgetItem *currentItem = nullptr;
    bool anyColour = false;
    for (const TabListEntry &e : std::as_const(m_entries))
        anyColour = anyColour || e.color.isValid();

    if (!anyColour) {
        for (const TabListEntry &e : std::as_const(m_entries)) {
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
            for (const TabListEntry &e : std::as_const(m_entries)) {
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
