#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace pixelforge::win32 {

class LocalAgentToolSession;

struct CodexGenerateRequest {
    std::wstring repo_root;
    std::wstring executable_path;
    std::string prompt;
    std::string agent_contract;
    LocalAgentToolSession* tool_session = nullptr;
};

class CodexAppClient {
public:
    using StatusCallback = std::function<void(std::wstring)>;
    using CompletionCallback = std::function<void(bool, std::wstring)>;

    CodexAppClient() = default;
    CodexAppClient(const CodexAppClient&) = delete;
    CodexAppClient& operator=(const CodexAppClient&) = delete;
    ~CodexAppClient();

    bool generate_async(CodexGenerateRequest request,
                        StatusCallback status,
                        CompletionCallback completion,
                        std::wstring& error);

    void shutdown();
    [[nodiscard]] bool busy() const noexcept { return busy_.load(std::memory_order_relaxed); }

private:
    bool ensure_server(const CodexGenerateRequest& request, std::wstring& error);
    bool launch_server(const CodexGenerateRequest& request, std::wstring& error);
    bool initialize_server(std::wstring& error);
    bool run_generation(const CodexGenerateRequest& request,
                        const StatusCallback& status,
                        std::wstring& error);

    bool request(std::string_view method, std::string_view params_json,
                 std::string& result_json, std::wstring& error);
    bool wait_for_response(std::int64_t id, std::string& result_json, std::wstring& error);
    bool read_line(std::string& line);
    bool write_line(std::string_view line);
    void handle_server_request(std::string_view line);
    void handle_notification(std::string_view line, const StatusCallback* status = nullptr);
    void stop_process();

    mutable std::mutex process_mutex_;
    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_log_ = nullptr;
    DWORD process_id_ = 0;
    std::string receive_buffer_;
    std::int64_t next_request_id_ = 1;

    std::wstring configured_repo_root_;
    std::wstring configured_executable_;
    LocalAgentToolSession* active_tool_session_ = nullptr;

    std::atomic_bool busy_{false};
    std::atomic_bool shutting_down_{false};
    std::thread worker_;
};

} // namespace pixelforge::win32
