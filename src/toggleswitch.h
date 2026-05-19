#pragma once

#include <QAbstractButton>
#include <QColor>

class QPropertyAnimation;

class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    // NOTIFY signal not needed: this property is animated by a
    // QPropertyAnimation we own; nothing outside the widget reads it.
    // The "thumbX" property exists solely to let the animation framework
    // call setThumbX() each tick.
    Q_PROPERTY(int thumbX READ thumbX WRITE setThumbX NOTIFY thumbXChanged)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    int thumbX() const { return m_thumbX; }
    void setThumbX(int x);

    // Theme-aware colors
    void setThemeColors(const QColor &onColor, const QColor &offColor);

signals:
    void thumbXChanged(int x);

protected:
    void paintEvent(QPaintEvent *) override;
    void checkStateSet() override;
    void nextCheckState() override;

private:
    void animate();
    int m_thumbX = 3;
    QPropertyAnimation *m_anim = nullptr;
    QColor m_onColor{"#4CAF50"};
    QColor m_offColor{"#888888"};
};
