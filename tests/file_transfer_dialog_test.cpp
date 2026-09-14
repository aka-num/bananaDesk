#include "file_transfer.h"
#include "file_transfer_dialog.h"
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QtTest>
#include <memory>

// These tests exercise only the file UI and the backend's local request state.
// No desktop Host, network connection, native file dialog, or OS lock is used.
class FileTransferDialogTest : public QObject {
    Q_OBJECT
private slots:
    void unavailableAndDisconnectedDisableActions() {
        ld::FileTransferClient client;
        ld::FileTransferDialog dialog(&client); dialog.show();
        auto *files = dialog.findChild<QTreeWidget *>("remoteFiles"); QVERIFY(files);
        for (const char *name : {"refreshFiles", "uploadFile", "downloadFile", "cancelFileTransfer"}) {
            auto *button = dialog.findChild<QPushButton *>(name); QVERIFY(button); QVERIFY(!button->isEnabled());
        }
        client.setAvailable(true);
        QTRY_VERIFY(client.busy()); // Becoming available refreshes an already-open dialog.
        client.cancel();
        client.listing(QJsonArray{QJsonObject{{"name", "example.txt"}, {"size", 42}}});
        QCOMPARE(files->topLevelItemCount(), 1);
        files->setCurrentItem(files->topLevelItem(0)); files->topLevelItem(0)->setSelected(true);
        QVERIFY(dialog.findChild<QPushButton *>("downloadFile")->isEnabled());
        client.setAvailable(false);
        QVERIFY(!client.busy()); QCOMPARE(files->topLevelItemCount(), 0);
        for (const char *name : {"refreshFiles", "uploadFile", "downloadFile", "cancelFileTransfer"})
            QVERIFY(!dialog.findChild<QPushButton *>(name)->isEnabled());
        QCOMPARE(dialog.findChild<QProgressBar *>("fileTransferProgress")->value(), 0);
    }

    void listingSelectionAndPlainTextAt640By480() {
        ld::FileTransferClient client; client.setAvailable(true);
        ld::FileTransferDialog dialog(&client); dialog.show();
        auto *files = dialog.findChild<QTreeWidget *>("remoteFiles");
        auto *download = dialog.findChild<QPushButton *>("downloadFile");
        auto *status = dialog.findChild<QLabel *>("fileTransferStatus");
        QVERIFY(files && download && status); QVERIFY(!download->isEnabled());
        const QString longName = QString(240, 'a') + ".txt";
        client.listing(QJsonArray{
            QJsonObject{{"name", "<img src=x>"}, {"size", 2048}},
            QJsonObject{{"name", longName}, {"size", 4294967296.0}},
            QJsonObject{{"name", "../outside.txt"}, {"size", 1}}});
        QCOMPARE(files->topLevelItemCount(), 2);
        QCOMPARE(files->topLevelItem(0)->text(0), QStringLiteral("<img src=x>"));
        files->setCurrentItem(files->topLevelItem(0)); files->topLevelItem(0)->setSelected(true);
        QVERIFY(download->isEnabled());
        client.status("<b>" + QString(400, 'x') + "</b>");
        QCOMPARE(status->textFormat(), Qt::PlainText);
        QVERIFY(status->text().startsWith("<b>"));
        for (const QSize size : {QSize(640, 480), QSize(900, 640), QSize(640, 480)}) {
            dialog.resize(size); QCoreApplication::processEvents();
            QCOMPARE(dialog.size(), size);
            for (auto *widget : {static_cast<QWidget *>(files), static_cast<QWidget *>(status),
                                static_cast<QWidget *>(dialog.findChild<QProgressBar *>("fileTransferProgress"))})
                QVERIFY(dialog.rect().contains(widget->geometry()));
        }
        client.busyChanged(true);
        QVERIFY(!download->isEnabled());
        QVERIFY(dialog.findChild<QPushButton *>("cancelFileTransfer")->isEnabled());
        client.busyChanged(false);
        QVERIFY(download->isEnabled());
    }

    void largeFileProgressDoesNotOverflow() {
        ld::FileTransferClient client; client.setAvailable(true);
        ld::FileTransferDialog dialog(&client);
        auto *progress = dialog.findChild<QProgressBar *>("fileTransferProgress"); QVERIFY(progress);
        const qint64 total = qint64(8) * 1024 * 1024 * 1024;
        client.busyChanged(true); client.progress(total / 2, total);
        QCOMPARE(progress->maximum(), 1000); QCOMPARE(progress->value(), 500);
        QVERIFY(progress->format().contains("GiB"));
        client.progress(total, total); QCOMPARE(progress->value(), 1000);
        client.busyChanged(false); QCOMPARE(progress->value(), 1000);
    }

    void cancelAndClosePreserveSessionAvailability() {
        ld::FileTransferClient client; client.setAvailable(true);
        ld::FileTransferDialog dialog(&client); dialog.show();
        QSignalSpy outgoing(&client, &ld::FileTransferClient::send);
        auto *cancel = dialog.findChild<QPushButton *>("cancelFileTransfer"); QVERIFY(cancel);
        client.refresh(); QVERIFY(client.busy()); QVERIFY(cancel->isEnabled());
        QTest::mouseClick(cancel, Qt::LeftButton);
        QVERIFY(!client.busy()); QVERIFY(client.available()); QVERIFY(dialog.isVisible());
        client.refresh(); QVERIFY(client.busy());
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible()); QVERIFY(!client.busy()); QVERIFY(client.available());
        const int messages = outgoing.count();
        dialog.show(); QCoreApplication::processEvents();
        QCOMPARE(outgoing.count(), messages); // The parent toolbar owns show-time refresh.
        client.refresh(); QVERIFY(client.busy());
        dialog.close();
        QVERIFY(!client.busy()); QVERIFY(client.available());
    }

    void backendDestructionDisablesTheDialog() {
        auto client = std::make_unique<ld::FileTransferClient>(); client->setAvailable(true);
        ld::FileTransferDialog dialog(client.get()); dialog.show();
        client.reset();
        for (const char *name : {"refreshFiles", "uploadFile", "downloadFile", "cancelFileTransfer"})
            QVERIFY(!dialog.findChild<QPushButton *>(name)->isEnabled());
        dialog.close(); // A destroyed backend must never be dereferenced here.
    }
};

QTEST_MAIN(FileTransferDialogTest)
#include "file_transfer_dialog_test.moc"
