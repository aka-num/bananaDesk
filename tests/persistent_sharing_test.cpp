#include "app.h"
#include "identity_store.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {
QString messages(const QSignalSpy &status) {
    QStringList result;
    for (const auto &item : status) result.append(item.front().toString());
    return result.join('\n');
}
void configure(ld::Host &host) {
    // View-only sessions do not enable clipboard, input, or file operations.
    host.setLockOnDisconnect(false);
    host.configureVideo(15, "jpeg");
}
bool hostHasPeer(const ld::Host &host) {
    for (const auto *socket : host.findChildren<QSslSocket *>())
        if (socket->state() != QAbstractSocket::UnconnectedState) return true;
    return false;
}
void stopProcess(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
    process.terminate();
    if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(3000); }
}

// A directory at the destination reliably rejects QSaveFile commits on both
// Windows and POSIX, including when a test user can bypass chmod permissions.
// The lock stays at its original path and the original data is always restored.
class ObstructIdentityFile {
public:
    explicit ObstructIdentityFile(const QString &path) : path_(path), backup_(path + ".test-backup") {
        moved_ = QFile::rename(path_, backup_);
        valid_ = moved_ && QDir().mkdir(path_);
    }
    ~ObstructIdentityFile() { restore(); }
    bool valid() const { return valid_; }
    bool restore() {
        if (!moved_) return true;
        if (valid_ && !QDir().rmdir(path_)) return false;
        valid_ = false;
        if (!QFile::rename(backup_, path_)) return false;
        moved_ = false;
        return true;
    }
private:
    QString path_, backup_;
    bool moved_ = false, valid_ = false;
};
}

class PersistentSharingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(QSslSocket::supportsSsl());
        QCOMPARE(qgetenv("LANDESK_ISOLATED_TEST"), QByteArray("1"));
#ifndef Q_OS_WIN
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        QVERIFY(!qgetenv("LANDESK_SHARING_PRIVATE_DIRECTORY").isEmpty());
        QCOMPARE(qgetenv("DISPLAY"), qgetenv("LANDESK_SHARING_PRIVATE_DISPLAY"));
