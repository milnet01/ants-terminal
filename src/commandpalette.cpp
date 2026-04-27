#include "commandpalette.h"

#include <QKeyEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QApplication>

void GhostLineEdit::setGhostSuffix(const QString &suffix) {
    if (m_ghost == suffix) return;
    m_ghost = suffix;
    update();
}

void GhostLineEdit::paintEvent(QPaintEvent *event) {
    QLineEdit::paintEvent(event);
    if (m_ghost.isEmpty()) return;

    // Anchor the dimmed suffix to the right edge of the caret so the
    // completion sits flush against what the user typed. palette[Text]
    // at α=0.45 matches the design contract in
    // tests/features/command_palette_ghost_completion/spec.md.
    QPainter p(this);
    QRect cr = cursorRect();
    QFontMetrics fm(font());
    QColor c = palette().color(QPalette::Text);
    c.setAlphaF(0.45);
    p.setPen(c);
    int x = cr.right() + 1;
    int y = cr.top() + fm.ascent();
    p.drawText(x, y, m_ghost);
}

CommandPalette::CommandPalette(QWidget *parent) : QWidget(parent) {
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("commandPalette");

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_input = new GhostLineEdit(this);
    m_input->setObjectName("commandPaletteInput");
    m_input->setPlaceholderText("Type a command...");
    // Screen-reader label. Placeholder text is documentation, not the
    // accessible name — Orca speaks accessibleName first. 0.7.41.
    m_input->setAccessibleName("Command palette search");
    m_input->setAccessibleDescription(
        "Type to filter actions; Tab to commit; Esc to dismiss");
    m_input->installEventFilter(this);
    m_layout->addWidget(m_input);

    // IMPORTANT: do NOT construct the QListWidget here.
    //
    // QListWidget inherits QAbstractItemView, which schedules
    // continuous internal timers (layout scheduler, selection
    // animation driver, view-update timer, hover tracker) the moment
    // it's attached to the widget tree — even if the parent is
    // hidden. When the palette was built at MainWindow init, those
    // timers drove a ~54 Hz LayoutRequest → UpdateRequest →
    // full-widget-tree paint cascade against the main window at
    // idle, and the cascade showed up as visible dropdown flicker
    // whenever a menu was open (diagnosed 2026-04-20 via
    // ANTS_PAINT_LOG=2 — the Timer events were all attributed to
    // `cls=QListWidget name=commandPaletteList parent=CommandPalette`).
    // setUpdatesEnabled(false) + WA_DontShowOnScreen didn't stop the
    // timers because those attributes only affect painting, not the
    // view's internal timer machinery.
    //
    // Lazy-create the list in ensureListReady() on first show(). No
    // QListWidget means no QAbstractItemView timers, period.
    connect(m_input, &QLineEdit::textChanged, this, &CommandPalette::filterActions);

    hide();
}

void CommandPalette::ensureListReady() {
    if (m_list) return;
    m_list = new QListWidget(this);
    m_list->setObjectName("commandPaletteList");
    m_list->setAccessibleName("Command palette results");
    m_list->setAccessibleDescription(
        "Available actions matching the current filter");
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_layout->addWidget(m_list);
    connect(m_list, &QListWidget::itemActivated, this,
            &CommandPalette::executeSelected);
}

void CommandPalette::setActions(const QList<QAction *> &actions) {
    m_allActions.clear();
    m_allActions.reserve(actions.size());
    for (QAction *a : actions)
        m_allActions.append(QPointer<QAction>(a));
}

void CommandPalette::show() {
    // First-show: build the QListWidget. After this point the view
    // exists for the lifetime of the palette — its internal timers
    // fire only when we interact, which is fine (a visible palette
    // with a live list is what users expect).
    ensureListReady();

    m_input->blockSignals(true);
    m_input->clear();
    m_input->setGhostSuffix(QString());
    m_input->blockSignals(false);
    populateList("");
    positionAndResize();
    QWidget::show();
    raise();
    m_input->setFocus();
}

void CommandPalette::positionAndResize() {
    QWidget *p = parentWidget();
    if (!p) return;
    int w = std::min(p->width() - 40, 500);
    int x = (p->width() - w) / 2;
    int y = 40; // Below title bar + menu
    setGeometry(x, y, w, 0); // Height set by populateList
}

