// ANTS-1242 — see dialogchrome.h for design notes.

#include "dialogchrome.h"

#include "config.h"
#include "themes.h"
#include "titlebar.h"

#include <QCursor>
#include <QDialog>
#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QPointer>
#include <QScreen>
#include <QSizeGrip>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace DialogChrome {

namespace {
// Module-level cache so a dialog ctor doesn't need to receive the
// theme name as a parameter. MainWindow keeps this in sync via
// setActiveTheme() on every theme change.
QString g_activeTheme;

// ANTS-1842 — process-wide Config for D3 size persistence. Set once by
// MainWindow (mirrors g_activeTheme). nullptr → D3 silently inert.
Config *g_config = nullptr;

// ANTS-1842 — D2–D4 affordance driver attached to a dialog as an event
// filter (no Q_OBJECT needed: only the virtual eventFilter() is used).
// Keeps the QSizeGrip pinned to the bottom-right corner (D2), restores
// the persisted size on first show and saves it on close (D3), and
// re-centers over the parent window's current frame on every open (D4).
class ChromeGuard : public QObject {
public:
    ChromeGuard(QDialog *dlg, QSizeGrip *grip, QString sizeKey)
        : QObject(dlg), m_dlg(dlg), m_dlgObj(dlg), m_grip(grip),
          m_sizeKey(std::move(sizeKey)) {
        dlg->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        // Identity-check against the QObject base, never the QPointer<QDialog>
        // (ANTS-1869): events still flow to this filter while `dlg` is mid-
        // destruction (a ~QVBoxLayout ChildRemoved arrives during ~QWidget,
        // before ~QObject clears QPointers). Comparing against
        // QPointer<QDialog> there calls data(), which downcasts the already-
        // QWidget-degraded object to QDialog* — a UBSan vptr error that was
        // reding every dialog test under the sanitizer build.
        if (obj == m_dlgObj) {
            switch (ev->type()) {
            case QEvent::Show:   onShow();      break;
            case QEvent::Resize: positionGrip(); break;
            case QEvent::Close:  saveSize();    break;
            default: break;
            }
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    void onShow() {
        if (!m_restored) {
            m_restored = true;
            // D3 — restore AFTER the ctor (which may resize() to a
            // default); persisted size wins, absent size falls through to
            // the dialog's default. First show only, before first paint.
            if (g_config && !m_sizeKey.isEmpty() && m_dlg) {
                const QSize sz = g_config->dialogSize(m_sizeKey);
                if (sz.isValid()) m_dlg->resize(sz);
            }
        }
        recenter();
        positionGrip();
    }

    void recenter() {
        if (!m_dlg) return;
        // D4 — center over the parent window's *live* frame each open, so
        // a moved/resized terminal still lands the dialog under the eye.
        if (QWidget *p = m_dlg->parentWidget()
                             ? m_dlg->parentWidget()->window() : nullptr)
            m_dlg->move(p->frameGeometry().center() - m_dlg->rect().center());
        else if (QScreen *s = QGuiApplication::screenAt(QCursor::pos()))
            m_dlg->move(s->geometry().center() - m_dlg->rect().center());
    }

    void positionGrip() {
        if (!m_grip || !m_dlg) return;
        m_grip->adjustSize();
        m_grip->move(m_dlg->width() - m_grip->width(),
                     m_dlg->height() - m_grip->height());
        m_grip->raise();
    }

    void saveSize() {
        if (g_config && !m_sizeKey.isEmpty() && m_dlg)
            g_config->setDialogSize(m_sizeKey, m_dlg->size());
    }

    QPointer<QDialog> m_dlg;
    QPointer<QObject> m_dlgObj;  // same object, base type — downcast-free identity check
    QPointer<QSizeGrip> m_grip;
    QString m_sizeKey;
    bool m_restored = false;
};
}  // namespace

void setActiveTheme(const QString &name) {
    g_activeTheme = name;
}

void setConfig(Config *config) {
    g_config = config;
}

QString activeTheme() {
    return g_activeTheme;
}

void applyTheme(QDialog *dlg, TitleBar *bar, const QString &themeName) {
    if (!dlg && !bar) return;
    const Theme &th = Themes::byName(themeName);
    if (bar) {
        bar->setThemeColors(th.bgSecondary, th.textPrimary,
                            th.accent, th.border);
    }
    if (dlg) {
        QPalette dp = dlg->palette();
        dp.setColor(QPalette::Window, th.bgPrimary);
        dp.setColor(QPalette::WindowText, th.textPrimary);
        dp.setColor(QPalette::Base, th.bgPrimary);
        dp.setColor(QPalette::AlternateBase, th.bgSecondary);
        dp.setColor(QPalette::Text, th.textPrimary);
        dp.setColor(QPalette::ButtonText, th.textPrimary);
        dp.setColor(QPalette::Button, th.bgSecondary);
        dlg->setPalette(dp);
        dlg->setAutoFillBackground(true);
    }
}

InstallResult install(QDialog *dlg, const QString &themeName,
                      bool resizable, const QString &sizeKey) {
    InstallResult r{nullptr, nullptr};
    if (!dlg) return r;

    dlg->setWindowFlag(Qt::FramelessWindowHint);
    dlg->setAttribute(Qt::WA_StyledBackground, true);

    auto *bar = new TitleBar(dlg);
    bar->setTitle(dlg->windowTitle());
    bar->centerButton()->hide();
    QObject::connect(bar, &TitleBar::closeRequested, dlg, &QDialog::reject);
    QObject::connect(bar, &TitleBar::minimizeRequested,
                     dlg, &QWidget::showMinimized);
    QObject::connect(bar, &TitleBar::maximizeRequested, dlg, [dlg]() {
        if (dlg->isMaximized()) dlg->showNormal();
        else dlg->showMaximized();
    });

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(bar);

    auto *content = new QWidget(dlg);
    root->addWidget(content, 1);

    applyTheme(dlg, bar, themeName.isEmpty() ? g_activeTheme : themeName);

    if (resizable) {
        // D2 — the frameless window has no OS border to drag, so add a
        // bottom-right size grip. Parented on the dialog (not the layout)
        // so ChromeGuard can float it over the corner regardless of the
        // dialog's own content layout. The guard also drives D3 + D4.
        auto *grip = new QSizeGrip(dlg);
        grip->setFixedSize(grip->sizeHint());
        new ChromeGuard(dlg, grip, sizeKey);
    }

    r.titleBar = bar;
    r.contentArea = content;
    return r;
}

}  // namespace DialogChrome
