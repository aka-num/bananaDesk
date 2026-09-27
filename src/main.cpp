#include "app.h"
#include "video_codec.h"
#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <csignal>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <X11/Xlib.h>
#endif

static volatile std::sig_atomic_t interrupted = 0;
static void onSignal(int) { interrupted = 1; }
static QFile diagnosticLog;
static QMutex diagnosticMutex;
static void logMessage(QtMsgType type, const QMessageLogContext &, const QString &message) {
    const QByteArray line = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
        + QStringLiteral(" [%1] ").arg(int(type)) + message + '\n').toUtf8();
    QMutexLocker lock(&diagnosticMutex);
    diagnosticLog.write(line); diagnosticLog.flush();
    std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
}
static int startupFailure(const QString &message, bool interactive = true) {
    qCritical().noquote() << message;
#ifdef Q_OS_WIN
    if (interactive) QMessageBox::critical(nullptr, QStringLiteral("bananaDesk 无法启动"), message);
#else
    Q_UNUSED(interactive);
#endif
    return 2;
}
int main(int argc, char **argv) {
#ifndef Q_OS_WIN
    XInitThreads();
#endif
#ifdef Q_OS_WIN
    using AwarenessFn = BOOL (WINAPI *)(HANDLE);
    auto fn = reinterpret_cast<AwarenessFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (fn) fn(reinterpret_cast<HANDLE>(-4));
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("bananaDesk"); QCoreApplication::setApplicationVersion("0.7.6");
    QGuiApplication::setDesktopFileName("bananaDesk");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/bananaDesk.png")));
    qRegisterMetaType<quint64>("quint64");
    QCommandLineParser parser; parser.setApplicationDescription(QStringLiteral("局域网远程桌面，TLS 指纹验证与可手动重置的固定共享码"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"host", "Start sharing after opening the window"});
    parser.addOption({"autostart", "Started by the per-user login autostart entry"});
    parser.addOption({"prelogin-host", "Linux only: share the X11 display-manager login screen"});
    parser.addOption({"view-only", "Allow viewing only when sharing"});
    parser.addOption({"no-lock-on-disconnect", "Do not lock the host desktop when an authenticated session ends"});
    parser.addOption({"no-file-transfer", "Disable file transfer when sharing"});
    parser.addOption({"transfer-dir", "Folder allowed for remote uploads and downloads", "path"});
    parser.addOption({"bind", "Local IPv4 address to bind", "address"});
    parser.addOption({"port", "TCP port", "port", QString::number(ld::DefaultPort)});
    parser.addOption({"invite-file", "Write the private invitation to this file (host mode)", "path"});
    parser.addOption({"connect-file", "Read an invitation file and connect", "path"});
    parser.addOption({"fps", "Target frame rate: 15, 30 or 60", "fps", "60"});
    parser.addOption({"codec", "Video codec: auto, h264 or jpeg", "codec", "auto"});
    parser.addOption({"stats-file", "Write client frame counters to a JSON file", "path"});
    parser.addOption({"log-file", "Write diagnostic messages (no invitation or input content)", "path"});
    parser.addOption({"diagnostics-file", "Check runtime dependencies, write JSON and exit", "path"});
    parser.process(app);
#ifdef Q_OS_WIN
    if (parser.isSet("prelogin-host"))
        return startupFailure(QStringLiteral("--prelogin-host 仅支持 Linux X11/systemd，被控端 Windows 不支持登录前远控"), false);
#else
    if (parser.isSet("prelogin-host") && !parser.isSet("host"))
        return startupFailure(QStringLiteral("--prelogin-host 必须和 --host 一起使用"), false);
#endif
    QString logPath = parser.value("log-file");
#ifdef Q_OS_WIN
    if (logPath.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (QDir().mkpath(dir)) logPath = dir + "/bananaDesk.log";
    }
#endif
    if (!logPath.isEmpty()) {
        diagnosticLog.setFileName(logPath);
        if (!diagnosticLog.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return startupFailure(QStringLiteral("无法写入日志文件：%1").arg(logPath), !parser.isSet("diagnostics-file"));
        qInstallMessageHandler(logMessage);
    }
    const bool tlsAvailable = QSslSocket::supportsSsl();
    const bool h264Available = ld::VideoEncoder::available() && ld::VideoDecoder::available();
    qInfo().noquote() << QStringLiteral("bananaDesk %1 · Qt %2 · TLS %3 · H.264 %4")
        .arg(QCoreApplication::applicationVersion(), qVersion(), QSslSocket::sslLibraryVersionString(), h264Available ? "available" : "unavailable");
    if (parser.isSet("diagnostics-file")) {
        QJsonObject report{{"application", QCoreApplication::applicationName()}, {"version", QCoreApplication::applicationVersion()}, {"qt", qVersion()},
            {"application_icon_available", !QApplication::windowIcon().isNull()},
            {"tls_available", tlsAvailable}, {"tls_version", QSslSocket::sslLibraryVersionString()},
            {"h264_available", h264Available}, {"platform", QGuiApplication::platformName()}};
        QSaveFile file(parser.value("diagnostics-file"));
        if (!file.open(QIODevice::WriteOnly)) return startupFailure(QStringLiteral("无法写入诊断文件"), false);
        const QByteArray data = QJsonDocument(report).toJson();
        if (file.write(data) != data.size() || !file.commit()) return startupFailure(QStringLiteral("诊断文件写入失败"), false);
        return tlsAvailable && h264Available ? 0 : 2;
    }
    bool validPort = false; const int port = parser.value("port").toInt(&validPort);
    if (!validPort || port < 1024 || port > 65535) return startupFailure(QStringLiteral("端口必须在 1024 到 65535 之间"));
    const int fps = parser.value("fps").toInt();
    const QString codec = parser.value("codec");
    if ((fps != 15 && fps != 30 && fps != 60) || (codec != "auto" && codec != "h264" && codec != "jpeg")) {
        return startupFailure(QStringLiteral("不支持的帧率或编码参数"));
    }
    if (codec == "h264" && !h264Available) return startupFailure(QStringLiteral("H.264 编码或解码不可用，请重新完整解压运行包"));
    if (!tlsAvailable) return startupFailure(QStringLiteral("TLS 不可用，请重新完整解压运行包，确认 OpenSSL DLL 未缺失"));
    ld::Window window;
    window.configureFiles(parser.value("transfer-dir"), !parser.isSet("no-file-transfer"));
    window.configureVideo(fps, codec); window.setStatsFile(parser.value("stats-file"));
    if (parser.isSet("view-only")) window.setViewOnly();
    if (parser.isSet("no-lock-on-disconnect")) window.setLockOnDisconnect(false);
    const bool headlessHost = parser.isSet("prelogin-host");
    if (!headlessHost) window.show();
    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
    QTimer terminationTimer;
    QObject::connect(&terminationTimer, &QTimer::timeout, &app, [&] { if (interrupted) app.quit(); });
    terminationTimer.start(100);
    QTimer::singleShot(0, &window, [&] {
        if (parser.isSet("host") && !window.startHost(parser.value("bind"), parser.isSet("port") ? quint16(port) : 0, parser.value("invite-file"))) { app.exit(2); return; }
        if (parser.isSet("autostart") && parser.isSet("host")) window.hide();
        if (parser.isSet("connect-file")) window.connectFile(parser.value("connect-file"));
    });
    return app.exec();
}
