#include "desktop_wake.h"
#include <QCoreApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QtTest>
#include <memory>
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>

namespace {
struct DisplayCloser { void operator()(Display *display) const { if (display) XCloseDisplay(display); } };
using DisplayPtr = std::unique_ptr<Display, DisplayCloser>;
}

// This test creates its own Xvfb and accepts only the display number that this
// child allocated. It never opens the inherited DISPLAY or any real lockscreen.
class DesktopWakeTest : public QObject {
    Q_OBJECT
private:
    QProcess server_;
    QByteArray displayName_;
    DisplayPtr display_;

    int saverState() const {
        XScreenSaverInfo *info = XScreenSaverAllocInfo();
        const bool ok = XScreenSaverQueryInfo(display_.get(), DefaultRootWindow(display_.get()), info);
        const int state = ok ? info->state : -1;
        XFree(info);
        return state;
    }

private slots:
    void initTestCase() {
        // Even a startup failure must never fall back to the caller's desktop.
        qunsetenv("DISPLAY");
        qunsetenv("WAYLAND_DISPLAY");
        server_.start(QStringLiteral("Xvfb"), {QStringLiteral("-displayfd"), QStringLiteral("1"),
            QStringLiteral("-screen"), QStringLiteral("0"), QStringLiteral("1280x720x24"),
            QStringLiteral("-nolisten"), QStringLiteral("tcp")});
        QVERIFY2(server_.waitForStarted(3000), qPrintable(server_.errorString()));
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += server_.readAllStandardOutput()).contains('\n'), 5000);
        const QByteArray number = output.trimmed();
        QVERIFY2(QRegularExpression(QStringLiteral("^[0-9]+$")).match(QString::fromLatin1(number)).hasMatch(), output.constData());
        QVERIFY(server_.state() == QProcess::Running);
        displayName_ = ':' + number;
        qputenv("DISPLAY", displayName_);
        display_.reset(XOpenDisplay(displayName_.constData()));
        QVERIFY(display_ != nullptr);
        int eventBase = 0, errorBase = 0;
        QVERIFY(XScreenSaverQueryExtension(display_.get(), &eventBase, &errorBase));
    }

    void missingDisplayReportsFailure() {
        qunsetenv("DISPLAY");
        QString error;
        const bool ok = ld::wakeDesktop(error);
        qputenv("DISPLAY", displayName_);
        QVERIFY(!ok);
        QVERIFY(error.contains(QStringLiteral("X11")));
    }

    void wakesActiveScreenSaver() {
        XForceScreenSaver(display_.get(), ScreenSaverActive);
        XSync(display_.get(), False);
        QCOMPARE(saverState(), int(ScreenSaverOn));
        QString error = QStringLiteral("old error");
        QVERIFY2(ld::wakeDesktop(error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QCOMPARE(saverState(), int(ScreenSaverOff));
    }

    void preservesScreenSaverConfiguration() {
        XSetScreenSaver(display_.get(), 1234, 57, PreferBlanking, AllowExposures);
        XForceScreenSaver(display_.get(), ScreenSaverActive);
        XSync(display_.get(), False);
        QString error;
        QVERIFY2(ld::wakeDesktop(error), qPrintable(error));
        int timeout = 0, interval = 0, blanking = 0, exposures = 0;
        XGetScreenSaver(display_.get(), &timeout, &interval, &blanking, &exposures);
        QCOMPARE(timeout, 1234);
        QCOMPARE(interval, 57);
        QCOMPARE(blanking, int(PreferBlanking));
        QCOMPARE(exposures, int(AllowExposures));
    }

    void preservesInputGrabsAndDoesNotType() {
        DisplayPtr locker(XOpenDisplay(displayName_.constData()));
        DisplayPtr contender(XOpenDisplay(displayName_.constData()));
        QVERIFY(locker != nullptr && contender != nullptr);
        Window window = XCreateSimpleWindow(locker.get(), DefaultRootWindow(locker.get()), 0, 0,
            1280, 720, 0, 0, 0x2563eb);
        XSelectInput(locker.get(), window, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask);
        XMapRaised(locker.get(), window);
        XSync(locker.get(), False);
        QCOMPARE(XGrabKeyboard(locker.get(), window, False, GrabModeAsync, GrabModeAsync, CurrentTime), int(GrabSuccess));
        QCOMPARE(XGrabPointer(locker.get(), window, False, ButtonPressMask | ButtonReleaseMask,
            GrabModeAsync, GrabModeAsync, 0, 0, CurrentTime), int(GrabSuccess));
        XForceScreenSaver(locker.get(), ScreenSaverActive);
        XSync(locker.get(), False);
        QString error;
        QVERIFY2(ld::wakeDesktop(error), qPrintable(error));
        QCOMPARE(saverState(), int(ScreenSaverOff));
        QCOMPARE(XGrabKeyboard(contender.get(), window, False, GrabModeAsync, GrabModeAsync, CurrentTime), int(AlreadyGrabbed));
        QCOMPARE(XGrabPointer(contender.get(), window, False, ButtonPressMask | ButtonReleaseMask,
            GrabModeAsync, GrabModeAsync, 0, 0, CurrentTime), int(AlreadyGrabbed));
        XSync(locker.get(), False);
        while (XPending(locker.get())) {
            XEvent event;
            XNextEvent(locker.get(), &event);
            QVERIFY(event.type != KeyPress && event.type != KeyRelease);
            QVERIFY(event.type != ButtonPress && event.type != ButtonRelease);
        }
    }

    void cleanupTestCase() {
        display_.reset();
        qunsetenv("DISPLAY");
        server_.terminate();
        if (!server_.waitForFinished(3000)) { server_.kill(); server_.waitForFinished(3000); }
    }
};

int main(int argc, char **argv) {
    XInitThreads();
    QCoreApplication app(argc, argv);
    DesktopWakeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "desktop_wake_test.moc"
