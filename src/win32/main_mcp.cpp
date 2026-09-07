#include "AgentMcpServer.hpp"
#include "AgentTask.hpp"
#include "CanvasPlayback.hpp"
#include "ImageIO.hpp"
#include "PixelDocument.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace pixelforge;
using pixelforge::win32::ImageData;

namespace {

constexpr int ID_PROMPT = 1001;
constexpr int ID_BEGIN = 1002;
constexpr int ID_WIDTH = 1003;
constexpr int ID_HEIGHT = 1004;
constexpr int ID_ACCEPT = 1005;
constexpr int ID_REJECT = 1006;
constexpr int ID_FINISH = 1007;
constexpr int ID_CONTENT_REF = 1008;
constexpr int ID_STYLE_REF = 1009;
constexpr int ID_SAVE = 1010;
constexpr int ID_UNDO = 1011;
constexpr int ID_REDO = 1012;
constexpr UINT WM_AGENT_UPDATED = WM_APP + 1;
constexpr UINT_PTR DISPLAY_ANIMATION_TIMER = 78;
constexpr UINT DISPLAY_ANIMATION_INTERVAL_MS = 16;

struct AppState {
    PixelDocument document;
    AgentTaskController task{document};
    ImageData content_reference;
    ImageData style_reference;
    std::wstring content_path;
    std::wstring style_path;
    pixelforge::win32::CanvasPlayback playback;
    std::uint32_t active_color = 0xff50c8c8u;
    bool drawing = false;
    RECT canvas_rect{};
};

AppState g_app;
std::mutex g_state_mutex;
HWND g_prompt = nullptr;
HWND g_width = nullptr;
HWND g_height = nullptr;

void reset_playback_to_document_locked() {
    const auto snap = g_app.task.snapshot();
    g_app.playback.reset_to_authoritative(
        snap.id,
        g_app.document.width(),
        g_app.document.height(),
        g_app.document.revision(),
        g_app.document.pixels());
}

bool observe_playback_locked() {
    const auto snap = g_app.task.snapshot();
    const bool canvas_ready = snap.state == TaskState::Accepted || snap.state == TaskState::Finished;
    return g_app.playback.observe_authoritative(
        snap.id,
        canvas_ready,
        g_app.document.width(),
        g_app.document.height(),
        g_app.document.revision(),
        g_app.document.pixels());
}

bool tick_playback_locked() {
    bool changed = observe_playback_locked();
    changed = g_app.playback.tick(GetTickCount64()) || changed;
    return changed;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::wstring get_window_text(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(len) + 1u, L'\0');
    if (len > 0) GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<std::size_t>(len));
    return text;
}

int get_int(HWND hwnd, int fallback) {
    BOOL ok = FALSE;
    const int value = static_cast<int>(GetDlgItemInt(GetParent(hwnd), GetDlgCtrlID(hwnd), &ok, FALSE));
    return ok ? value : fallback;
}

void show_error(HWND hwnd, const std::wstring& text) {
    MessageBoxW(hwnd, text.c_str(), L"PixelForge", MB_OK | MB_ICONERROR);
}

std::wstring choose_open_image(HWND hwnd) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Images (*.png;*.bmp;*.jpg;*.jpeg)\0*.png;*.bmp;*.jpg;*.jpeg\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? path : L"";
}

std::wstring choose_save_png(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"sprite.png";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn) ? path : L"";
}

void draw_text(HDC dc, int x, int y, const std::wstring& text, COLORREF color = RGB(220, 230, 234)) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
}

