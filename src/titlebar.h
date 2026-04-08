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

signals:
    void windowMoved(const QPoint &newPos);

public:
    void setKnownWindowPos(const QPoint &pos) { m_knownWindowPos = pos; }
    // Call after system drag ends to compute and emit new position
    void finishSystemDrag();

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QLabel *m_titleLabel = nullptr;
    QPoint m_cursorOffset;     // Cursor offset from window corner at drag start
    QPoint m_knownWindowPos;   // Last known window position (from tracker)
    bool m_systemDragActive = false;
    QToolButton *m_centerBtn = nullptr;
    QToolButton *m_minimizeBtn = nullptr;
    QToolButton *m_maximizeBtn = nullptr;
    QToolButton *m_closeBtn = nullptr;
};
