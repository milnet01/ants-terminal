#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QAction>
#include <QVBoxLayout>
#include <QList>

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
    QLineEdit *m_input = nullptr;
    QListWidget *m_list = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QList<QAction *> m_allActions;

    void positionAndResize();
    void populateList(const QString &filter);
};
