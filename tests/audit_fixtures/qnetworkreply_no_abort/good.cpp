// Safe patterns — MUST NOT match the rule's regex. The rule looks for a
// `[` immediately after the QNetworkReply signal's trailing comma (the
// 3-arg-with-bare-lambda shape). All variants here put a context object
// (`this`, `mgr`, etc.) in that slot, which Qt's auto-disconnect protects
// against owner destruction.

#include <QObject>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUrl>

class SafeFetcher : public QObject {
public:
    void start() {
        auto *mgr = new QNetworkAccessManager(this);
        QNetworkReply *reply = mgr->get(QNetworkRequest(QUrl("https://example.com")));

        // 4-arg form, member-function slot — auto-disconnect on `this` dtor.
        connect(reply, &QNetworkReply::finished, this, &SafeFetcher::onDone);

        // 4-arg form, lambda slot with context = this — same auto-disconnect
        // protection. Note `this,` between the signal and the lambda capture.
        connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
            stream(reply);
        });

        // Self-cleanup: reply parented to a manager that's parented to this.
        connect(reply, &QNetworkReply::finished, mgr, &QNetworkAccessManager::deleteLater);
    }

private:
    void onDone() {}
    void stream(QNetworkReply *) {}
};
