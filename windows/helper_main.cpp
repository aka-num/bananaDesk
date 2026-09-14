#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include "helper_protocol.h"

namespace {
using namespace ldhelper;
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE release() { HANDLE out = h; h = nullptr; return out; }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const std::wstring &sddl) {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
std::wstring tokenSid(HANDLE token) {
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) return {};
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) return {};
    std::wstring sid(text); LocalFree(text); return sid;
}
std::wstring currentSid() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h)) return {};
    return tokenSid(token.h);
}
bool isSystem() { return currentSid() == L"S-1-5-18"; }
bool validSid(const std::wstring &text) {
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(text.c_str(), &sid)) return false;
    bool result = IsValidSid(sid) && text != L"S-1-5-18";
    LocalFree(sid); return result;
}
std::wstring imagePath() {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    return size && size < path.size() ? std::wstring(path.data(), size) : std::wstring();
}
std::wstring installDirectory() {
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, SHGFP_TYPE_CURRENT, path))) return {};
    return std::wstring(path) + L"\\LanDeskUnlock";
}
std::wstring installedImage() { auto path = installDirectory(); return path.empty() ? path : path + L"\\landesk-helper.exe"; }
std::wstring workerPipe(DWORD session) { return std::wstring(BrokerPipe) + L"-desktop-" + std::to_wstring(session); }
std::wstring ownerSid() {
    wchar_t sid[256]{}; DWORD bytes = sizeof(sid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, RegistryKey, L"OwnerSid", RRF_RT_REG_SZ, nullptr, sid, &bytes) != ERROR_SUCCESS) return {};
    std::wstring value(sid); return validSid(value) ? value : std::wstring();
}
std::wstring pipeAcl(const std::wstring &sid) {
    // FILE_READ_DATA|FILE_WRITE_DATA|READ_ATTRIBUTES|WRITE_ATTRIBUTES|SYNCHRONIZE.
    // Deliberately excludes FILE_CREATE_PIPE_INSTANCE (which aliases APPEND_DATA).
    return L"D:P(A;;GA;;;SY)(A;;0x00100183;;;" + sid + L")";
}
// Ordinary users must be able to verify a pipe server's SYSTEM identity. Only
// QUERY access is granted: this does not permit token duplication or impersonation.
bool allowOwnerToVerify(const std::wstring &sid) {
    Security processAcl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x1000;;;" + sid + L")");
    Security tokenAcl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0008;;;" + sid + L")");
    Handle token;
    return processAcl.descriptor && tokenAcl.descriptor &&
        SetKernelObjectSecurity(GetCurrentProcess(), DACL_SECURITY_INFORMATION, processAcl.descriptor) &&
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | WRITE_DAC, &token.h) &&
        SetKernelObjectSecurity(token.h, DACL_SECURITY_INFORMATION, tokenAcl.descriptor);
}
bool enablePrivilege(const wchar_t *name) {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token.h)) return false;
    TOKEN_PRIVILEGES value{}; value.PrivilegeCount = 1; value.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, name, &value.Privileges[0].Luid)) return false;
    SetLastError(ERROR_SUCCESS);
    return AdjustTokenPrivileges(token.h, FALSE, &value, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
}
bool transfer(HANDLE pipe, void *buffer, DWORD bytes, bool writing, HANDLE stop, DWORD timeout) {
    auto *cursor = static_cast<BYTE *>(buffer);
    ULONGLONG deadline = GetTickCount64() + timeout;
    while (bytes) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event) return false;
        OVERLAPPED overlapped{}; overlapped.hEvent = event.h; DWORD count = 0;
        BOOL done = writing ? WriteFile(pipe, cursor, bytes, &count, &overlapped) : ReadFile(pipe, cursor, bytes, &count, &overlapped);
        if (!done && GetLastError() == ERROR_IO_PENDING) {
            HANDLE events[]{event.h, stop};
            auto now = GetTickCount64();
            DWORD wait = now < deadline ? DWORD(std::min<ULONGLONG>(deadline - now, 0x7fffffff)) : 0;
            DWORD result = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, wait);
            if (result != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped); GetOverlappedResult(pipe, &overlapped, &count, TRUE); return false;
            }
            done = GetOverlappedResult(pipe, &overlapped, &count, FALSE);
        }
        if (!done || !count || count > bytes) return false;
        cursor += count; bytes -= count;
    }
    return true;
}
bool accept(HANDLE pipe, HANDLE stop, DWORD timeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event) return false;
    OVERLAPPED overlapped{}; overlapped.hEvent = event.h;
    if (ConnectNamedPipe(pipe, &overlapped)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    HANDLE events[]{event.h, stop};
    DWORD result = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout);
    DWORD count = 0;
    if (result != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped); GetOverlappedResult(pipe, &overlapped, &count, TRUE); return false;
    }
    return GetOverlappedResult(pipe, &overlapped, &count, FALSE) != FALSE;
}
HANDLE createPipe(const std::wstring &name, const std::wstring &sid) {
    Security security(pipeAcl(sid));
    if (!security.descriptor) return INVALID_HANDLE_VALUE;
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 3000, &security.attributes);
}
bool verifyClient(HANDLE pipe, const std::wstring &sid, DWORD &session) {
    ULONG pid = 0, reportedSession = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !GetNamedPipeClientSessionId(pipe, &reportedSession) ||
        reportedSession == 0 || reportedSession != WTSGetActiveConsoleSessionId()) return false;
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    Handle token;
    bool ok = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.h) && tokenSid(token.h) == sid;
    DWORD actualSession = 0, bytes = 0;
    ok = ok && GetTokenInformation(token.h, TokenSessionId, &actualSession, sizeof(actualSession), &bytes) && actualSession == reportedSession;
    // Always revert before any privileged operation or response.
    if (!RevertToSelf()) ExitProcess(ERROR_CANNOT_IMPERSONATE);
    if (!ok) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    std::array<wchar_t, 32768> path{}; DWORD size = DWORD(path.size());
    if (!process || !QueryFullProcessImageNameW(process.h, 0, path.data(), &size)) return false;
    DWORD processSession = 0;
    if (!ProcessIdToSessionId(pid, &processSession) || processSession != reportedSession) return false;
    session = actualSession; return true;
}
bool validRequest(const Request &r) {
    if (r.magic != Magic || r.version != Version || r.bytes != 0) return false;
    return r.op >= Op::Connect && r.op <= Op::Wake;
}
Response responseFor(const Request &request) {
    Response result; result.op = request.op; result.sequence = request.sequence; return result;
}

