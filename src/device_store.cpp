#include "device_store.h"
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <utility>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ld {
namespace {
constexpr QFileDevice::Permissions PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions PrivateDirectory = PrivateFile | QFileDevice::ExeOwner;
const QByteArray DeviceHeader = QByteArrayLiteral("bananaDesk-device-dpapi-v1\n");

bool prepareDirectory(const QString &directory, QString &error) {
    if (!QDir().mkpath(directory)) {
        error = QStringLiteral("无法创建设备记录目录：%1").arg(directory);
        return false;
    }
#ifndef Q_OS_WIN
    struct stat st{};
    if (::lstat(QFile::encodeName(directory).constData(), &st) != 0 || !S_ISDIR(st.st_mode)
        || st.st_uid != ::geteuid() || !QFile::setPermissions(directory, PrivateDirectory)) {
        error = QStringLiteral("设备记录目录必须由当前用户拥有，并且只能由当前用户访问");
        return false;
    }
#endif
    return true;
}

bool protect(const QByteArray &plain, QByteArray &sealed, QString &error) {
#ifdef Q_OS_WIN
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"bananaDesk device records", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = QStringLiteral("无法使用当前 Windows 用户保护设备记录（错误 %1）").arg(GetLastError());
        return false;
    }
    sealed = DeviceHeader + QByteArray(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
#else
    Q_UNUSED(error);
    sealed = plain;
    return true;
#endif
}

bool unprotect(const QByteArray &sealed, QByteArray &plain, QString &error) {
#ifdef Q_OS_WIN
    if (!sealed.startsWith(DeviceHeader)) {
        error = QStringLiteral("设备记录格式损坏，请重新输入共享码");
        return false;
    }
    const auto payload = sealed.mid(DeviceHeader.size());
    DATA_BLOB input{DWORD(payload.size()), reinterpret_cast<BYTE *>(const_cast<char *>(payload.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = QStringLiteral("无法解密设备记录，请重新输入共享码");
        return false;
    }
    plain = QByteArray(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
#else
    Q_UNUSED(error);
    plain = sealed;
    return true;
#endif
}

bool readFile(const QString &path, QByteArray &plain, QString &error) {
    QFile file(path);
#ifndef Q_OS_WIN
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) {
        if (errno == ENOENT) { plain.clear(); return true; }
        error = QStringLiteral("无法读取设备记录"); return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink != 1) {
        error = QStringLiteral("设备记录必须是当前用户拥有的普通文件"); return false;
    }
#else
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD reason = GetLastError();
        if (reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND) { plain.clear(); return true; }
        error = QStringLiteral("无法读取设备记录（错误 %1）").arg(reason); return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !QFileInfo(path).isFile()) {
        error = QStringLiteral("设备记录必须是普通文件"); return false;
    }
#endif
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) {
        error = QStringLiteral("无法读取设备记录或文件过大"); return false;
    }
    const QByteArray sealed = file.readAll();
    if (file.error() != QFileDevice::NoError) { error = QStringLiteral("读取设备记录失败"); return false; }
    return unprotect(sealed, plain, error);
}

bool writeFile(const QString &directory, const QString &path, const QByteArray &plain, QString &error) {
    if (!prepareDirectory(directory, error)) return false;
    QByteArray sealed;
    if (!protect(plain, sealed, error)) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Unbuffered) || !file.setPermissions(PrivateFile)
        || file.write(sealed) != sealed.size() || !file.commit()) {
        error = QStringLiteral("无法保存设备记录：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool validHex(const QString &value, int length) {
    return QRegularExpression(QStringLiteral("^[0-9a-f]{%1}$").arg(length)).match(value).hasMatch();
}

QJsonObject savedToJson(const SavedDevice &device) {
    return {{"id", device.deviceId}, {"name", device.name}, {"host", device.invitation.host},
            {"port", device.invitation.port}, {"pin", device.invitation.fingerprint},
            {"last_connected", device.lastConnected.toUTC().toString(Qt::ISODate)} };
}
bool savedFromJson(const QJsonObject &o, SavedDevice &device) {
    QHostAddress address;
    const int port = o.value("port").toInt(-1);
    if (!validHex(o.value("id").toString(), 32) || !address.setAddress(o.value("host").toString())
        || address.protocol() != QAbstractSocket::IPv4Protocol || port < 1 || port > 65535
        || !validHex(o.value("pin").toString(), 64)) return false;
    device.deviceId = o.value("id").toString(); device.name = o.value("name").toString();
    device.invitation.host = address.toString(); device.invitation.port = quint16(port);
    device.invitation.fingerprint = o.value("pin").toString(); device.invitation.token.clear();
    device.lastConnected = QDateTime::fromString(o.value("last_connected").toString(), Qt::ISODate);
    if (!device.lastConnected.isValid()) device.lastConnected = QDateTime::currentDateTimeUtc();
    return true;
}

QJsonObject trustedToJson(const TrustedDevice &device) {
    return {{"id", device.id}, {"name", device.name}, {"secret", QString::fromLatin1(device.secret.toHex())},
            {"pin", device.hostFingerprint}, {"first_seen", device.firstSeen.toUTC().toString(Qt::ISODate)},
            {"last_seen", device.lastSeen.toUTC().toString(Qt::ISODate)}};
}
bool trustedFromJson(const QJsonObject &o, TrustedDevice &device) {
    const QString secret = o.value("secret").toString();
    if (!validHex(o.value("id").toString(), 32) || !validHex(secret, 64) || !validHex(o.value("pin").toString(), 64)) return false;
    device.id = o.value("id").toString(); device.name = o.value("name").toString();
    device.secret = QByteArray::fromHex(secret.toLatin1()); device.hostFingerprint = o.value("pin").toString();
    device.firstSeen = QDateTime::fromString(o.value("first_seen").toString(), Qt::ISODate);
    device.lastSeen = QDateTime::fromString(o.value("last_seen").toString(), Qt::ISODate);
    if (!device.firstSeen.isValid()) device.firstSeen = QDateTime::currentDateTimeUtc();
    if (!device.lastSeen.isValid()) device.lastSeen = device.firstSeen;
    return device.valid();
}
}

QByteArray randomBytes(int size) {
    QByteArray result(size, Qt::Uninitialized);
    return RAND_bytes(reinterpret_cast<unsigned char *>(result.data()), size) == 1 ? result : QByteArray();
}
QByteArray hmacSha256(const QByteArray &key, const QByteArray &message) {
    unsigned int length = 0; unsigned char output[EVP_MAX_MD_SIZE];
    if (!HMAC(EVP_sha256(), key.constData(), key.size(), reinterpret_cast<const unsigned char *>(message.constData()), size_t(message.size()), output, &length)) return {};
    return QByteArray(reinterpret_cast<const char *>(output), int(length));
}

DeviceIdentityStore::DeviceIdentityStore(QString directory) : directory_(QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}
QString DeviceIdentityStore::filePath() const { return QDir(directory_).filePath(QStringLiteral("controller-device-v1.dat")); }
bool DeviceIdentityStore::loadOrCreate(DeviceIdentity &out, QString &error) {
    QByteArray bytes;
    if (!readFile(filePath(), bytes, error)) return false;
    if (bytes.isEmpty()) {
        DeviceIdentity candidate;
        candidate.id = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        candidate.secret = randomBytes(32);
        if (!candidate.valid()) { error = QStringLiteral("无法生成控制设备凭据"); return false; }
        const QByteArray data = json({{"v", 1}, {"id", candidate.id}, {"secret", QString::fromLatin1(candidate.secret.toHex())}});
        if (!writeFile(directory_, filePath(), data, error)) return false;
        out = std::move(candidate);
        return true;
    }
    QJsonObject o; if (!object(bytes, o) || o.value("v").toInt() != 1 || !validHex(o.value("id").toString(), 32)
        || !validHex(o.value("secret").toString(), 64)) { error = QStringLiteral("控制设备凭据损坏，请重新配对"); return false; }
    out.id = o.value("id").toString(); out.secret = QByteArray::fromHex(o.value("secret").toString().toLatin1());
    return out.valid();
}

DeviceProfileStore::DeviceProfileStore(QString directory) : directory_(QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}
bool DeviceProfileStore::load(QList<SavedDevice> &out, QString &error) const {
    QByteArray bytes; if (!readFile(QDir(directory_).filePath("connections-v1.dat"), bytes, error)) return false;
    out.clear(); if (bytes.isEmpty()) return true;
    QJsonDocument doc = QJsonDocument::fromJson(bytes); if (!doc.isArray()) { error = QStringLiteral("已保存的连接记录损坏"); return false; }
    for (const auto &value : doc.array()) { SavedDevice item; if (value.isObject() && savedFromJson(value.toObject(), item)) out.append(item); }
    return true;
}
bool DeviceProfileStore::upsert(const SavedDevice &device, QString &error) {
    QList<SavedDevice> items; if (!load(items, error)) return false;
    bool found = false; for (auto &item : items) if (item.invitation.fingerprint == device.invitation.fingerprint) { item = device; found = true; break; }
    if (!found) items.append(device);
    QJsonArray array; for (const auto &item : items) array.append(savedToJson(item));
    return writeFile(directory_, QDir(directory_).filePath("connections-v1.dat"), QJsonDocument(array).toJson(QJsonDocument::Compact), error);
}
bool DeviceProfileStore::remove(const QString &hostFingerprint, QString &error) {
    QList<SavedDevice> items; if (!load(items, error)) return false;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const SavedDevice &item) { return item.invitation.fingerprint == hostFingerprint; }), items.end());
    QJsonArray array; for (const auto &item : items) array.append(savedToJson(item));
    return writeFile(directory_, QDir(directory_).filePath("connections-v1.dat"), QJsonDocument(array).toJson(QJsonDocument::Compact), error);
}

