#include "desktop_wake.h"
#ifndef Q_OS_WIN
#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#endif

namespace ld {
bool wakeDesktop(QString &error) {
    error.clear();
#ifdef Q_OS_WIN
    error = QStringLiteral("Windows 登录界面的唤醒需要已启用的系统服务");
    return false;
#else
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        error = QStringLiteral("无法连接当前 X11 桌面，不能唤醒屏幕");
        return false;
    }

    // Reset only display inactivity. Do not call ScreenSaver.SetActive(false),
    // logind Unlock, or change screen-lock / power-management settings.
    XResetScreenSaver(display);
    int eventBase = 0, errorBase = 0;
    if (DPMSQueryExtension(display, &eventBase, &errorBase) && DPMSCapable(display)) {
        // Keep the enabled check and request together: a concurrent DPMSDisable
        // would otherwise turn the ForceLevel request into an X11 BadMatch.
        // No GUI work or external calls happen while this brief grab is held.
        XGrabServer(display);
        CARD16 level = DPMSModeOn;
        BOOL enabled = False;
        if (DPMSInfo(display, &level, &enabled) && enabled && level != DPMSModeOn)
            DPMSForceLevel(display, DPMSModeOn);
        XUngrabServer(display);
    }
    XFlush(display);
    XCloseDisplay(display);
    return true;
#endif
}
}