#endif
    }

    void stoppingAndRecreatingHostKeepsUsableCode() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        QString code, error;
        ld::Invitation invitation;
        {
            ld::Host host(nullptr, profile.path()); configure(host);
            QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
            code = host.invitation();
            QVERIFY(ld::Invitation::decode(code, invitation, error));
            ld::Client client(nullptr);
            QSignalSpy frames(&client, &ld::Client::frame), status(&client, &ld::Client::status);
            client.start(invitation);
            QTRY_VERIFY2_WITH_TIMEOUT(!frames.isEmpty(), qPrintable(messages(status)), 8000);
            host.stop();
            QVERIFY(!host.running());
            client.stop();
            QVERIFY2(host.start(QHostAddress::LocalHost, invitation.port, false, error), qPrintable(error));
            QCOMPARE(host.invitation(), code);
            frames.clear(); client.start(invitation);
            QTRY_VERIFY2_WITH_TIMEOUT(!frames.isEmpty(), qPrintable(messages(status)), 8000);
            client.stop(); host.stop();
        }
        // The second Host must load its credentials from the same user profile.
        ld::Host restarted(nullptr, profile.path()); configure(restarted);
        QVERIFY2(restarted.start(QHostAddress::LocalHost, invitation.port, false, error), qPrintable(error));
        QCOMPARE(restarted.invitation(), code);
        ld::Client client(nullptr);
        QSignalSpy frames(&client, &ld::Client::frame), status(&client, &ld::Client::status);
        client.start(invitation);
        QTRY_VERIFY2_WITH_TIMEOUT(!frames.isEmpty(), qPrintable(messages(status)), 8000);
        client.stop(); restarted.stop();
    }

    void resetDisconnectsAndRejectsBothOldSecrets() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        ld::Host host(nullptr, profile.path()); configure(host);
        QSignalSpy locks(&host, &ld::Host::desktopLockRequested);
        QString error; ld::Invitation original, replacement;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), original, error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability), frames(&client, &ld::Client::frame);
        QSignalSpy status(&client, &ld::Client::status), disconnected(&client, &ld::Client::disconnected);
        client.start(original);
        QTRY_VERIFY2_WITH_TIMEOUT(!frames.isEmpty(), qPrintable(messages(status)), 8000);
        disconnected.clear();
        QVERIFY2(host.resetSharingCode(error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!disconnected.isEmpty(), 3000);
        QVERIFY(host.running());
        QVERIFY(ld::Invitation::decode(host.invitation(), replacement, error));
        QCOMPARE(replacement.host, original.host);
        QCOMPARE(replacement.port, original.port);
        QVERIFY(replacement.fingerprint != original.fingerprint);
        QVERIFY(replacement.token != original.token);

        ready.clear(); frames.clear(); status.clear();
        client.start(original); disconnected.clear();
        QTRY_VERIFY2_WITH_TIMEOUT(!disconnected.isEmpty(), qPrintable(messages(status)), 5000);
        QVERIFY(messages(status).contains(QStringLiteral("证书指纹不匹配")));
        QVERIFY(ready.isEmpty() && frames.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!hostHasPeer(host), 3000);

        // A peer that knows the new public certificate still cannot use the old
        // access token. This separately exercises the authentication boundary.
        auto staleToken = replacement; staleToken.token = original.token;
        status.clear(); client.start(staleToken); disconnected.clear();
        QTRY_VERIFY2_WITH_TIMEOUT(!disconnected.isEmpty(), qPrintable(messages(status)), 5000);
        QVERIFY(!messages(status).contains(QStringLiteral("证书指纹不匹配")));
        QVERIFY(ready.isEmpty() && frames.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!hostHasPeer(host), 3000);

        client.start(replacement);
        QTRY_VERIFY2_WITH_TIMEOUT(!frames.isEmpty(), qPrintable(messages(status)), 8000);
        QCOMPARE(ready.count(), 1);
        client.stop(); host.stop();
        QCOMPARE(locks.count(), 0);
        QVERIFY2(host.start(QHostAddress::LocalHost, replacement.port, false, error), qPrintable(error));
        QCOMPARE(host.invitation(), replacement.encode());
    }

    void failedResetPreservesActiveCodeAndSession() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        ld::Host host(nullptr, profile.path()); configure(host);
        QString error; ld::Invitation original;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
        const QString code = host.invitation();
        QVERIFY(ld::Invitation::decode(code, original, error));
        ld::Client client(nullptr);
        QSignalSpy frames(&client, &ld::Client::frame), disconnected(&client, &ld::Client::disconnected);
        client.start(original);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 8000);
        disconnected.clear();
        ObstructIdentityFile obstruction(ld::IdentityStore(profile.path()).filePath());
        QVERIFY(obstruction.valid());
        error.clear();
        QVERIFY(!host.resetSharingCode(error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(host.invitation(), code);
        QVERIFY(host.running());
        const int count = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > count, 3000);
        QCOMPARE(disconnected.count(), 0);
        QVERIFY(obstruction.restore());
        client.stop(); host.stop();
        ld::Host restarted(nullptr, profile.path()); configure(restarted);
        QVERIFY2(restarted.start(QHostAddress::LocalHost, original.port, false, error), qPrintable(error));
        QCOMPARE(restarted.invitation(), code);
    }

    void resetWhileStoppedPersistsForNextStart() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        QString error; ld::Invitation original, replacement;
        {
            ld::Host host(nullptr, profile.path()); configure(host);
            QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
            QVERIFY(ld::Invitation::decode(host.invitation(), original, error));
            host.stop();
            QVERIFY2(host.resetSharingCode(error), qPrintable(error));
            QVERIFY(!host.running());
        }
        ld::Host restarted(nullptr, profile.path()); configure(restarted);
        QVERIFY2(restarted.start(QHostAddress::LocalHost, original.port, false, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(restarted.invitation(), replacement, error));
        QVERIFY(replacement.fingerprint != original.fingerprint);
        QVERIFY(replacement.token != original.token);
        ld::Client client(nullptr);
        QSignalSpy frames(&client, &ld::Client::frame);
        client.start(replacement);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 8000);
        client.stop();
    }

    void profileCannotServeAnOldIdentityFromAnotherHost() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        ld::Host first(nullptr, profile.path()), second(nullptr, profile.path());
        configure(first); configure(second);
        QString error; ld::Invitation original;
        QVERIFY2(first.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
        const QString code = first.invitation();
        QVERIFY(ld::Invitation::decode(code, original, error));
        QVERIFY(!second.start(QHostAddress::LocalHost, 0, false, error));
        QVERIFY(!error.isEmpty() && !second.running());
        error.clear();
        QVERIFY(!second.resetSharingCode(error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(first.invitation(), code);
        ld::Client client(nullptr);
        QSignalSpy frames(&client, &ld::Client::frame);
        client.start(original);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 8000);
        client.stop(); first.stop();
        QVERIFY2(second.start(QHostAddress::LocalHost, original.port, false, error), qPrintable(error));
        QCOMPARE(second.invitation(), code);
    }

    void failedListenReleasesProfileLock() {
        QTemporaryDir profile; QVERIFY(profile.isValid());
        QTcpServer occupied; occupied.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        ld::Host first(nullptr, profile.path()), second(nullptr, profile.path());
        configure(first); configure(second);
        QString error;
        QVERIFY(!first.start(QHostAddress::LocalHost, occupied.serverPort(), false, error));
        QVERIFY(!first.running());
        QVERIFY2(second.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
    }

    void windowResetAndRestartLifecycle() {
        QTemporaryDir sharedRoot; QVERIFY(sharedRoot.isValid());
        const QString exportPath = sharedRoot.filePath("private-invitation.txt");
        const auto exportedCode = [&exportPath] {
            QFile file(exportPath);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        QTcpServer reservation; reservation.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const quint16 port = reservation.serverPort(); reservation.close();
        QString retained;
        {
            ld::Window window;
            window.setViewOnly(); window.setLockOnDisconnect(false);
            window.configureVideo(15, "jpeg");
            window.configureFiles(sharedRoot.path(), false);
            auto *share = window.findChild<QPushButton *>("shareButton");
            auto *reset = window.findChild<QPushButton *>("resetSharingCode");
            auto *code = window.findChild<QPlainTextEdit *>("sharingCode");
            auto *host = window.findChild<ld::Host *>();
            QVERIFY(share && reset && code && host);
            QVERIFY2(window.startHost("127.0.0.1", port, exportPath), "Window must start sharing on the test endpoint");
            const QString original = code->toPlainText();
            QVERIFY(!original.isEmpty());
            QCOMPARE(exportedCode(), original);
            share->click(); QVERIFY(!host->running());
            QVERIFY(!share->text().contains(QStringLiteral("撤销")));
            share->click(); QVERIFY(host->running());
            QCOMPARE(code->toPlainText(), original);

            // Cancelling is a real no-op; accepting changes the displayed code
            // and keeps the listener running without enabling remote control.
            bool answered = false;
            QTimer::singleShot(0, &window, [&answered] {
                if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    if (auto *button = dialog->button(QMessageBox::No)) { answered = true; button->click(); }
                }
            });
            reset->click(); QVERIFY(answered);
            QCOMPARE(code->toPlainText(), original);
            answered = false;
            QTimer::singleShot(0, &window, [&answered] {
                if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    if (auto *button = dialog->button(QMessageBox::Yes)) { answered = true; button->click(); }
                }
            });
            reset->click(); QVERIFY(answered);
            QVERIFY(host->running());
            retained = code->toPlainText();
            QVERIFY(!retained.isEmpty() && retained != original);
            QCOMPARE(retained, host->invitation());
            QCOMPARE(exportedCode(), retained);
            share->click(); QVERIFY(!host->running());

            // A stopped reset has no usable endpoint to export. Restarting
            // through the UI must retain and refresh the selected export file.
            answered = false;
            QTimer::singleShot(0, &window, [&answered] {
                if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    if (auto *button = dialog->button(QMessageBox::Yes)) { answered = true; button->click(); }
                }
            });
            reset->click(); QVERIFY(answered);
            QVERIFY(!host->running());
            share->click(); QVERIFY(host->running());
            const QString afterStoppedReset = code->toPlainText();
            QVERIFY(!afterStoppedReset.isEmpty() && afterStoppedReset != retained);
            QCOMPARE(afterStoppedReset, host->invitation());
            QCOMPARE(exportedCode(), afterStoppedReset);
            retained = afterStoppedReset;
            share->click(); QVERIFY(!host->running());
        }
        ld::Window restarted;
        restarted.setViewOnly(); restarted.setLockOnDisconnect(false);
        restarted.configureVideo(15, "jpeg"); restarted.configureFiles(sharedRoot.path(), false);
        auto *address = restarted.findChild<QComboBox *>("sharingAddress");
        auto *savedPort = restarted.findChild<QSpinBox *>("sharingPort");
        QVERIFY(address && savedPort);
        QCOMPARE(address->currentText(), QStringLiteral("127.0.0.1"));
        QCOMPARE(savedPort->value(), int(port));
        QVERIFY(restarted.startHost());
        auto *code = restarted.findChild<QPlainTextEdit *>("sharingCode"); QVERIFY(code);
        QCOMPARE(code->toPlainText(), retained);
    }
};

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    if (qgetenv("LANDESK_ISOLATED_TEST") != "1") return 77;
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("bananaDesk-persistent-sharing-test");
    QStandardPaths::setTestModeEnabled(true);
    PersistentSharingTest test;
    return QTest::qExec(&test, argc, argv);
