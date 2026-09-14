#include "desktop_lock.h"
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#endif

namespace ld {
bool lockDesktop(QString &error) {
    error.clear();
#ifdef Q_OS_WIN
    if (LockWorkStation()) return true;
    const DWORD code = GetLastError();
    error = QStringLiteral("Windows 未接受锁屏请求（错误 %1）").arg(code);
    return false;
#else
    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        error = QStringLiteral("无法连接当前桌面的 D-Bus 会话：%1").arg(bus.lastError().message());
        return false;
    }
    struct Service { const char *name; const char *path; };
    const Service services[] = {
        {"org.gnome.ScreenSaver", "/org/gnome/ScreenSaver"},
        {"org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver"},
    };
    for (const auto &service : services) {
        auto request = QDBusMessage::createMethodCall(QString::fromLatin1(service.name),
            QString::fromLatin1(service.path), QString::fromLatin1(service.name), QStringLiteral("Lock"));
        // Only use the screen locker already provided by the logged-in desktop.
        request.setAutoStartService(false);
        const auto reply = bus.call(request, QDBus::Block, 1000);
        if (reply.type() == QDBusMessage::ReplyMessage) return true;
        const QString name = reply.errorName();
        const bool unavailable = name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
            || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
            || name == QLatin1String("org.freedesktop.DBus.Error.UnknownObject")
            || name == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface")
            || name == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod");
        if (!unavailable) {
            error = QStringLiteral("锁屏服务未接受请求：%1（%2）").arg(reply.errorMessage(), name);
            return false;
        }
    }
    error = QStringLiteral("当前桌面没有可用的锁屏服务（GNOME / freedesktop ScreenSaver）");
    return false;
#endif
}
}
