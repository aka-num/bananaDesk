#include "app.h"
#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>
#include <memory>

#ifndef Q_OS_WIN
#include <X11/Xlib.h>
#endif

namespace {
struct Message { ld::Packet type; QByteArray payload; };

// An intentionally low-level authenticated peer can send requests the normal
// Client correctly suppresses. No test creates a Window or invokes an OS lock.
class Peer : public QObject {
public:
    QSslSocket socket{this};
    ld::Wire wire{&socket, ld::MaxPacket, this};
    QList<Message> messages;
    Peer() {
        connect(&wire, &ld::Wire::packet, this, [this](ld::Packet type, const QByteArray &payload) {
            messages.append({type, payload});
        });
    }
    void open(const ld::Invitation &invitation) {
        socket.setPeerVerifyMode(QSslSocket::VerifyNone);
        auto config = socket.sslConfiguration(); config.setProtocol(QSsl::TlsV1_2OrLater);
        socket.setSslConfiguration(config);
        socket.connectToHostEncrypted(invitation.host, invitation.port);
    }
    bool authenticate(const ld::Invitation &invitation) {
        return wire.send(ld::Packet::Auth, ld::json({{"v", 1}, {"token", invitation.token},
                         {"codecs", QJsonArray{"jpeg"}}, {"window", 1}}));
    }
    int count(ld::Packet type) const {
        int result = 0; for (const auto &message : messages) if (message.type == type) ++result;
        return result;
    }
    QByteArray last(ld::Packet type) const {
        for (auto it = messages.crbegin(); it != messages.crend(); ++it)
            if (it->type == type) return it->payload;
        return {};
    }
};

// This real TLS server models an older host by omitting login_screen from
// Welcome. It never captures a desktop or sends keyboard/mouse input.
class ProtocolHost : public QObject {
public:
    ld::Listener listener{this};
    ld::Identity identity;
    QSslSocket *socket = nullptr;
    ld::Wire *wire = nullptr;
    QJsonObject welcome{{"v", 1}, {"width", 640}, {"height", 360}, {"control", true},
                        {"codec", "jpeg"}, {"fps", 15}, {"window", 1}};
    QList<Message> received;
    bool authenticated = false;
    ProtocolHost() {
        connect(&listener, &ld::Listener::accepted, this, [this](qintptr descriptor) {
            socket = new QSslSocket(this);
            if (!socket->setSocketDescriptor(descriptor)) return;
            auto config = QSslConfiguration::defaultConfiguration();
            config.setProtocol(QSsl::TlsV1_2OrLater);
            config.setPeerVerifyMode(QSslSocket::VerifyNone);
            config.setLocalCertificate(identity.certificate); config.setPrivateKey(identity.key);
            socket->setSslConfiguration(config);
            wire = new ld::Wire(socket, 4096, socket);
            connect(wire, &ld::Wire::packet, this, [this](ld::Packet type, const QByteArray &payload) {
                received.append({type, payload});
                if (!authenticated) {
                    QJsonObject object;
                    if (type != ld::Packet::Auth || !ld::object(payload, object)
                        || object.value("token").toString() != identity.token) {
                        socket->abort(); return;
                    }
                    authenticated = true;
                    wire->send(ld::Packet::Welcome, ld::json(welcome));
                } else if (type == ld::Packet::Ping) wire->send(ld::Packet::Pong);
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
    int count(ld::Packet type) const {
        int result = 0; for (const auto &message : received) if (message.type == type) ++result;
        return result;
    }
};

bool hostHasPeer(const ld::Host &host) {
    for (const auto *socket : host.findChildren<QSslSocket *>())
        if (socket->state() != QAbstractSocket::UnconnectedState) return true;
    return false;
}
}

class UnlockProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY2(QSslSocket::supportsSsl(), "This protocol test requires the TLS backend");
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        QVERIFY(qEnvironmentVariable("DISPLAY").startsWith(':'));
        const QByteArray root = qgetenv("LANDESK_UNLOCK_PRIVATE_DIRECTORY");
        QVERIFY(!root.isEmpty());
        QVERIFY(qgetenv("DBUS_SESSION_BUS_ADDRESS").startsWith("unix:path=" + root + "/bus"));
    }

    void hostRejectsUnauthorizedWake_data() {
        QTest::addColumn<bool>("authenticate");
        QTest::addColumn<bool>("control");
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("before-authentication") << false << true << QByteArray();
        QTest::newRow("view-only") << true << false << QByteArray();
        QTest::newRow("non-empty-payload") << true << true << QByteArray("unexpected");
    }

    void hostRejectsUnauthorizedWake() {
        QFETCH(bool, authenticate); QFETCH(bool, control); QFETCH(QByteArray, payload);
        ld::Host host(nullptr); host.setLockOnDisconnect(false); host.configureVideo(15, "jpeg");
        QSignalSpy locks(&host, &ld::Host::desktopLockRequested);
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, control, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        Peer peer; peer.open(invitation);
        QTRY_VERIFY(peer.socket.isEncrypted());
        QCOMPARE(QString::fromLatin1(peer.socket.peerCertificate().digest(QCryptographicHash::Sha256).toHex()), invitation.fingerprint);
        if (authenticate) {
            QVERIFY(peer.authenticate(invitation));
            QTRY_COMPARE(peer.count(ld::Packet::Welcome), 1);
            QJsonObject welcome; QVERIFY(ld::object(peer.last(ld::Packet::Welcome), welcome));
            QCOMPARE(welcome.value("login_screen").toBool(), control);
        }
        QVERIFY(peer.wire.send(ld::Packet::Wake, payload));
        QTRY_COMPARE(peer.socket.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(peer.count(ld::Packet::Wake), 0);
        if (!authenticate) QCOMPARE(peer.count(ld::Packet::Image), 0);
        QVERIFY(host.running());
        QCOMPARE(locks.count(), 0);
        host.stop(); QCOMPARE(locks.count(), 0);
    }

    void linuxWakeKeepsSessionAndInvitation() {
        ld::Host host(nullptr); host.setLockOnDisconnect(false); host.configureVideo(15, "jpeg");
        QSignalSpy locks(&host, &ld::Host::desktopLockRequested);
        QString error; ld::Invitation invitation;
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, true, error), qPrintable(error));
        const QString original = host.invitation();
        QVERIFY(ld::Invitation::decode(original, invitation, error));
        for (int connection = 0; connection < 2; ++connection) {
            Peer peer; peer.open(invitation);
            QTRY_VERIFY(peer.socket.isEncrypted());
            QCOMPARE(QString::fromLatin1(peer.socket.peerCertificate().digest(QCryptographicHash::Sha256).toHex()), invitation.fingerprint);
            QVERIFY(peer.authenticate(invitation));
            QTRY_COMPARE(peer.count(ld::Packet::Welcome), 1);
            QJsonObject welcome; QVERIFY(ld::object(peer.last(ld::Packet::Welcome), welcome));
            QVERIFY(welcome.value("login_screen").isBool());
            QVERIFY(welcome.value("login_screen").toBool());
            QVERIFY(peer.wire.send(ld::Packet::Wake));
            QTRY_COMPARE(peer.count(ld::Packet::Wake), 1);
            QJsonObject result; QVERIFY(ld::object(peer.last(ld::Packet::Wake), result));
            QVERIFY(result.value("ok").isBool());
            QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
            // A rate-limited follow-up is a failure response, not a disconnection.
            QVERIFY(peer.wire.send(ld::Packet::Wake));
            QTRY_COMPARE(peer.count(ld::Packet::Wake), 2);
            QVERIFY(ld::object(peer.last(ld::Packet::Wake), result));
            QCOMPARE(result.value("ok").toBool(), false);
            QVERIFY(!result.value("error").toString().isEmpty());
            QVERIFY(peer.wire.send(ld::Packet::Ping));
            QTRY_COMPARE(peer.count(ld::Packet::Pong), 1);
            QCOMPARE(peer.socket.state(), QAbstractSocket::ConnectedState);
            QCOMPARE(host.invitation(), original);
            peer.socket.abort();
            QTRY_VERIFY(!hostHasPeer(host));
            QCOMPARE(host.invitation(), original);
        }
        QCOMPARE(locks.count(), 0);
        host.stop(); QCOMPARE(locks.count(), 0);
    }

    void clientRequiresAdvertisedCapability_data() {
        QTest::addColumn<int>("advertised"); // -1 models a host predating the field.
        QTest::addColumn<bool>("control");
        QTest::addColumn<bool>("expected");
        QTest::newRow("old-host-no-field") << -1 << true << false;
        QTest::newRow("capability-disabled") << 0 << true << false;
        QTest::newRow("capability-enabled") << 1 << true << true;
        QTest::newRow("view-only-despite-advertisement") << 1 << false << false;
    }

    void clientRequiresAdvertisedCapability() {
        QFETCH(int, advertised); QFETCH(bool, control); QFETCH(bool, expected);
        ProtocolHost server;
        server.welcome.insert("control", control);
        if (advertised >= 0) server.welcome.insert("login_screen", advertised == 1);
        QString error; QVERIFY2(server.start(error), qPrintable(error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability);
        QSignalSpy login(&client, &ld::Client::loginScreenCapability);
        client.wakeDesktop(); // A disconnected Client cannot produce a request.
        client.start(server.invitation());
        QTRY_COMPARE(ready.count(), 1);
        QTRY_VERIFY(!login.isEmpty());
        QCOMPARE(login.last()[0].toBool(), expected);
        client.wakeDesktop();
        client.release();
        // Release is a same-stream barrier: any Wake sent by the preceding call
        // must arrive first. No timing-only assertion is used for suppressed U.
        // Enabled wake also releases any held remote keys before sending U.
        QTRY_COMPARE(server.count(ld::Packet::Release), expected ? 2 : 1);
        QCOMPARE(server.count(ld::Packet::Wake), expected ? 1 : 0);
        for (const auto &message : server.received)
            if (message.type == ld::Packet::Wake) QVERIFY(message.payload.isEmpty());
        client.stop();
        QVERIFY(!login.last()[0].toBool());
    }

    void clientCoalescesPendingWakeAndAcceptsFailureReply() {
        ProtocolHost server; server.welcome.insert("login_screen", true);
        QString error; QVERIFY2(server.start(error), qPrintable(error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability), status(&client, &ld::Client::status);
        client.start(server.invitation());
        QTRY_COMPARE(ready.count(), 1);
        client.wakeDesktop(); client.wakeDesktop(); client.release();
        QTRY_COMPARE(server.count(ld::Packet::Release), 2);
        QCOMPARE(server.count(ld::Packet::Wake), 1);
        status.clear();
        QVERIFY(server.wire->send(ld::Packet::Wake, ld::json({{"ok", false}, {"error", "test denial"}})));
        QTRY_VERIFY(!status.isEmpty() && status.last()[0].toString().contains("test denial"));
        QCOMPARE(server.socket->state(), QAbstractSocket::ConnectedState);
        client.wakeDesktop(); client.release();
        QTRY_COMPARE(server.count(ld::Packet::Release), 4);
        QCOMPARE(server.count(ld::Packet::Wake), 2);
        QVERIFY(server.wire->send(ld::Packet::Wake, ld::json({{"ok", true}})));
        const int statusCount = status.count();
        QTRY_VERIFY(status.count() > statusCount);
        QCOMPARE(server.socket->state(), QAbstractSocket::ConnectedState);
        client.stop();
    }

    void clientCapabilityRevocationAndReconnectToOldHost() {
        ProtocolHost server; server.welcome.insert("login_screen", true);
        ProtocolHost oldServer;
        QString error; QVERIFY2(server.start(error), qPrintable(error));
        QVERIFY2(oldServer.start(error), qPrintable(error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability), login(&client, &ld::Client::loginScreenCapability);
        client.start(server.invitation());
        QTRY_COMPARE(ready.count(), 1); QVERIFY(login.last()[0].toBool());
        server.welcome.insert("login_screen", false);
        QVERIFY(server.wire->send(ld::Packet::Welcome, ld::json(server.welcome)));
        QTRY_VERIFY(!login.last()[0].toBool());
        client.wakeDesktop(); client.release();
        QTRY_COMPARE(server.count(ld::Packet::Release), 1);
        QCOMPARE(server.count(ld::Packet::Wake), 0);
        client.start(oldServer.invitation());
        QTRY_VERIFY(oldServer.authenticated);
        QTRY_COMPARE(ready.count(), 3);
        QVERIFY(!login.last()[0].toBool());
        client.wakeDesktop(); client.release();
        QTRY_COMPARE(oldServer.count(ld::Packet::Release), 1);
        QCOMPARE(oldServer.count(ld::Packet::Wake), 0);
        client.stop();
    }
};

namespace {
void stopProcess(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
    process.terminate();
    if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(3000); }
}
}

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    Q_UNUSED(argc); Q_UNUSED(argv);
    // The Host success case deliberately calls the Linux wake implementation.
    return 77;
#else
    const bool child = argc > 1 && QByteArray(argv[1]) == "--private-unlock-protocol-child";
    if (child) {
        const QByteArray root = qgetenv("LANDESK_UNLOCK_PRIVATE_DIRECTORY");
        if (root.isEmpty() || !qgetenv("DBUS_SESSION_BUS_ADDRESS").startsWith("unix:path=" + root + "/bus")) return 2;
        XInitThreads();
        // Remove the bootstrap-only argument before QtTest parses its arguments.
        for (int n = 1; n < argc - 1; ++n) argv[n] = argv[n + 1];
        --argc; argv[argc] = nullptr;
        QApplication app(argc, argv);
        UnlockProtocolTest test;
        return QTest::qExec(&test, argc, argv);
    }
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    QProcess xvfb, bus, tests;
    xvfb.start(QStringLiteral("Xvfb"), {"-displayfd", "1", "-screen", "0", "1280x800x24", "-nolisten", "tcp", "-ac"});
    if (!xvfb.waitForStarted(3000) || !xvfb.waitForReadyRead(3000)) {
        QTextStream(stderr) << "Cannot start private Xvfb: " << xvfb.errorString() << Qt::endl;
        stopProcess(xvfb); return 2;
    }
    const QByteArray display = xvfb.readLine().trimmed();
    bool validDisplay = false; display.toUInt(&validDisplay);
    if (!validDisplay || display.isEmpty()) { stopProcess(xvfb); return 2; }
    bus.start(QStringLiteral("dbus-daemon"), {"--session", "--nofork", "--print-address=1",
              "--address=unix:path=" + directory.path() + "/bus"});
    if (!bus.waitForStarted(3000) || !bus.waitForReadyRead(3000)) {
        stopProcess(bus); stopProcess(xvfb); return 2;
    }
    const QByteArray address = bus.readLine().trimmed();
    if (!address.startsWith("unix:path=" + directory.path().toUtf8() + "/bus")) {
        stopProcess(bus); stopProcess(xvfb); return 2;
    }
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("DISPLAY", ":" + QString::fromLatin1(display));
    env.insert("DBUS_SESSION_BUS_ADDRESS", QString::fromUtf8(address));
    env.insert("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=" + directory.path() + "/no-system-bus");
    env.insert("LANDESK_UNLOCK_PRIVATE_DIRECTORY", directory.path());
    env.insert("XDG_SESSION_TYPE", "x11"); env.insert("QT_QPA_PLATFORM", "xcb");
    env.insert("XDG_RUNTIME_DIR", directory.path());
    env.insert("QT_ACCESSIBILITY", "0"); env.insert("NO_AT_BRIDGE", "1");
    env.remove("SESSION_MANAGER"); env.remove("AT_SPI_BUS_ADDRESS");
    env.remove("XAUTHORITY"); env.remove("QT_SCALE_FACTOR"); env.remove("QT_AUTO_SCREEN_SCALE_FACTOR");
    tests.setProcessEnvironment(env); tests.setProcessChannelMode(QProcess::ForwardedChannels);
    auto arguments = app.arguments().mid(1); arguments.prepend("--private-unlock-protocol-child");
    tests.start(app.applicationFilePath(), arguments);
    int result = 2;
    if (tests.waitForStarted(3000) && tests.waitForFinished(120000) && tests.exitStatus() == QProcess::NormalExit)
        result = tests.exitCode();
    stopProcess(tests); stopProcess(bus); stopProcess(xvfb);
    return result;
#endif
}

#include "unlock_protocol_test.moc"
