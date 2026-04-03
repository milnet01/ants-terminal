#pragma once

#include <QWidget>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>

class TitleBar : public QWidget {
    Q_OBJECT

public:
    explicit TitleBar(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setThemeColors(const QColor &bg, const QColor &fg,
                        const QColor &accent, const QColor &border);

    QToolButton *centerButton() const { return m_centerBtn; }

signals:
    void minimizeRequested();
    void maximizeRequested();
    void closeRequested();

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QLabel *m_titleLabel = nullptr;
    QToolButton *m_centerBtn = nullptr;
    QToolButton *m_minimizeBtn = nullptr;
    QToolButton *m_maximizeBtn = nullptr;
    QToolButton *m_closeBtn = nullptr;
};
