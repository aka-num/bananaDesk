#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#endif
#include "windows_bridge.h"
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include "../windows/helper_protocol.h"
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace ld {
namespace {
using namespace ldhelper;
struct Bridge {
    std::mutex mutex;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool enabled = false;
    DWORD session = 0;
    std::uint32_t sequence = 0;
    ~Bridge() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }
    void close() { if (pipe != INVALID_HANDLE_VALUE) { CancelIoEx(pipe, nullptr); CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; } }
};
Bridge &bridge() { static Bridge state; return state; }
std::wstring sidForToken(HANDLE token) {
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return {};
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &text)) return {};
    std::wstring result(text); LocalFree(text); return result;
}
std::wstring currentSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    auto result = sidForToken(token); CloseHandle(token); return result;
}
std::wstring installedPath() {
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, path))) return {};
    return std::wstring(path) + L"\\LanDeskUnlock\\landesk-helper.exe";
}
bool installed() {
    wchar_t sid[256]{}; DWORD size = sizeof(sid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, RegistryKey, L"OwnerSid", RRF_RT_REG_SZ, nullptr, sid, &size) != ERROR_SUCCESS ||
        currentSid() != sid) return false;
    DWORD attributes = GetFileAttributesW(installedPath().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    SC_HANDLE service = OpenServiceW(manager, ServiceName, SERVICE_QUERY_STATUS);
    bool ok = service != nullptr;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager); return ok;
}
bool verifyServer(HANDLE pipe, DWORD expectedSession) {
    ULONG pid = 0, pipeSession = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid) || !GetNamedPipeServerSessionId(pipe, &pipeSession) || pipeSession != expectedSession) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    std::array<wchar_t, 32768> path{}; DWORD size = DWORD(path.size());
    bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &size) && _wcsicmp(path.data(), installedPath().c_str()) == 0;
    HANDLE token = nullptr;
    ok = ok && OpenProcessToken(process, TOKEN_QUERY, &token);
    if (ok) ok = sidForToken(token) == L"S-1-5-18";
    if (token) CloseHandle(token);
    CloseHandle(process); return ok;
}
bool transfer(HANDLE pipe, void *buffer, DWORD bytes, bool writing, QElapsedTimer &elapsed) {
    auto *cursor = static_cast<BYTE *>(buffer);
    while (bytes) {
        if (elapsed.elapsed() >= 3000) return false;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!event) return false;
        OVERLAPPED operation{}; operation.hEvent = event; DWORD count = 0;
        BOOL done = writing ? WriteFile(pipe, cursor, bytes, &count, &operation) : ReadFile(pipe, cursor, bytes, &count, &operation);
        if (!done && GetLastError() == ERROR_IO_PENDING) {
            DWORD timeout = DWORD(std::max<qint64>(0, 3000 - elapsed.elapsed()));
            if (WaitForSingleObject(event, timeout) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE); CloseHandle(event); return false;
            }
            done = GetOverlappedResult(pipe, &operation, &count, FALSE);
        }
        CloseHandle(event);
        if (!done || !count || count > bytes) return false;
        cursor += count; bytes -= count;
    }
    return true;
}
bool exchange(HANDLE pipe, Request &request, Response &response, QElapsedTimer &elapsed) {
    return transfer(pipe, &request, sizeof(request), true, elapsed) && transfer(pipe, &response, sizeof(response), false, elapsed) &&
        response.magic == Magic && response.version == Version && response.op == request.op &&
        response.sequence == request.sequence && response.bytes <= MaxPixelsBytes &&
        (request.op == Op::Capture || response.bytes == 0) && (!response.status || response.bytes == 0);
}
HANDLE openPipe(const std::wstring &name, DWORD session, QElapsedTimer &clock) {
    do {
        HANDLE pipe = CreateFileW(name.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            if (verifyServer(pipe, session)) return pipe;
            CloseHandle(pipe); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return INVALID_HANDLE_VALUE;
        QThread::msleep(20);
    } while (clock.elapsed() < 3000);
    SetLastError(ERROR_TIMEOUT); return INVALID_HANDLE_VALUE;
}
bool connect(Bridge &state, QString &error, QElapsedTimer &clock) {
    if (!state.enabled) { error = QStringLiteral("Windows 远程解锁通道已停用"); return false; }
    if (state.pipe != INVALID_HANDLE_VALUE) return true;
    if (!installed() || !ProcessIdToSessionId(GetCurrentProcessId(), &state.session) || state.session == 0) {
        error = QStringLiteral("请先为当前 Windows 用户安装远程解锁组件"); return false;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, ServiceName, SERVICE_START | SERVICE_QUERY_STATUS) : nullptr;
    DWORD serviceError = ERROR_SUCCESS;
    if (!service) serviceError = GetLastError();
    else if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) serviceError = GetLastError();
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    if (serviceError) { error = QStringLiteral("无法启动 Windows 远程解锁服务（%1）").arg(serviceError); return false; }
    HANDLE broker = openPipe(BrokerPipe, 0, clock);
    if (broker == INVALID_HANDLE_VALUE) { error = QStringLiteral("无法验证或连接 Windows 远程解锁服务（%1）").arg(GetLastError()); return false; }
    Request hello; hello.op = Op::Connect; hello.sequence = ++state.sequence;
    Response response;
    bool ok = exchange(broker, hello, response, clock) && !response.status && response.session == state.session;
    CloseHandle(broker);
    if (!ok) { error = QStringLiteral("Windows 远程解锁服务拒绝当前用户或会话（%1）").arg(response.status); return false; }
    std::wstring name = std::wstring(BrokerPipe) + L"-desktop-" + std::to_wstring(state.session);
    state.pipe = openPipe(name, state.session, clock);
    hello.sequence = ++state.sequence;
    if (state.pipe == INVALID_HANDLE_VALUE || !exchange(state.pipe, hello, response, clock) || response.status || response.session != state.session) {
        state.close(); error = QStringLiteral("无法连接 Windows 安全桌面工作进程（%1）").arg(response.status); return false;
    }
    return true;
}
bool request(Bridge &state, Request &command, Response &response, QString &error, QElapsedTimer &clock) {
    if (!connect(state, error, clock)) return false;
    command.sequence = ++state.sequence;
    if (!exchange(state.pipe, command, response, clock) || response.session != state.session) {
        state.close(); error = QStringLiteral("Windows 远程解锁通道已断开或超时"); return false;
    }
    if (response.status) {
        error = QStringLiteral("Windows 桌面暂不可用（%1），请稍候重试").arg(response.status); return false;
    }
    return true;
}
int virtualKey(int key) {
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
    default:
        if (key >= 32 && key <= 126) { SHORT value = VkKeyScanW(WCHAR(key)); return value == -1 ? 0 : (value & 255); }
        return 0;
    }
}
}
bool windowsHelperInstalled() { return installed(); }
void windowsHelperSetEnabled(bool enabled) {
    auto &state = bridge(); std::lock_guard<std::mutex> lock(state.mutex);
    state.enabled = enabled; if (!enabled) state.close();
}
bool windowsHelperCapture(QImage &image, QRect &bounds, QString &error) {
    auto &state = bridge(); std::lock_guard<std::mutex> lock(state.mutex);
    QElapsedTimer retries; retries.start();
    Request command; command.op = Op::Capture; Response response;
    for (;;) {
        if (request(state, command, response, error, retries)) break;
        if (!state.enabled || state.pipe == INVALID_HANDLE_VALUE || !response.status || retries.elapsed() >= 500) return false;
        QThread::msleep(40);
    }
    const quint64 expectedBytes = quint64(response.pixelWidth) * response.pixelHeight * 4;
    if (response.width <= 0 || response.height <= 0 || response.width > 32768 || response.height > 32768 ||
        !response.pixelWidth || response.pixelWidth > MaxWidth || !response.pixelHeight || response.pixelHeight > MaxHeight ||
        expectedBytes != response.bytes || response.bytes > MaxPixelsBytes) {
        state.close(); error = QStringLiteral("Windows 桌面图像尺寸无效"); return false;
    }
    QImage result(int(response.pixelWidth), int(response.pixelHeight), QImage::Format_RGB32);
    if (result.isNull() || result.bytesPerLine() != int(response.pixelWidth * 4) ||
        !transfer(state.pipe, result.bits(), response.bytes, false, retries)) {
        state.close(); error = QStringLiteral("Windows 桌面图像读取失败或超时"); return false;
    }
    image = result; bounds = QRect(response.x, response.y, response.width, response.height); return true;
}
WindowsInputResult windowsHelperInput(const QJsonObject &event, QString &error) {
    Request command; command.op = Op::Input;
    const QString type = event.value("kind").toString();
    error = QStringLiteral("远端键鼠事件无效或此键位尚不支持");
    if (type == "key") {
        if (!event.value("key").isDouble() || !event.value("down").isBool()) return WindowsInputResult::Unsupported;
        int key = virtualKey(event.value("key").toInt()); if (!key) return WindowsInputResult::Unsupported;
        command.args[0] = int(InputKind::Key); command.args[1] = key; command.args[2] = event.value("down").toBool();
    } else {
        double x = event.value("x").toDouble(-1), y = event.value("y").toDouble(-1);
        if (!event.value("x").isDouble() || !event.value("y").isDouble() || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return WindowsInputResult::Unsupported;
        command.args[3] = qRound(x * 1000000); command.args[4] = qRound(y * 1000000);
        if (type == "move") command.args[0] = int(InputKind::Move);
        else if (type == "button") {
            int button = event.value("button").toInt();
            if (button < 1 || button > 3 || !event.value("down").isBool()) return WindowsInputResult::Unsupported;
            command.args[0] = int(InputKind::Button); command.args[2] = event.value("down").toBool(); command.args[5] = button;
        } else if (type == "wheel") {
            int steps = event.value("steps").toInt(); if (!steps || steps < -10 || steps > 10) return WindowsInputResult::Unsupported;
            command.args[0] = int(InputKind::Wheel); command.args[6] = steps;
        } else return WindowsInputResult::Unsupported;
    }
    error.clear();
    auto &state = bridge(); std::lock_guard<std::mutex> lock(state.mutex); Response response;
    QElapsedTimer clock; clock.start();
    if (request(state, command, response, error, clock)) return WindowsInputResult::Applied;
    if (state.pipe != INVALID_HANDLE_VALUE && response.status == ERROR_RETRY) {
        error = QStringLiteral("Windows 桌面已切换，等待新画面后再输入");
        return WindowsInputResult::Unsupported;
    }
    return WindowsInputResult::Failed;
}
void windowsHelperRelease() {
    auto &state = bridge(); std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.enabled || state.pipe == INVALID_HANDLE_VALUE) return;
    Request command; command.op = Op::Release; Response response; QString error;
    QElapsedTimer clock; clock.start();
    request(state, command, response, error, clock);
}
bool windowsHelperWake(QString &error) {
    auto &state = bridge(); std::lock_guard<std::mutex> lock(state.mutex);
    Request command; command.op = Op::Wake; Response response;
    QElapsedTimer clock; clock.start();
    return request(state, command, response, error, clock);
}
void windowsHelperDisconnect() {
    auto &state = bridge(); std::lock_guard<std::mutex> lock(state.mutex); state.close();
}
}
#else
namespace ld {
bool windowsHelperInstalled() { return false; }
void windowsHelperSetEnabled(bool) {}
bool windowsHelperCapture(QImage &, QRect &, QString &error) { error = QStringLiteral("Windows 解锁组件仅用于 Windows"); return false; }
WindowsInputResult windowsHelperInput(const QJsonObject &, QString &error) { error = QStringLiteral("Windows 解锁组件仅用于 Windows"); return WindowsInputResult::Unsupported; }
void windowsHelperRelease() {}
bool windowsHelperWake(QString &error) { error = QStringLiteral("Windows 解锁组件仅用于 Windows"); return false; }
void windowsHelperDisconnect() {}
}
#endif
