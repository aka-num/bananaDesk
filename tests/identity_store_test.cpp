#include "identity_store.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <openssl/pem.h>
#include <openssl/x509.h>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#else
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray unseal(const QByteArray &bytes) {
#ifdef Q_OS_WIN
    const auto header = QByteArrayLiteral("bananaDesk-identity-dpapi-v1\n");
    if (!bytes.startsWith(header)) return {};
    auto payload = bytes.mid(header.size());
    DATA_BLOB input{DWORD(payload.size()), reinterpret_cast<BYTE *>(payload.data())}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    QByteArray plain(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    return plain;
#else
    return bytes;
#endif
}
QByteArray seal(const QByteArray &bytes) {
#ifdef Q_OS_WIN
    auto plain = bytes;
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(plain.data())}, output{};
    if (!CryptProtectData(&input, L"bananaDesk test identity", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    QByteArray encrypted = QByteArrayLiteral("bananaDesk-identity-dpapi-v1\n")
        + QByteArray(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    return encrypted;
#else
    return bytes;
#endif
}
bool changeCertificateDates(QJsonObject &record, int fromYears, int untilYears) {
    auto certificatePem = record.value("certificate").toString().toLatin1();
    auto keyPem = record.value("private_key").toString().toLatin1();
    using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
    using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    Bio certBio(BIO_new_mem_buf(certificatePem.constData(), certificatePem.size()), BIO_free);
    Bio keyBio(BIO_new_mem_buf(keyPem.constData(), keyPem.size()), BIO_free);
    if (!certBio || !keyBio) return false;
    Cert cert(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), X509_free);
    Key key(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    Bio output(BIO_new(BIO_s_mem()), BIO_free);
    const auto now = QDateTime::currentDateTimeUtc();
    const auto from = now.addYears(fromYears).toString("yyyyMMddHHmmss'Z'").toLatin1();
    const auto until = now.addYears(untilYears).toString("yyyyMMddHHmmss'Z'").toLatin1();
    if (!cert || !key || !output || !ASN1_TIME_set_string_X509(X509_getm_notBefore(cert.get()), from.constData())
        || !ASN1_TIME_set_string_X509(X509_getm_notAfter(cert.get()), until.constData())
        || !X509_sign(cert.get(), key.get(), EVP_sha256()) || !PEM_write_bio_X509(output.get(), cert.get())) return false;
    char *data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    certificatePem = QByteArray(data, int(size));
    record["certificate"] = QString::fromLatin1(certificatePem);
    record["fingerprint"] = QString::fromLatin1(QSslCertificate(certificatePem).digest(QCryptographicHash::Sha256).toHex());
    return true;
}
}

class IdentityStoreTest : public QObject {
    Q_OBJECT
private slots:
    void stableAcrossStopAndStoreRecreation() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::Identity first; QString error;
        const auto directory = temp.filePath("identity");
        QByteArray bytes;
        {
            ld::IdentityStore store(directory);
            QVERIFY2(store.loadOrCreate(first, error), qPrintable(error));
            bytes = readFile(store.filePath()); QVERIFY(!bytes.isEmpty());
            store.release();
            ld::Identity again;
            QVERIFY2(store.loadOrCreate(again, error), qPrintable(error));
            QCOMPARE(again.token, first.token);
            QCOMPARE(again.fingerprint, first.fingerprint);
            QCOMPARE(again.key.toPem(), first.key.toPem());
            QCOMPARE(readFile(store.filePath()), bytes);
        }
        ld::IdentityStore reopened(directory);
        ld::Identity restored;
        QVERIFY2(reopened.loadOrCreate(restored, error), qPrintable(error));
        QCOMPARE(restored.token, first.token);
        QCOMPARE(restored.certificate.toDer(), first.certificate.toDer());
        QCOMPARE(readFile(reopened.filePath()), bytes);
    }

    void resetChangesCredentialsAndPersists() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::IdentityStore store(temp.filePath("identity"));
        ld::Identity original; QString error;
        QVERIFY2(store.loadOrCreate(original, error), qPrintable(error));
        ld::Identity current = original;
        QVERIFY2(store.reset(current, error), qPrintable(error));
        QVERIFY(current.token != original.token);
        QVERIFY(current.fingerprint != original.fingerprint);
        QVERIFY(current.key.toPem() != original.key.toPem());
        store.release();
        ld::Identity restored;
        QVERIFY2(store.loadOrCreate(restored, error), qPrintable(error));
        QCOMPARE(restored.token, current.token);
        QCOMPARE(restored.fingerprint, current.fingerprint);
    }

    void exclusiveLockProtectsActiveCredential() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::IdentityStore first(temp.filePath("identity")), second(temp.filePath("identity"));
        ld::Identity original, attempted; QString error;
        QVERIFY2(first.loadOrCreate(original, error), qPrintable(error));
        const auto bytes = readFile(first.filePath());
        QVERIFY(!second.loadOrCreate(attempted, error));
        QVERIFY(error.contains(QStringLiteral("另一个")));
        QVERIFY(!second.reset(attempted, error));
        QVERIFY(attempted.certificate.isNull());
        QCOMPARE(readFile(first.filePath()), bytes);
        first.release();
        QVERIFY2(second.loadOrCreate(attempted, error), qPrintable(error));
        QCOMPARE(attempted.token, original.token);
        second.release();
        QVERIFY2(first.reset(attempted, error), qPrintable(error));
    }

    void certificateCoversLongTermUse() {
        ld::Identity identity; QString error;
        QVERIFY2(identity.create(error), qPrintable(error));
        const auto now = QDateTime::currentDateTimeUtc();
        QVERIFY(identity.certificate.effectiveDate() < now);
        QVERIFY(identity.certificate.expiryDate() > now.addYears(99));
        QVERIFY(identity.certificate.expiryDate() <= now.addYears(100).addSecs(1));
        QCOMPARE(identity.certificate.subjectInfo(QSslCertificate::CommonName), QStringList{QStringLiteral("bananaDesk persistent host")});
    }

    void corruptedStore_data() {
        QTest::addColumn<QString>("kind");
        for (const QString kind : {"json", "version", "token", "fingerprint", "certificate", "key_pair", "expired", "future", "oversized"})
            QTest::newRow(qPrintable(kind)) << kind;
    }
    void corruptedStore() {
        QFETCH(QString, kind);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::IdentityStore store(temp.filePath("identity"));
        ld::Identity original; QString error;
        QVERIFY2(store.loadOrCreate(original, error), qPrintable(error));
        auto record = QJsonDocument::fromJson(unseal(readFile(store.filePath()))).object();
        QVERIFY(!record.isEmpty());
        if (kind == "version") record["v"] = 2;
        if (kind == "token") record["token"] = "invalid";
        if (kind == "fingerprint") record["fingerprint"] = QString(64, '0');
        if (kind == "certificate") record["certificate"] = "bad certificate";
        if (kind == "key_pair") {
            ld::Identity other; QVERIFY2(other.create(error), qPrintable(error));
            record["private_key"] = QString::fromLatin1(other.key.toPem());
        }
        if (kind == "expired") QVERIFY(changeCertificateDates(record, -2, -1));
        if (kind == "future") QVERIFY(changeCertificateDates(record, 1, 100));
        auto changed = seal(kind == "json" ? QByteArrayLiteral("not json") : ld::json(record));
        if (kind == "oversized") changed = QByteArray(64 * 1024 + 1, 'x');
        QVERIFY(!changed.isEmpty());
        QVERIFY(writeFile(store.filePath(), changed));
        store.release();
        ld::Identity untouched = original;
        QVERIFY(!store.loadOrCreate(untouched, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(untouched.token, original.token);
        QCOMPARE(untouched.fingerprint, original.fingerprint);
        QCOMPARE(readFile(store.filePath()), changed);
        // Only an explicit reset recovers a corrupt file.
        QVERIFY2(store.reset(untouched, error), qPrintable(error));
        QVERIFY(untouched.token != original.token);
    }

    void failedResetPreservesDiskAndMemory() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::IdentityStore store(temp.filePath("identity"));
        ld::Identity original; QString error;
        QVERIFY2(store.loadOrCreate(original, error), qPrintable(error));
        const auto bytes = readFile(store.filePath());
        ld::Identity unchanged = original;
#ifdef Q_OS_WIN
        // Keep the destination readable but forbid replacement until the
        // handle closes, exercising QSaveFile commit failure on Windows.
        const auto path = QDir::toNativeSeparators(store.filePath());
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        const bool resetSucceeded = store.reset(unchanged, error);
        CloseHandle(handle);
#else
        // Apply a tiny per-process file limit only while saving, so even an
        // atomic-write failure must leave the previous identity intact.
        struct rlimit previous{};
        QVERIFY(::getrlimit(RLIMIT_FSIZE, &previous) == 0);
        struct sigaction ignored{}, previousAction{};
        ignored.sa_handler = SIG_IGN; ::sigemptyset(&ignored.sa_mask);
        QVERIFY(::sigaction(SIGXFSZ, &ignored, &previousAction) == 0);
        auto small = previous; small.rlim_cur = 128;
        const bool limited = ::setrlimit(RLIMIT_FSIZE, &small) == 0;
        const bool resetSucceeded = limited && store.reset(unchanged, error);
        const bool restored = ::setrlimit(RLIMIT_FSIZE, &previous) == 0;
        const bool signalRestored = ::sigaction(SIGXFSZ, &previousAction, nullptr) == 0;
        QVERIFY(limited); QVERIFY(restored); QVERIFY(signalRestored);
#endif
        QVERIFY(!resetSucceeded); QVERIFY(!error.isEmpty());
        QCOMPARE(unchanged.token, original.token);
        QCOMPARE(unchanged.fingerprint, original.fingerprint);
        QCOMPARE(readFile(store.filePath()), bytes);
        QVERIFY2(store.reset(unchanged, error), qPrintable(error));
        QVERIFY(unchanged.token != original.token);
    }

    void unavailableDirectoryIsReported() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto blocked = temp.filePath("file");
        QVERIFY(writeFile(blocked, "sentinel"));
        ld::IdentityStore store(QDir(blocked).filePath("identity"));
        ld::Identity identity; QString error;
        QVERIFY(!store.loadOrCreate(identity, error));
        QVERIFY(!error.isEmpty()); QVERIFY(identity.certificate.isNull());
        QVERIFY(!store.reset(identity, error));
        QCOMPARE(readFile(blocked), QByteArrayLiteral("sentinel"));
    }

    void protectedStorage() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::IdentityStore store(temp.filePath("identity"));
        ld::Identity identity; QString error;
        QVERIFY2(store.loadOrCreate(identity, error), qPrintable(error));