TrustedDeviceStore::TrustedDeviceStore(QString directory) : directory_(QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}
bool TrustedDeviceStore::load(QList<TrustedDevice> &out, QString &error) const {
    QByteArray bytes; if (!readFile(QDir(directory_).filePath("trusted-devices-v1.dat"), bytes, error)) return false;
    out.clear(); if (bytes.isEmpty()) return true;
    QJsonDocument doc = QJsonDocument::fromJson(bytes); if (!doc.isArray()) { error = QStringLiteral("已认证设备记录损坏"); return false; }
    for (const auto &value : doc.array()) { TrustedDevice item; if (value.isObject() && trustedFromJson(value.toObject(), item)) out.append(item); }
    return true;
}
bool TrustedDeviceStore::find(const QString &id, TrustedDevice &out, QString &error) const {
    QList<TrustedDevice> items; if (!load(items, error)) return false;
    for (const auto &item : items) if (item.id == id) { out = item; return true; }
    return false;
}
bool TrustedDeviceStore::upsert(const TrustedDevice &device, QString &error) {
    QList<TrustedDevice> items; if (!load(items, error)) return false;
    bool found = false; for (auto &item : items) if (item.id == device.id) { item = device; found = true; break; }
    if (!found) items.append(device);
    QJsonArray array; for (const auto &item : items) array.append(trustedToJson(item));
    return writeFile(directory_, QDir(directory_).filePath("trusted-devices-v1.dat"), QJsonDocument(array).toJson(QJsonDocument::Compact), error);
}
bool TrustedDeviceStore::remove(const QString &id, QString &error) {
    QList<TrustedDevice> items; if (!load(items, error)) return false;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const TrustedDevice &item) { return item.id == id; }), items.end());
    QJsonArray array; for (const auto &item : items) array.append(trustedToJson(item));
    return writeFile(directory_, QDir(directory_).filePath("trusted-devices-v1.dat"), QJsonDocument(array).toJson(QJsonDocument::Compact), error);
}
bool TrustedDeviceStore::clear(QString &error) {
    return writeFile(directory_, QDir(directory_).filePath("trusted-devices-v1.dat"), QByteArray("[]"), error);
}
}
