#include "RobotCom.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <windows.h>

// Read-only C ABI over the supplied x64 vendor library. No motion/IO exports.
using STEPROBOTSDK::RobotComClient;
struct Client { RobotComClient sdk; bool initialized = false; };
#define API extern "C" __declspec(dllexport)
constexpr int Invalid = -1000;
constexpr int Exception = -1001;
constexpr int Disconnected = -1002;

API int step_abi_version() noexcept { return 1; }
API int step_create(void** output) noexcept {
    if (!output) return Invalid;
    *output = nullptr;
    try { *output = new Client(); return 0; } catch (...) { return Exception; }
}
API void step_destroy(void* handle) noexcept {
    if (!handle) return;
    auto client = static_cast<Client*>(handle);
    try { if (client->initialized) client->sdk.close(); } catch (...) {}
    try { delete client; } catch (...) {}
}
API int step_connect(void* handle, const char* ip, unsigned short port) noexcept {
    if (!handle || !ip || !port) return Invalid;
    auto& client = *static_cast<Client*>(handle);
    if (client.initialized) return Invalid;
    try {
        int result = client.sdk.init(ip, port);
        client.initialized = result == 0;
        return result;
    } catch (...) { return Exception; }
}
API int step_connected(void* handle) noexcept {
    if (!handle) return 0;
    auto& client = *static_cast<Client*>(handle);
    // This C++ release returns 0 for a running link (EasyTcpClient::runStatus).
    // Confirmed against the local virtual controller; it is not a boolean getter.
    try { return client.initialized && client.sdk.ConnectStatus() == 0 ? 1 : 0; }
    catch (...) { return 0; }
}
API int step_connection_status(void* handle) noexcept {
    if (!handle) return Invalid;
    auto& client = *static_cast<Client*>(handle);
    if (!client.initialized) return Disconnected;
    try { return client.sdk.ConnectStatus(); } catch (...) { return Exception; }
}
// values: six joints, six WORLD Cartesian components, override percentage.
// flags: enabled, operation mode (vendor ordinal), current line, message ID.
API int step_read(void* handle, double* values, int valueCount, int* flags, int flagCount) noexcept {
    if (!handle || !values || valueCount != 13 || !flags || flagCount != 4) return Invalid;
    if (!step_connected(handle)) return Disconnected;
    try {
        auto& sdk = static_cast<Client*>(handle)->sdk;
        const auto joints = sdk.getAxisPos();
        const auto cart = sdk.getCartPosWorld();
        std::copy_n(joints.m_Joint, 6, values);
        std::copy_n(cart.cart, 6, values + 6);
        values[12] = sdk.getOverride();
        flags[0] = sdk.getMotorEnableState();
        flags[1] = static_cast<int>(sdk.getOperationMode());
        flags[2] = sdk.getCurrentLine();
        flags[3] = sdk.getMessageData().m_MessageID;
        return step_connected(handle) ? 0 : Disconnected;
    } catch (...) { return Exception; }
}
API int step_text(void* handle, int field, char* buffer, int capacity) noexcept {
    if (!handle || !buffer || capacity < 2) return Invalid;
    buffer[0] = '\0';
    if (field != 0 && !step_connected(handle)) return Disconnected;
    try {
        auto& sdk = static_cast<Client*>(handle)->sdk;
        std::string raw;
        switch (field) {
        case 0: raw = sdk.SDKVersion(); break;
        case 1: raw = sdk.getProjectName(); break;
        case 2: raw = sdk.getProgramName(); break;
        case 3: raw = sdk.getToolName(); break;
        case 4: {
            const auto message = sdk.getMessageData();
            raw.assign(message.m_MessageString, strnlen_s(message.m_MessageString, 96));
            break;
        }
        default: return Invalid;
        }
        if (raw.empty()) return 0;
        // This controller emits UTF-8. Preserve valid UTF-8; accept older ANSI releases too.
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        int count = MultiByteToWideChar(codePage, flags, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        if (count <= 0) {
            codePage = CP_ACP;
            flags = 0;
            count = MultiByteToWideChar(codePage, flags, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        }
        if (count <= 0) return Invalid;
        std::wstring wide(count, L'\0');
        MultiByteToWideChar(codePage, flags, raw.data(), static_cast<int>(raw.size()), &wide[0], count);
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0 || bytes >= capacity) return Invalid;
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, buffer, bytes, nullptr, nullptr);
        buffer[bytes] = '\0';
        return 0;
    } catch (...) { return Exception; }
}
