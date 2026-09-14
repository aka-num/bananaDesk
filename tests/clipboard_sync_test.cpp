#include "clipboard_sync.h"
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QImage>
#include <QLineEdit>
#include <QMimeData>
#include <QSignalSpy>
#include <QtTest>
#include <cstdio>
#include <cstdlib>
#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
const QString OriginMime = QStringLiteral("application/x-bananadesk-clipboard-origin");
QByteArray hash(const QByteArray &text) { return QCryptographicHash::hash(text, QCryptographicHash::Sha256); }
QClipboard *clipboard() { return QApplication::clipboard(); }
void copy(const QString &text) { clipboard()->setText(text, QClipboard::Clipboard); }
void copyImage() {
    auto *mime = new QMimeData;
    QImage image(4, 4, QImage::Format_RGB32); image.fill(Qt::yellow);
    mime->setImageData(image);
    clipboard()->setMimeData(mime, QClipboard::Clipboard);
}
}

class ClipboardSyncTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        copy(QString::fromLatin1(QTest::currentTestFunction()) + QStringLiteral(" initial local text"));
    }
    void cleanup() { clipboard()->clear(QClipboard::Clipboard); }

    void disabledAndInitialSnapshotStayLocal() {
        ld::ClipboardSync sync; QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        QVERIFY(!sync.enabled()); QVERIFY(!sync.receive("disabled remote text"));
        copy("text copied while disabled"); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        sync.setEnabled(true); sync.setEnabled(true); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        // An explicit copy after connection can contain exactly the old text.
        copy("text copied while disabled"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000);
        QCOMPARE(sent.front()[0].toByteArray(), QByteArray("text copied while disabled"));
    }

    void unicodeMultilineReceiveDoesNotPasteOrKeepHtml() {
        QLineEdit field; field.setText(QStringLiteral("unchanged field")); field.show(); field.setFocus();
        auto *rich = new QMimeData; rich->setText("old rich text"); rich->setHtml("<b>old rich text</b>");
        clipboard()->setMimeData(rich, QClipboard::Clipboard);
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        const QByteArray text = QString::fromUtf8("中文剪贴板 🍌\nsecond line\r\nthird line").toUtf8();
        QVERIFY(sync.receive(text)); QTest::qWait(150);
        QCOMPARE(clipboard()->text(QClipboard::Clipboard), QString::fromUtf8(text));
        QVERIFY(clipboard()->mimeData(QClipboard::Clipboard)->hasFormat(OriginMime));
        QVERIFY(!clipboard()->mimeData(QClipboard::Clipboard)->hasHtml());
        QCOMPARE(field.text(), QStringLiteral("unchanged field")); QCOMPARE(sent.size(), 0);
    }

    void localUnicodeAndRapidChangesAreCoalesced() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        copy("first rapid copy"); QTest::qWait(25);
        copy("second rapid copy"); QTest::qWait(25);
        const QString text = QString::fromUtf8("最后一条 🍌\nline two"); copy(text);
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000); QCOMPARE(sent.front()[0].toByteArray(), text.toUtf8());
        copy(text); copy(text); QTest::qWait(150); QCOMPARE(sent.size(), 1);
    }

    void multipleInstancesDoNotRelayRemoteOrManagerEcho() {
        ld::ClipboardSync host, client; host.setEnabled(true); client.setEnabled(true);
        QSignalSpy fromHost(&host, &ld::ClipboardSync::send), fromClient(&client, &ld::ClipboardSync::send);
        const QByteArray text("received from a remote session");
        QVERIFY(host.receive(text)); QTest::qWait(150);
        QCOMPARE(fromHost.size(), 0); QCOMPARE(fromClient.size(), 0);
        // Simulate a clipboard manager stripping our private MIME marker.
        copy(QString::fromUtf8(text)); copy(QString::fromUtf8(text)); QTest::qWait(150);
        QCOMPARE(fromHost.size(), 0); QCOMPARE(fromClient.size(), 0);
        QVERIFY(client.receive(text)); QTest::qWait(150);
        QCOMPARE(fromHost.size(), 0); QCOMPARE(fromClient.size(), 0);
    }

    void receivedTextCanBeCopiedAgainAfterDifferentLocalContent() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        const QString old = QStringLiteral("common phrase received previously");
        QVERIFY(sync.receive(old.toUtf8())); copy(old); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        copy("a genuinely new local clipboard value"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000);
        copy(old); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 2, 1000); QCOMPARE(sent.back()[0].toByteArray(), old.toUtf8());
    }

    void markedForeignProcessContentIsNotRelayed() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        auto *mime = new QMimeData;
        mime->setText("remote text written by another bananaDesk process"); mime->setData(OriginMime, "remote");
        clipboard()->setMimeData(mime, QClipboard::Clipboard); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        copy("remote text written by another bananaDesk process"); QTest::qWait(150); QCOMPARE(sent.size(), 0);
    }

    void localOnlyInvitationAndPendingTextNeverLeaveProcess() {
        ld::ClipboardSync first, second; first.setEnabled(true); second.setEnabled(true);
        QSignalSpy sentFirst(&first, &ld::ClipboardSync::send), sentSecond(&second, &ld::ClipboardSync::send);
        copy("pending local text superseded before debounce");
        const QString code = QStringLiteral("synthetic-access-code-for-test-only");
        ld::ClipboardSync::setLocalOnlyText(code); QTest::qWait(150);
        QCOMPARE(clipboard()->text(QClipboard::Clipboard), code);
        QCOMPARE(sentFirst.size(), 0); QCOMPARE(sentSecond.size(), 0);
        copy(code); QTest::qWait(150); QCOMPARE(sentFirst.size(), 0); QCOMPARE(sentSecond.size(), 0);
    }

    void remoteWriteCancelsPendingLocalTextInEveryInstance() {
        ld::ClipboardSync first, second; first.setEnabled(true); second.setEnabled(true);
        QSignalSpy sentFirst(&first, &ld::ClipboardSync::send), sentSecond(&second, &ld::ClipboardSync::send);
        copy("unsent local pending value"); QVERIFY(first.receive("remote replacement before timer"));
        QTest::qWait(150); QCOMPARE(sentFirst.size(), 0); QCOMPARE(sentSecond.size(), 0);
    }

    void disconnectCancelsPendingAndReenableDoesNotSendOldText() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        copy("copied immediately before disconnect"); sync.setEnabled(false);
        QTest::qWait(150); QCOMPARE(sent.size(), 0);
        sync.setEnabled(true); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        copy("copied immediately before disconnect"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000);
    }

    void emptyTextClearsInBothDirections() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        copy(QString()); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000);
        QVERIFY(sent.front()[0].toByteArray().isEmpty());
        copy("text before remote clear"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 2, 1000);
        QVERIFY(sync.receive({})); QTest::qWait(150); QVERIFY(clipboard()->text(QClipboard::Clipboard).isEmpty());
        QCOMPARE(sent.size(), 2);
        copy("fresh local text after a received clear"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 3, 1000);
        clipboard()->clear(QClipboard::Clipboard); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 4, 1000);
        QVERIFY(sent.back()[0].toByteArray().isEmpty());
    }

    void imageFileListAndUrlsOnlyAreIgnored() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        copyImage(); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        auto *files = new QMimeData;
        files->setUrls({QUrl::fromLocalFile("/test-only/file.txt")}); files->setText("/test-only/file.txt");
        clipboard()->setMimeData(files, QClipboard::Clipboard); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        auto *urls = new QMimeData; urls->setUrls({QUrl("https://example.invalid/test-only")});
        clipboard()->setMimeData(urls, QClipboard::Clipboard); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        auto *plain = new QMimeData; plain->setText("plain text from rich content"); plain->setHtml("<i>plain text from rich content</i>");
        clipboard()->setMimeData(plain, QClipboard::Clipboard);
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000); QCOMPARE(sent.front()[0].toByteArray(), QByteArray("plain text from rich content"));
    }

    void imageInterruptingPendingTextDoesNotPoisonDuplicateBaseline() {
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        copy("candidate A not yet sent"); copyImage(); QTest::qWait(150); QCOMPARE(sent.size(), 0);
        copy("candidate A not yet sent"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000);
    }

    void selectionAndFindBufferAreNotMonitored() {
        if (!clipboard()->supportsSelection()) QSKIP("This platform does not support the X11 selection buffer.");
        ld::ClipboardSync sync; sync.setEnabled(true); QSignalSpy sent(&sync, &ld::ClipboardSync::send);
        const QString original = clipboard()->text(QClipboard::Clipboard);
        clipboard()->setText("selected text is not copied text", QClipboard::Selection);
        if (clipboard()->supportsFindBuffer()) clipboard()->setText("find buffer text", QClipboard::FindBuffer);
        QTest::qWait(150); QCOMPARE(sent.size(), 0); QCOMPARE(clipboard()->text(QClipboard::Clipboard), original);
        clipboard()->clear(QClipboard::Selection);
    }

    void invalidRemoteUtf8_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("nul") << QByteArray("a\0b", 3);
        QTest::newRow("lone-continuation") << QByteArray::fromHex("80");
        QTest::newRow("truncated") << QByteArray::fromHex("f09f8d");
        QTest::newRow("overlong-two") << QByteArray::fromHex("c080");
        QTest::newRow("overlong-three") << QByteArray::fromHex("e08080");
        QTest::newRow("surrogate") << QByteArray::fromHex("eda080");
        QTest::newRow("beyond-unicode") << QByteArray::fromHex("f4908080");
        QTest::newRow("overlong-four") << QByteArray::fromHex("f0808080");
        QTest::newRow("five-byte") << QByteArray::fromHex("f888808080");
        QTest::newRow("wrong-continuation") << QByteArray::fromHex("e228a1");
        QTest::newRow("oversize") << QByteArray(ld::MaxClipboardBytes + 1, 'x');
    }
    void invalidRemoteUtf8() {
        QFETCH(QByteArray, bytes);
        ld::ClipboardSync sync; sync.setEnabled(true);
        QSignalSpy sent(&sync, &ld::ClipboardSync::send), status(&sync, &ld::ClipboardSync::status);
        const QString original = clipboard()->text(QClipboard::Clipboard);
        QVERIFY(!sync.receive(bytes)); QCOMPARE(status.size(), 1); QCOMPARE(sent.size(), 0);
        QCOMPARE(clipboard()->text(QClipboard::Clipboard), original);
    }

    void byteLimitsAndLocalOversizeFeedback() {
        ld::ClipboardSync sync; sync.setEnabled(true);
        QSignalSpy sent(&sync, &ld::ClipboardSync::send), status(&sync, &ld::ClipboardSync::status);
        const QByteArray boundary(ld::MaxClipboardBytes, 'z');
        QVERIFY(sync.receive(boundary)); QCOMPARE(clipboard()->text(QClipboard::Clipboard).size(), ld::MaxClipboardBytes);
        copy("release protection with a different local value"); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 1, 1000);
        copy(QString::fromUtf8(boundary)); QTRY_COMPARE_WITH_TIMEOUT(sent.size(), 2, 1000);
        QCOMPARE(hash(sent.back()[0].toByteArray()), hash(boundary));
        copy(QString(ld::MaxClipboardBytes + 1, 'x')); QTest::qWait(150);
        QCOMPARE(sent.size(), 2); QVERIFY(!status.isEmpty());
        const QString multibyte = QString::fromUtf8("🍌").repeated(ld::MaxClipboardBytes / 4 + 1);
        const int before = status.size(); copy(multibyte); QTest::qWait(150);
        QCOMPARE(sent.size(), 2); QVERIFY(status.size() > before);
        copy(QString("local") + QChar(0) + "nul"); QTest::qWait(150); QCOMPARE(sent.size(), 2);
        const QByteArray bom = QByteArray::fromHex("efbbbf") + "valid BOM text";
        QVERIFY(sync.receive(bom));
    }
};