struct Desktop {
    HDESK original = GetThreadDesktop(GetCurrentThreadId()), selected = nullptr;
    HDC display = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr; HGDIOBJ prior = nullptr; void *pixels = nullptr;
    RECT bounds{}; DWORD width = 0, height = 0;
    std::array<bool, 256> keys{}; std::array<bool, 4> buttons{};
    std::wstring desktopName;
    bool capturedCurrentDesktop = false;
    ~Desktop() { select(); release(); clearImage(); if (selected) { SetThreadDesktop(original); CloseDesktop(selected); } }
    void clearImage() {
        capturedCurrentDesktop = false;
        if (prior && memory) SelectObject(memory, prior);
        prior = nullptr;
        if (bitmap) DeleteObject(bitmap);
        bitmap = nullptr; pixels = nullptr;
        if (memory) DeleteDC(memory);
        memory = nullptr;
        if (display) ReleaseDC(nullptr, display);
        display = nullptr;
        width = height = 0;
    }
    bool emitKey(WORD key, bool down) {
        INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = key;
        input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        if ((key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE || key == VK_LWIN)
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        return SendInput(1, &input, sizeof(input)) == 1;
    }
    bool emitButton(int button, bool down) {
        INPUT input{}; input.type = INPUT_MOUSE;
        static constexpr DWORD press[]{0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN};
        static constexpr DWORD release[]{0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP};
        input.mi.dwFlags = down ? press[button] : release[button];
        return SendInput(1, &input, sizeof(input)) == 1;
    }
    void release() {
        for (unsigned i = 0; i < keys.size(); ++i) if (keys[i]) emitKey(WORD(i), false);
        for (int i = 1; i <= 3; ++i) if (buttons[i]) emitButton(i, false);
        keys.fill(false); buttons.fill(false);
    }
    DWORD select() {
        HDESK next = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW);
        if (!next) return GetLastError();
        std::array<wchar_t, 256> name{}; DWORD length = 0;
        if (!GetUserObjectInformationW(next, UOI_NAME, name.data(), DWORD(name.size() * sizeof(wchar_t)), &length)) {
            DWORD error = GetLastError(); CloseDesktop(next); return error;
        }
        if (selected && desktopName == name.data()) { CloseDesktop(next); return ERROR_SUCCESS; }
        auto heldKeys = keys; auto heldButtons = buttons;
        release(); clearImage();
        if (!SetThreadDesktop(next)) {
            DWORD error = GetLastError(); CloseDesktop(next);
            keys = heldKeys; buttons = heldButtons; return error;
        }
        if (selected) CloseDesktop(selected);
        selected = next; desktopName = name.data();
        // Key-up events are also sent on the newly active desktop, preventing
        // modifiers surviving a desktop transition. No key contents are logged.
        for (unsigned key = 0; key < heldKeys.size(); ++key) if (heldKeys[key]) emitKey(WORD(key), false);
        for (int button = 1; button <= 3; ++button) if (heldButtons[unsigned(button)]) emitButton(button, false);
        return ERROR_SUCCESS;
    }
    DWORD capture(Response &response, std::vector<BYTE> &out) {
        DWORD error = select(); if (error) return error;
        POINT origin{}; MONITORINFO info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info)) return GetLastError();
        auto r = info.rcMonitor;
        long sourceW = r.right - r.left, sourceH = r.bottom - r.top;
        if (sourceW <= 0 || sourceH <= 0 || sourceW > 32768 || sourceH > 32768) return ERROR_INVALID_DATA;
        double scale = std::min({1.0, double(MaxWidth) / sourceW, double(MaxHeight) / sourceH});
        DWORD w = std::max(1L, long(sourceW * scale)), h = std::max(1L, long(sourceH * scale));
        if (!display || w != width || h != height || !EqualRect(&r, &bounds)) {
            clearImage(); bounds = r; width = w; height = h;
            display = GetDC(nullptr); if (display) memory = CreateCompatibleDC(display);
            if (!display || !memory) return ERROR_INVALID_HANDLE;
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = LONG(width); info.bmiHeader.biHeight = -LONG(height);
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(display, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap || !pixels) { clearImage(); return ERROR_NOT_ENOUGH_MEMORY; }
            prior = SelectObject(memory, bitmap);
            if (!prior || prior == HGDI_ERROR) { prior = nullptr; clearImage(); return ERROR_INVALID_HANDLE; }
        }
        SetStretchBltMode(memory, COLORONCOLOR);
        if (!StretchBlt(memory, 0, 0, int(width), int(height), display, r.left, r.top, sourceW, sourceH, SRCCOPY | CAPTUREBLT) || !GdiFlush()) {
            error = GetLastError(); clearImage(); return error ? error : ERROR_ACCESS_DENIED;
        }
        response.x = r.left; response.y = r.top; response.width = sourceW; response.height = sourceH;
        response.pixelWidth = width; response.pixelHeight = height; response.bytes = width * height * 4;
        auto *bytes = static_cast<BYTE *>(pixels); out.assign(bytes, bytes + response.bytes);
        capturedCurrentDesktop = true;
        return ERROR_SUCCESS;
    }
    DWORD input(const Request &request) {
        DWORD error = select(); if (error) return error;
        // Drop the event that discovers a local desktop switch and all events
        // before its first successful capture. This is a local transition guard,
        // not an end-to-end viewer frame/desktop generation acknowledgement.
        if (!capturedCurrentDesktop) { release(); return ERROR_RETRY; }
        auto kind = static_cast<InputKind>(request.args[0]);
        if (kind == InputKind::Key) {
            int key = request.args[1], down = request.args[2];
            if (key < 1 || key > 255 || (down != 0 && down != 1)) return ERROR_INVALID_DATA;
            bool ok = emitKey(WORD(key), down != 0); if (ok) keys[unsigned(key)] = down != 0;
            return ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
        }
        if (kind != InputKind::Move && kind != InputKind::Button && kind != InputKind::Wheel) return ERROR_INVALID_DATA;
        int nx = request.args[3], ny = request.args[4], button = request.args[5], down = request.args[2], steps = request.args[6];
        if (nx < 0 || nx > 1000000 || ny < 0 || ny > 1000000) return ERROR_INVALID_DATA;
        if (kind == InputKind::Button && (button < 1 || button > 3 || (down != 0 && down != 1))) return ERROR_INVALID_DATA;
        if (kind == InputKind::Wheel && (!steps || steps < -10 || steps > 10)) return ERROR_INVALID_DATA;
        POINT origin{}; MONITORINFO info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info)) return GetLastError();
        int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN), virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (virtualW <= 1 || virtualH <= 1) return ERROR_INVALID_DATA;
        auto r = info.rcMonitor;
        LONG x = r.left + LONG((std::int64_t(nx) * (r.right - r.left - 1) + 500000) / 1000000);
        LONG y = r.top + LONG((std::int64_t(ny) * (r.bottom - r.top - 1) + 500000) / 1000000);
        INPUT move{}; move.type = INPUT_MOUSE;
        move.mi.dx = LONG(std::int64_t(x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / (virtualW - 1));
        move.mi.dy = LONG(std::int64_t(y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / (virtualH - 1));
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        if (SendInput(1, &move, sizeof(move)) != 1) return ERROR_ACCESS_DENIED;
        if (kind == InputKind::Button) {
            bool ok = emitButton(button, down != 0); if (ok) buttons[unsigned(button)] = down != 0;
            return ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
        }
        if (kind == InputKind::Wheel) {
            INPUT wheel{}; wheel.type = INPUT_MOUSE; wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
            wheel.mi.mouseData = DWORD(steps * WHEEL_DELTA);
            if (SendInput(1, &wheel, sizeof(wheel)) != 1) return ERROR_ACCESS_DENIED;
        }
        return ERROR_SUCCESS;
    }
};

