#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace aion {
class QueryProxy;
struct ChatBridgeStatus {
    bool installed{};
    bool running{};
    DWORD processId{};
    DWORD exitCode{};
    std::string message;
    std::string logTail;
};

class ChatBridge {
public:
    ChatBridge();
    ~ChatBridge();
    ChatBridge(const ChatBridge&)=delete;
    ChatBridge& operator=(const ChatBridge&)=delete;
    bool start();
    void stop();
    ChatBridgeStatus status();
    void syncGameServer(const QueryProxy& proxy);
private:
    void refresh();
    std::filesystem::path executable_;
    std::filesystem::path log_;
    HANDLE process_{INVALID_HANDLE_VALUE};
    HANDLE job_{nullptr};
    DWORD processId_{};
    DWORD exitCode_{};
    std::string message_;
    ULONGLONG identityTick_{};
};
}