// Bootstrap a brand-new Xvfb before QApplication exists, so even Qt platform
// initialization cannot attach to the user's real clipboard. Windows runs are
// permitted only when their caller explicitly marks an isolated test desktop.
#ifndef Q_OS_WIN
class PrivateDisplay {
    pid_t process_ = -1;
public:
    bool start() {
        qunsetenv("DISPLAY"); qunsetenv("WAYLAND_DISPLAY");
        int descriptors[2]; if (::pipe(descriptors)) return false;
        process_ = ::fork();
        if (process_ == 0) {
            ::close(descriptors[0]);
            char descriptor[32]; std::snprintf(descriptor, sizeof(descriptor), "%d", descriptors[1]);
            ::execlp("Xvfb", "Xvfb", "-displayfd", descriptor, "-screen", "0", "1024x768x24", "-nolisten", "tcp", static_cast<char *>(nullptr));
            ::_exit(127);
        }
        ::close(descriptors[1]);
        if (process_ < 0) { ::close(descriptors[0]); return false; }
        pollfd wait{descriptors[0], POLLIN, 0};
        QByteArray number;
        if (::poll(&wait, 1, 5000) > 0) { char buffer[32]; const auto count = ::read(descriptors[0], buffer, sizeof(buffer)); if (count > 0) number = QByteArray(buffer, int(count)).trimmed(); }
        ::close(descriptors[0]);
        if (number.isEmpty()) return false;
        for (char c : number) if (c < '0' || c > '9') return false;
        qputenv("DISPLAY", ":" + number); qputenv("QT_QPA_PLATFORM", "xcb");
        qputenv("XDG_SESSION_TYPE", "x11"); qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-bananadesk-clipboard-test-bus");
        return true;
    }
    ~PrivateDisplay() {
        if (process_ <= 0) return;
        ::kill(process_, SIGTERM);
        for (int i = 0; i < 100; ++i) { if (::waitpid(process_, nullptr, WNOHANG) == process_) return; ::usleep(10000); }
        ::kill(process_, SIGKILL); ::waitpid(process_, nullptr, 0);
    }
};
#endif

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariable("LANDESK_CLIPBOARD_TEST_DESKTOP") != QLatin1String("1")) {
        std::fputs("Refusing clipboard test without an isolated Windows test desktop marker.\n", stderr); return 77;
    }
#else
    PrivateDisplay display;
    if (!display.start()) { std::fputs("Failed to start private Xvfb; no user clipboard was opened.\n", stderr); return 2; }
#endif
    QApplication app(argc, argv);
    ClipboardSyncTest tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "clipboard_sync_test.moc"
