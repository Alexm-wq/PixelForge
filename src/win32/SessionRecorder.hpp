#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace pixelforge::win32 {

// Local-only recorder for the visible PixelForge client area. Recorded frames are
// never exposed through MCP; agents can only start/stop/query recording state.
class SessionRecorder {
public:
    SessionRecorder() = default;
    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;
    ~SessionRecorder();

    bool start(HWND hwnd, std::wstring output_path, int fps, std::wstring& error);
    bool stop(std::wstring& error);

    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_written() const noexcept { return frames_written_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::wstring output_path() const;

private:
    void capture_loop();
    void signal_init(bool ok, std::wstring error = {});

    HWND hwnd_ = nullptr;
    int fps_ = 30;
    std::wstring output_path_;

    std::atomic_bool active_{false};
    std::atomic_bool stop_requested_{false};
    std::atomic<std::uint64_t> frames_written_{0};

    mutable std::mutex state_mutex_;
    std::condition_variable init_cv_;
    bool init_done_ = false;
    bool init_ok_ = false;
    std::wstring last_error_;
    std::thread worker_;
};

} // namespace pixelforge::win32