int workerMain(DWORD requestedSession, const std::wstring &stopName) {
    DWORD session = 0;
    if (!isSystem() || !ProcessIdToSessionId(GetCurrentProcessId(), &session) || session != requestedSession ||
        session != WTSGetActiveConsoleSessionId() || stopName.rfind(L"Global\\LanDeskUnlock-", 0) != 0) return ERROR_ACCESS_DENIED;
    auto sid = ownerSid(); if (sid.empty() || !allowOwnerToVerify(sid)) return ERROR_ACCESS_DENIED;
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stopName.c_str())); if (!stop) return int(GetLastError());
    Handle pipe(createPipe(workerPipe(session), sid)); if (!pipe) return int(GetLastError());
    if (!accept(pipe.h, stop.h, 30000)) return ERROR_TIMEOUT;
    Request first{};
    if (!transfer(pipe.h, &first, sizeof(first), false, stop.h, 3000) || !validRequest(first) || first.op != Op::Connect) return ERROR_INVALID_DATA;
    DWORD clientSession = 0;
    if (!verifyClient(pipe.h, sid, clientSession) || clientSession != session) return ERROR_ACCESS_DENIED;
    Response hello = responseFor(first); hello.session = session;
    if (!transfer(pipe.h, &hello, sizeof(hello), true, stop.h, 3000)) return ERROR_BROKEN_PIPE;
    Desktop desktop;
    for (;;) {
        Request request{};
        if (!transfer(pipe.h, &request, sizeof(request), false, stop.h, 30000) || !validRequest(request)) break;
        if (session != WTSGetActiveConsoleSessionId()) break; // Never control another fast-user-switch session.
        Response response = responseFor(request); response.session = session;
        std::vector<BYTE> image;
        switch (request.op) {
        case Op::Capture: response.status = desktop.capture(response, image); break;
        case Op::Input: response.status = desktop.input(request); break;
        case Op::Release: response.status = desktop.select(); desktop.release(); break;
        case Op::Wake:
            response.status = desktop.select();
            if (!response.status) {
                if (!SetThreadExecutionState(ES_DISPLAY_REQUIRED)) response.status = ERROR_GEN_FAILURE;
            }
            break;
        default: response.status = ERROR_INVALID_FUNCTION; break;
        }
        if (response.status) { response.bytes = 0; image.clear(); }
        if (!transfer(pipe.h, &response, sizeof(response), true, stop.h, 3000)) break;
        if (!image.empty() && !transfer(pipe.h, image.data(), DWORD(image.size()), true, stop.h, 5000)) break;
    }
    desktop.select(); desktop.release(); DisconnectNamedPipe(pipe.h); return 0;
}

