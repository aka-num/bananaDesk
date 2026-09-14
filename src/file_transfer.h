#pragma once
#include <QObject>
#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <memory>

namespace ld {
constexpr qint64 MaxTransferFileSize = qint64(8) * 1024 * 1024 * 1024;
constexpr int FileChunkSize = 32 * 1024;
constexpr int MaxFilePayload = 64 * 1024;

// The caller admits packets only after TLS authentication and file permission.
// configure/reset never create directories. The remote namespace is flat.
class FileTransferHost : public QObject {
    Q_OBJECT
public:
    explicit FileTransferHost(QObject *parent = nullptr);
    ~FileTransferHost() override;
    void configure(const QString &root);
    void receive(const QByteArray &payload);
    void reset();
    bool busy() const;
    bool offering() const;
    bool offerAvailable() const;
    void setOfferAvailable(bool available);
    void offerFile(const QString &localPath);
    void cancelOffer();
signals:
    void send(QByteArray payload);
    void status(QString text);
    void busyChanged(bool busy);
    void offerAvailableChanged(bool available);
    void progress(qint64 done, qint64 total);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class FileTransferClient : public QObject {
    Q_OBJECT
public:
    explicit FileTransferClient(QObject *parent = nullptr);
    ~FileTransferClient() override;
    bool available() const;
    bool busy() const;
    void setAvailable(bool available);
    bool offerAvailable() const;
    void setOfferAvailable(bool available);
    void acceptOffer(const QString &id, const QString &localPath);
    void declineOffer(const QString &id);
    void refresh();
    void upload(const QString &localPath);
    void download(const QString &remoteName, const QString &localPath);
    void cancel();
    void receive(const QByteArray &payload);
    void reset();
signals:
    void send(QByteArray payload);
    void listing(QJsonArray files); // Each item: {"name": string, "size": number}.
    void progress(qint64 done, qint64 total);
    void status(QString text);
    void busyChanged(bool busy);
    void availableChanged(bool available);
    void offered(QString id, QString name, qint64 size);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
