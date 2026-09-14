#pragma once
#include <QObject>
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslKey>
#include <QJsonObject>
#include <QHostAddress>
#include <QCryptographicHash>

namespace ld {
constexpr quint16 DefaultPort = 24832;
constexpr quint32 MaxPacket = 8 * 1024 * 1024;
enum class Packet : char { Auth = 'A', Welcome = 'W', Image = 'F', Ack = 'K', Input = 'I', Release = 'R', Ping = 'P', Pong = 'Q', Error = 'E' };
QByteArray json(const QJsonObject &object);
bool object(const QByteArray &data, QJsonObject &out);
bool equalSecret(const QByteArray &a, const QByteArray &b);

struct Identity {
    QSslCertificate certificate;
    QSslKey key;
    QString fingerprint;
    QString token;
    bool create(QString &error);
};
struct Invitation {
    QString host;
    quint16 port = DefaultPort;
    QString fingerprint;
    QString token;
    QString encode() const;
    static bool decode(const QString &text, Invitation &out, QString &error);
};

// One bounded message protocol; session semantics do not depend on discovery or NAT traversal.
class Wire : public QObject {
    Q_OBJECT
public:
    explicit Wire(QSslSocket *socket, quint32 receiveLimit, QObject *parent);
    QSslSocket *socket() const { return socket_; }
    bool send(Packet type, const QByteArray &payload = {});
signals:
    void packet(ld::Packet type, QByteArray payload);
    void failure(QString message);
private:
    void read();
    QSslSocket *socket_;
    quint32 receiveLimit_;
    QByteArray incoming_;
};
}
