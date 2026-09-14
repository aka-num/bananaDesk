#include "app.h"
#include <QtTest>
#include <QSignalSpy>

class InteractionTest : public QObject {
    Q_OBJECT
private slots:
    void authenticatedSessionLockPolicy() {
#ifndef Q_OS_WIN
        if (QGuiApplication::platformName() != "xcb") QSKIP("Session policy test requires isolated Xvfb");
#endif
        // Use Host directly: observe policy signals, never execute an OS lock.
        ld::Host host(nullptr); ld::Client client(nullptr);
        QSignalSpy locks(&host, &ld::Host::desktopLockRequested);
        QSignalSpy ready(&client, &ld::Client::capability);
        QSignalSpy status(&host, &ld::Host::status);
        QString error; ld::Invitation invitation;
        host.configureVideo(15, "jpeg");
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
        host.stop(); QCOMPARE(locks.count(), 0);
        for (const bool enabled : {true, false}) {
            host.setLockOnDisconnect(enabled);
            QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
            QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
            ready.clear(); locks.clear(); client.start(invitation);
            QTRY_COMPARE(ready.count(), 1); // Authenticated view-only sessions count too.
            client.stop();
            QTRY_VERIFY(!status.isEmpty() && (status.last()[0].toString().contains(QStringLiteral("结束")) || status.last()[0].toString().contains(QStringLiteral("断开"))));
            QCOMPARE(locks.count(), enabled ? 1 : 0);
            host.stop(); QCOMPARE(locks.count(), enabled ? 1 : 0);
        }
        host.setLockOnDisconnect(true); locks.clear();
        QVERIFY2(host.start(QHostAddress::LocalHost, 0, false, error), qPrintable(error));
        QVERIFY(ld::Invitation::decode(host.invitation(), invitation, error));
        auto wrong = invitation; wrong.token = QString(48, '0');
        status.clear(); client.start(wrong);
        QTRY_VERIFY(!status.isEmpty() && status.last()[0].toString().contains(QStringLiteral("验证失败")));
        QCOMPARE(locks.count(), 0);
        client.stop(); ready.clear(); client.start(invitation);
        QTRY_COMPARE(ready.count(), 1);
        host.stop(); QCOMPARE(locks.count(), 1);
        host.stop(); client.stop(); QCOMPARE(locks.count(), 1);
    }
    void controllerWindowCanShrink() {
        ld::ControlWindow window; window.show();
        auto *viewer = window.viewer();
        // The independent viewer window must shrink even with live statistics.
        window.setStatistics(QStringLiteral("H264 · 目标 60 fps · 解码 60.0 fps · 绘制 60.0 fps · 12.00 Mbps"));
        for (const QSize size : {QSize(800, 520), QSize(640, 480), QSize(1200, 800), QSize(640, 480)}) {
            window.resize(size);
            QTRY_COMPARE(window.size(), size);
            QVERIFY(viewer->width() > 0 && viewer->height() > 0);
            QVERIFY(window.centralWidget()->rect().contains(viewer->mapTo(window.centralWidget(), viewer->rect().bottomRight())));
        }
        ld::Window settings; settings.show();
        auto *tabs = settings.findChild<QTabWidget *>(); QVERIFY(tabs);
        settings.resize(640, 480);
        tabs->setCurrentIndex(0); QCoreApplication::processEvents();
        QCOMPARE(settings.size(), QSize(640, 480));
        tabs->setCurrentIndex(1); QCoreApplication::processEvents();
        QCOMPARE(settings.size(), QSize(640, 480));
        QVERIFY(!settings.isAncestorOf(viewer));
        QSignalSpy disconnects(&window, &ld::ControlWindow::disconnectRequested);
        window.close(); QCOMPARE(disconnects.count(), 1);
    }
    void coordinatesAfterResize() {
        ld::Viewer viewer; viewer.show();
        QImage frame(1600, 900, QImage::Format_RGB32); frame.fill(Qt::blue);
        viewer.setFrame(frame); viewer.setControl(true);
        QSignalSpy inputs(&viewer, &ld::Viewer::input);
        for (const QSize size : {QSize(640, 360), QSize(360, 640), QSize(1000, 400)}) {
            viewer.resize(size); QCoreApplication::processEvents();
            QCOMPARE(viewer.size(), size);
            inputs.clear();
            QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(size.width()/2, size.height()/2));
            QCOMPARE(inputs.count(), 2);
            auto event = qvariant_cast<QJsonObject>(inputs[0][0]);
            QVERIFY(std::abs(event["x"].toDouble() - 0.5) < 0.01);
            QVERIFY(std::abs(event["y"].toDouble() - 0.5) < 0.01);
            inputs.clear();
            QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(size.width()/4, size.height()/2));
            QCOMPARE(inputs.count(), 2);
            event = qvariant_cast<QJsonObject>(inputs[0][0]);
            // A wide viewport has bars at the sides; visible image width is 400 * 16/9.
            const double expectedX = size == QSize(1000, 400) ? 0.14865 : 0.25;
            QVERIFY(std::abs(event["x"].toDouble() - expectedX) < 0.01);
            if (size != QSize(640, 360)) {
                inputs.clear(); QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(2, 2));
                QCOMPARE(inputs.count(), 0);
            }
            inputs.clear();
            QTest::mousePress(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(size.width()/2, size.height()/2));
            QTest::mouseRelease(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(size.width()+20, size.height()+20));
            QCOMPARE(inputs.count(), 2);
            event = qvariant_cast<QJsonObject>(inputs[1][0]);
            QCOMPARE(event["down"].toBool(), false);
            QCOMPARE(event["x"].toDouble(), 1.0); QCOMPARE(event["y"].toDouble(), 1.0);
        }
    }
    void cancelledVideoJobs() {
        ld::CaptureWorker capture;
        ld::EncodeWorker encoder;
        ld::DecodeWorker decoder;
        QSignalSpy captureErrors(&capture, &ld::CaptureWorker::failed);
        QSignalSpy encodeErrors(&encoder, &ld::EncodeWorker::failed);
        QSignalSpy decodeErrors(&decoder, &ld::DecodeWorker::failed);
        capture.activate(2); encoder.activate(2); decoder.activate(2);
        capture.produce(1);
        encoder.encode(1, QImage(), QRect(), "jpeg", 60, 0, "test");
        decoder.decode(1, "jpeg", QByteArray("invalid"));
        QCOMPARE(captureErrors.count(), 0); QCOMPARE(encodeErrors.count(), 0); QCOMPARE(decodeErrors.count(), 0);
        // Initial generation zero must initialize safely rather than dereference null.
        encoder.activate(0); decoder.activate(0);
        encoder.encode(0, QImage(), QRect(), "jpeg", 60, 0, "test");
        decoder.decode(0, "jpeg", QByteArray("invalid"));
        QCOMPARE(encodeErrors.count(), 1); QCOMPARE(decodeErrors.count(), 1);
    }
    void invitationValidation() {
        ld::Invitation source{"192.168.1.25", 24832, QString(64, 'a'), QString(48, 'b')}, result;
        QString error;
        QVERIFY(ld::Invitation::decode(source.encode(), result, error));
        QCOMPARE(result.host, source.host); QCOMPARE(result.token, source.token);
        source.host = "0.0.0.0"; QVERIFY(!ld::Invitation::decode(source.encode(), result, error));
        source.host = "192.168.1.25"; source.fingerprint = "bad";
        QVERIFY(!ld::Invitation::decode(source.encode(), result, error));
        QVERIFY(!ld::Invitation::decode("landesk1:garbage", result, error));
        QVERIFY(!ld::equalSecret("ab", "ac")); QVERIFY(!ld::equalSecret("a", "ab"));
    }
    void letterboxCoordinatesAndFocus() {
        ld::Viewer viewer; viewer.resize(800, 600); viewer.show();
        QImage frame(1600, 900, QImage::Format_RGB32); frame.fill(Qt::blue);
        viewer.setFrame(frame); viewer.setControl(true);
        QSignalSpy inputs(&viewer, &ld::Viewer::input), releases(&viewer, &ld::Viewer::releaseKeys);
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(400, 20));
        QCOMPARE(inputs.count(), 0); // Letterbox is never mapped into the remote screen.
        QTest::mouseClick(&viewer, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
        QCOMPARE(inputs.count(), 2);
        auto object = qvariant_cast<QJsonObject>(inputs[0][0]);
        QVERIFY(std::abs(object["x"].toDouble() - 0.5) < 0.01);
        QVERIFY(std::abs(object["y"].toDouble() - 0.5) < 0.01);
        inputs.clear();
        QTest::keyClick(&viewer, Qt::Key_Tab);
        QCOMPARE(inputs.count(), 2); // Tab must reach the remote machine, not switch local widgets.
        QCOMPARE(qvariant_cast<QJsonObject>(inputs[0][0])["key"].toInt(), int(Qt::Key_Tab));
        inputs.clear();
        QTest::keyClick(&viewer, Qt::Key_A);
        QCOMPARE(inputs.count(), 2);
        viewer.setControl(false); QVERIFY(releases.count() > 0);
        inputs.clear(); QTest::keyClick(&viewer, Qt::Key_B); QCOMPARE(inputs.count(), 0);
    }
};
QTEST_MAIN(InteractionTest)
#include "viewer_test.moc"
