#include "desktop_lock.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>
#include <memory>

// This test always owns a private session bus. It never contacts the user's
// real session bus, and the mock service never calls a desktop locking API.
class MockScreenSaver : public QDBusVirtualObject {
public:
    MockScreenSaver(QString service, QString result) : service_(std::move(service)), result_(std::move(result)) {}
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (message.interface() != service_ || message.member() != QLatin1String("Lock")) return false;
        QTextStream(stdout) << "LOCK " << service_ << Qt::endl;
        if (result_ == QLatin1String("timeout")) return true;
        if (result_ == QLatin1String("denied"))
            bus.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"), QStringLiteral("test denial")));
        else
            bus.send(message.createReply());
        return true;
    }
private:
    QString service_, result_;
};

class DesktopLockTest : public QObject {
    Q_OBJECT
private:
    QTemporaryDir directory_;
    QProcess daemon_;
    std::unique_ptr<QProcess> mock_;
    QByteArray mockOutput_;

    void startMock(const QString &service, const QString &result = QStringLiteral("ok")) {
        mock_ = std::make_unique<QProcess>();
        mockOutput_.clear();
        mock_->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--lock-mock"), service, result});
        QVERIFY2(mock_->waitForStarted(3000), qPrintable(mock_->errorString()));
        QTRY_VERIFY_WITH_TIMEOUT((mockOutput_ += mock_->readAllStandardOutput()).contains("READY\n"), 3000);
    }

private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        daemon_.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"), QStringLiteral("--nofork"),
            QStringLiteral("--print-address=1"), QStringLiteral("--address=unix:path=%1/bus").arg(directory_.path())});
        QVERIFY2(daemon_.waitForStarted(3000), qPrintable(daemon_.errorString()));
        QVERIFY(daemon_.waitForReadyRead(3000));
        const QByteArray address = daemon_.readLine().trimmed();
        // Abort all tests unless our daemon supplied an address in our private
        // directory. An existing desktop bus address is never accepted here.
        QVERIFY2(address.startsWith("unix:path=" + directory_.path().toUtf8() + "/"), address.constData());
        qputenv("DBUS_SESSION_BUS_ADDRESS", address);
        QVERIFY(QDBusConnection::sessionBus().isConnected());
    }

    void unavailableService() {
        QString error;
        QVERIFY(!ld::lockDesktop(error));
        QVERIFY(error.contains(QStringLiteral("没有可用的锁屏服务")));
    }

    void gnomeRequestAccepted() {
        startMock(QStringLiteral("org.gnome.ScreenSaver"));
        QString error = QStringLiteral("old error");
        QVERIFY2(ld::lockDesktop(error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QTRY_VERIFY((mockOutput_ += mock_->readAllStandardOutput()).contains("LOCK org.gnome.ScreenSaver\n"));
    }

    void freedesktopFallbackAccepted() {
        startMock(QStringLiteral("org.freedesktop.ScreenSaver"));
        QString error;
        QVERIFY2(ld::lockDesktop(error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QTRY_VERIFY((mockOutput_ += mock_->readAllStandardOutput()).contains("LOCK org.freedesktop.ScreenSaver\n"));
    }

    void serviceFailureReported() {
        startMock(QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("denied"));
        QString error;
        QVERIFY(!ld::lockDesktop(error));
        QVERIFY(error.contains(QStringLiteral("test denial")));
        QVERIFY(error.contains(QStringLiteral("AccessDenied")));
    }

    void unresponsiveServiceHasBoundedWait() {
        startMock(QStringLiteral("org.gnome.ScreenSaver"), QStringLiteral("timeout"));
        QElapsedTimer timer; timer.start();
        QString error;
        QVERIFY(!ld::lockDesktop(error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(timer.elapsed() < 2500, qPrintable(QStringLiteral("Lock call took %1 ms").arg(timer.elapsed())));
    }

    void cleanup() {
        if (mock_) {
            mock_->terminate();
            if (!mock_->waitForFinished(3000)) { mock_->kill(); mock_->waitForFinished(3000); }
            mock_.reset();
        }
    }

    void cleanupTestCase() {
        daemon_.terminate();
        if (!daemon_.waitForFinished(3000)) { daemon_.kill(); daemon_.waitForFinished(3000); }
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == QLatin1String("--lock-mock")) {
        // Mock subprocesses inherit only the isolated bus selected by the test.
        const QString service = args[2];
        if (service != QLatin1String("org.gnome.ScreenSaver") && service != QLatin1String("org.freedesktop.ScreenSaver")) return 2;
        MockScreenSaver mock(service, args[3]);
        auto bus = QDBusConnection::sessionBus();
        const QString path = service == QLatin1String("org.gnome.ScreenSaver")
            ? QStringLiteral("/org/gnome/ScreenSaver") : QStringLiteral("/org/freedesktop/ScreenSaver");
        if (!bus.registerVirtualObject(path, &mock) || !bus.registerService(service)) return 3;
        QTextStream(stdout) << "READY" << Qt::endl;
        return app.exec();
    }
    DesktopLockTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "desktop_lock_test.moc"
