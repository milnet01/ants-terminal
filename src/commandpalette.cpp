#include "commandpalette.h"

#include <QKeyEvent>
#include <QPainter>
#include <QApplication>
#include <QMenu>

CommandPalette::CommandPalette(QWidget *parent) : QWidget(parent) {
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("commandPalette");

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_input = new QLineEdit(this);
    m_input->setObjectName("commandPaletteInput");
    m_input->setPlaceholderText("Type a command...");
    m_input->installEventFilter(this);
    m_layout->addWidget(m_input);

    m_list = new QListWidget(this);
    m_list->setObjectName("commandPaletteList");
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_layout->addWidget(m_list);

    connect(m_input, &QLineEdit::textChanged, this, &CommandPalette::filterActions);
    connect(m_list, &QListWidget::itemActivated, this, &CommandPalette::executeSelected);

    hide();
}

void CommandPalette::setActions(const QList<QAction *> &actions) {
    m_allActions.clear();
    m_allActions.reserve(actions.size());
    for (QAction *a : actions)
        m_allActions.append(QPointer<QAction>(a));
}

void CommandPalette::show() {
    m_input->clear();
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
