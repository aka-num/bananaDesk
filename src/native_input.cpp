#include "native_input.h"
#include <QSet>
#include <Qt>
#include <cmath>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <QLibrary>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

namespace ld {
struct NativeInput::Impl {
    QSet<int> keys, buttons;
    QString error;
#ifndef Q_OS_WIN
    Display *display = nullptr;
    QLibrary xtst{"Xtst", 6};
    using KeyFn = int (*)(Display *, unsigned int, Bool, unsigned long);
    using MotionFn = int (*)(Display *, int, int, int, unsigned long);
    using QueryFn = Bool (*)(Display *, int *, int *, int *, int *);
    KeyFn keyFn = nullptr, buttonFn = nullptr;
    MotionFn motionFn = nullptr;
#endif
    int nativeKey(int key) const;
    bool key(int code, bool down);
    bool button(int code, bool down);
    bool move(int x, int y);
};
int NativeInput::Impl::nativeKey(int key) const {
#ifdef Q_OS_WIN
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return key;
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return key;
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) return VK_F1 + key - Qt::Key_F1;
    switch (key) {
    case Qt::Key_Escape: return VK_ESCAPE; case Qt::Key_Tab: case Qt::Key_Backtab: return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK; case Qt::Key_Return: case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Insert: return VK_INSERT; case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME; case Qt::Key_End: return VK_END;
    case Qt::Key_Left: return VK_LEFT; case Qt::Key_Right: return VK_RIGHT; case Qt::Key_Up: return VK_UP; case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_PageUp: return VK_PRIOR; case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Shift: return VK_SHIFT; case Qt::Key_Control: return VK_CONTROL; case Qt::Key_Alt: return VK_MENU;
    case Qt::Key_Meta: return VK_LWIN; case Qt::Key_CapsLock: return VK_CAPITAL; case Qt::Key_Space: return VK_SPACE;
    default: if (key >= 32 && key <= 126) { SHORT v = VkKeyScanW(WCHAR(key)); return v == -1 ? 0 : (v & 255); } return 0;
    }
#else
    KeySym sym = NoSymbol;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) sym = XK_a + key - Qt::Key_A;
    else if (key >= 32 && key <= 126) sym = KeySym(key);
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F35) sym = XK_F1 + key - Qt::Key_F1;
    else switch (key) {
    case Qt::Key_Escape: sym = XK_Escape; break; case Qt::Key_Tab: case Qt::Key_Backtab: sym = XK_Tab; break;
    case Qt::Key_Backspace: sym = XK_BackSpace; break; case Qt::Key_Return: case Qt::Key_Enter: sym = XK_Return; break;
    case Qt::Key_Insert: sym = XK_Insert; break; case Qt::Key_Delete: sym = XK_Delete; break;
    case Qt::Key_Home: sym = XK_Home; break; case Qt::Key_End: sym = XK_End; break;
    case Qt::Key_Left: sym = XK_Left; break; case Qt::Key_Right: sym = XK_Right; break;
    case Qt::Key_Up: sym = XK_Up; break; case Qt::Key_Down: sym = XK_Down; break;
    case Qt::Key_PageUp: sym = XK_Page_Up; break; case Qt::Key_PageDown: sym = XK_Page_Down; break;
    case Qt::Key_Shift: sym = XK_Shift_L; break; case Qt::Key_Control: sym = XK_Control_L; break;
    case Qt::Key_Alt: sym = XK_Alt_L; break; case Qt::Key_Meta: sym = XK_Super_L; break;
    case Qt::Key_CapsLock: sym = XK_Caps_Lock; break; default: return 0;
    }
    return sym == NoSymbol ? 0 : XKeysymToKeycode(display, sym);
