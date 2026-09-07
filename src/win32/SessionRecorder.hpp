#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace pixelforge::win32 {

// Local-only recorder for the visible PixelForge top-level window. Frames are
// captured from the desktop so owned UI such as the user-review popup is part
// of the demo recording too. Recorded frames are never exposed through MCP.
class SessionRecorder {
public:
    SessionRecorder() = default;
    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;
    ~SessionRecorder();

    bool start(HWND hwnd, std::wstring output_path, int fps, std::wstring& error);
    bool stop(std::wstring& error);

    // Automatic/demo mode keeps a recording alive across agent passes. Agent
    // stop requests become no-ops until the host explicitly permits finalization
    // after the user accepts the final review.
    void set_user_review_gate(bool enabled) noexcept;
    void permit_finalization() noexcept;

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
    std::atomic_bool user_review_gate_{false};
    std::atomic_bool finalization_permitted_{false};

    mutable std::mutex state_mutex_;
    std::condition_variable init_cv_;
    bool init_done_ = false;
    bool init_ok_ = false;
    std::wstring last_error_;
    std::thread worker_;
};

} // namespace pixelforge::win32