void fill_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void draw_reference(HDC dc, const RECT& rect, const ImageData& image, const wchar_t* title) {
    fill_rect(dc, rect, RGB(24, 29, 33));
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    draw_text(dc, rect.left + 8, rect.top + 7, title, RGB(111, 220, 215));
    if (!image.valid()) {
        draw_text(dc, rect.left + 8, rect.top + 32, L"No image loaded", RGB(130, 138, 142));
        return;
    }

    const int pad = 10;
    RECT area{rect.left + pad, rect.top + 28, rect.right - pad, rect.bottom - pad};
    const int aw = static_cast<int>(area.right - area.left);
    const int ah = static_cast<int>(area.bottom - area.top);
    const double scale = std::min(static_cast<double>(aw) / image.width, static_cast<double>(ah) / image.height);
    const int dw = std::max(1, static_cast<int>(image.width * scale));
    const int dh = std::max(1, static_cast<int>(image.height * scale));
    const int dx = static_cast<int>(area.left) + (aw - dw) / 2;
    const int dy = static_cast<int>(area.top) + (ah - dh) / 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = image.width;
    bmi.bmiHeader.biHeight = -image.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, image.width, image.height,
                  image.bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

void draw_canvas(HDC dc, RECT area) {
    fill_rect(dc, area, RGB(18, 22, 25));
    draw_text(dc, static_cast<int>(area.left) + 8, static_cast<int>(area.top) + 7, L"CANVAS", RGB(111, 220, 215));

    const int width = g_app.document.width();
    const int height = g_app.document.height();
    if (width <= 0 || height <= 0) {
        draw_text(dc, static_cast<int>(area.left) + 8, static_cast<int>(area.top) + 35,
                  L"Agent must accept a task and choose the canvas size.", RGB(140, 148, 152));
        g_app.canvas_rect = {};
        return;
    }

    RECT usable{area.left + 10, area.top + 30, area.right - 10, area.bottom - 36};
    const int uw = static_cast<int>(usable.right - usable.left);
    const int uh = static_cast<int>(usable.bottom - usable.top);
    if (uw <= 0 || uh <= 0) { g_app.canvas_rect = {}; return; }
    const double fit = std::min(static_cast<double>(uw) / width, static_cast<double>(uh) / height);
    const int cell = fit >= 1 ? std::min(32, static_cast<int>(fit)) : 0;
    const int draw_w = cell ? width * cell : std::max(1, static_cast<int>(width * fit));
    const int draw_h = cell ? height * cell : std::max(1, static_cast<int>(height * fit));
    RECT canvas{usable.left + (uw - draw_w) / 2, usable.top + (uh - draw_h) / 2,
                usable.left + (uw - draw_w) / 2 + draw_w, usable.top + (uh - draw_h) / 2 + draw_h};
    g_app.canvas_rect = canvas;

    const auto& display_pixels = g_app.playback.pixels_or(g_app.document.pixels(), width, height);
    std::vector<std::uint32_t> preview(display_pixels.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            const auto argb = display_pixels[index];
            const unsigned a = argb >> 24;
            const unsigned bg = ((x + y) & 1) ? 42 : 54;
            auto blend = [&](unsigned c, unsigned b) { return (c * a + b * (255 - a) + 127) / 255; };
            preview[index] =
                (blend((argb >> 16) & 255, bg) << 16) |
                (blend((argb >> 8) & 255, bg + 4) << 8) | blend(argb & 255, bg + 7);
        }
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, canvas.left, canvas.top, draw_w, draw_h, 0, 0, width, height,
                  preview.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    if (cell >= 8) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(62, 68, 72));
        HGDIOBJ old = SelectObject(dc, pen);
        for (int x = 0; x <= width; ++x) {
            MoveToEx(dc, canvas.left + x * cell, canvas.top, nullptr);
            LineTo(dc, canvas.left + x * cell, canvas.bottom);
        }
        for (int y = 0; y <= height; ++y) {
            MoveToEx(dc, canvas.left, canvas.top + y * cell, nullptr);
            LineTo(dc, canvas.right, canvas.top + y * cell);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    const auto snap = g_app.task.snapshot();
    std::wstring info = std::to_wstring(width) + L"x" + std::to_wstring(height) +
                        L"   revision " + std::to_wstring(snap.document_revision);
    if (g_app.playback.animating()) info += L"   drawing...";
    draw_text(dc, static_cast<int>(area.left) + 8, static_cast<int>(area.bottom) - 24, info, RGB(160, 168, 172));
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    {
        std::lock_guard lock(g_state_mutex);
        RECT client{};
        GetClientRect(hwnd, &client);
        fill_rect(dc, client, RGB(14, 18, 20));

        const int left = 340;
        const int right = 330;
        RECT canvas{left + 8, 8, client.right - right - 8, client.bottom - 8};
        draw_canvas(dc, canvas);

        const int rx = static_cast<int>(client.right) - right;
        const int ref_height = std::max(180, static_cast<int>((client.bottom - 30) / 2));
        RECT content{rx + 8, 8, client.right - 8, 8 + ref_height};
        RECT style{rx + 8, 16 + ref_height, client.right - 8, client.bottom - 8};
        draw_reference(dc, content, g_app.content_reference, L"CONTENT REFERENCE");
        draw_reference(dc, style, g_app.style_reference, L"STYLE REFERENCE");

        const auto snap = g_app.task.snapshot();
        draw_text(dc, 12, 305, L"TASK STATE", RGB(111, 220, 215));
        draw_text(dc, 12, 326, utf8_to_wide(task_state_name(snap.state)),
                  snap.state == TaskState::Rejected || snap.state == TaskState::Aborted
                      ? RGB(235, 110, 105) : RGB(210, 220, 224));
        if (!snap.status_message.empty()) {
            RECT status{12, 350, 324, 430};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(156, 164, 168));
            const auto status_text = utf8_to_wide(snap.status_message);
            DrawTextW(dc, status_text.c_str(), -1, &status, DT_WORDBREAK);
        }

        draw_text(dc, 12, 485, L"PALETTE", RGB(111, 220, 215));
        constexpr std::array<std::uint32_t, 8> palette = {
            0xff11181cu, 0xff36505au, 0xff557983u, 0xff78aab0u,
            0xff50c8c8u, 0xff8eece6u, 0xffd8d7cbu, 0xff9b5f66u
        };
        for (std::size_t i = 0; i < palette.size(); ++i) {
            RECT r{
                12 + static_cast<int>(i % 4) * 48,
                510 + static_cast<int>(i / 4) * 48,
                52 + static_cast<int>(i % 4) * 48,
                550 + static_cast<int>(i / 4) * 48
            };
            const auto p = palette[i];
            fill_rect(dc, r, RGB((p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff));
            if (p == g_app.active_color) FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        }
    }
    EndPaint(hwnd, &ps);
}

bool map_canvas_point_locked(int mx, int my, int& px, int& py) {
    const RECT& r = g_app.canvas_rect;
    if (r.right <= r.left || mx < r.left || mx >= r.right || my < r.top || my >= r.bottom) return false;
    const int w = g_app.document.width();
    const int h = g_app.document.height();
    if (w <= 0 || h <= 0) return false;
    px = (mx - static_cast<int>(r.left)) * w / static_cast<int>(r.right - r.left);
    py = (my - static_cast<int>(r.top)) * h / static_cast<int>(r.bottom - r.top);
    return px >= 0 && py >= 0 && px < w && py < h;
}

void paint_pixel(HWND hwnd, int mx, int my, bool erase) {
    {
        std::lock_guard lock(g_state_mutex);
        if (g_app.task.state() != TaskState::Accepted) return;
        int px = 0, py = 0;
        if (!map_canvas_point_locked(mx, my, px, py)) return;
        auto tx = g_app.document.begin_transaction();
        tx.set_pixel(px, py, erase ? 0x00000000u : g_app.active_color);
        if (tx.commit()) reset_playback_to_document_locked();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void choose_palette(int mx, int my) {
    constexpr std::array<std::uint32_t, 8> palette = {
        0xff11181cu, 0xff36505au, 0xff557983u, 0xff78aab0u,
        0xff50c8c8u, 0xff8eece6u, 0xffd8d7cbu, 0xff9b5f66u
    };
    std::lock_guard lock(g_state_mutex);
    for (std::size_t i = 0; i < palette.size(); ++i) {
        RECT r{
            12 + static_cast<int>(i % 4) * 48,
            510 + static_cast<int>(i / 4) * 48,
            52 + static_cast<int>(i % 4) * 48,
            550 + static_cast<int>(i / 4) * 48
        };
        POINT p{mx, my};
        if (PtInRect(&r, p)) g_app.active_color = palette[i];
    }
}

void load_reference(HWND hwnd, bool style) {
    const auto path = choose_open_image(hwnd);
    if (path.empty()) return;
    ImageData data;
    std::wstring error;
    if (!pixelforge::win32::load_image_wic(path, data, error)) {
        show_error(hwnd, error);
        return;
    }

    ReferenceSlot slot;
    slot.path = wide_to_utf8(path);
    slot.width = data.width;
    slot.height = data.height;
    slot.present = true;
    {
        std::lock_guard lock(g_state_mutex);
        if (style) {
            g_app.style_reference = std::move(data);
            g_app.style_path = path;
            g_app.task.set_style_reference(std::move(slot));
        } else {
            g_app.content_reference = std::move(data);
            g_app.content_path = path;
            g_app.task.set_content_reference(std::move(slot));
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void save_current_png(HWND hwnd) {
    const auto path = choose_save_png(hwnd);
    if (path.empty()) return;
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    {
        std::lock_guard lock(g_state_mutex);
        width = g_app.document.width();
        height = g_app.document.height();
        pixels = g_app.document.pixels();
    }
    std::wstring error;
    if (!pixelforge::win32::save_png_wic(path, width, height, pixels, error)) show_error(hwnd, error);
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            CreateWindowW(L"STATIC", L"USER PROMPT", WS_CHILD | WS_VISIBLE, 12, 10, 180, 20, hwnd, nullptr, nullptr, nullptr);
            g_prompt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                12, 32, 312, 120, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROMPT)), nullptr, nullptr);
            SendMessageW(g_prompt, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            auto button = [&](const wchar_t* text, int id, int x, int y, int w = 96) {
                HWND h = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       x, y, w, 28, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            };
            button(L"Begin task", ID_BEGIN, 12, 162, 312);
            CreateWindowW(L"STATIC", L"W", WS_CHILD | WS_VISIBLE, 12, 202, 18, 20, hwnd, nullptr, nullptr, nullptr);
            g_width = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"64", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                      32, 198, 58, 24, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WIDTH)), nullptr, nullptr);
            CreateWindowW(L"STATIC", L"H", WS_CHILD | WS_VISIBLE, 102, 202, 18, 20, hwnd, nullptr, nullptr, nullptr);
            g_height = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"64", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                       122, 198, 58, 24, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_HEIGHT)), nullptr, nullptr);
            button(L"Accept", ID_ACCEPT, 192, 196, 64);
            button(L"Reject", ID_REJECT, 260, 196, 64);
            button(L"Finish", ID_FINISH, 12, 232, 96);
            button(L"Undo", ID_UNDO, 116, 232, 96);
            button(L"Redo", ID_REDO, 220, 232, 96);
            button(L"Content ref", ID_CONTENT_REF, 12, 440, 96);
            button(L"Style ref", ID_STYLE_REF, 116, 440, 96);
            button(L"Save PNG", ID_SAVE, 220, 440, 96);
            SetTimer(hwnd, DISPLAY_ANIMATION_TIMER, DISPLAY_ANIMATION_INTERVAL_MS, nullptr);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            if (id == ID_BEGIN) {
                {
                    std::lock_guard lock(g_state_mutex);
                    g_app.task.begin(wide_to_utf8(get_window_text(g_prompt)));
                    g_app.playback.note_task(g_app.task.snapshot().id);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_ACCEPT) {
                const int width = get_int(g_width, 64);
                const int height = get_int(g_height, 64);
                std::string error;
                {
                    std::lock_guard lock(g_state_mutex);
                    if (g_app.task.accept(width, height, &error)) reset_playback_to_document_locked();
                }
                if (!error.empty()) show_error(hwnd, utf8_to_wide(error));
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_REJECT) {
                std::string error;
                {
                    std::lock_guard lock(g_state_mutex);
                    g_app.task.reject("Manually rejected from the debug UI. MCP agents provide their own semantic reason.", &error);
                }
                if (!error.empty()) show_error(hwnd, utf8_to_wide(error));
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_FINISH) {
                std::string error;
                {
                    std::lock_guard lock(g_state_mutex);
                    g_app.task.finish("Task finished from the debug UI.", &error);
                }
                if (!error.empty()) show_error(hwnd, utf8_to_wide(error));
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_CONTENT_REF) {
                load_reference(hwnd, false);
            } else if (id == ID_STYLE_REF) {
                load_reference(hwnd, true);
            } else if (id == ID_SAVE) {
                save_current_png(hwnd);
            } else if (id == ID_UNDO) {
                {
                    std::lock_guard lock(g_state_mutex);
                    if (g_app.document.undo()) reset_playback_to_document_locked();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == ID_REDO) {
                {
                    std::lock_guard lock(g_state_mutex);
                    if (g_app.document.redo()) reset_playback_to_document_locked();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            choose_palette(x, y);
            paint_pixel(hwnd, x, y, false);
            SetCapture(hwnd);
            g_app.drawing = true;
            return 0;
        }
        case WM_RBUTTONDOWN:
            paint_pixel(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
            return 0;
        case WM_MOUSEMOVE:
            if (g_app.drawing && (wparam & MK_LBUTTON))
                paint_pixel(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
            return 0;
        case WM_LBUTTONUP:
            g_app.drawing = false;
            ReleaseCapture();
            return 0;
        case WM_AGENT_UPDATED: {
            {
                std::lock_guard lock(g_state_mutex);
                observe_playback_locked();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wparam == DISPLAY_ANIMATION_TIMER) {
                bool changed = false;
                {
                    std::lock_guard lock(g_state_mutex);
                    changed = tick_playback_locked();
                }
                if (changed) InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd, DISPLAY_ANIMATION_TIMER);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool mcp_mode_enabled() {
    const wchar_t* command_line = GetCommandLineW();
    return command_line && std::wstring_view(command_line).find(L"--mcp") != std::wstring_view::npos;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool mcp_mode = mcp_mode_enabled();

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = L"PixelForgeWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    const wchar_t* title = mcp_mode
        ? L"PixelForge - Live Codex MCP Pixel Editor"
        : L"PixelForge - Agent Native Pixel Editor";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 900,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    if (mcp_mode) {
        pixelforge::win32::AgentMcpBindings bindings;
        bindings.document = &g_app.document;
        bindings.task = &g_app.task;
        bindings.content_reference = &g_app.content_reference;
        bindings.style_reference = &g_app.style_reference;
        bindings.content_path = &g_app.content_path;
        bindings.style_path = &g_app.style_path;
        bindings.state_mutex = &g_state_mutex;
        bindings.hwnd = hwnd;
        std::thread([bindings] {
            pixelforge::win32::run_mcp_stdio(bindings);
        }).detach();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(msg.wParam);
}
