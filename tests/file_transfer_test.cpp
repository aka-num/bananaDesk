#include "file_transfer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
const QString FirstId(32, 'a'), SecondId(32, 'b');
QByteArray packet(const QString &id, const char *op, QJsonObject fields = {}) {
    fields["v"] = 1; fields["id"] = id; fields["op"] = QString::fromLatin1(op);
    return QJsonDocument(fields).toJson(QJsonDocument::Compact);
}
QJsonObject request(ld::FileTransferHost &host, const QString &id, const char *op, QJsonObject fields = {}) {
    QSignalSpy spy(&host, &ld::FileTransferHost::send);
    host.receive(packet(id, op, fields));
    if (spy.size() != 1) return {{"test_error", QStringLiteral("Expected exactly one reply")}};
    return QJsonDocument::fromJson(spy.front()[0].toByteArray()).object();
}
bool writeFile(const QString &path, const QByteArray &data) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray readFile(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
QByteArray contents(int size) {
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) data[i] = char((i * 67 + i / 197) & 255);
    return data;
}
QByteArray hash(const QByteArray &data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
QStringList entries(const QString &root) { return QDir(root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot); }
void connectPair(ld::FileTransferClient &client, ld::FileTransferHost &host) {
    QObject::connect(&client, &ld::FileTransferClient::send, &host, &ld::FileTransferHost::receive, Qt::QueuedConnection);
    QObject::connect(&host, &ld::FileTransferHost::send, &client, &ld::FileTransferClient::receive, Qt::QueuedConnection);
    client.setAvailable(true);
}
void enableOffers(ld::FileTransferClient &client, ld::FileTransferHost &host) {
    client.setOfferAvailable(true); host.setOfferAvailable(true);
}
}