SERVICE_STATUS_HANDLE serviceHandle = nullptr;
SERVICE_STATUS serviceStatus{};
HANDLE serviceStop = nullptr;
void reportState(DWORD state, DWORD code = ERROR_SUCCESS) {
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = state; serviceStatus.dwWin32ExitCode = code;
    serviceStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    serviceStatus.dwWaitHint = state == SERVICE_STOP_PENDING ? 10000 : 0;
    if (serviceHandle) SetServiceStatus(serviceHandle, &serviceStatus);
}
DWORD WINAPI controlService(DWORD control, DWORD, void *, void *) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportState(SERVICE_STOP_PENDING); if (serviceStop) SetEvent(serviceStop); return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}
std::wstring stopEventName() {
    std::array<BYTE, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), ULONG(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    std::wstring result = L"Global\\LanDeskUnlock-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (BYTE b : random) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
HANDLE spawnWorker(DWORD session, const std::wstring &stopName) {
    if (!enablePrivilege(SE_TCB_NAME) || !enablePrivilege(SE_INCREASE_QUOTA_NAME) || !enablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME)) return nullptr;
    Handle current, token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &current.h) ||
        !DuplicateTokenEx(current.h, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &token.h) ||
        !SetTokenInformation(token.h, TokenSessionId, &session, sizeof(session))) return nullptr;
    std::wstring executable = installedImage();
    std::wstring command = L"\"" + executable + L"\" --worker " + std::to_wstring(session) + L" " + stopName;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); wchar_t desktop[] = L"winsta0\\default"; startup.lpDesktop = desktop;
    PROCESS_INFORMATION process{};
    // Windows forbids handle inheritance across sessions. The worker uses its
    // own restricted local pipe instead; no handle/path is supplied by clients.
    if (!CreateProcessAsUserW(token.h, executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, installDirectory().c_str(), &startup, &process)) return nullptr;
    CloseHandle(process.hThread); return process.hProcess;
}
void WINAPI serviceMain(DWORD, LPWSTR *) {
    serviceHandle = RegisterServiceCtrlHandlerExW(ServiceName, controlService, nullptr);
    if (!serviceHandle) return;
    reportState(SERVICE_START_PENDING);
    auto sid = ownerSid();
    if (!isSystem() || sid.empty() || _wcsicmp(imagePath().c_str(), installedImage().c_str()) || !allowOwnerToVerify(sid)) {
        reportState(SERVICE_STOPPED, ERROR_ACCESS_DENIED); return;
    }
    auto eventName = stopEventName(); Security eventAcl(L"D:P(A;;GA;;;SY)");
    SetLastError(ERROR_SUCCESS);
    Handle stop(eventName.empty() || !eventAcl.descriptor ? nullptr : CreateEventW(&eventAcl.attributes, TRUE, FALSE, eventName.c_str()));
    if (!stop || GetLastError() == ERROR_ALREADY_EXISTS) { reportState(SERVICE_STOPPED, ERROR_ACCESS_DENIED); return; }
    serviceStop = stop.h; reportState(SERVICE_RUNNING);
    while (WaitForSingleObject(stop.h, 0) != WAIT_OBJECT_0) {
        Handle pipe(createPipe(BrokerPipe, sid));
        if (!pipe) { reportState(SERVICE_STOPPED, GetLastError()); serviceStop = nullptr; return; }
        if (!accept(pipe.h, stop.h, INFINITE)) break;
        Request request{};
        if (!transfer(pipe.h, &request, sizeof(request), false, stop.h, 3000) || !validRequest(request) || request.op != Op::Connect) continue;
        DWORD session = 0;
        if (!verifyClient(pipe.h, sid, session)) continue;
        Handle worker(spawnWorker(session, eventName));
        Response response = responseFor(request); response.session = session;
        response.status = worker ? ERROR_SUCCESS : (GetLastError() ? GetLastError() : ERROR_PROCESS_ABORTED);
        transfer(pipe.h, &response, sizeof(response), true, stop.h, 3000);
        DisconnectNamedPipe(pipe.h);
        if (worker) {
            HANDLE events[]{worker.h, stop.h};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                // Worker IO observes the same stop event, releases input and exits.
                WaitForSingleObject(worker.h, 10000); break;
            }
        }
    }
    serviceStop = nullptr; reportState(SERVICE_STOPPED);
}

