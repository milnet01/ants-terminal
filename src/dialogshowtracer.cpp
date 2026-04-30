#include "dialogshowtracer.h"

#include "debuglog.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QMetaObject>
#include <QRect>
#include <QString>
#include <QWidget>

#include <cstdio>

namespace {
// Process-global instance pointer. Owned by qApp when installed; null
// when not. setActive() is the only mutator.
DialogShowTracer *s_tracer = nullptr;
}  // namespace

DialogShowTracer::DialogShowTracer(QObject *parent) : QObject(parent) {}

bool DialogShowTracer::eventFilter(QObject *obj, QEvent *ev) {
    if (ev->type() != QEvent::Show) return false;
    auto *w = qobject_cast<QWidget *>(obj);
    if (!w) return false;
    // Top-level only — embedded children are uninteresting and would
    // flood the log. A widget is "top-level" when it has no parent
    // widget OR when its windowFlag indicates a window of its own.
    if (w->parentWidget() != nullptr && !w->isWindow()) return false;
    const bool isDialog = qobject_cast<QDialog *>(w) != nullptr;
    if (!isDialog && !w->isWindow()) return false;

    const QString cls    = QString::fromLatin1(w->metaObject()->className());
    const QString name   = w->objectName().isEmpty() ? "<unnamed>" : w->objectName();
    const QString title  = w->windowTitle().isEmpty() ? "<no-title>" : w->windowTitle();
    const QRect geom     = w->geometry();
    const QObject *par   = w->parent();
    const QString parCls = par
        ? QString::fromLatin1(par->metaObject()->className())
        : QStringLiteral("<no-parent>");
    const QString parName = par && !par->objectName().isEmpty()
        ? par->objectName() : QStringLiteral("<unnamed>");

    const QString line = QString(
        "[ANTS_TRACE_DIALOGS %1] show: cls=%2 obj=%3 title=\"%4\" "
        "geom=%5x%6+%7+%8 parent=%9/%10")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
        .arg(cls, name, title)
        .arg(geom.width()).arg(geom.height())
        .arg(geom.x()).arg(geom.y())
        .arg(parCls, parName);

    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    std::fflush(stderr);
    if (DebugLog::enabled(DebugLog::Events))
        DebugLog::write(DebugLog::Events, line);
    return false;
}

void DialogShowTracer::setActive(bool on) {
    if (on == active()) return;
    if (on) {
        s_tracer = new DialogShowTracer(qApp);
        qApp->installEventFilter(s_tracer);
        std::fprintf(stderr,
            "[ANTS_TRACE_DIALOGS] enabled — every top-level "
            "QWidget / QDialog show will be logged.\n");
        std::fflush(stderr);
    } else if (s_tracer) {
        qApp->removeEventFilter(s_tracer);
        s_tracer->deleteLater();
        s_tracer = nullptr;
        std::fprintf(stderr, "[ANTS_TRACE_DIALOGS] disabled.\n");
        std::fflush(stderr);
    }
}

bool DialogShowTracer::active() {
    return s_tracer != nullptr;
}
