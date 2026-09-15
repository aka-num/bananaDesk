#include "identity_store.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <openssl/pem.h>
#include <openssl/x509.h>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ld {
namespace {
constexpr qint64 MaxIdentityBytes = 64 * 1024;
constexpr QFileDevice::Permissions PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions PrivateDirectory = PrivateFile | QFileDevice::ExeOwner;

bool decodeIdentity(const QByteArray &bytes, Identity &out, QString &error) {
    auto invalid = [&error] {
        error = QStringLiteral("保存的共享身份损坏或已失效；请使用“重置共享码”重新创建，旧共享码将失效");
        return false;
    };
    QJsonObject record;
    if (!object(bytes, record) || record.value("v").toInt(-1) != 1) return invalid();
    const auto certPem = record.value("certificate").toString().toLatin1();
    const auto keyPem = record.value("private_key").toString().toLatin1();
    Identity candidate;
    const auto certificates = QSslCertificate::fromData(certPem, QSsl::Pem);
    if (certificates.size() != 1) return invalid();
    candidate.certificate = certificates.front();
    candidate.key = QSslKey(keyPem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    candidate.token = record.value("token").toString();
    candidate.fingerprint = record.value("fingerprint").toString();
    static const QRegularExpression tokenPattern(QStringLiteral("^[0-9a-f]{48}$"));
    const auto now = QDateTime::currentDateTimeUtc();
    if (candidate.key.isNull() || !tokenPattern.match(candidate.token).hasMatch()
        || candidate.fingerprint != QString::fromLatin1(candidate.certificate.digest(QCryptographicHash::Sha256).toHex())
        || !candidate.certificate.effectiveDate().isValid() || !candidate.certificate.expiryDate().isValid()
        || candidate.certificate.effectiveDate() > now || candidate.certificate.expiryDate() <= now) return invalid();
    using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    Bio certificateBio(BIO_new_mem_buf(certPem.constData(), certPem.size()), BIO_free);
    Bio keyBio(BIO_new_mem_buf(keyPem.constData(), keyPem.size()), BIO_free);
    if (!certificateBio || !keyBio) return invalid();
    Cert certificate(PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr), X509_free);
    Key key(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!certificate || !key || X509_check_private_key(certificate.get(), key.get()) != 1
        || X509_verify(certificate.get(), key.get()) != 1) return invalid();
    out = std::move(candidate);
    return true;
}

#ifdef Q_OS_WIN
const QByteArray ProtectedHeader = QByteArrayLiteral("bananaDesk-identity-dpapi-v1\n");
bool protect(const QByteArray &plain, QByteArray &sealed, QString &error) {
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"bananaDesk sharing identity", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = QStringLiteral("无法使用当前 Windows 用户保护共享身份（错误 %1）").arg(GetLastError());
        return false;
    }
    sealed = ProtectedHeader + QByteArray(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}
bool unprotect(const QByteArray &sealed, QByteArray &plain, QString &error) {
    if (!sealed.startsWith(ProtectedHeader)) {
        error = QStringLiteral("保存的共享身份格式损坏；请手动重置共享码");
        return false;
    }
    const auto payload = sealed.mid(ProtectedHeader.size());
    DATA_BLOB input{DWORD(payload.size()), reinterpret_cast<BYTE *>(const_cast<char *>(payload.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = QStringLiteral("无法解密共享身份：文件损坏或不属于当前 Windows 用户；可手动重置共享码");
        return false;
    }
    plain = QByteArray(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}
#else
bool noSymlinkComponents(const QString &path) {
    QString prefix;
    for (const auto &component : path.split('/', Qt::SkipEmptyParts)) {
        prefix += '/' + component;
        struct stat st{};
        if (::lstat(QFile::encodeName(prefix).constData(), &st) == 0) {
            if (S_ISLNK(st.st_mode)) return false;
        } else if (errno != ENOENT) return false;
    }
    return true;
}
#endif
}

IdentityStore::IdentityStore(QString directory) : directory_(QDir::cleanPath(QFileInfo(directory).absoluteFilePath())) {}
IdentityStore::~IdentityStore() { release(); }
QString IdentityStore::filePath() const { return QDir(directory_).filePath(QStringLiteral("identity-v1.dat")); }
void IdentityStore::release() { lock_.reset(); }

bool IdentityStore::prepareDirectory(QString &error) {
#ifndef Q_OS_WIN
    if (!noSymlinkComponents(directory_)) {
        error = QStringLiteral("共享身份目录不能使用符号链接或不可访问的路径");
        return false;
    }
#endif
    if (!QDir().mkpath(directory_)) {
        error = QStringLiteral("无法创建共享身份目录：%1").arg(directory_);
        return false;
    }
#ifndef Q_OS_WIN
    struct stat st{};
    if (::lstat(QFile::encodeName(directory_).constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()
        || !QFile::setPermissions(directory_, PrivateDirectory)) {
        error = QStringLiteral("共享身份目录必须由当前用户拥有，并且只能由当前用户访问");
        return false;
    }
#endif
    return true;
}

bool IdentityStore::acquire(QString &error) {
    if (!prepareDirectory(error)) return false;
    if (lock_) return true;
    const auto lockPath = QDir(directory_).filePath(QStringLiteral("sharing.lock"));
#ifndef Q_OS_WIN
    struct stat st{};
    if (::lstat(QFile::encodeName(lockPath).constData(), &st) == 0
        && (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink != 1)) {
        error = QStringLiteral("共享身份锁文件不安全，请检查应用数据目录");
        return false;
    }
#endif
    auto candidate = std::make_unique<QLockFile>(lockPath);
    candidate->setStaleLockTime(0);
    if (!candidate->tryLock(0)) {
        error = candidate->error() == QLockFile::LockFailedError
            ? QStringLiteral("另一个 bananaDesk 程序正在共享或重置共享码，请先停止它的共享")
            : QStringLiteral("无法锁定共享身份目录，请检查目录权限");
        return false;
    }
    lock_ = std::move(candidate);
    return true;
}

bool IdentityStore::checkFile(QString &error) const {
#ifndef Q_OS_WIN
    struct stat st{};
    if (::lstat(QFile::encodeName(filePath()).constData(), &st) != 0) {
        if (errno == ENOENT) return true;
        error = QStringLiteral("无法访问保存的共享身份");
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink != 1) {
        error = QStringLiteral("保存的共享身份必须是当前用户拥有的普通文件，不能是链接");
        return false;
    }
#else
    const QFileInfo info(filePath());
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        error = QStringLiteral("保存的共享身份必须是普通文件，不能是链接");
        return false;
    }
#endif
    return true;
}

bool IdentityStore::read(Identity &out, QString &error) const {
    if (!checkFile(error)) return false;
    QFile file(filePath());
#ifndef Q_OS_WIN
    const int descriptor = ::open(QFile::encodeName(filePath()).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) { error = QStringLiteral("无法读取保存的共享身份；请检查文件权限"); return false; }
    struct stat st{};
    if (::fstat(descriptor, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink != 1
        || ::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        ::close(descriptor);
        error = QStringLiteral("无法安全读取保存的共享身份");
        return false;
    }
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
#else
    if (!file.open(QIODevice::ReadOnly)) {
#endif
        error = QStringLiteral("无法读取保存的共享身份；请检查文件权限");
        return false;
    }
    QByteArray bytes = file.read(MaxIdentityBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > MaxIdentityBytes) {
        error = QStringLiteral("保存的共享身份无法读取或文件过大；请手动重置共享码");
        return false;
    }
#ifdef Q_OS_WIN
    QByteArray plain;
    if (!unprotect(bytes, plain, error)) return false;
    bytes = std::move(plain);
#endif
    return decodeIdentity(bytes, out, error);
}

bool IdentityStore::save(const Identity &identity, QString &error) const {
    if (!checkFile(error)) return false;
    QByteArray bytes = json({{"v", 1}, {"certificate", QString::fromLatin1(identity.certificate.toPem())},
        {"private_key", QString::fromLatin1(identity.key.toPem())}, {"fingerprint", identity.fingerprint}, {"token", identity.token}});
#ifdef Q_OS_WIN
    QByteArray sealed;
    if (!protect(bytes, sealed, error)) return false;
    bytes = std::move(sealed);
#endif
    QSaveFile file(filePath());
    file.setDirectWriteFallback(false);
    // Qt 5 buffered commit can report success after a short flush. Observe
    // the actual write count before permitting replacement of the identity.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Unbuffered) || !file.setPermissions(PrivateFile)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("无法保存共享身份：%1；原共享码保持不变").arg(file.errorString());
        return false;
    }
    return true;
}

bool IdentityStore::loadOrCreate(Identity &out, QString &error) {
    const bool hadLock = bool(lock_);
    auto fail = [&] { if (!hadLock) release(); return false; };
    if (!acquire(error) || !checkFile(error)) return fail();
    Identity candidate;
    if (QFileInfo::exists(filePath())) {
        if (!read(candidate, error)) return fail();
    } else if (!candidate.create(error) || !save(candidate, error)) return fail();
    out = std::move(candidate);
    return true;
}

bool IdentityStore::reset(Identity &out, QString &error) {
    const bool hadLock = bool(lock_);
    auto fail = [&] { if (!hadLock) release(); return false; };
    if (!acquire(error)) return fail();
    Identity candidate;
    if (!candidate.create(error) || !save(candidate, error)) return fail();
    out = std::move(candidate);
    return true;
}
}