#endif
}
bool NativeInput::Impl::key(int code, bool down) {
#ifdef Q_OS_WIN
    INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = WORD(code);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if ((code >= VK_PRIOR && code <= VK_DOWN) || code == VK_INSERT || code == VK_DELETE || code == VK_LWIN) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return SendInput(1, &input, sizeof(input)) == 1;
#else
    return keyFn(display, unsigned(code), down, CurrentTime) != 0;
#endif
}
bool NativeInput::Impl::button(int code, bool down) {
#ifdef Q_OS_WIN
    INPUT input{}; input.type = INPUT_MOUSE;
    if (code == 1) input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    else if (code == 2) input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    else if (code == 3) input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else return false;
    return SendInput(1, &input, sizeof(input)) == 1;
#else
    return buttonFn(display, unsigned(code), down, CurrentTime) != 0;
#endif
}
bool NativeInput::Impl::move(int x, int y) {
#ifdef Q_OS_WIN
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 1 || h <= 1) return false;
    INPUT input{}; input.type = INPUT_MOUSE;
    input.mi.dx = LONG((qint64(x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535) / (w - 1));
    input.mi.dy = LONG((qint64(y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535) / (h - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return SendInput(1, &input, sizeof(input)) == 1;
#else
    return motionFn(display, -1, x, y, CurrentTime) != 0;
#endif
}
NativeInput::NativeInput() : impl_(new Impl) {
#ifndef Q_OS_WIN
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display || !impl_->xtst.load()) { impl_->error = QStringLiteral("X11 或 libXtst.so.6 不可用"); return; }
    impl_->keyFn = reinterpret_cast<Impl::KeyFn>(impl_->xtst.resolve("XTestFakeKeyEvent"));
    impl_->buttonFn = reinterpret_cast<Impl::KeyFn>(impl_->xtst.resolve("XTestFakeButtonEvent"));
    impl_->motionFn = reinterpret_cast<Impl::MotionFn>(impl_->xtst.resolve("XTestFakeMotionEvent"));
    auto query = reinterpret_cast<Impl::QueryFn>(impl_->xtst.resolve("XTestQueryExtension"));
    int a, b, c, d;
    if (!impl_->keyFn || !impl_->buttonFn || !impl_->motionFn || !query || !query(impl_->display, &a, &b, &c, &d)) impl_->error = QStringLiteral("XTEST 扩展不可用");
#endif
}
NativeInput::~NativeInput() {
    releaseAll();
#ifndef Q_OS_WIN
    if (impl_->display) XCloseDisplay(impl_->display);
#endif
}
bool NativeInput::available() const { return impl_->error.isEmpty(); }
QString NativeInput::error() const { return impl_->error; }
bool NativeInput::apply(const QJsonObject &e, const QRect &screen) {
    if (!available() || screen.isEmpty()) return false;
    const QString type = e.value("kind").toString();
    bool ok = true;
    if (type == "move" || type == "button" || type == "wheel") {
        if (!e.value("x").isDouble() || !e.value("y").isDouble()) return false;
        double x = e.value("x").toDouble(), y = e.value("y").toDouble();
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return false;
        const int button = e.value("button").toInt();
        const int steps = e.value("steps").toInt();
        if (type == "button" && (button < 1 || button > 3 || !e.value("down").isBool())) return false;
        if (type == "wheel" && (steps == 0 || steps < -10 || steps > 10)) return false;
        ok = impl_->move(screen.x() + qRound(x * (screen.width() - 1)), screen.y() + qRound(y * (screen.height() - 1)));
        if (type == "button") {
            bool down = e.value("down").toBool();
            if (down) impl_->buttons.insert(button); else impl_->buttons.remove(button);
            ok = impl_->button(button, down) && ok;
        } else if (type == "wheel") {
#ifdef Q_OS_WIN
            INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_WHEEL; input.mi.mouseData = DWORD(steps * WHEEL_DELTA);
            ok = SendInput(1, &input, sizeof(input)) == 1 && ok;
#else
            for (int i = 0; i < std::abs(steps); ++i) { const int b = steps > 0 ? 4 : 5; ok = impl_->button(b, true) && impl_->button(b, false) && ok; }
#endif
        }
    } else if (type == "key") {
        if (!e.value("key").isDouble() || !e.value("down").isBool()) return false;
        const int code = impl_->nativeKey(e.value("key").toInt());
        if (!code) return false;
        bool down = e.value("down").toBool();
        if (down) impl_->keys.insert(code); else impl_->keys.remove(code);
        ok = impl_->key(code, down);
    } else return false;
#ifndef Q_OS_WIN
    XFlush(impl_->display);
#endif
    return ok;
}
void NativeInput::releaseAll() {
    if (!available()) return;
    for (int key : impl_->keys) impl_->key(key, false);
    for (int button : impl_->buttons) impl_->button(button, false);
    impl_->keys.clear(); impl_->buttons.clear();
#ifndef Q_OS_WIN
    XFlush(impl_->display);
#endif
}
}
