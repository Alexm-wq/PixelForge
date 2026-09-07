#include "PipeBridge.hpp"

#include <windows.h>
#include <sddl.h>

#include <array>
#include <chrono>
#include <utility>

namespace pixelforge::win32 {

namespace {

constexpr DWORD kPipeBufferBytes = 1024u * 1024u;
constexpr DWORD kBridgeChunkBytes = 64u * 1024u;

class LocalPipeSecurity {
public:
    LocalPipeSecurity() {
        // Codex may launch MCP children with a restricted/low-integrity token.
        // The pipe name is unguessable per PixelForge process and remote clients
        // are explicitly rejected, so granting local Everyone access is safe and
        // avoids a medium-integrity mandatory-label handshake failure.
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;WD)S:(ML;;NW;;;LW)",
                SDDL_REVISION_1,
                &descriptor_,
                nullptr)) {
            attributes_.nLength = sizeof(attributes_);
            attributes_.lpSecurityDescriptor = descriptor_;
            attributes_.bInheritHandle = FALSE;
        }
    }

    ~LocalPipeSecurity() {
        if (descriptor_) LocalFree(descriptor_);
    }

    SECURITY_ATTRIBUTES* get() noexcept {
        return descriptor_ ? &attributes_ : nullptr;
    }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

bool copy_bytes(HANDLE input, HANDLE output, std::atomic_bool& stopped) {
    std::array<char, kBridgeChunkBytes> buffer{};
    while (!stopped.load(std::memory_order_relaxed)) {
        DWORD read = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            return true;
        }
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(output, buffer.data() + offset, read - offset, &written, nullptr) || written == 0) {
                return false;
            }
            offset += written;
        }
    }
    return true;
}

} // namespace

std::wstring make_agent_pipe_name() {
    return L"\\\\.\\pipe\\PixelForge.Agent." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(GetTickCount64());
}

PipeMcpHost::~PipeMcpHost() {
    stop();
}

bool PipeMcpHost::start(std::wstring pipe_name, AgentMcpBindings bindings, std::wstring& error) {
    if (thread_.joinable()) {
        error = L"PixelForge MCP pipe host is already running.";
        return false;
    }
    if (pipe_name.empty()) {
        error = L"PixelForge MCP pipe name is empty.";
        return false;
    }
    pipe_name_ = std::move(pipe_name);
    bindings_ = bindings;
    // A bridge disconnect must never close the real editor window. The GUI
    // refreshes while Codex is active via a lightweight timer instead.
    bindings_.hwnd = nullptr;
    stop_requested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { serve(); });
    return true;
}

void PipeMcpHost::stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (!thread_.joinable()) return;

    // Wake ConnectNamedPipe if the host is idle.
    HANDLE wake = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);

    CancelSynchronousIo(thread_.native_handle());
    thread_.join();
}

void PipeMcpHost::serve() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        LocalPipeSecurity security;
        HANDLE pipe = CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            kPipeBufferBytes,
            kPipeBufferBytes,
            0,
            security.get());
        if (pipe == INVALID_HANDLE_VALUE) return;

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected || stop_requested_.load(std::memory_order_relaxed)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        // Reuse the existing MCP implementation without creating a second
        // PixelDocument. This currently routes the GUI process std handles only
        // for the lifetime of the connected artwork bridge.
        HANDLE old_input = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE old_output = GetStdHandle(STD_OUTPUT_HANDLE);
        SetStdHandle(STD_INPUT_HANDLE, pipe);
        SetStdHandle(STD_OUTPUT_HANDLE, pipe);
        run_mcp_stdio(bindings_);
        SetStdHandle(STD_INPUT_HANDLE, old_input);
        SetStdHandle(STD_OUTPUT_HANDLE, old_output);

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

int run_mcp_bridge_stdio(const std::wstring& pipe_name) {
    if (pipe_name.empty()) return 2;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 80; ++attempt) {
        pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND && error != ERROR_ACCESS_DENIED) return 3;
        WaitNamedPipeW(pipe_name.c_str(), 250);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 4;

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        return 5;
    }

    std::atomic_bool stopped{false};
    std::thread inbound([&] {
        copy_bytes(input, pipe, stopped);
        stopped.store(true, std::memory_order_relaxed);
        CancelIoEx(pipe, nullptr);
    });

    const bool output_ok = copy_bytes(pipe, output, stopped);
    stopped.store(true, std::memory_order_relaxed);
    CancelIoEx(pipe, nullptr);
    CancelSynchronousIo(inbound.native_handle());
    if (inbound.joinable()) inbound.join();
    CloseHandle(pipe);
    return output_ok ? 0 : 6;
}

} // namespace pixelforge::win32
