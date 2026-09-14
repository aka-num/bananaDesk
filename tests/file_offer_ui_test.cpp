#include "app.h"
#include "file_transfer.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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
QString operation(const QByteArray &bytes) {
    return QJsonDocument::fromJson(bytes).object().value("op").toString();
}
}

// Exercise the actual Window and its non-native file dialogs. Only the file
// backends exchange messages: sharing, desktop capture, and TLS never start.
// The runner provides a private desktop and temporary Downloads/Documents.
class FileOfferUiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<QJsonArray>("QJsonArray");
        if (qEnvironmentVariable("LANDESK_ISOLATED_TEST") != "1")
            QSKIP("Run this suite on a disposable desktop with temporary user directories");
#ifndef Q_OS_WIN
        if (QGuiApplication::platformName() != "xcb")
            QSKIP("Run this Window integration suite on private Xvfb");
#endif
    }

    void explicitAcceptanceSavesBeforeSenderConfirmation() {
        QTemporaryDir sharedRoot, sourceRoot, destinationRoot;
        QVERIFY(sharedRoot.isValid() && sourceRoot.isValid() && destinationRoot.isValid());
        const QByteArray bytes = payload(2 * ld::FileChunkSize + 173);
        const QString source = sourceRoot.filePath("offered-binary.dat");
        const QString destination = destinationRoot.filePath("accepted-binary.dat");
        QVERIFY(writeFile(source, bytes));

        ld::Window window;
        window.setLockOnDisconnect(false);
        window.configureFiles(sharedRoot.path(), true);
        auto *host = window.findChild<ld::Host *>();
        auto *client = window.findChild<ld::Client *>();
        QVERIFY(host && client); QVERIFY(!host->running());
        auto *receiver = client->fileTransfer();
        QByteArray heldReceipt;
        ld::FileTransferHost sender;
        sender.configure(sharedRoot.path()); sender.setOfferAvailable(true);
        receiver->setAvailable(true); receiver->setOfferAvailable(true);
        connect(&sender, &ld::FileTransferHost::send, receiver,
                &ld::FileTransferClient::receive, Qt::QueuedConnection);
        connect(receiver, &ld::FileTransferClient::send, &sender,
                [&sender, &heldReceipt](const QByteArray &packet) {
            if (operation(packet) == "received") heldReceipt = packet;
            else sender.receive(packet);
        }, Qt::QueuedConnection);
        QSignalSpy outgoing(&sender, &ld::FileTransferHost::send);
        QSignalSpy replies(receiver, &ld::FileTransferClient::send);
        QSignalSpy status(&sender, &ld::FileTransferHost::status);

        sender.offerFile(source);
        QTRY_VERIFY(window.findChild<QFileDialog *>("receiveFileOfferDialog"));
        QPointer<QFileDialog> dialog = window.findChild<QFileDialog *>("receiveFileOfferDialog");
        QTRY_VERIFY(dialog && dialog->isVisible());
        QVERIFY(dialog->testOption(QFileDialog::DontUseNativeDialog));
        QCOMPARE(dialog->acceptMode(), QFileDialog::AcceptSave);
        QCOMPARE(dialog->labelText(QFileDialog::Accept), QStringLiteral("接收"));
        QCOMPARE(dialog->labelText(QFileDialog::Reject), QStringLiteral("拒绝"));
        // With a visible save dialog, selectFile() can retain its initial name
        // after changing directories. Exercise the user's filename edit instead.
        dialog->setDirectory(destinationRoot.path());
        auto *fileName = dialog->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(fileName);
        fileName->setFocus(); fileName->selectAll();
        QTest::keyClicks(fileName, QFileInfo(destination).fileName());
        QTRY_COMPARE(dialog->selectedFiles(), QStringList{destination});
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(QDir(destinationRoot.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
        QCOMPARE(outgoing.count(), 1);
        QCOMPARE(operation(outgoing.first()[0].toByteArray()), QStringLiteral("offer"));
        QCOMPARE(replies.count(), 0);
        QVERIFY(sender.busy() && sender.offering() && receiver->busy());

        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept", Qt::DirectConnection));
        QTRY_VERIFY(!dialog || !dialog->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!heldReceipt.isEmpty(), 10000);
        QVERIFY(!receiver->busy());
        QCOMPARE(readFile(destination), bytes);
        QCOMPARE(readFile(source), bytes);
        // Sending the final bytes does not claim that the other side saved them.
        QVERIFY(sender.busy() && sender.offering());
        for (const auto &event : status)
            QVERIFY(!event[0].toString().contains(QStringLiteral("对方已接收并保存")));
        sender.receive(heldReceipt);
        QVERIFY(!sender.busy() && !sender.offering());
        QVERIFY(!status.isEmpty());
        QVERIFY(status.last()[0].toString().contains(QStringLiteral("对方已接收并保存")));
        QVERIFY(!host->running());
    }

    void refusalAndWithdrawalClosePendingPicker_data() {
        QTest::addColumn<bool>("withdraw");
        QTest::newRow("recipient-refuses") << false;
        QTest::newRow("sender-withdraws") << true;
    }

    void refusalAndWithdrawalClosePendingPicker() {
        QFETCH(bool, withdraw);
        QTemporaryDir sourceRoot, destinationRoot;
        QVERIFY(sourceRoot.isValid() && destinationRoot.isValid());
        const QString source = sourceRoot.filePath("pending.dat");
        const QString destination = destinationRoot.filePath("must-not-exist.dat");
        QVERIFY(writeFile(source, payload(1024)));
        ld::Window window;
        window.setLockOnDisconnect(false);
        window.configureFiles(sourceRoot.path(), true);
        auto *client = window.findChild<ld::Client *>(); QVERIFY(client);
        auto *receiver = client->fileTransfer();
        ld::FileTransferHost sender;
        sender.configure(sourceRoot.path()); sender.setOfferAvailable(true);
        receiver->setAvailable(true); receiver->setOfferAvailable(true);
        connect(&sender, &ld::FileTransferHost::send, receiver,
                &ld::FileTransferClient::receive, Qt::QueuedConnection);
        connect(receiver, &ld::FileTransferClient::send, &sender,
                &ld::FileTransferHost::receive, Qt::QueuedConnection);
        QSignalSpy replies(receiver, &ld::FileTransferClient::send);

        sender.offerFile(source);
        QTRY_VERIFY(window.findChild<QFileDialog *>("receiveFileOfferDialog"));
        QPointer<QFileDialog> dialog = window.findChild<QFileDialog *>("receiveFileOfferDialog");
        QTRY_VERIFY(dialog && dialog->isVisible());
        dialog->setDirectory(destinationRoot.path());
        auto *fileName = dialog->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(fileName);
        fileName->setFocus(); fileName->selectAll();
        QTest::keyClicks(fileName, QFileInfo(destination).fileName());
        QTRY_COMPARE(dialog->selectedFiles(), QStringList{destination});
        if (withdraw) sender.cancelOffer();
        else {
            auto *buttons = dialog->findChild<QDialogButtonBox *>(); QVERIFY(buttons);
            auto *refuse = buttons->button(QDialogButtonBox::Cancel); QVERIFY(refuse);
            QTest::mouseClick(refuse, Qt::LeftButton);
        }
        QTRY_VERIFY(!dialog || !dialog->isVisible());
        QTRY_VERIFY(!sender.busy() && !sender.offering() && !receiver->busy());
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(QDir(destinationRoot.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
        for (const auto &event : replies)
            QVERIFY(operation(event[0].toByteArray()) != "accept");
        QVERIFY(sender.offerAvailable() && receiver->offerAvailable());
    }

    void senderChooserFollowsCapabilityAndCancelState() {
        QTemporaryDir sharedRoot;
        QVERIFY(sharedRoot.isValid());
        const QString source = sharedRoot.filePath("chosen.dat");
        QVERIFY(writeFile(source, payload(4096)));
        ld::Window window;
        window.setLockOnDisconnect(false);
        window.configureFiles(sharedRoot.path(), true);
        auto *host = window.findChild<ld::Host *>(); QVERIFY(host);
        auto *sender = host->fileTransfer();
        auto *send = window.findChild<QPushButton *>("sendHostFile");
        auto *cancel = window.findChild<QPushButton *>("cancelHostFile");
        QVERIFY(send && cancel);
        QVERIFY(!send->isEnabled() && !cancel->isEnabled());
        QSignalSpy outgoing(sender, &ld::FileTransferHost::send);
        sender->setOfferAvailable(true);
        QVERIFY(send->isEnabled()); QVERIFY(!cancel->isEnabled());

        send->click();
        QPointer<QFileDialog> dialog = window.findChild<QFileDialog *>("chooseHostFileDialog");
        QVERIFY(dialog); QTRY_VERIFY(dialog->isVisible());
        QVERIFY(dialog->testOption(QFileDialog::DontUseNativeDialog));
        QCOMPARE(dialog->fileMode(), QFileDialog::ExistingFile);
        QCOMPARE(dialog->directory().canonicalPath(), QDir(sharedRoot.path()).canonicalPath());
        QVERIFY(!send->isEnabled() && !cancel->isEnabled());
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(send->isEnabled()); QCOMPARE(outgoing.count(), 0);

        send->click();
        dialog = window.findChild<QFileDialog *>("chooseHostFileDialog");
        QVERIFY(dialog); QTRY_VERIFY(dialog->isVisible());
        dialog->selectFile(source);
        QTRY_COMPARE(dialog->selectedFiles(), QStringList{source});
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept", Qt::DirectConnection));
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(outgoing.count(), 1);
        QCOMPARE(operation(outgoing.first()[0].toByteArray()), QStringLiteral("offer"));
        QVERIFY(sender->busy() && sender->offering());
        QVERIFY(!send->isEnabled() && cancel->isEnabled());
        cancel->click();
        QVERIFY(!sender->busy() && !sender->offering());
        QVERIFY(send->isEnabled() && !cancel->isEnabled());

        send->click();
        dialog = window.findChild<QFileDialog *>("chooseHostFileDialog");
        QVERIFY(dialog); QTRY_VERIFY(dialog->isVisible());
        sender->setOfferAvailable(false);
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(!send->isEnabled() && !cancel->isEnabled());
        QVERIFY(!host->running());
    }
};

QTEST_MAIN(FileOfferUiTest)
#include "file_offer_ui_test.moc"
