// INV-15: a Qt signals: block parses with errors; afterError follows it.
class Widget : public QObject {
    Q_OBJECT
signals:
    void changed();
};

int afterError() { return 1; }
