#include "app.h"
#include <QApplication>
#include <QClipboard>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {
// A real TLS listener, not a desktop Host. It sends only controlled Welcome
// and clipboard packets, and never captures, injects input, or invokes a lock.
class ClipboardProtocolHost : public QObject {
public:
    ld::Listener listener{this};
    ld::Identity identity;
    QSslSocket *socket = nullptr;
    ld::Wire *wire = nullptr;
    QJsonObject welcome{{"v", 1}, {"width", 640}, {"height", 360}, {"control", true},
                        {"codec", "jpeg"}, {"fps", 15}, {"window", 1}};
    QList<ld::Packet> received;
    bool sendWelcome = true, authenticated = false, advertisedClipboard = false, wireFailed = false;

    ClipboardProtocolHost() {
        connect(&listener, &ld::Listener::accepted, this, [this](qintptr descriptor) {
            socket = new QSslSocket(this);
            if (!socket->setSocketDescriptor(descriptor)) { wireFailed = true; return; }
            auto config = QSslConfiguration::defaultConfiguration();
            config.setProtocol(QSsl::TlsV1_2OrLater);
            config.setPeerVerifyMode(QSslSocket::VerifyNone);
            config.setLocalCertificate(identity.certificate); config.setPrivateKey(identity.key);
            socket->setSslConfiguration(config);
            // Accept a full clipboard packet so a forbidden send cannot hide
            // behind this test server's smaller keyboard-message limit.
            wire = new ld::Wire(socket, ld::MaxClipboardBytes + 1, socket);
            connect(wire, &ld::Wire::failure, this, [this] { wireFailed = true; });
            connect(wire, &ld::Wire::packet, this, [this](ld::Packet type, const QByteArray &payload) {
                received.append(type);
                if (!authenticated) {
                    QJsonObject auth;
                    if (type != ld::Packet::Auth || !ld::object(payload, auth)
                        || auth.value("token").toString() != identity.token) {
                        socket->abort(); return;
                    }
                    authenticated = true;
                    advertisedClipboard = auth.value("clipboard").isBool() && auth.value("clipboard").toBool();
                    if (sendWelcome && !wire->send(ld::Packet::Welcome, ld::json(welcome))) wireFailed = true;
                } else if (type == ld::Packet::Ping && !wire->send(ld::Packet::Pong)) wireFailed = true;
            });
            socket->startServerEncryption();
        });
    }
    bool start(QString &error) {
        return identity.create(error) && listener.listen(QHostAddress::LocalHost, 0);
    }
    ld::Invitation invitation() const {
        return {"127.0.0.1", listener.serverPort(), identity.fingerprint, identity.token};
    }
    int count(ld::Packet type) const { return received.count(type); }
};

void stopProcess(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
    process.terminate();
    if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(3000); }
}
}

class ClipboardClientTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(QSslSocket::supportsSsl());
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        QVERIFY(!qEnvironmentVariable("LANDESK_CLIPBOARD_CLIENT_PRIVATE_DIRECTORY").isEmpty());
        QCOMPARE(qgetenv("DISPLAY"), qgetenv("LANDESK_CLIPBOARD_CLIENT_PRIVATE_DISPLAY"));
    }

    void localCopiesRequireBothCapabilityAndControl_data() {
        QTest::addColumn<bool>("control");
        QTest::addColumn<bool>("advertise");
        QTest::newRow("old-host-missing-capability") << true << false;
        QTest::newRow("view-only-despite-capability") << false << true;
    }
    void localCopiesRequireBothCapabilityAndControl() {
        QFETCH(bool, control); QFETCH(bool, advertise);
        ClipboardProtocolHost server;
        server.welcome["control"] = control;
        if (advertise) server.welcome["clipboard"] = true;
        QString error; QVERIFY2(server.start(error), qPrintable(error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        client.start(server.invitation());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 5000);
        QVERIFY(server.authenticated && server.advertisedClipboard);
        QCOMPARE(ready.last()[0].toBool(), control);
        disconnected.clear();
        const QString local = QStringLiteral("private local copy ") + QLatin1String(QTest::currentDataTag());
        QApplication::clipboard()->setText(local);
        QTest::qWait(250); // Longer than the production 100 ms debounce.
        client.release();
        // Same-stream barrier proves the connection is alive after the copy
        // and that any preceding accidental clipboard packet was observed.
        QTRY_COMPARE_WITH_TIMEOUT(server.count(ld::Packet::Release), 1, 3000);
        QCOMPARE(server.count(ld::Packet::Clipboard), 0);
        QCOMPARE(disconnected.count(), 0);
        QVERIFY(!server.wireFailed);
        QCOMPARE(QApplication::clipboard()->text(), local);
        client.stop();
    }

    void unauthorizedRemoteClipboardDisconnectsWithoutChangingLocalText_data() {
        QTest::addColumn<bool>("welcome");
        QTest::addColumn<bool>("control");
        QTest::addColumn<bool>("advertise");
        QTest::newRow("before-welcome") << false << true << true;
        QTest::newRow("old-host-no-permission") << true << true << false;
        QTest::newRow("view-only") << true << false << true;
    }
    void unauthorizedRemoteClipboardDisconnectsWithoutChangingLocalText() {
        QFETCH(bool, welcome); QFETCH(bool, control); QFETCH(bool, advertise);
        const QString sentinel = QStringLiteral("private clipboard must survive ") + QLatin1String(QTest::currentDataTag());
        QApplication::clipboard()->setText(sentinel);
        ClipboardProtocolHost server;
        server.sendWelcome = welcome; server.welcome["control"] = control;
        if (advertise) server.welcome["clipboard"] = true;
        QString error; QVERIFY2(server.start(error), qPrintable(error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        client.start(server.invitation());
        QTRY_VERIFY_WITH_TIMEOUT(server.authenticated, 5000);
        QVERIFY(server.advertisedClipboard);
        if (welcome) QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
        else QCOMPARE(ready.count(), 0);
        disconnected.clear();
        QVERIFY(server.wire->send(ld::Packet::Clipboard, QByteArray("unauthorized remote replacement")));
        QTRY_VERIFY_WITH_TIMEOUT(!disconnected.isEmpty(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.socket->state(), QAbstractSocket::UnconnectedState, 3000);
        QCOMPARE(QApplication::clipboard()->text(), sentinel);
        QCOMPARE(server.count(ld::Packet::Clipboard), 0);
        QVERIFY(!server.wireFailed);
        client.stop();
    }
};

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    Q_UNUSED(argc); Q_UNUSED(argv);
    return 77; // This suite owns Linux Xvfb; never use a real Windows clipboard.
#else
    const bool child = argc > 1 && QByteArray(argv[1]) == "--private-clipboard-client-child";
    if (child) {
        if (qgetenv("LANDESK_CLIPBOARD_CLIENT_PRIVATE_DIRECTORY").isEmpty()
            || !qgetenv("DISPLAY").startsWith(':')
            || qgetenv("DISPLAY") != qgetenv("LANDESK_CLIPBOARD_CLIENT_PRIVATE_DISPLAY")) return 2;
        for (int n = 1; n < argc - 1; ++n) argv[n] = argv[n + 1];
        --argc; argv[argc] = nullptr;
        QApplication app(argc, argv);
        ClipboardClientTest test;
        return QTest::qExec(&test, argc, argv);
    }
    // Bootstrap before constructing QApplication or accessing any clipboard.
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    QProcess xvfb, tests;
    xvfb.start(QStringLiteral("Xvfb"), {"-displayfd", "1", "-screen", "0", "960x640x24", "-nolisten", "tcp", "-ac"});
    if (!xvfb.waitForStarted(3000) || !xvfb.waitForReadyRead(3000)) {
        QTextStream(stderr) << "Cannot start private Xvfb" << Qt::endl;
        stopProcess(xvfb); return 2;
    }
    const QByteArray number = xvfb.readLine().trimmed();
    bool valid = false; number.toUInt(&valid);
    if (!valid || number.isEmpty()) { stopProcess(xvfb); return 2; }
    auto env = QProcessEnvironment::systemEnvironment();
    const QString display = ":" + QString::fromLatin1(number);
    env.insert("DISPLAY", display);
    env.insert("LANDESK_CLIPBOARD_CLIENT_PRIVATE_DISPLAY", display);
    env.insert("LANDESK_CLIPBOARD_CLIENT_PRIVATE_DIRECTORY", directory.path());
    env.insert("QT_QPA_PLATFORM", "xcb"); env.insert("XDG_SESSION_TYPE", "x11");
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
    auto arguments = app.arguments().mid(1); arguments.prepend("--private-clipboard-client-child");
    tests.start(app.applicationFilePath(), arguments);
    int result = 2;
    if (tests.waitForStarted(3000) && tests.waitForFinished(60000) && tests.exitStatus() == QProcess::NormalExit)
        result = tests.exitCode();
    stopProcess(tests); stopProcess(xvfb);
    return result;
#endif
}

#include "clipboard_client_test.moc"
