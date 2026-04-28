#include "toggleswitch.h"

#include <QAccessible>
#include <QAccessibleEvent>
#include <QPainter>
#include <QPropertyAnimation>

static constexpr int kW = 44;
static constexpr int kH = 22;
static constexpr int kThumb = 16;
static constexpr int kMargin = 3;
static constexpr int kOff = kMargin;
static constexpr int kOn = kW - kThumb - kMargin;

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setFixedSize(kW, kH);
    m_anim = new QPropertyAnimation(this, "thumbX", this);
    m_anim->setDuration(150);
    m_anim->setEasingCurve(QEasingCurve::InOutCubic);
    // 0.7.54 (2026-04-27 indie-review WCAG) — accessibility plumbing.
    // Without an accessibleName the screen reader announces the widget
    // as "QAbstractButton" with no semantic context. Callers should
    // also set their own accessibleName via setAccessibleName when
    // the switch's purpose is more specific (e.g. "Auto color
    // scheme"); this default is the fallback for sites that haven't.
    setAccessibleName(tr("Toggle switch"));
}

QSize ToggleSwitch::sizeHint() const { return {kW, kH}; }

void ToggleSwitch::setThumbX(int x) {
    m_thumbX = x;
    update();
}

void ToggleSwitch::setThemeColors(const QColor &onColor, const QColor &offColor) {
    m_onColor = onColor;
    m_offColor = offColor;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QColor track = isEnabled()
        ? (isChecked() ? m_onColor : m_offColor)
        : m_offColor.darker(150);
    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(0, 0, kW, kH, kH / 2, kH / 2);

    QColor thumb = isEnabled() ? Qt::white : m_offColor.lighter(110);
    p.setBrush(thumb);
    p.drawEllipse(m_thumbX, (kH - kThumb) / 2, kThumb, kThumb);
}

// 0.7.54 (2026-04-27 indie-review WCAG) — fire StateChanged accessibility
// events so AT-SPI / UIAutomation consumers (Orca on Linux, NVDA on
// Windows, VoiceOver on macOS) get a notification when the toggle
// flips. Without this, the screen reader reads the new state only on
// next focus traversal, leaving keyboard users without immediate
// confirmation that their toggle landed.
static void emitStateChanged(QObject *obj) {
    if (!QAccessible::isActive()) return;
    QAccessibleStateChangeEvent ev(obj, QAccessible::State{});
    QAccessible::updateAccessibility(&ev);
}

void ToggleSwitch::checkStateSet() {
    animate();
    emitStateChanged(this);
}
void ToggleSwitch::nextCheckState() {
    QAbstractButton::nextCheckState();
    animate();
    emitStateChanged(this);
}

void ToggleSwitch::animate() {
    m_anim->stop();
    m_anim->setStartValue(m_thumbX);
    m_anim->setEndValue(isChecked() ? kOn : kOff);
    m_anim->start();
}
