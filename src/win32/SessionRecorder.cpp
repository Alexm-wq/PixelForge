#include "SessionRecorder.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pixelforge::win32 {

namespace {

std::wstring hresult_message(HRESULT hr) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, hr, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = buffer ? buffer : L"Unknown Media Foundation error.";
    if (buffer) LocalFree(buffer);
    return text;
}

bool set_video_type(IMFMediaType* type, REFGUID subtype, int width, int height, int fps,
                    bool output, std::wstring& error) {
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr) && output) hr = type->SetUINT32(MF_MT_AVG_BITRATE, 8'000'000);
    if (FAILED(hr)) {
        error = hresult_message(hr);
        return false;
    }
    return true;
}

} // namespace

SessionRecorder::~SessionRecorder() {
    std::wstring ignored;
    stop(ignored);
}

bool SessionRecorder::start(HWND hwnd, std::wstring output_path, int fps, std::wstring& error) {
    if (!hwnd || !IsWindow(hwnd)) {
        error = L"PixelForge recording target window is invalid.";
        return false;
    }
    if (fps < 1 || fps > 60) {
        error = L"Recording FPS must be between 1 and 60.";
        return false;
    }
    if (output_path.empty()) {
        error = L"Recording output path is empty.";
        return false;
    }
    if (active_.load(std::memory_order_relaxed) || worker_.joinable()) {
        error = L"A PixelForge recording is already active.";
        return false;
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) {
        error = L"Could not create the recording output directory.";
        return false;
    }

    hwnd_ = hwnd;
    fps_ = fps;
    output_path_ = std::move(output_path);
    stop_requested_.store(false, std::memory_order_relaxed);
    active_.store(false, std::memory_order_relaxed);
    frames_written_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(state_mutex_);
        init_done_ = false;
        init_ok_ = false;
        last_error_.clear();
    }

    worker_ = std::thread([this] { capture_loop(); });

    std::unique_lock lock(state_mutex_);
    init_cv_.wait(lock, [this] { return init_done_; });
    const bool ok = init_ok_;
    error = last_error_;
    lock.unlock();
    if (!ok && worker_.joinable()) worker_.join();
    return ok;
}

bool SessionRecorder::stop(std::wstring& error) {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) worker_.join();
    active_.store(false, std::memory_order_relaxed);
    std::lock_guard lock(state_mutex_);
    error = last_error_;
    return error.empty();
}

std::wstring SessionRecorder::output_path() const {
    std::lock_guard lock(state_mutex_);
    return output_path_;
}

void SessionRecorder::signal_init(bool ok, std::wstring error) {
    {
        std::lock_guard lock(state_mutex_);
        init_done_ = true;
        init_ok_ = ok;
        if (!error.empty()) last_error_ = std::move(error);
    }
    init_cv_.notify_all();
}

