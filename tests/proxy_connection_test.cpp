#include "app.h"
#include <QApplication>
#include <QNetworkProxy>
#include <QNetworkInterface>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {
QHostAddress localTestAddress() {
    // Qt bypasses application proxies for loopback addresses, so 127.0.0.1
    // would let the broken implementation pass. Bind an existing local IPv4
    // on an ephemeral port; all captured pixels still belong to private Xvfb.
    for (const auto &address : QNetworkInterface::allAddresses())
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()
            && address != QHostAddress::AnyIPv4 && !address.isMulticast()) return address;
    return {};
}
// Only this test process is changed. Never writes an OS proxy setting, and
// restores the original application proxy even when a QVERIFY returns early.
class ScopedApplicationProxy {
public:
    explicit ScopedApplicationProxy(QNetworkProxy::ProxyType type)
        : original_(QNetworkProxy::applicationProxy()) {
        trap_.setProxy(QNetworkProxy::NoProxy);
        QObject::connect(&trap_, &QTcpServer::newConnection, &trap_, [this] {
            while (trap_.hasPendingConnections()) {
                auto *socket = trap_.nextPendingConnection(); ++connections_;
                socket->abort(); socket->deleteLater();
            }
        });
        valid_ = trap_.listen(QHostAddress::LocalHost, 0);
        if (valid_) QNetworkProxy::setApplicationProxy(
            QNetworkProxy(type, QStringLiteral("127.0.0.1"), trap_.serverPort()));
    }
    ~ScopedApplicationProxy() { QNetworkProxy::setApplicationProxy(original_); }
    bool valid() const { return valid_; }
    int connections() const { return connections_; }
private:
    QNetworkProxy original_;
    QTcpServer trap_;
    int connections_ = 0;
    bool valid_ = false;
};

// Explicitly direct test fixture isolates the production Client behavior.
// It has no desktop and intentionally grants viewing only, without clipboard.
class DirectProtocolHost : public QObject {
public:
    ld::Listener listener{this};
    ld::Identity identity;
    QSslSocket *socket = nullptr;
    ld::Wire *wire = nullptr;
    bool authenticated = false, failed = false;
    int releases = 0;
    DirectProtocolHost() {
        listener.setProxy(QNetworkProxy::NoProxy);
        connect(&listener, &ld::Listener::accepted, this, [this](qintptr descriptor) {
            socket = new QSslSocket(this); socket->setProxy(QNetworkProxy::NoProxy);
            if (!socket->setSocketDescriptor(descriptor)) { failed = true; return; }
            auto config = QSslConfiguration::defaultConfiguration();
            config.setProtocol(QSsl::TlsV1_2OrLater);
            config.setPeerVerifyMode(QSslSocket::VerifyNone);
            config.setLocalCertificate(identity.certificate); config.setPrivateKey(identity.key);
            socket->setSslConfiguration(config);
            wire = new ld::Wire(socket, 4096, socket);
            connect(wire, &ld::Wire::failure, this, [this] { failed = true; });
            connect(wire, &ld::Wire::packet, this, [this](ld::Packet type, const QByteArray &payload) {
                if (!authenticated) {
                    QJsonObject auth;
                    if (type != ld::Packet::Auth || !ld::object(payload, auth)
                        || auth.value("token").toString() != identity.token) { socket->abort(); return; }
                    authenticated = true;
                    const QJsonObject welcome{{"v", 1}, {"width", 640}, {"height", 360},
                        {"control", false}, {"codec", "jpeg"}, {"fps", 15}, {"window", 1}};
                    if (!wire->send(ld::Packet::Welcome, ld::json(welcome))) failed = true;
                } else if (type == ld::Packet::Release) ++releases;
                else if (type == ld::Packet::Ping && !wire->send(ld::Packet::Pong)) failed = true;
            });
            socket->startServerEncryption();
        });
    }
    bool start(QString &error) {
        if (!identity.create(error)) return false;
        if (!listener.listen(localTestAddress(), 0)) { error = listener.errorString(); return false; }
        return true;
    }
    ld::Invitation invitation() const {
        return {listener.serverAddress().toString(), listener.serverPort(), identity.fingerprint, identity.token};
    }
};

void proxyRows() {
    QTest::addColumn<int>("proxyType");
    QTest::newRow("http-caching-invalid-for-tcp") << int(QNetworkProxy::HttpCachingProxy);
    QTest::newRow("rejecting-http-proxy") << int(QNetworkProxy::HttpProxy);
    QTest::newRow("rejecting-socks5-proxy") << int(QNetworkProxy::Socks5Proxy);
}
QString messages(const QSignalSpy &status) {
    QStringList result;
    for (const auto &item : status) result.append(item.front().toString());
    return result.join('\n');
}
void stopProcess(QProcess &process) {
    if (process.state() == QProcess::NotRunning) return;
    process.terminate();
    if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(3000); }
}
}

class ProxyConnectionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(QSslSocket::supportsSsl());
        QVERIFY2(!localTestAddress().isNull(), "A non-loopback local IPv4 address is needed to exercise proxy inheritance");
        QCOMPARE(qgetenv("LANDESK_ISOLATED_TEST"), QByteArray("1"));
