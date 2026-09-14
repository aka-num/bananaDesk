#include "app.h"
#include <QtTest>
#include <QSignalSpy>
#include <QLayout>
#include <QKeyEvent>

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
    void cleanViewFillsClientAreaAndStaysCleanDuringUpdates() {
        ld::ControlWindow window; const QSize normalSize(800, 520); window.resize(normalSize);
        auto *viewer = window.viewer(); auto *root = window.centralWidget();
        const QMargins originalMargins = root->layout()->contentsMargins();
        const int originalSpacing = root->layout()->spacing();
        QImage frame(1600, 900, QImage::Format_RGB32); frame.fill(Qt::blue);
        viewer->setFrame(frame); viewer->setControl(true);
        window.setStatus("connected status"); window.setStatistics("60 fps test statistics");
        window.setLoginScreenCapability(true); window.setFileCapability(true);
        QSignalSpy disconnects(&window, &ld::ControlWindow::disconnectRequested);
        QSignalSpy changes(&window, &ld::ControlWindow::cleanViewChanged);
        QSignalSpy releases(&window, &ld::ControlWindow::releaseRequested);
        QSignalSpy inputs(viewer, &ld::Viewer::input);
        QVERIFY(!window.cleanView());
        window.setCleanView(true); window.setCleanView(true);
        QCoreApplication::processEvents();
        // Restoring a saved preference must not display an idle control window.
        QVERIFY(!window.isVisible());
        QVERIFY(window.cleanView()); QCOMPARE(changes.count(), 1);
        QVERIFY(changes.last()[0].toBool()); QVERIFY(!releases.isEmpty());
        QCOMPARE(disconnects.count(), 0);
        window.show(); QTRY_VERIFY(window.isVisible()); QTRY_VERIFY(window.isFullScreen());
        for (int update = 0; update < 2; ++update) {
            window.setStatus(QStringLiteral("updated status %1").arg(update));
            window.setStatistics(QStringLiteral("updated statistics %1").arg(update));
            window.setLoginScreenCapability(false); window.setFileCapability(false);
            window.setLoginScreenCapability(true); window.setFileCapability(true);
            QCoreApplication::processEvents();
            QVERIFY(window.isFullScreen());
            QTRY_COMPARE(root->geometry(), window.rect());
            QTRY_COMPARE(QRect(viewer->mapTo(root, QPoint()), viewer->size()), root->rect());
            QCOMPARE(root->layout()->contentsMargins(), QMargins());
            QCOMPARE(root->layout()->spacing(), 0);
            QVERIFY(viewer->isVisible());
            for (auto *widget : root->findChildren<QWidget *>())
                if (widget != viewer && !viewer->isAncestorOf(widget)) QVERIFY(!widget->isVisible());
            // The image still maps to remote coordinates after controls and
            // margins disappear; black bars belong to the aspect-ratio view.
            inputs.clear();
            QTest::mouseClick(viewer, Qt::LeftButton, Qt::NoModifier, viewer->rect().center());
            QCOMPARE(inputs.count(), 2);
            const auto event = qvariant_cast<QJsonObject>(inputs.front()[0]);
            QVERIFY(std::abs(event["x"].toDouble() - 0.5) < 0.01);
            QVERIFY(std::abs(event["y"].toDouble() - 0.5) < 0.01);
        }
        window.setCleanView(false); QTest::qWait(100);
        QVERIFY(!window.cleanView()); QCOMPARE(changes.count(), 2);
        QTRY_VERIFY(!window.isFullScreen()); QVERIFY(!window.isMaximized());
        QTRY_COMPARE(window.size(), normalSize);
        QVERIFY(!changes.last()[0].toBool()); QCOMPARE(disconnects.count(), 0);
        QCOMPARE(root->layout()->contentsMargins(), originalMargins);
        QCOMPARE(root->layout()->spacing(), originalSpacing);
        for (auto *button : root->findChildren<QPushButton *>()) QVERIFY(button->isVisible());
        for (auto *label : root->findChildren<QLabel *>()) QVERIFY(label->isVisible());
        // Fullscreen also preserves maximized state and remains selected when
        // a finished session hides the window until the next connection.
        // Give a real window manager time to acknowledge transitions; checking
        // only Qt's immediate requested state misses later WM state overrides.
        window.showMaximized(); QTest::qWait(100); QTRY_VERIFY(window.isMaximized());
        window.setCleanView(true); QTest::qWait(100); QTRY_VERIFY(window.isFullScreen());
        window.sessionEnded(); QTest::qWait(100); QVERIFY(!window.isVisible()); QVERIFY(window.cleanView());
        window.show(); QTest::qWait(100); QTRY_VERIFY(window.isVisible()); QTRY_VERIFY(window.isFullScreen());
        for (auto *widget : root->findChildren<QWidget *>())
            if (widget != viewer && !viewer->isAncestorOf(widget)) QVERIFY(!widget->isVisible());
        window.setCleanView(false); QTest::qWait(100); QTRY_VERIFY(!window.isFullScreen());
        QTRY_VERIFY(window.isMaximized());
        window.showNormal(); QTest::qWait(100); QTRY_COMPARE(window.size(), normalSize);
        window.showMinimized(); QTest::qWait(100); QTRY_VERIFY(window.isMinimized());
        window.setCleanView(true); QTest::qWait(100); QTRY_VERIFY(window.isFullScreen()); QVERIFY(!window.isMinimized());
        window.setCleanView(false); QTest::qWait(100); QTRY_VERIFY(!window.isFullScreen()); QVERIFY(!window.isMinimized());
        QTRY_COMPARE(window.size(), normalSize);
        window.hide(); window.setCleanView(true); window.setCleanView(false);
        QVERIFY(!window.isVisible());
        QCOMPARE(disconnects.count(), 0);
    }
    void cleanViewShortcutRestoresControlsAndCloseStillDisconnects() {
        ld::ControlWindow window; window.resize(800, 520);
        auto *viewer = window.viewer();
        QImage frame(1600, 900, QImage::Format_RGB32); frame.fill(Qt::blue);
        viewer->setFrame(frame); viewer->setControl(true);
        window.show(); window.activateWindow(); viewer->setFocus();
        QTRY_VERIFY(viewer->hasFocus());
        window.setCleanView(true); QTest::qWait(100);
        QTRY_VERIFY(window.isFullScreen()); QTRY_VERIFY(viewer->hasFocus());
        QSignalSpy disconnects(&window, &ld::ControlWindow::disconnectRequested);
        QSignalSpy releases(&window, &ld::ControlWindow::releaseRequested);
        QSignalSpy changes(&window, &ld::ControlWindow::cleanViewChanged);
        QSignalSpy inputs(viewer, &ld::Viewer::input);
        QSet<int> remoteHeld;
        connect(viewer, &ld::Viewer::input, &window, [&](const QJsonObject &event) {
            if (event["kind"].toString() != "key") return;
            if (event["down"].toBool()) remoteHeld.insert(event["key"].toInt());
            else remoteHeld.remove(event["key"].toInt());
        });
        connect(&window, &ld::ControlWindow::releaseRequested, &window, [&] { remoteHeld.clear(); });
        connect(viewer, &ld::Viewer::releaseKeys, &window, [&] { remoteHeld.clear(); });
        // A modifier may reach the remote side before Qt recognizes the full
        // shortcut. Switching modes must release it without disconnecting.
        QTest::keyPress(viewer, Qt::Key_Control);
        QVERIFY(remoteHeld.contains(int(Qt::Key_Control)));
        const auto modifiers = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
        QTest::keyClick(viewer, Qt::Key_H, modifiers);
        QTest::qWait(100);
        QTRY_VERIFY(!window.cleanView());
        QTRY_VERIFY(!window.isFullScreen()); QTRY_COMPARE(window.size(), QSize(800, 520));
        QVERIFY(!releases.isEmpty()); QVERIFY(remoteHeld.isEmpty());
        QTest::keyRelease(viewer, Qt::Key_Control);
        QCOMPARE(changes.count(), 1); QVERIFY(!changes.last()[0].toBool());
        QCOMPARE(disconnects.count(), 0);
        for (const auto &entry : inputs) {
            const auto event = qvariant_cast<QJsonObject>(entry[0]);
            QVERIFY(!(event["kind"].toString() == "key" && event["key"].toInt() == Qt::Key_H));
        }
        viewer->setFocus(); QTRY_VERIFY(viewer->hasFocus());
        inputs.clear(); QTest::keyClick(viewer, Qt::Key_H);
        QCOMPARE(inputs.count(), 2);
        for (int i = 0; i < inputs.count(); ++i) {
            const auto event = qvariant_cast<QJsonObject>(inputs[i][0]);
            QCOMPARE(event["kind"].toString(), QStringLiteral("key"));
            QCOMPARE(event["key"].toInt(), int(Qt::Key_H));
            QCOMPARE(event["down"].toBool(), i == 0);
        }
        QVERIFY(remoteHeld.isEmpty());
        // Adding the shortcut modifiers while a normal H is already repeating
        // must not consume the H-up paired with the previously forwarded down.
        inputs.clear(); QTest::keyPress(viewer, Qt::Key_H);
        QVERIFY(remoteHeld.contains(int(Qt::Key_H)));
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_H, modifiers, QString(), true);
        QCoreApplication::sendEvent(viewer, &repeat);
        QTest::keyRelease(viewer, Qt::Key_H);
        QCOMPARE(inputs.count(), 2); QVERIFY(remoteHeld.isEmpty());
        QCOMPARE(changes.count(), 1); QVERIFY(!window.cleanView());
        inputs.clear();
        QTest::keyClick(viewer, Qt::Key_H, modifiers);
        QTRY_VERIFY(window.cleanView());
        QTRY_VERIFY(window.isFullScreen());
        QCOMPARE(changes.count(), 2); QVERIFY(changes.last()[0].toBool());
        QCOMPARE(disconnects.count(), 0); QVERIFY(remoteHeld.isEmpty());
        for (const auto &entry : inputs) {
            const auto event = qvariant_cast<QJsonObject>(entry[0]);
            QVERIFY(!(event["kind"].toString() == "key" && event["key"].toInt() == Qt::Key_H));
        }
        inputs.clear(); QTest::keyPress(viewer, Qt::Key_Control);
        QVERIFY(remoteHeld.contains(int(Qt::Key_Control)));
        const int previousReleases = releases.count();
        QTest::keyClick(viewer, Qt::Key_D, modifiers);
        QCOMPARE(disconnects.count(), 1); QVERIFY(releases.count() > previousReleases);
        QVERIFY(remoteHeld.isEmpty()); QTest::keyRelease(viewer, Qt::Key_Control);
        for (const auto &entry : inputs) {
            const auto event = qvariant_cast<QJsonObject>(entry[0]);
            QVERIFY(!(event["kind"].toString() == "key" && event["key"].toInt() == Qt::Key_D));
        }
        QVERIFY(remoteHeld.isEmpty());
        window.close(); QCOMPARE(disconnects.count(), 2);
    }
    void coordinatesAfterResize() {
        ld::Viewer viewer; viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        QImage frame(1600, 900, QImage::Format_RGB32); frame.fill(Qt::blue);
        viewer.setFrame(frame); viewer.setControl(true);
        QSignalSpy inputs(&viewer, &ld::Viewer::input);
        for (const QSize size : {QSize(640, 360), QSize(360, 640), QSize(1000, 400)}) {
            viewer.resize(size); QCoreApplication::processEvents();
            QTRY_COMPARE(viewer.size(), size);
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
