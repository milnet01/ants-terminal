#pragma once

// ANTS-5759 — a click on a status-bar pill that has no click action of its
// own opens a window showing the pill's tooltip text, for a reader whose
// magnifier cannot take in a tooltip. The window follows the tooltip while
// it is open and sizes itself so all of the text is visible, up to the
// screen; past that the text scrolls. A pill whose tooltip is empty opens
// nothing.
//
// Header-only, like elidedlabel.h: both MainWindow and the Claude status
// bar attach it, and neither library should own it.

#include "dialogchrome.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

namespace statusinfo {

class InfoWindow : public QDialog {
public:
    InfoWindow(QWidget *source, const QString &title)
        : QDialog(source->window()), m_source(source) {
        setObjectName(QStringLiteral("statusInfoWindow"));
        setWindowTitle(title);
        setAttribute(Qt::WA_DeleteOnClose);
        auto chrome = DialogChrome::install(this, QString(), /*resizable=*/true);

        auto *layout = new QVBoxLayout(chrome.contentArea);
        m_text = new QLabel(this);
        m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_scroll = new QScrollArea(this);
        m_scroll->setWidget(m_text);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        layout->addWidget(m_scroll);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        layout->addWidget(buttons);

        refresh();
    }

    // Shows the source's current tooltip, then grows or shrinks to fit it.
    void refresh() {
        if (!m_source) return;
        m_text->setText(m_source->toolTip());
        m_text->adjustSize();
        const QSize textSize = m_text->sizeHint();
        const QSize chromeExtra = sizeHint() - m_scroll->sizeHint();
        QSize want = textSize + chromeExtra
            + QSize(m_scroll->verticalScrollBar()->sizeHint().width(), 0);
        if (QScreen *screen = this->screen() ? this->screen()
                                             : QGuiApplication::primaryScreen()) {
            const QSize cap = screen->availableGeometry().size() * 9 / 10;
            want = want.boundedTo(cap);
        }
        resize(want.expandedTo(minimumSizeHint()));
    }

private:
    QPointer<QWidget> m_source;
    QLabel *m_text = nullptr;
    QScrollArea *m_scroll = nullptr;
};

// Watches one pill: a left click opens (or raises) its window, and a
// tooltip change updates an open one. Owned by the pill.
class ClickWatcher : public QObject {
public:
    ClickWatcher(QWidget *pill, const QString &title)
        : QObject(pill), m_pill(pill), m_title(title) {
        pill->installEventFilter(this);
        pill->setCursor(Qt::PointingHandCursor);
    }

    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (obj != m_pill) return false;
        if (ev->type() == QEvent::ToolTipChange) {
            if (m_window) {
                if (m_pill->toolTip().isEmpty()) m_window->close();
                else m_window->refresh();
            }
        } else if (ev->type() == QEvent::MouseButtonRelease) {
            const auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() != Qt::LeftButton) return false;
            // A drag that selected text in a selectable label is not a click.
            if (auto *label = qobject_cast<QLabel *>(m_pill);
                label && label->hasSelectedText())
                return false;
            if (!m_pill->rect().contains(me->position().toPoint())) return false;
            open();
        }
        return false;
    }

private:
    void open() {
        if (m_pill->toolTip().isEmpty()) return;
        if (!m_window) {
            m_window = new InfoWindow(m_pill, m_title);
            m_window->show();
        } else {
            m_window->refresh();
        }
        m_window->raise();
        m_window->activateWindow();
    }

    QWidget *m_pill;
    QString m_title;
    QPointer<InfoWindow> m_window;
};

// Makes a click on `pill` open its tooltip window. For a pill with no click
// action of its own; one with an action keeps it and gets no window.
inline void attach(QWidget *pill, const QString &title) {
    if (pill) new ClickWatcher(pill, title);
}

}  // namespace statusinfo
