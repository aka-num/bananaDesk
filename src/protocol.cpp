#include "protocol.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QtEndian>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <memory>

namespace ld {
QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
bool object(const QByteArray &data, QJsonObject &out) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = doc.object(); return true;
}
bool equalSecret(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.constData(), b.constData(), size_t(a.size())) == 0;
}
bool Identity::create(QString &error) {
    using Ctx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
    using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
    auto fail = [&] { error = QStringLiteral("无法生成 TLS 身份"); return false; };
    Ctx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *raw = nullptr;
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0 || EVP_PKEY_keygen(ctx.get(), &raw) <= 0) return fail();
    Key privateKey(raw, EVP_PKEY_free);
    Cert cert(X509_new(), X509_free);
    if (!cert || !X509_set_version(cert.get(), 2) || !ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) || !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400 * 30) ||
        !X509_set_pubkey(cert.get(), privateKey.get())) return fail();
    X509_NAME *name = X509_get_subject_name(cert.get());
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("LanDesk ephemeral host"), -1, -1, 0) ||
        !X509_set_issuer_name(cert.get(), name) || !X509_sign(cert.get(), privateKey.get(), EVP_sha256())) return fail();
    Bio certBio(BIO_new(BIO_s_mem()), BIO_free), keyBio(BIO_new(BIO_s_mem()), BIO_free);
    if (!certBio || !keyBio || !PEM_write_bio_X509(certBio.get(), cert.get()) || !PEM_write_bio_PrivateKey(keyBio.get(), privateKey.get(), nullptr, nullptr, 0, nullptr, nullptr)) return fail();
    char *data = nullptr;
    long len = BIO_get_mem_data(certBio.get(), &data);
    certificate = QSslCertificate(QByteArray(data, int(len)), QSsl::Pem);
    len = BIO_get_mem_data(keyBio.get(), &data);
    key = QSslKey(QByteArray(data, int(len)), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    unsigned char secret[24];
    if (certificate.isNull() || key.isNull() || RAND_bytes(secret, sizeof(secret)) != 1) return fail();
    token = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(secret), sizeof(secret)).toHex());
    fingerprint = QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
    return true;
}
QString Invitation::encode() const {
    const auto data = json({{"v", 1}, {"host", host}, {"port", port}, {"pin", fingerprint}, {"token", token}});
    return QStringLiteral("landesk1:") + QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
bool Invitation::decode(const QString &text, Invitation &out, QString &error) {
    const auto s = text.trimmed();
    QJsonObject o;
    if (s.size() > 2048 || !s.startsWith("landesk1:") || !object(QByteArray::fromBase64(s.mid(9).toLatin1(), QByteArray::Base64UrlEncoding), o)) {
        error = QStringLiteral("连接码格式不正确"); return false;
    }
    static const QRegularExpression hex64("^[0-9a-f]{64}$"), hex48("^[0-9a-f]{48}$");
    const auto host = o.value("host").toString();
    QHostAddress address;
    const int port = o.value("port").toInt(-1);
    if (o.value("v").toInt() != 1 || !address.setAddress(host) || address.protocol() != QAbstractSocket::IPv4Protocol ||
        address.isMulticast() || address == QHostAddress::AnyIPv4 || address == QHostAddress::Broadcast ||
        port < 1 || port > 65535 || !hex64.match(o.value("pin").toString()).hasMatch() || !hex48.match(o.value("token").toString()).hasMatch()) {
        error = QStringLiteral("连接码版本、IPv4 地址、端口或密钥无效"); return false;
    }
    out = {host, quint16(port), o.value("pin").toString(), o.value("token").toString()};
    return true;
}
Wire::Wire(QSslSocket *socket, quint32 limit, QObject *parent) : QObject(parent), socket_(socket), receiveLimit_(limit) {
    socket_->setReadBufferSize(qint64(limit) + 4);
    connect(socket_, &QIODevice::readyRead, this, &Wire::read);
}
bool Wire::send(Packet type, const QByteArray &payload) {
    if (!socket_->isEncrypted() || quint64(payload.size()) + 1 > MaxPacket || socket_->bytesToWrite() > MaxPacket) return false;
    QByteArray header(5, '\0');
    qToBigEndian<quint32>(quint32(payload.size()) + 1, reinterpret_cast<uchar *>(header.data()));
    header[4] = char(type);
    return socket_->write(header) == header.size() && (payload.isEmpty() || socket_->write(payload) == payload.size());
}
void Wire::read() {
    while (socket_->bytesAvailable() > 0) {
        const qint64 room = qint64(receiveLimit_) + 4 - incoming_.size();
        if (room <= 0) { emit failure(QStringLiteral("接收缓冲区超限")); socket_->abort(); return; }
        incoming_ += socket_->read(room);
        while (incoming_.size() >= 4) {
            const quint32 size = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(incoming_.constData()));
            if (size < 1 || size > receiveLimit_) { emit failure(QStringLiteral("消息长度无效")); socket_->abort(); return; }
            if (quint64(incoming_.size()) < quint64(size) + 4) break;
            const Packet type = Packet(incoming_.at(4));
            const auto payload = incoming_.mid(5, int(size) - 1);
            incoming_.remove(0, int(size) + 4);
            emit packet(type, payload);
            if (socket_->state() == QAbstractSocket::UnconnectedState) return;
        }
    }
}
}