#else
    const bool child = argc > 1 && QByteArray(argv[1]) == "--private-sharing-test-child";
    if (child) {
        if (qgetenv("LANDESK_SHARING_PRIVATE_DIRECTORY").isEmpty()
            || !qgetenv("DISPLAY").startsWith(':')
            || qgetenv("DISPLAY") != qgetenv("LANDESK_SHARING_PRIVATE_DISPLAY")) return 2;
        for (int n = 1; n < argc - 1; ++n) argv[n] = argv[n + 1];
        --argc; argv[argc] = nullptr;
        QApplication app(argc, argv);
        QCoreApplication::setApplicationName("bananaDesk-persistent-sharing-test");
        QStandardPaths::setTestModeEnabled(true);
        PersistentSharingTest test;
        return QTest::qExec(&test, argc, argv);
    }
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    QProcess xvfb, tests;
    xvfb.start("Xvfb", {"-displayfd", "1", "-screen", "0", "960x640x24", "-nolisten", "tcp", "-ac"});
    if (!xvfb.waitForStarted(3000) || !xvfb.waitForReadyRead(3000)) {
        QTextStream(stderr) << "Cannot start private Xvfb" << Qt::endl;
        stopProcess(xvfb); return 2;
    }
    const QByteArray number = xvfb.readLine().trimmed();
    bool valid = false; number.toUInt(&valid);
    if (!valid || number.isEmpty()) { stopProcess(xvfb); return 2; }
    auto env = QProcessEnvironment::systemEnvironment();
    const QString display = ":" + QString::fromLatin1(number);
    env.insert("DISPLAY", display); env.insert("LANDESK_SHARING_PRIVATE_DISPLAY", display);
    env.insert("LANDESK_SHARING_PRIVATE_DIRECTORY", directory.path());
    env.insert("LANDESK_ISOLATED_TEST", "1");
    env.insert("QT_QPA_PLATFORM", "xcb"); env.insert("XDG_SESSION_TYPE", "x11");
    env.insert("HOME", directory.path());
    for (const QString &name : {QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("XDG_DATA_HOME"),
                               QStringLiteral("XDG_CACHE_HOME"), QStringLiteral("XDG_RUNTIME_DIR")})
        env.insert(name, directory.path());
    env.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + directory.path() + "/no-session-bus");
    env.insert("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=" + directory.path() + "/no-system-bus");
    env.insert("QT_ACCESSIBILITY", "0"); env.insert("NO_AT_BRIDGE", "1");
    for (const QString &name : {QStringLiteral("XAUTHORITY"), QStringLiteral("SESSION_MANAGER"),
                               QStringLiteral("AT_SPI_BUS_ADDRESS"), QStringLiteral("QT_SCALE_FACTOR"),
                               QStringLiteral("QT_AUTO_SCREEN_SCALE_FACTOR")}) env.remove(name);
    tests.setProcessEnvironment(env); tests.setProcessChannelMode(QProcess::ForwardedChannels);
    auto arguments = app.arguments().mid(1); arguments.prepend("--private-sharing-test-child");
    tests.start(app.applicationFilePath(), arguments);
    int result = 2;
    if (tests.waitForStarted(3000) && tests.waitForFinished(120000) && tests.exitStatus() == QProcess::NormalExit)
        result = tests.exitCode();
    stopProcess(tests); stopProcess(xvfb);
    return result;
#endif
}

#include "persistent_sharing_test.moc"
