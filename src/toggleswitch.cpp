#include "toggleswitch.h"

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

void ToggleSwitch::checkStateSet() { animate(); }
void ToggleSwitch::nextCheckState() { QAbstractButton::nextCheckState(); animate(); }

void ToggleSwitch::animate() {
    m_anim->stop();
    m_anim->setStartValue(m_thumbX);
    m_anim->setEndValue(isChecked() ? kOn : kOff);
    m_anim->start();
}
