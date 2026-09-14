#pragma once
#include <cstdint>

namespace ldhelper {
constexpr std::uint32_t Magic = 0x4c445535; // LDU5
constexpr std::uint16_t Version = 1;
constexpr std::uint32_t MaxWidth = 1920, MaxHeight = 1080;
constexpr std::uint32_t MaxPixelsBytes = MaxWidth * MaxHeight * 4;
constexpr wchar_t ServiceName[] = L"LanDeskUnlock";
constexpr wchar_t BrokerPipe[] = L"\\\\.\\pipe\\LanDeskUnlock-v1";
constexpr wchar_t RegistryKey[] = L"SOFTWARE\\LanDeskUnlock";
enum class Op : std::uint16_t { Connect = 1, Capture = 2, Input = 3, Release = 4, Wake = 5 };
enum class InputKind : std::int32_t { Move = 1, Button = 2, Wheel = 3, Key = 4 };
struct Request {
    std::uint32_t magic = Magic;
    std::uint16_t version = Version;
    Op op = Op::Connect;
    std::uint32_t sequence = 0;
    std::uint32_t bytes = 0; // Requests never contain a variable payload.
    std::int32_t args[8]{};
};
struct Response {
    std::uint32_t magic = Magic;
    std::uint16_t version = Version;
    Op op = Op::Connect;
    std::uint32_t sequence = 0;
    std::uint32_t status = 0; // A Win32 error code; never credential/input text.
    std::uint32_t bytes = 0;
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    std::uint32_t pixelWidth = 0, pixelHeight = 0;
    std::uint32_t session = 0;
};
static_assert(sizeof(Request) == 48, "Request ABI");
static_assert(sizeof(Response) == 48, "Response ABI");
}