#ifdef Q_OS_WIN
        const auto bytes = readFile(store.filePath());
        QVERIFY(!bytes.contains(identity.token.toLatin1()));
        QVERIFY(!bytes.contains("PRIVATE KEY"));
        QVERIFY(unseal(bytes).contains(identity.token.toLatin1()));
#else
        struct stat st{};
        QVERIFY(::stat(QFile::encodeName(temp.filePath("identity")).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0700)); QCOMPARE(st.st_uid, ::geteuid());
        QVERIFY(::stat(QFile::encodeName(store.filePath()).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0600)); QCOMPARE(st.st_uid, ::geteuid());
        QVERIFY(::chmod(QFile::encodeName(store.filePath()).constData(), 0644) == 0);
        QVERIFY(::chmod(QFile::encodeName(temp.filePath("identity")).constData(), 0755) == 0);
        store.release();
        QVERIFY2(store.loadOrCreate(identity, error), qPrintable(error));
        QVERIFY(::stat(QFile::encodeName(store.filePath()).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0600));
        QVERIFY(::stat(QFile::encodeName(temp.filePath("identity")).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0700));
#endif
    }

    void unreadableIdentityIsNotRegenerated() {
#ifdef Q_OS_WIN
        QSKIP("POSIX file permission check; Windows storage is protected with DPAPI");
#else
        QTemporaryDir temp; QVERIFY(temp.isValid());
        ld::IdentityStore store(temp.filePath("identity"));
        ld::Identity identity; QString error;
        QVERIFY2(store.loadOrCreate(identity, error), qPrintable(error));
        const auto bytes = readFile(store.filePath());
        store.release();
        QVERIFY(::chmod(QFile::encodeName(store.filePath()).constData(), 0) == 0);
        ld::Identity unloaded;
        const bool loaded = store.loadOrCreate(unloaded, error);
        QVERIFY(::chmod(QFile::encodeName(store.filePath()).constData(), 0600) == 0);
        QVERIFY(!loaded); QVERIFY(!error.isEmpty()); QVERIFY(unloaded.certificate.isNull());
        QCOMPARE(readFile(store.filePath()), bytes);
#endif
    }

    void linksCannotRedirectIdentityStorage() {
#ifdef Q_OS_WIN
        QSKIP("POSIX symlink and hard-link checks");
#else
        QTemporaryDir temp; QVERIFY(temp.isValid());
        QVERIFY(QDir().mkdir(temp.filePath("target")));
        QVERIFY(::symlink(QFile::encodeName(temp.filePath("target")).constData(), QFile::encodeName(temp.filePath("link")).constData()) == 0);
        ld::Identity identity; QString error;
        ld::IdentityStore linkedDirectory(temp.filePath("link/identity"));
        QVERIFY(!linkedDirectory.loadOrCreate(identity, error));
        QVERIFY(!QFileInfo::exists(temp.filePath("target/identity")));
        ld::IdentityStore store(temp.filePath("identity"));
        QVERIFY2(store.loadOrCreate(identity, error), qPrintable(error));
        store.release();
        QVERIFY(QFile::remove(store.filePath()));
        const auto sentinel = temp.filePath("sentinel");
        QVERIFY(writeFile(sentinel, "do not overwrite"));
        QVERIFY(::symlink(QFile::encodeName(sentinel).constData(), QFile::encodeName(store.filePath()).constData()) == 0);
        QVERIFY(!store.loadOrCreate(identity, error));
        QVERIFY(!store.reset(identity, error));
        QCOMPARE(readFile(sentinel), QByteArrayLiteral("do not overwrite"));
        QVERIFY(QFile::remove(store.filePath()));
        QVERIFY(::link(QFile::encodeName(sentinel).constData(), QFile::encodeName(store.filePath()).constData()) == 0);
        QVERIFY(!store.loadOrCreate(identity, error));
        QVERIFY(!store.reset(identity, error));
        QCOMPARE(readFile(sentinel), QByteArrayLiteral("do not overwrite"));
#endif
    }
};
QTEST_GUILESS_MAIN(IdentityStoreTest)
#include "identity_store_test.moc"