bool stopExistingService(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{}; DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status), sizeof(status), &bytes)) return false;
    if (status.dwCurrentState == SERVICE_STOPPED) return true;
    SERVICE_STATUS ignored{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) return false;
    ULONGLONG deadline = GetTickCount64() + 15000;
    do {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status), sizeof(status), &bytes)) return false;
        if (status.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    SetLastError(ERROR_TIMEOUT); return false;
}
int install(const std::wstring &sid) {
    if (!validSid(sid)) return ERROR_INVALID_SID;
    auto directory = installDirectory(), destination = installedImage(), source = imagePath();
    if (directory.empty() || source.empty()) return ERROR_PATH_NOT_FOUND;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager) return int(GetLastError());
    SC_HANDLE existing = OpenServiceW(manager, ServiceName, SERVICE_QUERY_STATUS);
    if (existing) { CloseServiceHandle(existing); CloseServiceHandle(manager); return ERROR_SERVICE_EXISTS; }
    HKEY priorRegistry = nullptr;
    LONG registryExists = RegOpenKeyExW(HKEY_LOCAL_MACHINE, RegistryKey, 0, KEY_READ, &priorRegistry);
    if (priorRegistry) RegCloseKey(priorRegistry);
    if (registryExists != ERROR_FILE_NOT_FOUND) {
        CloseServiceHandle(manager); return registryExists == ERROR_SUCCESS ? ERROR_ALREADY_EXISTS : int(registryExists);
    }
    // Never elevate writes through a pre-existing directory/reparse point.
    if (!CreateDirectoryW(directory.c_str(), nullptr)) { DWORD e = GetLastError(); CloseServiceHandle(manager); return int(e); }
    Security directoryAcl(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)");
    if (!directoryAcl.descriptor || !SetFileSecurityW(directory.c_str(), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, directoryAcl.descriptor) ||
            !CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
        DWORD e = GetLastError(); DeleteFileW(destination.c_str()); RemoveDirectoryW(directory.c_str()); CloseServiceHandle(manager); return int(e);
    }
    HKEY registry = nullptr;
    Security registryAcl(L"D:P(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)");
    LONG result = registryAcl.descriptor ? RegCreateKeyExW(HKEY_LOCAL_MACHINE, RegistryKey, 0, nullptr, 0,
        KEY_SET_VALUE | WRITE_DAC, &registryAcl.attributes, &registry, nullptr) : ERROR_INVALID_SECURITY_DESCR;
    if (result == ERROR_SUCCESS) {
        result = RegSetValueExW(registry, L"OwnerSid", 0, REG_SZ, reinterpret_cast<const BYTE *>(sid.c_str()), DWORD((sid.size() + 1) * sizeof(wchar_t)));
        if (result == ERROR_SUCCESS) result = RegSetKeySecurity(registry, DACL_SECURITY_INFORMATION, registryAcl.descriptor);
        RegCloseKey(registry);
    }
    std::wstring command = L"\"" + destination + L"\" --service";
    SC_HANDLE service = result == ERROR_SUCCESS ? CreateServiceW(manager, ServiceName, L"LanDesk Remote Unlock Helper", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr) : nullptr;
    if (!service && result == ERROR_SUCCESS) result = LONG(GetLastError());
    if (service) {
        Security serviceAcl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0014;;;" + sid + L")");
        if (!serviceAcl.descriptor || !SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, serviceAcl.descriptor)) result = LONG(GetLastError());
        SERVICE_DESCRIPTIONW description{};
        wchar_t text[] = L"Optional LanDesk secure desktop capture/input for the installing user's active console session. No network listener. Start on demand; removable using the LanDesk uninstall helper.";
        description.lpDescription = text; ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
        if (result != ERROR_SUCCESS) DeleteService(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    if (result != ERROR_SUCCESS) {
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryKey); DeleteFileW(destination.c_str()); RemoveDirectoryW(directory.c_str());
    }
    return int(result);
}
int uninstall() {
    auto directory = installDirectory(), executable = installedImage();
    DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return int(GetLastError());
    SC_HANDLE service = OpenServiceW(manager, ServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    DWORD error = ERROR_SUCCESS;
    if (service) {
        if (!stopExistingService(service) || !DeleteService(service)) error = GetLastError();
        CloseServiceHandle(service);
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) error = GetLastError();
    CloseServiceHandle(manager);
    if (error) return int(error);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, RegistryKey);
    if (!DeleteFileW(executable.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return int(GetLastError());
    if (!RemoveDirectoryW(directory.c_str()) && GetLastError() != ERROR_PATH_NOT_FOUND) return int(GetLastError());
    return 0;
}
int elevate(const std::wstring &arguments) {
    auto path = imagePath();
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas"; info.lpFile = path.c_str(); info.lpParameters = arguments.c_str(); info.nShow = SW_SHOW;
    if (!ShellExecuteExW(&info)) return int(GetLastError());
    Handle process(info.hProcess); if (!process) return ERROR_PROCESS_ABORTED;
    WaitForSingleObject(process.h, INFINITE); DWORD code = ERROR_PROCESS_ABORTED;
    GetExitCodeProcess(process.h, &code); return int(code);
}
}
int wmain(int argc, wchar_t **argv) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (argc == 2 && !wcscmp(argv[1], L"--service")) {
        SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(ServiceName), serviceMain}, {nullptr, nullptr}};
        return StartServiceCtrlDispatcherW(table) ? 0 : int(GetLastError());
    }
    if (argc == 4 && !wcscmp(argv[1], L"--worker")) {
        wchar_t *end = nullptr; unsigned long session = wcstoul(argv[2], &end, 10);
        if (!end || *end || session == 0 || session == 0xffffffffUL) return ERROR_INVALID_PARAMETER;
        SetProcessDPIAware(); return workerMain(DWORD(session), argv[3]);
    }
    int result = ERROR_INVALID_PARAMETER;
    if (argc == 2 && !wcscmp(argv[1], L"--install")) {
        auto sid = currentSid(); result = sid.empty() ? ERROR_INVALID_SID : elevate(L"--install-elevated " + sid);
    } else if (argc == 3 && !wcscmp(argv[1], L"--install-elevated")) result = install(argv[2]);
    else if (argc == 2 && !wcscmp(argv[1], L"--uninstall")) result = elevate(L"--uninstall-elevated");
    else if (argc == 2 && !wcscmp(argv[1], L"--uninstall-elevated")) result = uninstall();
    else if (argc == 2 && !wcscmp(argv[1], L"--version")) { std::puts("LanDesk optional Windows unlock helper protocol 1"); return 0; }
    else { std::puts("Usage: landesk-helper.exe --install | --uninstall | --version"); return result; }
    if (!result) std::puts("Operation completed. Restart LanDesk after installing/removing the optional unlock helper.");
    else std::fprintf(stderr, "Operation failed: Windows error %d. Installation requires UAC approval. An existing installation must be uninstalled first.\n", result);
    return result;
}
