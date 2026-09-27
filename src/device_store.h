#pragma once

#include "protocol.h"
#include <QDateTime>
#include <QList>
#include <QString>

namespace ld {

struct DeviceIdentity {
    QString id;
    QByteArray secret;
    bool valid() const { return !id.isEmpty() && secret.size() == 32; }
};

struct SavedDevice {
    QString deviceId;
    QString name;
    Invitation invitation;
    QDateTime lastConnected;
};

struct TrustedDevice {
    QString id;
    QString name;
    QByteArray secret;
    QString hostFingerprint;
    QDateTime firstSeen;
    QDateTime lastSeen;
    bool valid() const { return !id.isEmpty() && secret.size() == 32 && hostFingerprint.size() == 64; }
};

QByteArray randomBytes(int size);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &message);

class DeviceIdentityStore {
public:
    explicit DeviceIdentityStore(QString directory);
    bool loadOrCreate(DeviceIdentity &out, QString &error);
    QString filePath() const;
private:
    QString directory_;
};

class DeviceProfileStore {
public:
    explicit DeviceProfileStore(QString directory);
    bool load(QList<SavedDevice> &out, QString &error) const;
    bool upsert(const SavedDevice &device, QString &error);
    bool remove(const QString &hostFingerprint, QString &error);
private:
    QString directory_;
};

class TrustedDeviceStore {
public:
    explicit TrustedDeviceStore(QString directory);
    bool load(QList<TrustedDevice> &out, QString &error) const;
    bool find(const QString &id, TrustedDevice &out, QString &error) const;
    bool upsert(const TrustedDevice &device, QString &error);
    bool remove(const QString &id, QString &error);
    bool clear(QString &error);
private:
    QString directory_;
};
}
