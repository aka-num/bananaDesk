#include "app.h"
#include "file_transfer.h"
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace {
QByteArray payload(int size) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) bytes[i] = char((i * 37 + i / 251) & 255);
    return bytes;
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class FileTransferNativeClientTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<QJsonArray>("QJsonArray");
        if (qEnvironmentVariable("LANDESK_ISOLATED_TEST") != "1")
            QSKIP("This suite requires a disposable desktop, set LANDESK_ISOLATED_TEST=1");
#ifndef Q_OS_WIN
        if (QGuiApplication::platformName() != "xcb")
            QSKIP("Run this TLS/native-client integration suite on private Xvfb");
#endif
    }
    void nativeClientUploadsDownloadsAndLists() {
        QTemporaryDir hostRoot, localRoot;
        QVERIFY(hostRoot.isValid() && localRoot.isValid());
        ld::Host host(nullptr); ld::Client client(nullptr);
        host.configureFiles(hostRoot.path(), true);
        host.configureVideo(15, "jpeg"); host.setLockOnDisconnect(false);
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, true, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        auto *files = client.fileTransfer();
        QSignalSpy frames(&client, &ld::Client::frame);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        QSignalSpy lists(files, &ld::FileTransferClient::listing);
        QSignalSpy progress(files, &ld::FileTransferClient::progress);
        client.start(invitation);
        QTRY_VERIFY_WITH_TIMEOUT(files->available() && !frames.isEmpty(), 8000);
        disconnected.clear();

        const QByteArray bytes = payload(3 * ld::FileChunkSize + 997);
        const QString source = localRoot.filePath("native-binary.dat");
        QVERIFY(writeFile(source, bytes));
        files->upload(source);
        QTRY_VERIFY_WITH_TIMEOUT(!files->busy(), 10000);
        QVERIFY(QFile::exists(hostRoot.filePath("native-binary.dat")));
        QCOMPARE(readFile(hostRoot.filePath("native-binary.dat")), bytes);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last()[0].toLongLong(), qint64(bytes.size()));

        lists.clear(); files->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!lists.isEmpty(), 5000);
        const auto entries = qvariant_cast<QJsonArray>(lists.last()[0]);
        bool found = false;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            if (entry["name"].toString() == "native-binary.dat") {
                QCOMPARE(entry["size"].toVariant().toLongLong(), qint64(bytes.size())); found = true;
            }
        }
        QVERIFY(found);
        const QString destination = localRoot.filePath("downloaded.dat");
        files->download("native-binary.dat", destination);
        QTRY_VERIFY_WITH_TIMEOUT(!files->busy(), 10000);
        QVERIFY(QFile::exists(destination)); QCOMPARE(readFile(destination), bytes);
        QCOMPARE(disconnected.count(), 0);
        const int frameCount = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > frameCount, 5000);
        client.stop(); host.stop();
    }
    void cancellationPreservesConnectionAndFiles() {
        QTemporaryDir hostRoot, localRoot;
        QVERIFY(hostRoot.isValid() && localRoot.isValid());
        ld::Host host(nullptr); ld::Client client(nullptr);
        host.configureFiles(hostRoot.path(), true);
        host.configureVideo(15, "jpeg"); host.setLockOnDisconnect(false);
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, true, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        auto *files = client.fileTransfer();
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        QSignalSpy lists(files, &ld::FileTransferClient::listing);
        client.start(invitation);
        QTRY_VERIFY_WITH_TIMEOUT(files->available(), 8000);
        disconnected.clear();
        const QByteArray bytes = payload(1024 * 1024);
        const QString source = localRoot.filePath("cancel-upload.dat");
        QVERIFY(writeFile(source, bytes));
        bool uploadCancelled = false;
        auto cancelUpload = connect(files, &ld::FileTransferClient::progress, this,
            [&](qint64 done, qint64 total) {
                if (!uploadCancelled && done > 0 && done < total) { uploadCancelled = true; files->cancel(); }
            });
        files->upload(source);
        QTRY_VERIFY_WITH_TIMEOUT(uploadCancelled && !files->busy(), 10000);
        disconnect(cancelUpload);
        QVERIFY(!QFile::exists(hostRoot.filePath("cancel-upload.dat")));
        lists.clear(); files->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!lists.isEmpty(), 5000);
        for (const auto &value : qvariant_cast<QJsonArray>(lists.last()[0]))
            QVERIFY(value.toObject()["name"].toString() != "cancel-upload.dat");

        QVERIFY(writeFile(hostRoot.filePath("cancel-download.dat"), bytes));
        const QString destination = localRoot.filePath("existing-target.dat");
        const QByteArray original("existing target must survive cancellation");
        QVERIFY(writeFile(destination, original));
        bool downloadCancelled = false;
        auto cancelDownload = connect(files, &ld::FileTransferClient::progress, this,
            [&](qint64 done, qint64 total) {
                if (!downloadCancelled && done > 0 && done < total) { downloadCancelled = true; files->cancel(); }
            });
        files->download("cancel-download.dat", destination);
        QTRY_VERIFY_WITH_TIMEOUT(downloadCancelled && !files->busy(), 10000);
        disconnect(cancelDownload);
        QCOMPARE(readFile(destination), original);
        lists.clear(); files->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!lists.isEmpty(), 5000);
        QCOMPARE(disconnected.count(), 0);
        client.stop(); host.stop();
    }
    void hostOffersExternalFileAndWaitsForRecipientAcceptance() {
        QTemporaryDir hostRoot, sourceRoot, destinationRoot;
        QVERIFY(hostRoot.isValid() && sourceRoot.isValid() && destinationRoot.isValid());
        const QByteArray bytes = payload(4 * ld::FileChunkSize + 257);
        const QString source = sourceRoot.filePath("chosen-outside-share.dat");
        const QString destination = destinationRoot.filePath("accepted-file.dat");
        QVERIFY(writeFile(source, bytes));
        QVERIFY(QFileInfo(source).absolutePath() != hostRoot.path());

        ld::Host host(nullptr); ld::Client client(nullptr);
        host.configureFiles(hostRoot.path(), true);
        host.configureVideo(15, "jpeg"); host.setLockOnDisconnect(false);
        auto *hostFiles = host.fileTransfer();
        auto *clientFiles = client.fileTransfer();
        QSignalSpy offered(clientFiles, &ld::FileTransferClient::offered);
        QSignalSpy hostPackets(hostFiles, &ld::FileTransferHost::send);
        QSignalSpy hostStatus(hostFiles, &ld::FileTransferHost::status);
        QSignalSpy hostProgress(hostFiles, &ld::FileTransferHost::progress);
        QSignalSpy frames(&client, &ld::Client::frame);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        QVERIFY(!hostFiles->offerAvailable());
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, true, error), qPrintable(error));
        QVERIFY(!hostFiles->offerAvailable()); // Listening alone does not authorize an offer.
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        client.start(invitation);
        QTRY_VERIFY_WITH_TIMEOUT(clientFiles->available() && clientFiles->offerAvailable() &&
            hostFiles->offerAvailable() && !frames.isEmpty(), 8000);
        disconnected.clear(); hostPackets.clear(); hostStatus.clear();

        QVERIFY(!QFile::exists(destination));
        hostFiles->offerFile(source);
        QVERIFY(hostFiles->busy() && hostFiles->offering());
        QTRY_COMPARE_WITH_TIMEOUT(offered.count(), 1, 5000);
        const QString id = offered.front()[0].toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(offered.front()[1].toString(), QStringLiteral("chosen-outside-share.dat"));
        QCOMPARE(offered.front()[2].toLongLong(), qint64(bytes.size()));
        // The receiver has not chosen a destination yet. Only an invitation
        // packet may leave the host; there are no file bytes or saved files.
        QTest::qWait(150);
        QCOMPARE(hostPackets.count(), 1);
        QCOMPARE(QJsonDocument::fromJson(hostPackets.front()[0].toByteArray()).object().value("op").toString(), QStringLiteral("offer"));
        QVERIFY(!QFile::exists(destination));
        QVERIFY(QDir(destinationRoot.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(QDir(hostRoot.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

        clientFiles->acceptOffer(id, destination);
        QTRY_VERIFY_WITH_TIMEOUT(!hostFiles->busy() && !clientFiles->busy(), 10000);
        QVERIFY(!hostFiles->offering());
        QCOMPARE(readFile(destination), bytes);
        QCOMPARE(readFile(source), bytes);
        QVERIFY(!hostProgress.isEmpty());
        QCOMPARE(hostProgress.last()[0].toLongLong(), qint64(bytes.size()));
        QCOMPARE(hostProgress.last()[1].toLongLong(), qint64(bytes.size()));
        bool recipientConfirmedSave = false;
        for (const auto &event : hostStatus)
            recipientConfirmedSave |= event[0].toString().contains(QStringLiteral("对方已接收并保存"));
        QVERIFY(recipientConfirmedSave); // Host saw the receipt after the client's atomic commit.
        QCOMPARE(disconnected.count(), 0);
        const int before = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > before, 5000);
        client.stop(); host.stop();
    }
    void offeredFilesCanBeDeclinedWithdrawnAndRetriedAfterDisconnect() {
        QTemporaryDir hostRoot, sourceRoot, destinationRoot;
        QVERIFY(hostRoot.isValid() && sourceRoot.isValid() && destinationRoot.isValid());
        const QByteArray bytes = payload(1024 * 1024);
        const QString source = sourceRoot.filePath("retry-offer.dat");
        QVERIFY(writeFile(source, bytes));
        ld::Host host(nullptr); ld::Client client(nullptr);
        host.configureFiles(hostRoot.path(), true);
        host.configureVideo(15, "jpeg"); host.setLockOnDisconnect(false);
        auto *hostFiles = host.fileTransfer();
        auto *clientFiles = client.fileTransfer();
        QSignalSpy offered(clientFiles, &ld::FileTransferClient::offered);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        QSignalSpy frames(&client, &ld::Client::frame);
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, true, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        client.start(invitation);
        QTRY_VERIFY_WITH_TIMEOUT(clientFiles->offerAvailable() && hostFiles->offerAvailable() && !frames.isEmpty(), 8000);
        disconnected.clear();

        hostFiles->offerFile(source);
        QTRY_COMPARE_WITH_TIMEOUT(offered.count(), 1, 5000);
        clientFiles->declineOffer(offered.last()[0].toString());
        QTRY_VERIFY_WITH_TIMEOUT(!hostFiles->busy() && !clientFiles->busy(), 5000);
        QVERIFY(!hostFiles->offering());
        QVERIFY(QDir(destinationRoot.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());

        // Withdraw after some bytes were received, exercising atomic-file
        // cleanup instead of only dismissing the pending offer notification.
        hostFiles->offerFile(source);
        QTRY_COMPARE_WITH_TIMEOUT(offered.count(), 2, 5000);
        const QString withdrawnDestination = destinationRoot.filePath("withdrawn.dat");
        bool withdrawn = false;
        auto withdraw = connect(clientFiles, &ld::FileTransferClient::progress, this,
            [&](qint64 done, qint64 total) {
                if (!withdrawn && done > 0 && done < total) { withdrawn = true; hostFiles->cancelOffer(); }
            });
        clientFiles->acceptOffer(offered.last()[0].toString(), withdrawnDestination);
        QTRY_VERIFY_WITH_TIMEOUT(withdrawn && !hostFiles->busy() && !clientFiles->busy(), 10000);
        disconnect(withdraw);
        QVERIFY(!QFile::exists(withdrawnDestination));
        QVERIFY(QDir(destinationRoot.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        QCOMPARE(disconnected.count(), 0);
        QVERIFY(hostFiles->offerAvailable() && clientFiles->offerAvailable());

        // Disconnect while replacing a user-approved existing destination;
        // QSaveFile must retain the original and remove the incomplete file.
        const QString interruptedDestination = destinationRoot.filePath("existing.dat");
        const QByteArray original("existing local content survives disconnection");
        QVERIFY(writeFile(interruptedDestination, original));
        hostFiles->offerFile(source);
        QTRY_COMPARE_WITH_TIMEOUT(offered.count(), 3, 5000);
        bool interrupted = false;
        auto disconnectDuringReceive = connect(clientFiles, &ld::FileTransferClient::progress, this,
            [&](qint64 done, qint64 total) {
                if (!interrupted && done > 0 && done < total) { interrupted = true; client.stop(); }
            });
        clientFiles->acceptOffer(offered.last()[0].toString(), interruptedDestination);
        QTRY_VERIFY_WITH_TIMEOUT(interrupted && !hostFiles->busy() && !clientFiles->busy() && !hostFiles->offerAvailable(), 10000);
        disconnect(disconnectDuringReceive);
        QVERIFY(!clientFiles->offerAvailable());
        QCOMPARE(readFile(interruptedDestination), original);
        QCOMPARE(QDir(destinationRoot.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"existing.dat"});
        QVERIFY(host.running());

        // The same running host and invitation must permit another offer after
        // reconnecting; stale cancellation/receipt messages cannot poison it.
        client.start(invitation);
        QTRY_VERIFY_WITH_TIMEOUT(clientFiles->offerAvailable() && hostFiles->offerAvailable(), 8000);
        disconnected.clear();
        hostFiles->offerFile(source);
        QTRY_COMPARE_WITH_TIMEOUT(offered.count(), 4, 5000);
        const QString retriedDestination = destinationRoot.filePath("retried.dat");
        clientFiles->acceptOffer(offered.last()[0].toString(), retriedDestination);
        QTRY_VERIFY_WITH_TIMEOUT(!hostFiles->busy() && !clientFiles->busy(), 10000);
        QCOMPARE(readFile(retriedDestination), bytes);
        QVERIFY(!hostFiles->offering()); QCOMPARE(disconnected.count(), 0);
        const int before = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > before, 5000);
        client.stop(); host.stop();
    }
    void unavailablePermission_data() {
        QTest::addColumn<bool>("control");
        QTest::addColumn<bool>("filesEnabled");
        QTest::newRow("view-only") << false << true;
        QTest::newRow("disabled") << true << false;
    }
    void unavailablePermission() {
        QFETCH(bool, control); QFETCH(bool, filesEnabled);
        QTemporaryDir hostRoot, localRoot;
        QVERIFY(hostRoot.isValid() && localRoot.isValid());
        ld::Host host(nullptr); ld::Client client(nullptr);
        host.configureFiles(hostRoot.path(), filesEnabled);
        host.configureVideo(15, "jpeg"); host.setLockOnDisconnect(false);
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, control, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        QSignalSpy frames(&client, &ld::Client::frame);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        auto *files = client.fileTransfer();
        client.start(invitation);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 8000);
        disconnected.clear();
        QVERIFY(!files->available());
        QVERIFY(!files->offerAvailable());
        auto *hostFiles = host.fileTransfer();
        QVERIFY(!hostFiles->offerAvailable());
        QSignalSpy hostPackets(hostFiles, &ld::FileTransferHost::send);
        QSignalSpy offered(files, &ld::FileTransferClient::offered);
        const QString source = localRoot.filePath("forbidden.dat");
        QVERIFY(writeFile(source, QByteArray("never upload")));
        files->upload(source);
        QVERIFY(!files->busy());
        QVERIFY(!QFile::exists(hostRoot.filePath("forbidden.dat")));
        hostFiles->offerFile(source);
        QVERIFY(!hostFiles->busy()); QVERIFY(!hostFiles->offering());
        QTest::qWait(150);
        QCOMPARE(hostPackets.count(), 0); QCOMPARE(offered.count(), 0);
        QCOMPARE(disconnected.count(), 0);
        client.stop(); host.stop();
    }
};
QTEST_MAIN(FileTransferNativeClientTest)
#include "file_transfer_client_test.moc"