void CommandPalette::populateList(const QString &filter) {
    m_list->clear();
    QString lowerFilter = filter.toLower();

    for (int idx = 0; idx < m_allActions.size(); ++idx) {
        QAction *action = m_allActions[idx].data();
        if (!action || action->isSeparator() || action->text().isEmpty()) continue;
        // Strip & accelerator markers for display and matching
        QString name = action->text().remove('&');
        if (!lowerFilter.isEmpty() && !name.toLower().contains(lowerFilter)) continue;

        auto *item = new QListWidgetItem(name);
        // Show shortcut if available
        if (!action->shortcut().isEmpty()) {
            item->setText(name + "    " + action->shortcut().toString());
        }
        item->setData(Qt::UserRole, idx);
        m_list->addItem(item);
    }

    if (m_list->count() > 0)
        m_list->setCurrentRow(0);

    // Resize to fit contents (max 12 items visible)
    int itemH = m_list->sizeHintForRow(0);
    if (itemH <= 0) itemH = 28;
    int visibleItems = std::min(m_list->count(), 12);
    int inputH = m_input->sizeHint().height();
    int totalH = inputH + visibleItems * itemH + 4;
    setFixedHeight(totalH);

    updateGhostCompletion(filter);
}

void CommandPalette::updateGhostCompletion(const QString &filter) {
    // Ghost is shown only when the filter is a strict (case-insensitive)
    // prefix of the top match's action text. `contains`-only matches
    // (top match has the filter mid-string) get an empty ghost — the
    // visual contract is that the suffix appears flush after the
    // user's typed input, which only makes sense for prefix matches.
    if (filter.isEmpty() || !m_list || m_list->count() == 0) {
        m_input->setGhostSuffix(QString());
        return;
    }
    QListWidgetItem *top = m_list->item(0);
    if (!top) {
        m_input->setGhostSuffix(QString());
        return;
    }
    int idx = top->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_allActions.size()) {
        m_input->setGhostSuffix(QString());
        return;
    }
    QAction *action = m_allActions[idx].data();
    if (!action) {
        m_input->setGhostSuffix(QString());
        return;
    }
    QString name = action->text().remove('&');
    if (!name.toLower().startsWith(filter.toLower())) {
        m_input->setGhostSuffix(QString());
        return;
    }
    m_input->setGhostSuffix(name.mid(filter.length()));
}

void CommandPalette::commitGhost() {
    QString g = m_input->ghostSuffix();
    if (g.isEmpty()) return;
    // setText fires textChanged → filterActions → populateList →
    // updateGhostCompletion, which clears the ghost since the new
    // filter equals the action name exactly.
    m_input->setText(m_input->text() + g);
}

void CommandPalette::filterActions(const QString &text) {
    populateList(text);
}

void CommandPalette::executeSelected() {
    auto *item = m_list->currentItem();
    if (!item) return;

    int idx = item->data(Qt::UserRole).toInt();
    hide();
    emit closed();

    if (idx >= 0 && idx < m_allActions.size()) {
        QAction *action = m_allActions[idx].data();
        if (action && action->isEnabled())
            action->trigger();
    }
}

bool CommandPalette::eventFilter(QObject *obj, QEvent *event) {
    if (obj == m_input && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Escape:
            hide();
            emit closed();
            return true;
        case Qt::Key_Tab:
            // Always consume Tab so focus cannot leave the palette
            // input while it's open. If a ghost suggestion is live,
            // commit it; otherwise this is a no-op.
            commitGhost();
            return true;
        case Qt::Key_Down:
            if (m_list->count() > 0) {
                int row = m_list->currentRow();
                m_list->setCurrentRow(std::min(row + 1, m_list->count() - 1));
            }
            return true;
        case Qt::Key_Up:
            if (m_list->count() > 0) {
                int row = m_list->currentRow();
                m_list->setCurrentRow(std::max(row - 1, 0));
            }
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            executeSelected();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void CommandPalette::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(palette().window());
    p.drawRoundedRect(rect(), 8, 8);
}
