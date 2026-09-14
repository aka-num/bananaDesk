// Test-only CF_UNICODETEXT owner/observer. No network, installer, or service.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {
unsigned long changes = 0;
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLIPBOARDUPDATE) { ++changes; return 0; }
    return DefWindowProcW(window, message, w, l);
}
bool openClipboard(HWND window) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (OpenClipboard(window)) return true;
        Sleep(10);
    }
    return false;
}
std::string readText(HWND window, DWORD &error) {
    if (!openClipboard(window)) { error = GetLastError(); return {}; }
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) { CloseClipboard(); return {}; }
    auto *text = static_cast<const wchar_t *>(GlobalLock(data));
    if (!text) { error = GetLastError(); CloseClipboard(); return {}; }
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size_t(size) : 0, '\0');
    if (size > 0) { WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), size, nullptr, nullptr); result.pop_back(); }
    else error = GetLastError();
    GlobalUnlock(data); CloseClipboard(); return result;
}
bool writeText(HWND window, const std::string &text, DWORD &error) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size < 0 || (!text.empty() && size == 0)) { error = GetLastError(); return false; }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (size_t(size) + 1) * sizeof(wchar_t));
    if (!memory) { error = GetLastError(); return false; }
    auto *target = static_cast<wchar_t *>(GlobalLock(memory));
    if (!target) { error = GetLastError(); GlobalFree(memory); return false; }
    if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), target, size);
    target[size] = 0; GlobalUnlock(memory);
    if (!openClipboard(window)) { error = GetLastError(); GlobalFree(memory); return false; }
    bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    if (!ok) { error = GetLastError(); GlobalFree(memory); }
    CloseClipboard(); return ok;
}
void response(unsigned long sequence, DWORD error, const std::string &text) {
    { std::ofstream output("result-data", std::ios::binary | std::ios::trunc); output.write(text.data(), std::streamsize(text.size())); }
    { std::ofstream output("result-ready.tmp", std::ios::trunc); output << sequence << " " << error << " " << changes << "\n"; }
    MoveFileExW(L"result-ready.tmp", L"result-ready", MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}
int wmain(int argc, wchar_t **argv) {
    wchar_t testFlag[8]{};
    if (argc != 2 || GetEnvironmentVariableW(L"LANDESK_ISOLATED_TEST", testFlag, 8) != 1 || testFlag[0] != L'1') return 2;
    if (!SetCurrentDirectoryW(argv[1])) return 3;
    WNDCLASSW klass{}; klass.lpfnWndProc = windowProc; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = L"BananaDeskClipboardTestProbe";
    if (!RegisterClassW(&klass)) return 4;
    HWND window = CreateWindowW(klass.lpszClassName, L"Test clipboard probe", 0, 0, 0, 0, 0, nullptr, nullptr, klass.hInstance, nullptr);
    if (!window || !AddClipboardFormatListener(window)) return 5;
    unsigned long completed = 0;
    response(0, 0, {});
    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        unsigned long sequence = 0; char operation = 0;
        { std::ifstream input("command"); input >> sequence >> operation; }
        if (sequence > completed) {
            completed = sequence; DWORD error = 0;
            if (operation == 'S') {
                std::ifstream input("command-data", std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                if (text.size() > 1024 * 1024 || !writeText(window, text, error)) { response(sequence, error ? error : ERROR_INVALID_DATA, {}); continue; }
            } else if (operation != 'G') { response(sequence, ERROR_INVALID_FUNCTION, {}); continue; }
            std::string text = readText(window, error);
            response(sequence, error, text);
        }
        Sleep(10);
    }
}
