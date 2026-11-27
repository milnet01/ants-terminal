#include "dialogshowtracer.h"

#include "debuglog.h"

#include <QApplication>
#include <QDateTime>
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

// A control character (a newline in a shell's OSC title) is written as
// \xNN, so one show is always one log line.
QString visible(const QString &s) {
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.category() == QChar::Other_Control)
            out += QStringLiteral("\\x%1").arg(int(c.unicode()), 2, 16, QLatin1Char('0'));
        else
            out += c;
    }
    return out;
}
}  // namespace

DialogShowTracer::DialogShowTracer(QObject *parent) : QObject(parent) {}

QString DialogShowTracer::showLine(const QWidget *w) {
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

    // One multi-arg .arg(), so a %N inside a title is never filled in by a
    // later call.
    return QStringLiteral(
        "[ANTS_TRACE_DIALOGS %1] show: cls=%2 obj=%3 title=\"%4\" "
        "geom=%5x%6+%7+%8 parent=%9/%10")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
             visible(cls), visible(name), visible(title),
             QString::number(geom.width()), QString::number(geom.height()),
             QString::number(geom.x()), QString::number(geom.y()),
             visible(parCls), visible(parName));
}

bool DialogShowTracer::eventFilter(QObject *obj, QEvent *ev) {
    if (ev->type() != QEvent::Show) return false;
    auto *w = qobject_cast<QWidget *>(obj);
    if (!w) return false;
    // Windows only (a widget with no parent is always one) — embedded
    // children are uninteresting and would flood the log.
    if (!w->isWindow()) return false;

    const QString line = showLine(w);

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