class FileTransferTest : public QObject {
    Q_OBJECT
private slots:
    void configureAndListingDoNotCreateDirectories() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString root = temp.filePath("not-created/subfolder");
        ld::FileTransferHost host; host.configure(root); host.reset();
        QVERIFY(!QFileInfo::exists(root));
        const auto reply = request(host, FirstId, "list");
        QCOMPARE(reply.value("op").toString(), QStringLiteral("list"));
        QVERIFY(reply.value("files").toArray().isEmpty());
        QVERIFY(!QFileInfo::exists(root));
    }

    void invalidNames_data() {
        QTest::addColumn<QString>("name");
        const QStringList names{"", ".", "..", "../escape", "..\\escape", "/tmp/file", "C:secret", "a/b", "a\\b",
            "CON", "con.txt", "AUX", "PRN.log", "nul", "COM1", "LPT9.txt", "COM¹.txt", "CONIN$", "CONOUT$",
            "tail.", "tail ", "bad?name", "bad*name", "bad|name", "bad<name", "bad>name", "bad\"name",
            QString("zero") + QChar(0) + "byte", QString(256, 'a'), ".landesk-upload-private.part"};
        int index = 0;
        for (const auto &name : names) QTest::newRow(qPrintable(QString::number(index++))) << name;
    }
    void invalidNames() {
        QFETCH(QString, name);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString root = temp.filePath("root"); ld::FileTransferHost host; host.configure(root);
        const auto reply = request(host, FirstId, "put", {{"name", name}, {"size", 1}});
        QCOMPARE(reply.value("op").toString(), QStringLiteral("error"));
        QVERIFY(!QFileInfo::exists(root));
    }

    void binaryUploadIsAtomicAndHashMatches() {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString root = temp.filePath("root"); ld::FileTransferHost host; host.configure(root);
        const QByteArray data = contents(ld::FileChunkSize * 3 + 237);
        QCOMPARE(request(host, FirstId, "put", {{"name", "binary.dat"}, {"size", data.size()}}).value("op").toString(), QStringLiteral("ready"));
        for (int offset = 0; offset < data.size(); offset += ld::FileChunkSize) {
            QVERIFY(!QFileInfo::exists(root + "/binary.dat"));
            const auto chunk = data.mid(offset, ld::FileChunkSize);
            const auto response = request(host, FirstId, "data", {{"offset", offset}, {"data", QString::fromLatin1(chunk.toBase64())}});
            QCOMPARE(response.value("op").toString(), QStringLiteral("ack"));
            QCOMPARE(response.value("offset").toInt(), offset + chunk.size());
        }
        QCOMPARE(request(host, FirstId, "commit", {{"offset", data.size()}}).value("op").toString(), QStringLiteral("done"));
        QCOMPARE(hash(readFile(root + "/binary.dat")), hash(data));
        QCOMPARE(entries(root), QStringList{"binary.dat"});
    }

    void zeroByteUploadAndDownload() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.path());
        QCOMPARE(request(host, FirstId, "put", {{"name", "empty"}, {"size", 0}}).value("op").toString(), QStringLiteral("ready"));
        QCOMPARE(request(host, FirstId, "commit", {{"offset", 0}}).value("op").toString(), QStringLiteral("done"));
        QVERIFY(QFileInfo::exists(temp.filePath("empty")));
        QCOMPARE(request(host, SecondId, "get", {{"name", "empty"}}).value("size").toInt(-1), 0);
        const auto response = request(host, SecondId, "read", {{"offset", 0}});
        QCOMPARE(response.value("op").toString(), QStringLiteral("data"));
        QCOMPARE(response.value("data").toString(), QString());
        QVERIFY(response.value("eof").toBool());
        QCOMPARE(request(host, FirstId, "list").value("op").toString(), QStringLiteral("list"));
    }

    void cancelResetAndStaleIdDoNotPublish() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.path());
        request(host, FirstId, "put", {{"name", "cancelled"}, {"size", 3}});
        request(host, FirstId, "data", {{"offset", 0}, {"data", "YQ=="}});
        QCOMPARE(request(host, FirstId, "cancel").value("op").toString(), QStringLiteral("canceled"));
        QVERIFY(entries(temp.path()).isEmpty());
        request(host, SecondId, "put", {{"name", "second"}, {"size", 1}});
        request(host, FirstId, "cancel"); // A delayed cancel belongs to the old transaction.
        QCOMPARE(request(host, SecondId, "data", {{"offset", 0}, {"data", "Yg=="}}).value("op").toString(), QStringLiteral("ack"));
        QCOMPARE(request(host, SecondId, "commit", {{"offset", 1}}).value("op").toString(), QStringLiteral("done"));
        QCOMPARE(readFile(temp.filePath("second")), QByteArray("b"));
        request(host, FirstId, "put", {{"name", "reset"}, {"size", 20}});
        request(host, FirstId, "data", {{"offset", 0}, {"data", "YQ=="}});
        host.reset();
        QCOMPARE(entries(temp.path()), QStringList{"second"});
    }

    void wrongOffsetAndInvalidChunkAbortOnlyMatchingTransaction() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.path());
        request(host, FirstId, "put", {{"name", "offset"}, {"size", 3}});
        QCOMPARE(request(host, SecondId, "data", {{"offset", 0}, {"data", "YQ=="}}).value("op").toString(), QStringLiteral("error"));
        QCOMPARE(request(host, FirstId, "data", {{"offset", 1}, {"data", "YQ=="}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(entries(temp.path()).isEmpty());
        request(host, FirstId, "put", {{"name", "encoding"}, {"size", 3}});
        QCOMPARE(request(host, FirstId, "data", {{"offset", 0}, {"data", "YQ==!"}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(entries(temp.path()).isEmpty());
        request(host, FirstId, "put", {{"name", "overflow"}, {"size", 1}});
        QCOMPARE(request(host, FirstId, "data", {{"offset", 0}, {"data", "YWJj"}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(entries(temp.path()).isEmpty());
    }

    void noOverwriteIncludingDestinationCreatedDuringUpload() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.path());
        QVERIFY(writeFile(temp.filePath("existing"), "original"));
        QCOMPARE(request(host, FirstId, "put", {{"name", "existing"}, {"size", 1}}).value("op").toString(), QStringLiteral("error"));
        QCOMPARE(readFile(temp.filePath("existing")), QByteArray("original"));
        request(host, FirstId, "put", {{"name", "race"}, {"size", 1}});
        request(host, FirstId, "data", {{"offset", 0}, {"data", "eA=="}});
        QVERIFY(writeFile(temp.filePath("race"), "created locally while uploading"));
        QCOMPARE(request(host, FirstId, "commit", {{"offset", 1}}).value("op").toString(), QStringLiteral("error"));
        QCOMPARE(readFile(temp.filePath("race")), QByteArray("created locally while uploading"));
        QCOMPARE(entries(temp.path()), (QStringList{"existing", "race"}));
    }

    void downloadBinaryHashAndEndClosesStream() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.path());
        const auto data = contents(ld::FileChunkSize * 2 + 61); QVERIFY(writeFile(temp.filePath("source"), data));
        QCOMPARE(request(host, FirstId, "get", {{"name", "source"}}).value("size").toInt(), data.size());
        QByteArray received;
        while (received.size() < data.size()) {
            const auto reply = request(host, FirstId, "read", {{"offset", received.size()}});
            QCOMPARE(reply.value("op").toString(), QStringLiteral("data"));
            QCOMPARE(reply.value("offset").toInt(), received.size());
            const auto chunk = QByteArray::fromBase64(reply.value("data").toString().toLatin1());
            QVERIFY(!chunk.isEmpty() && chunk.size() <= ld::FileChunkSize); received += chunk;
            QCOMPARE(reply.value("eof").toBool(), received.size() == data.size());
        }
        QCOMPARE(hash(received), hash(data));
        QCOMPARE(request(host, SecondId, "list").value("op").toString(), QStringLiteral("list"));
    }

    void symlinksAndSubdirectoriesAreNotTransferable() {
#ifdef Q_OS_WIN
        QSKIP("This symlink fixture requires Linux; Windows reparse rejection is in openRegular.");
#else
        QTemporaryDir temp; const QString root = temp.filePath("root"); QVERIFY(QDir().mkpath(root + "/child"));
        QVERIFY(writeFile(temp.filePath("outside"), "private outside content"));
        QVERIFY(QFile::link(temp.filePath("outside"), root + "/link"));
        QVERIFY(QFile::link(temp.path(), root + "/directory-link"));
        QVERIFY(writeFile(root + "/ordinary", "ok"));
        ld::FileTransferHost host; host.configure(root);
        auto listing = request(host, FirstId, "list").value("files").toArray();
        QCOMPARE(listing.size(), 1); QCOMPARE(listing.first().toObject().value("name").toString(), QStringLiteral("ordinary"));
        for (const auto &name : {"link", "directory-link", "child"})
            QCOMPARE(request(host, FirstId, "get", {{"name", name}}).value("op").toString(), QStringLiteral("error"));
        QCOMPARE(request(host, FirstId, "put", {{"name", "link"}, {"size", 0}}).value("op").toString(), QStringLiteral("error"));
        const QString linkedRoot = temp.filePath("root-link"); QVERIFY(QFile::link(root, linkedRoot)); host.configure(linkedRoot);
        QCOMPARE(request(host, FirstId, "list").value("op").toString(), QStringLiteral("error"));
#endif
    }

    void fileLimitAndChunkLimitEnforced() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.filePath("root"));
        for (const auto size : {double(ld::MaxTransferFileSize) + 1, -1.0, 1.5})
            QCOMPARE(request(host, FirstId, "put", {{"name", "large"}, {"size", size}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(!QFileInfo::exists(temp.filePath("root")));
        QCOMPARE(request(host, FirstId, "put", {{"name", "bounded"}, {"size", double(ld::MaxTransferFileSize)}}).value("op").toString(), QStringLiteral("ready"));
        const auto large = QByteArray(ld::FileChunkSize + 1, 'x');
        QCOMPARE(request(host, FirstId, "data", {{"offset", 0}, {"data", QString::fromLatin1(large.toBase64())}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(entries(temp.filePath("root")).isEmpty());
    }

    void swappedRootCannotRedirectCommitOrDownload() {
#ifdef Q_OS_WIN
        QSKIP("Windows pins directory handles without FILE_SHARE_DELETE, preventing this POSIX rename fixture.");
#else
        QTemporaryDir temp;
        const QString root = temp.filePath("root"), moved = temp.filePath("moved"), outside = temp.filePath("outside");
        QVERIFY(QDir().mkpath(root)); QVERIFY(QDir().mkpath(outside));
        ld::FileTransferHost host; host.configure(root);
        request(host, FirstId, "put", {{"name", "upload.txt"}, {"size", 3}});
        request(host, FirstId, "data", {{"offset", 0}, {"data", "YWJj"}});
        QVERIFY(QDir().rename(root, moved)); QVERIFY(QFile::link(outside, root));
        QCOMPARE(request(host, FirstId, "commit", {{"offset", 3}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(entries(outside).isEmpty()); QVERIFY(entries(moved).isEmpty());
        QVERIFY(QFile::remove(root)); QVERIFY(QDir().rename(moved, root));
        request(host, FirstId, "put", {{"name", "upload.txt"}, {"size", 3}});
        request(host, FirstId, "data", {{"offset", 0}, {"data", "YWJj"}});
        QVERIFY(QDir().rename(root, moved)); QVERIFY(QDir().mkpath(root));
        QCOMPARE(request(host, FirstId, "commit", {{"offset", 3}}).value("op").toString(), QStringLiteral("error"));
        QVERIFY(entries(root).isEmpty()); QVERIFY(entries(moved).isEmpty());
        QVERIFY(writeFile(root + "/download.txt", "selected original"));
        QVERIFY(writeFile(outside + "/download.txt", "outside secret"));
        QCOMPARE(request(host, FirstId, "get", {{"name", "download.txt"}}).value("op").toString(), QStringLiteral("ready"));
        QVERIFY(QDir().rmdir(moved)); QVERIFY(QDir().rename(root, moved)); QVERIFY(QFile::link(outside, root));
        const auto response = request(host, FirstId, "read", {{"offset", 0}});
        QCOMPARE(QByteArray::fromBase64(response.value("data").toString().toLatin1()), QByteArray("selected original"));
#endif
    }

    void listingIsBoundedByWireBudget() {
        QTemporaryDir temp; ld::FileTransferHost host; host.configure(temp.path());
        for (int i = 0; i < 1005; ++i) QVERIFY(writeFile(temp.filePath(QStringLiteral("file-%1-").arg(i) + QString(160, 'x')), {}));
        QSignalSpy spy(&host, &ld::FileTransferHost::send); host.receive(packet(FirstId, "list"));
        QCOMPARE(spy.size(), 1); const auto payload = spy.front()[0].toByteArray(); QVERIFY(payload.size() < ld::MaxFilePayload);
        const auto reply = QJsonDocument::fromJson(payload).object();
        QVERIFY(reply.value("truncated").toBool());
        QVERIFY(reply.value("files").toArray().size() > 100 && reply.value("files").toArray().size() < 1000);
    }

    void realClientUploadDownloadAndAuthorizedReplacement() {
        QTemporaryDir temp; const QString remote = temp.filePath("remote"); QVERIFY(QDir().mkpath(remote));
        ld::FileTransferHost host; host.configure(remote); ld::FileTransferClient client; connectPair(client, host);
        const auto data = contents(ld::FileChunkSize * 4 + 9); QVERIFY(writeFile(temp.filePath("upload.bin"), data));
        QJsonArray listing;
        connect(&client, &ld::FileTransferClient::listing, this, [&](const QJsonArray &value) { listing = value; });
        client.upload(temp.filePath("upload.bin")); QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QCOMPARE(hash(readFile(remote + "/upload.bin")), hash(data)); QCOMPARE(listing.size(), 1);
        QVERIFY(writeFile(temp.filePath("saved.bin"), "old local file authorized for replacement"));
        client.download("upload.bin", temp.filePath("saved.bin")); QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QCOMPARE(hash(readFile(temp.filePath("saved.bin"))), hash(data));
    }

    void progressCancellationPreservesOriginalAndCleansUploads() {
        QTemporaryDir temp; const QString remote = temp.filePath("remote"); QVERIFY(QDir().mkpath(remote));
        const auto data = contents(ld::FileChunkSize * 5); QVERIFY(writeFile(temp.filePath("source.bin"), data));
        ld::FileTransferHost host; host.configure(remote); ld::FileTransferClient client; connectPair(client, host);
        auto connection = connect(&client, &ld::FileTransferClient::progress, &client, [&](qint64 done, qint64 total) {
            if (done > 0 && done < total) client.cancel();
        });
        client.upload(temp.filePath("source.bin")); QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QTest::qWait(20); QVERIFY(entries(remote).isEmpty());
        QVERIFY(writeFile(remote + "/source.bin", data)); QVERIFY(writeFile(temp.filePath("destination.bin"), "original"));
        client.download("source.bin", temp.filePath("destination.bin")); QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QTest::qWait(20); QCOMPARE(readFile(temp.filePath("destination.bin")), QByteArray("original"));
        disconnect(connection);
        client.download("source.bin", temp.filePath("destination.bin")); QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QCOMPARE(hash(readFile(temp.filePath("destination.bin"))), hash(data));
    }

    void lateReplyDoesNotCancelNewClientTransaction() {
        ld::FileTransferClient client; client.setAvailable(true);
        QSignalSpy sent(&client, &ld::FileTransferClient::send);
        client.refresh(); const auto old = QJsonDocument::fromJson(sent.back()[0].toByteArray()).object().value("id").toString();
        client.cancel(); client.refresh(); const auto current = QJsonDocument::fromJson(sent.back()[0].toByteArray()).object().value("id").toString();
        QVERIFY(old != current);
        client.receive(packet(old, "error", {{"error", "late failure"}})); QVERIFY(client.busy());
        client.receive(packet(old, "canceled")); QVERIFY(client.busy());
        client.receive(packet(current, "list", {{"files", QJsonArray{}}, {"truncated", false}})); QVERIFY(!client.busy());
        client.setAvailable(false); const int count = sent.size(); client.refresh(); QCOMPARE(sent.size(), count);
    }

    void offersRequirePermissionAndAnOrdinaryLocalSource() {
        QTemporaryDir temp; QVERIFY(writeFile(temp.filePath("chosen.txt"), "selected"));
        ld::FileTransferHost host; QSignalSpy sent(&host, &ld::FileTransferHost::send);
        host.offerFile(temp.filePath("chosen.txt")); QVERIFY(!host.busy()); QVERIFY(sent.isEmpty());
        host.setOfferAvailable(true);
        host.offerFile(temp.path()); QVERIFY(!host.busy()); QVERIFY(sent.isEmpty());
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(temp.filePath("chosen.txt"), temp.filePath("link")));
        host.offerFile(temp.filePath("link")); QVERIFY(!host.busy()); QVERIFY(sent.isEmpty());
#endif
        host.offerFile(temp.filePath("chosen.txt")); QVERIFY(host.busy()); QVERIFY(host.offering());
        QCOMPARE(sent.count(), 1);
        const auto offer = QJsonDocument::fromJson(sent.front()[0].toByteArray()).object();
        QCOMPARE(offer.value("name").toString(), QStringLiteral("chosen.txt"));
        QVERIFY(!sent.front()[0].toByteArray().contains(temp.path().toUtf8()));
        for (const bool filesAvailable : {false, true}) {
            ld::FileTransferClient client; client.setAvailable(filesAvailable);
            QSignalSpy offered(&client, &ld::FileTransferClient::offered);
            QSignalSpy rejected(&client, &ld::FileTransferClient::send);
            // No mutual offer capability, including ordinary old clients.
            client.receive(sent.front()[0].toByteArray());
            QVERIFY(offered.isEmpty()); QVERIFY(!client.busy()); QCOMPARE(rejected.count(), 1);
            QCOMPARE(QJsonDocument::fromJson(rejected.front()[0].toByteArray()).object()["op"].toString(), QStringLiteral("error"));
        }
        host.setOfferAvailable(false); QVERIFY(!host.busy()); QVERIFY(!host.offering());
    }

    void acceptedOfferWaitsForCommittedReceipt_data() {
        QTest::addColumn<int>("bytes");
        QTest::newRow("empty") << 0;
        QTest::newRow("binary-multiple-chunks") << ld::FileChunkSize * 3 + 317;
    }
    void acceptedOfferWaitsForCommittedReceipt() {
        QFETCH(int, bytes);
        QTemporaryDir temp; const auto data = contents(bytes);
        const QString source = temp.filePath("选择的文件.bin"), destination = temp.filePath("received.bin");
        QVERIFY(writeFile(source, data)); QVERIFY(writeFile(destination, "original"));
        ld::FileTransferHost host; host.configure(temp.filePath("different-share"));
        ld::FileTransferClient client; client.setAvailable(true); enableOffers(client, host);
        QByteArray receipt;
        connect(&host, &ld::FileTransferHost::send, &client, &ld::FileTransferClient::receive, Qt::QueuedConnection);
        connect(&client, &ld::FileTransferClient::send, &host, [&](const QByteArray &payload) {
            if (QJsonDocument::fromJson(payload).object()["op"] == "received") receipt = payload;
            else host.receive(payload);
        }, Qt::QueuedConnection);
        QSignalSpy offered(&client, &ld::FileTransferClient::offered);
        QSignalSpy hostStatus(&host, &ld::FileTransferHost::status);
        QSignalSpy progress(&host, &ld::FileTransferHost::progress);
        host.offerFile(source); QTRY_COMPARE(offered.count(), 1);
        const QString id = offered.front()[0].toString();
        QVERIFY(client.busy()); QVERIFY(host.busy()); QVERIFY(host.offering());
        QCOMPARE(offered.front()[1].toString(), QFileInfo(source).fileName());
        QCOMPARE(offered.front()[2].toLongLong(), qint64(bytes));
        QCOMPARE(readFile(destination), QByteArray("original"));
        client.acceptOffer(SecondId, destination); QVERIFY(client.busy()); QVERIFY(receipt.isEmpty());
        client.acceptOffer(id, destination);
        QTRY_VERIFY(!client.busy()); QTRY_VERIFY(!receipt.isEmpty());
        QCOMPARE(hash(readFile(destination)), hash(data));
        QVERIFY(!QFileInfo::exists(temp.filePath("different-share")));
        QVERIFY(host.busy()); QVERIFY(host.offering());
        QCOMPARE(progress.back()[0].toLongLong(), qint64(bytes));
        QVERIFY(!hostStatus.back()[0].toString().contains(QStringLiteral("已接收并保存")));
        host.receive(receipt); QVERIFY(!host.busy()); QVERIFY(!host.offering());
        QVERIFY(hostStatus.back()[0].toString().contains(QStringLiteral("已接收并保存")));
    }

    void pendingOfferTermination_data() {
        QTest::addColumn<QString>("action");
        for (const char *action : {"decline", "host-cancel", "client-cancel", "reset", "host-disabled", "client-disabled", "host-timeout", "client-timeout"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }
    void pendingOfferTermination() {
        QFETCH(QString, action);
        QTemporaryDir temp; QVERIFY(writeFile(temp.filePath("source"), "offered"));
        ld::FileTransferHost host; ld::FileTransferClient client;
        connectPair(client, host); enableOffers(client, host);
        QSignalSpy offered(&client, &ld::FileTransferClient::offered);
        host.offerFile(temp.filePath("source")); QTRY_COMPARE(offered.count(), 1);
        const QString id = offered.front()[0].toString();
        if (action == "decline") client.declineOffer(id);
        else if (action == "host-cancel") host.cancelOffer();
        else if (action == "client-cancel") client.cancel();
        else if (action == "reset") { client.reset(); host.reset(); }
        else if (action == "host-disabled") host.setOfferAvailable(false);
        else if (action == "client-disabled") client.setOfferAvailable(false);
        else {
            QObject *owner = action == "host-timeout" ? static_cast<QObject *>(&host) : static_cast<QObject *>(&client);
            auto *timer = owner->findChild<QTimer *>("fileOfferTimeout"); QVERIFY(timer);
            QCOMPARE(timer->interval(), 60000); QVERIFY(timer->isActive());
            timer->setInterval(1); // Exercise the real timer without a production timeout override API.
        }
        QTRY_VERIFY(!client.busy()); QTRY_VERIFY(!host.busy()); QVERIFY(!host.offering());
        client.acceptOffer(id, temp.filePath("late-output")); QTest::qWait(5);
        QVERIFY(!QFileInfo::exists(temp.filePath("late-output")));
        QCOMPARE(entries(temp.path()), QStringList{"source"});
    }

    void offeredDownloadCancellationAndWriteFailurePreserveFiles() {
        QTemporaryDir temp; const auto data = contents(ld::FileChunkSize * 4 + 9);
        const QString source = temp.filePath("source"), destination = temp.filePath("target");
        QVERIFY(writeFile(source, data)); QVERIFY(writeFile(destination, "original"));
        ld::FileTransferHost host; ld::FileTransferClient client;
        connectPair(client, host); enableOffers(client, host);
        QSignalSpy offered(&client, &ld::FileTransferClient::offered);
        QSignalSpy status(&host, &ld::FileTransferHost::status);
        auto cancellation = connect(&client, &ld::FileTransferClient::progress, &client, [&](qint64 done, qint64 total) {
            if (done > 0 && done < total) client.cancel();
        });
        host.offerFile(source); QTRY_COMPARE(offered.count(), 1);
        client.acceptOffer(offered.back()[0].toString(), destination);
        QTRY_VERIFY(!client.busy()); QTRY_VERIFY(!host.busy());
        QCOMPARE(readFile(destination), QByteArray("original"));
        QCOMPARE(entries(temp.path()), (QStringList{"source", "target"}));
        disconnect(cancellation);
        host.offerFile(source); QTRY_COMPARE(offered.count(), 2);
        client.acceptOffer(offered.back()[0].toString(), temp.filePath("missing-parent/output"));
        QTRY_VERIFY(!client.busy()); QTRY_VERIFY(!host.busy());
        QVERIFY(!QFileInfo::exists(temp.filePath("missing-parent")));
        QVERIFY(!status.back()[0].toString().contains(QStringLiteral("已接收并保存")));
    }

    void offerMetadataAndUnacceptedDataCannotWrite() {
        QTemporaryDir temp; ld::FileTransferClient client;
        client.setAvailable(true); client.setOfferAvailable(true);
        QSignalSpy offered(&client, &ld::FileTransferClient::offered);
        for (const auto &name : {QString("../escape"), QString("C:escape"), QString("CON"), QString("bad/name")})
            client.receive(packet(FirstId, "offer", {{"name", name}, {"size", 1}}));
        for (double size : {-1.0, 1.5, double(ld::MaxTransferFileSize) + 1})
            client.receive(packet(FirstId, "offer", {{"name", "valid"}, {"size", size}}));
        client.receive(packet("invalid-id", "offer", {{"name", "valid"}, {"size", 1}}));
        QVERIFY(!client.busy()); QVERIFY(offered.isEmpty());
        client.receive(packet(FirstId, "offer", {{"name", "valid"}, {"size", 1}}));
        QCOMPARE(offered.count(), 1); QVERIFY(client.busy());
        client.receive(packet(FirstId, "data", {{"offset", 0}, {"data", "eA=="}, {"eof", true}}));
        QVERIFY(!client.busy()); QVERIFY(entries(temp.path()).isEmpty());
        client.receive(packet(FirstId, "offer", {{"name", "valid"}, {"size", 1}}));
        client.acceptOffer(FirstId, temp.filePath("output"));
        client.receive(packet(FirstId, "ready", {{"offset", 0}, {"size", 2}}));
        QVERIFY(!client.busy()); QVERIFY(entries(temp.path()).isEmpty());
        client.receive(packet(FirstId, "offer", {{"name", "valid"}, {"size", 1}}));
        client.acceptOffer(FirstId, temp.filePath("output"));
        client.receive(packet(FirstId, "ready", {{"offset", 0}, {"size", 1}}));
        client.receive(packet(FirstId, "data", {{"offset", 1}, {"data", "eA=="}, {"eof", true}}));
        QVERIFY(!client.busy()); QVERIFY(entries(temp.path()).isEmpty());
    }

    void pendingOffersDoNotReplaceOtherTransfers() {
        QTemporaryDir temp; QVERIFY(writeFile(temp.filePath("source"), "data"));
        ld::FileTransferHost host; host.configure(temp.filePath("shared")); host.setOfferAvailable(true);
        request(host, FirstId, "put", {{"name", "upload"}, {"size", 1}});
        QVERIFY(host.busy()); QVERIFY(!host.offering());
        host.offerFile(temp.filePath("source")); host.cancelOffer();
        QVERIFY(host.busy()); QVERIFY(!host.offering());
        QCOMPARE(request(host, FirstId, "data", {{"offset", 0}, {"data", "eA=="}})["op"].toString(), QStringLiteral("ack"));
        request(host, FirstId, "commit", {{"offset", 1}});
        host.offerFile(temp.filePath("source")); QVERIFY(host.offering());
        QCOMPARE(request(host, SecondId, "get", {{"name", "upload"}})["op"].toString(), QStringLiteral("error"));
        QVERIFY(host.offering());
        ld::FileTransferClient client; client.setAvailable(true); client.setOfferAvailable(true);
        QSignalSpy offered(&client, &ld::FileTransferClient::offered);
        QSignalSpy sent(&client, &ld::FileTransferClient::send);
        client.refresh(); const QString transaction = QJsonDocument::fromJson(sent.back()[0].toByteArray()).object()["id"].toString();
        client.receive(packet(FirstId, "offer", {{"name", "valid"}, {"size", 1}}));
        QVERIFY(client.busy()); QVERIFY(offered.isEmpty());
        client.receive(packet(transaction, "list", {{"files", QJsonArray{}}})); QVERIFY(!client.busy());
        client.receive(packet(FirstId, "offer", {{"name", "valid"}, {"size", 1}}));
        const int count = sent.count();
        client.upload(temp.filePath("source")); client.download("upload", temp.filePath("target")); client.refresh();
        QCOMPARE(sent.count(), count); QVERIFY(client.busy());
        client.declineOffer(SecondId); QVERIFY(client.busy());
        client.receive(packet(SecondId, "canceled")); QVERIFY(client.busy());
        client.declineOffer(FirstId); QVERIFY(!client.busy());
    }

    void offeredSourceHandleCannotBeRedirected() {
#ifdef Q_OS_WIN
        QSKIP("Windows holds the selected file without FILE_SHARE_DELETE, preventing this rename fixture.");
#else
        QTemporaryDir temp; const QString source = temp.filePath("selected");
        QVERIFY(writeFile(source, "chosen bytes"));
        ld::FileTransferHost host; host.setOfferAvailable(true);
        QSignalSpy sent(&host, &ld::FileTransferHost::send);
        host.offerFile(source); QVERIFY(host.offering());
        const QString id = QJsonDocument::fromJson(sent.front()[0].toByteArray()).object()["id"].toString();
        QVERIFY(QFile::rename(source, temp.filePath("moved")));
        QVERIFY(writeFile(source, "unselected private replacement"));
        QCOMPARE(request(host, SecondId, "get", {{"name", source}})["op"].toString(), QStringLiteral("error"));
        QVERIFY(host.offering());
        QCOMPARE(request(host, id, "accept")["op"].toString(), QStringLiteral("ready"));
        const auto data = request(host, id, "read", {{"offset", 0}});
        QCOMPARE(QByteArray::fromBase64(data["data"].toString().toLatin1()), QByteArray("chosen bytes"));
        QVERIFY(host.busy());
        QCOMPARE(request(host, SecondId, "received", {{"size", 12}})["op"].toString(), QStringLiteral("error"));
        QVERIFY(host.busy());
        host.receive(packet(id, "received", {{"size", 12}})); QVERIFY(!host.busy());
#endif
    }
};

QTEST_GUILESS_MAIN(FileTransferTest)
#include "file_transfer_test.moc"
