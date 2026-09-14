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
signals:
    void send(QByteArray payload);
    void status(QString text);
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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
