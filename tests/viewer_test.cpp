#include "app.h"
#include <QtTest>
#include <QSignalSpy>

class InteractionTest : public QObject {
    Q_OBJECT
private slots:
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
