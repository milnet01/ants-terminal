#pragma once

#include <QAbstractButton>
#include <QColor>

class QPropertyAnimation;

class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    // No NOTIFY signal: this property is animated by a QPropertyAnimation
    // we own; nothing outside the widget reads it. The "thumbX" property
    // exists solely to let the animation framework call setThumbX() each
    // tick.
    Q_PROPERTY(int thumbX READ thumbX WRITE setThumbX)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    int thumbX() const { return m_thumbX; }
    void setThumbX(int x);

    // Theme-aware colors
    void setThemeColors(const QColor &onColor, const QColor &offColor);

protected:
    void paintEvent(QPaintEvent *) override;
    void checkStateSet() override;
    void nextCheckState() override;

private:
    void animate();
    int m_thumbX;  // starts at the off position (kOff, toggleswitch.cpp)
    QPropertyAnimation *m_anim = nullptr;
    QColor m_onColor{0x4C, 0xAF, 0x50};   // green
    QColor m_offColor{0x88, 0x88, 0x88};  // grey
};
