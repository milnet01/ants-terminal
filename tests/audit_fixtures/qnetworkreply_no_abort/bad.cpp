// Fixture for qnetworkreply_no_abort. Pattern: 3-arg connect to a
// QNetworkReply signal whose third arg is a bare lambda. With no context
// object, Qt cannot auto-disconnect when the captured `this` is destroyed —
// reply continues firing → use-after-free. Each @expect marker is one
// dangerous connect site. The good-side shows the 4-arg form which IS safe.

#include <QObject>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUrl>

class UnsafeFetcher : public QObject {
public:
    void start() {
        auto *mgr = new QNetworkAccessManager(this);
        QNetworkReply *reply = mgr->get(QNetworkRequest(QUrl("https://example.com")));
        connect(reply, &QNetworkReply::finished, [this, reply]() {  // @expect qnetworkreply_no_abort
            handle(reply);
        });
        connect(reply, &QNetworkReply::readyRead, [this, reply]() {  // @expect qnetworkreply_no_abort
            stream(reply);
        });
        connect(reply, &QNetworkReply::errorOccurred, [this](QNetworkReply::NetworkError) {  // @expect qnetworkreply_no_abort
            failure();
        });
    }

private:
    void handle(QNetworkReply *) {}
    void stream(QNetworkReply *) {}
    void failure() {}
};
