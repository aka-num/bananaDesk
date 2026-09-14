#include "app.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <cstdio>
#include <csignal>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static volatile std::sig_atomic_t interrupted = 0;
static void onSignal(int) { interrupted = 1; }
int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    using AwarenessFn = BOOL (WINAPI *)(HANDLE);
    auto fn = reinterpret_cast<AwarenessFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (fn) fn(reinterpret_cast<HANDLE>(-4));
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("LanDesk"); QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("局域网远程桌面，TLS 指纹验证与临时连接码"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"host", "Start sharing after opening the window"});
    parser.addOption({"view-only", "Allow viewing only when sharing"});
    parser.addOption({"bind", "Local IPv4 address to bind", "address"});
    parser.addOption({"port", "TCP port", "port", QString::number(ld::DefaultPort)});
    parser.addOption({"invite-file", "Write the private invitation to this file (host mode)", "path"});
    parser.addOption({"connect-file", "Read an invitation file and connect", "path"});
    parser.process(app);
    bool validPort = false; const int port = parser.value("port").toInt(&validPort);
    if (!validPort || port < 1024 || port > 65535) { std::fprintf(stderr, "Port must be between 1024 and 65535\n"); return 2; }
    if (!QSslSocket::supportsSsl()) { std::fprintf(stderr, "Qt TLS backend unavailable\n"); return 2; }
    ld::Window window;
    if (parser.isSet("view-only")) window.setViewOnly();
    window.show();
    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
    QTimer terminationTimer;
    QObject::connect(&terminationTimer, &QTimer::timeout, &app, [&] { if (interrupted) app.quit(); });
    terminationTimer.start(100);
    QTimer::singleShot(0, &window, [&] {
        if (parser.isSet("host") && !window.startHost(parser.value("bind"), quint16(port), parser.value("invite-file"))) { app.exit(2); return; }
        if (parser.isSet("connect-file")) window.connectFile(parser.value("connect-file"));
    });
    return app.exec();
}
