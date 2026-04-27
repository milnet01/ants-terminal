#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QAction>
#include <QVBoxLayout>
#include <QList>
#include <QPointer>
#include <QString>

// QLineEdit that paints an inline ghost-text suffix in dimmed colour
// after the cursor. Owned by CommandPalette; ghost state is set by
// CommandPalette::updateGhostCompletion as the result list rebuilds.
class GhostLineEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit GhostLineEdit(QWidget *parent = nullptr) : QLineEdit(parent) {}

    void setGhostSuffix(const QString &suffix);
    QString ghostSuffix() const { return m_ghost; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_ghost;
};

class CommandPalette : public QWidget {
    Q_OBJECT

public:
    explicit CommandPalette(QWidget *parent = nullptr);

    void setActions(const QList<QAction *> &actions);
    void show();

signals:
    void closed();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private slots:
    void filterActions(const QString &text);
    void executeSelected();

private:
    GhostLineEdit *m_input = nullptr;
    QListWidget *m_list = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QList<QPointer<QAction>> m_allActions;

    void positionAndResize();
    void populateList(const QString &filter);
    void ensureListReady();
    void updateGhostCompletion(const QString &filter);
    void commitGhost();
};