void SessionRecorder::capture_loop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        signal_init(false, hresult_message(com));
        return;
    }

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        signal_init(false, hresult_message(hr));
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    RECT window_rect{};
    if (!GetWindowRect(hwnd_, &window_rect)) {
        signal_init(false, L"Could not resolve the PixelForge window bounds for recording.");
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    int width = static_cast<int>(window_rect.right - window_rect.left);
    int height = static_cast<int>(window_rect.bottom - window_rect.top);
    // H.264 encoders commonly require even dimensions.
    width &= ~1;
    height &= ~1;
    if (width < 2 || height < 2) {
        signal_init(false, L"PixelForge window is too small to record.");
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(output_path_.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        signal_init(false, hresult_message(hr));
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    ComPtr<IMFMediaType> output_type;
    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(&output_type);
    std::wstring media_error;
    if (FAILED(hr) || !set_video_type(output_type.Get(), MFVideoFormat_H264, width, height, fps_, true, media_error)) {
        signal_init(false, FAILED(hr) ? hresult_message(hr) : media_error);
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    DWORD stream_index = 0;
    hr = writer->AddStream(output_type.Get(), &stream_index);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input_type);
    if (FAILED(hr) || !set_video_type(input_type.Get(), MFVideoFormat_ARGB32, width, height, fps_, false, media_error)) {
        signal_init(false, FAILED(hr) ? hresult_message(hr) : media_error);
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    if (SUCCEEDED(hr)) hr = input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width * 4));
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream_index, input_type.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) {
        signal_init(false, hresult_message(hr));
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    // Capture from the desktop rather than the client DC. This records the full
    // PixelForge chrome, prompt/input controls, and any owned review popup that
    // is visually over the main window.
    HDC source_dc = GetDC(nullptr);
    HDC memory_dc = source_dc ? CreateCompatibleDC(source_dc) : nullptr;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    // MF_MT_DEFAULT_STRIDE above is positive, so scanline zero must be
    // the top row. A positive DIB height would store the bottom row first
    // and vertically invert the encoded video.
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* dib_pixels = nullptr;
    HBITMAP bitmap = memory_dc ? CreateDIBSection(source_dc, &bmi, DIB_RGB_COLORS, &dib_pixels, nullptr, 0) : nullptr;
    HGDIOBJ old_bitmap = bitmap ? SelectObject(memory_dc, bitmap) : nullptr;
    if (!source_dc || !memory_dc || !bitmap || !dib_pixels) {
        if (old_bitmap) SelectObject(memory_dc, old_bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (memory_dc) DeleteDC(memory_dc);
        if (source_dc) ReleaseDC(nullptr, source_dc);
        writer->Finalize();
        signal_init(false, L"Could not allocate the PixelForge recording capture surface.");
        MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    SetStretchBltMode(memory_dc, COLORONCOLOR);
    active_.store(true, std::memory_order_relaxed);
    signal_init(true);

    const LONGLONG frame_duration = 10'000'000LL / fps_;
    const DWORD frame_bytes = static_cast<DWORD>(width * height * 4);
    auto next_frame = std::chrono::steady_clock::now();
    std::uint64_t frame_index = 0;

    while (!stop_requested_.load(std::memory_order_relaxed) && IsWindow(hwnd_)) {
        next_frame += std::chrono::milliseconds(1000 / fps_);

        RECT current{};
        if (!GetWindowRect(hwnd_, &current)) {
            std::lock_guard lock(state_mutex_);
            last_error_ = L"PixelForge window bounds could not be read during recording.";
            break;
        }
        const int source_width = std::max(1, static_cast<int>(current.right - current.left));
        const int source_height = std::max(1, static_cast<int>(current.bottom - current.top));
        if (!StretchBlt(memory_dc, 0, 0, width, height,
                        source_dc, current.left, current.top, source_width, source_height,
                        SRCCOPY | CAPTUREBLT)) {
            std::lock_guard lock(state_mutex_);
            last_error_ = L"PixelForge window capture failed.";
            break;
        }
        // Complete GDI writes before reading the DIB memory for the encoder.
        GdiFlush();

        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        hr = MFCreateMemoryBuffer(frame_bytes, &buffer);
        BYTE* destination = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, &max_length, &current_length);
        if (SUCCEEDED(hr)) {
            memcpy(destination, dib_pixels, frame_bytes);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(frame_bytes);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(static_cast<LONGLONG>(frame_index) * frame_duration);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(frame_duration);
        if (SUCCEEDED(hr)) hr = writer->WriteSample(stream_index, sample.Get());
        if (FAILED(hr)) {
            std::lock_guard lock(state_mutex_);
            last_error_ = hresult_message(hr);
            break;
        }

        ++frame_index;
        frames_written_.store(frame_index, std::memory_order_relaxed);
        std::this_thread::sleep_until(next_frame);
    }

    hr = writer->Finalize();
    if (FAILED(hr)) {
        std::lock_guard lock(state_mutex_);
        if (last_error_.empty()) last_error_ = hresult_message(hr);
    }

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, source_dc);
    active_.store(false, std::memory_order_relaxed);
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace pixelforge::win32