#ifndef Q_OS_WIN
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        QVERIFY(!qgetenv("LANDESK_PROXY_PRIVATE_DIRECTORY").isEmpty());
        QCOMPARE(qgetenv("DISPLAY"), qgetenv("LANDESK_PROXY_PRIVATE_DISPLAY"));
#endif
        originalProxy_ = QNetworkProxy::applicationProxy();
    }
    void cleanup() {
        const auto actual = QNetworkProxy::applicationProxy();
        QCOMPARE(actual.type(), originalProxy_.type());
        QCOMPARE(actual.hostName(), originalProxy_.hostName());
        QCOMPARE(actual.port(), originalProxy_.port());
        QCOMPARE(actual.user(), originalProxy_.user());
        QCOMPARE(actual.password(), originalProxy_.password());
    }

    void clientConnectsDirectlyDespiteApplicationProxy_data() { proxyRows(); }
    void clientConnectsDirectlyDespiteApplicationProxy() {
        QFETCH(int, proxyType);
        ScopedApplicationProxy configured{QNetworkProxy::ProxyType(proxyType)};
        QVERIFY(configured.valid());
        DirectProtocolHost server;
        QString error; QVERIFY2(server.start(error), qPrintable(error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability), status(&client, &ld::Client::status);
        QSignalSpy disconnected(&client, &ld::Client::disconnected);
        client.start(server.invitation());
        QTRY_VERIFY2_WITH_TIMEOUT(ready.count() == 1, qPrintable(messages(status)), 5000);
        QVERIFY(server.authenticated && !server.failed);
        QVERIFY(!ready.last()[0].toBool());
        disconnected.clear(); client.release();
        QTRY_COMPARE_WITH_TIMEOUT(server.releases, 1, 3000);
        QCOMPARE(disconnected.count(), 0);
        QCOMPARE(configured.connections(), 0);
        QCOMPARE(server.socket->state(), QAbstractSocket::ConnectedState);
        client.stop();
    }

    void realHostAndClientStreamDirectlyDespiteApplicationProxy_data() { proxyRows(); }
    void realHostAndClientStreamDirectlyDespiteApplicationProxy() {
        QFETCH(int, proxyType);
        ScopedApplicationProxy configured{QNetworkProxy::ProxyType(proxyType)};
        QVERIFY(configured.valid());
        ld::Host host(nullptr); host.setLockOnDisconnect(false); host.configureVideo(15, "jpeg");
        QSignalSpy locks(&host, &ld::Host::desktopLockRequested), hostStatus(&host, &ld::Host::status);
        QString error;
        // Viewing only keeps clipboard and input disabled on both endpoints.
        QVERIFY2(host.start(localTestAddress(), 0, false, error), qPrintable(error));
        ld::Invitation invitation;
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        ld::Client client(nullptr);
        QSignalSpy ready(&client, &ld::Client::capability), frames(&client, &ld::Client::frame);
        QSignalSpy status(&client, &ld::Client::status), disconnected(&client, &ld::Client::disconnected);
        client.start(invitation);
        QTRY_VERIFY2_WITH_TIMEOUT(!frames.isEmpty(), qPrintable(messages(status) + '\n' + messages(hostStatus)), 8000);
        QVERIFY(!ready.isEmpty() && !ready.last()[0].toBool());
        disconnected.clear();
        const int previousFrames = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > previousFrames, 3000);
        QCOMPARE(disconnected.count(), 0);
        QCOMPARE(configured.connections(), 0);
        client.stop(); host.stop(); QCOMPARE(locks.count(), 0);
    }
private:
    QNetworkProxy originalProxy_;
};

int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    // The caller must provide a disposable Windows/Wine desktop. Refuse normal
    // execution before QApplication or the capture-capable Host is created.
    if (qgetenv("LANDESK_ISOLATED_TEST") != "1") return 77;
    QApplication app(argc, argv); ProxyConnectionTest test;
    return QTest::qExec(&test, argc, argv);
#else
    const bool child = argc > 1 && QByteArray(argv[1]) == "--private-proxy-test-child";
    if (child) {
        if (qgetenv("LANDESK_PROXY_PRIVATE_DIRECTORY").isEmpty()
            || !qgetenv("DISPLAY").startsWith(':')
            || qgetenv("DISPLAY") != qgetenv("LANDESK_PROXY_PRIVATE_DISPLAY")) return 2;
        for (int n = 1; n < argc - 1; ++n) argv[n] = argv[n + 1];
        --argc; argv[argc] = nullptr;
        QApplication app(argc, argv); ProxyConnectionTest test;
        return QTest::qExec(&test, argc, argv);
    }
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
    env.insert("DISPLAY", display); env.insert("LANDESK_PROXY_PRIVATE_DISPLAY", display);
    env.insert("LANDESK_PROXY_PRIVATE_DIRECTORY", directory.path());
    env.insert("LANDESK_ISOLATED_TEST", "1");
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
    auto arguments = app.arguments().mid(1); arguments.prepend("--private-proxy-test-child");
    tests.start(app.applicationFilePath(), arguments);
    int result = 2;
    if (tests.waitForStarted(3000) && tests.waitForFinished(60000) && tests.exitStatus() == QProcess::NormalExit)
        result = tests.exitCode();
    stopProcess(tests); stopProcess(xvfb);
    return result;
#endif
}

#include "proxy_connection_test.moc"
