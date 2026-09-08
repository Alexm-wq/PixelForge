#include "PackWorkspaceUi.hpp"

#include "CanvasPlayback.hpp"
#include "ImageIO.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

constexpr UINT_PTR PACK_TIMER = 91;
constexpr UINT WM_PACK_REFRESH = WM_APP + 120;
constexpr int TAB_CANVAS = 0;
constexpr int TAB_CANVASES = 1;
constexpr int TAB_ANIMATION = 2;
constexpr int TAB_HEIGHT = 44;
constexpr int WORKSPACE_LEFT = 348;
constexpr int WORKSPACE_RIGHT = 338;
constexpr int WORKSPACE_TOP = 8;
constexpr int WORKSPACE_BOTTOM = 8;
constexpr std::uint64_t PACK_PRESENTATION_TASK = 1;

struct PixelSnapshot {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

struct CanvasPresentation {
    PixelSnapshot authoritative;
    CanvasPlayback playback;
};

struct UiState {
    std::uint64_t task_id = 0;
    std::uint64_t revision = 0;
    std::wstring project_directory;
    std::vector<PackUiCanvasInfo> canvases;
    std::unordered_map<std::string, CanvasPresentation> presentations;

    // Animation deliberately lags behind authoritative pack state. It is only
    // replaced after every concurrent canvas presentation has finished revealing
    // the latest pack update, so the player never mixes half-updated frames.
    std::vector<PackUiCanvasInfo> animation_canvases;
    std::unordered_map<std::string, PixelSnapshot> animation_snapshots;
    std::uint64_t animation_revision = 0;
    bool animation_refresh_pending = false;

    std::string selected_canvas;
    std::string selected_group;
    int tab = TAB_CANVAS;
    int fps = 10;
    bool playing = true;
    int animation_frame = 0;
    ULONGLONG last_frame_tick = 0;
    int scroll_y = 0;
};

std::mutex g_ui_mutex;
UiState g_ui;
HWND g_owner = nullptr;
HWND g_tabs_window = nullptr;
HWND g_content_window = nullptr;
HHOOK g_attach_hook = nullptr;
std::vector<std::pair<RECT, std::string>> g_canvas_hits;
std::vector<std::pair<RECT, int>> g_frame_hits;
RECT g_play_rect{};
RECT g_prev_group_rect{};
RECT g_next_group_rect{};
RECT g_fps_down_rect{};
RECT g_fps_up_rect{};

std::wstring utf8_to_wide_ui(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

void fill_rect_ui(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void text_ui(HDC dc, int x, int y, const std::wstring& text, COLORREF color = RGB(220, 230, 234)) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
}

std::filesystem::path canvas_path(std::wstring_view project_directory, const PackUiCanvasInfo& canvas) {
    const std::wstring folder = canvas.group.empty() ? L"ungrouped" : utf8_to_wide_ui(canvas.group);
    return std::filesystem::path(project_directory) / L"canvases" / folder /
           (utf8_to_wide_ui(canvas.name) + L".png");
}

PixelSnapshot argb_snapshot(const ImageData& image) {
    PixelSnapshot out;
    if (!image.valid()) return out;
    out.width = image.width;
    out.height = image.height;
    const std::size_t count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    out.pixels.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t p = i * 4u;
        out.pixels[i] = (static_cast<std::uint32_t>(image.bgra[p + 3]) << 24) |
                        (static_cast<std::uint32_t>(image.bgra[p + 2]) << 16) |
                        (static_cast<std::uint32_t>(image.bgra[p + 1]) << 8) |
                        static_cast<std::uint32_t>(image.bgra[p + 0]);
    }
    return out;
}

bool load_canvas_snapshot(std::wstring_view project_directory,
                          const PackUiCanvasInfo& canvas,
                          PixelSnapshot& snapshot) {
    if (project_directory.empty()) return false;
    ImageData image;
    std::wstring error;
    if (!load_image_wic(canvas_path(project_directory, canvas).wstring(), image, error)) return false;
    snapshot = argb_snapshot(image);
    return snapshot.valid();
}

void draw_pixels_fit(HDC dc,
                     const RECT& area,
                     int width,
                     int height,
                     const std::vector<std::uint32_t>& pixels) {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) return;

    const int aw = std::max(1, static_cast<int>(area.right - area.left));
    const int ah = std::max(1, static_cast<int>(area.bottom - area.top));
    const double fit = std::min(static_cast<double>(aw) / width, static_cast<double>(ah) / height);
    const int scale = fit >= 1.0 ? std::max(1, static_cast<int>(fit)) : 0;
    const int dw = scale ? width * scale : std::max(1, static_cast<int>(width * fit));
    const int dh = scale ? height * scale : std::max(1, static_cast<int>(height * fit));
    const int dx = static_cast<int>(area.left) + (aw - dw) / 2;
    const int dy = static_cast<int>(area.top) + (ah - dh) / 2;

    std::vector<std::uint32_t> composited(pixels.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            const std::uint32_t argb = pixels[i];
            const unsigned a = (argb >> 24) & 0xffu;
            const unsigned r = (argb >> 16) & 0xffu;
            const unsigned g = (argb >> 8) & 0xffu;
            const unsigned b = argb & 0xffu;
            const unsigned bg = ((x + y) & 1) ? 45u : 58u;
            auto blend = [&](unsigned c, unsigned base) { return (c * a + base * (255u - a) + 127u) / 255u; };
            composited[i] = (blend(r, bg) << 16) | (blend(g, bg + 3) << 8) | blend(b, bg + 6);
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
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, width, height,
                  composited.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

std::vector<std::string> animation_groups(const UiState& state) {
    std::vector<std::string> groups;
    for (const auto& canvas : state.animation_canvases) {
        if (canvas.frame < 0 || canvas.group.empty()) continue;
        if (std::find(groups.begin(), groups.end(), canvas.group) == groups.end()) groups.push_back(canvas.group);
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

std::vector<PackUiCanvasInfo> animation_frames(const UiState& state, std::string_view group) {
    std::vector<PackUiCanvasInfo> frames;
    for (const auto& canvas : state.animation_canvases)
        if (canvas.frame >= 0 && canvas.group == group) frames.push_back(canvas);
    std::stable_sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return a.name < b.name;
    });
    return frames;
}

bool any_canvas_animating_locked() {
    for (const auto& [name, presentation] : g_ui.presentations) {
        (void)name;
        if (presentation.playback.animating()) return true;
    }
    return false;
}

void normalize_animation_selection_locked() {
    const auto groups = animation_groups(g_ui);
    if (groups.empty()) {
        g_ui.selected_group.clear();
        g_ui.animation_frame = 0;
        return;
    }
    if (std::find(groups.begin(), groups.end(), g_ui.selected_group) == groups.end()) {
        g_ui.selected_group = groups.front();
        g_ui.animation_frame = 0;
    }
    const auto frames = animation_frames(g_ui, g_ui.selected_group);
    if (frames.empty()) g_ui.animation_frame = 0;
    else g_ui.animation_frame = std::clamp(g_ui.animation_frame, 0, static_cast<int>(frames.size()) - 1);
}

void commit_animation_snapshot_locked() {
    g_ui.animation_canvases.clear();
    g_ui.animation_snapshots.clear();
    for (const auto& canvas : g_ui.canvases) {
        if (canvas.frame < 0 || canvas.group.empty()) continue;
        const auto it = g_ui.presentations.find(canvas.name);
        if (it == g_ui.presentations.end() || !it->second.authoritative.valid()) continue;
        g_ui.animation_canvases.push_back(canvas);
        g_ui.animation_snapshots.emplace(canvas.name, it->second.authoritative);
    }
    g_ui.animation_revision = g_ui.revision;
    g_ui.animation_refresh_pending = false;
    normalize_animation_selection_locked();
    g_ui.last_frame_tick = GetTickCount64();
}

void layout_workspace_locked() {
    if (!g_owner || !IsWindow(g_owner)) return;
    RECT client{};
    GetClientRect(g_owner, &client);
    const int width = std::max(220, static_cast<int>(client.right) - WORKSPACE_LEFT - WORKSPACE_RIGHT);
    const int content_h = std::max(80, static_cast<int>(client.bottom) - WORKSPACE_TOP - WORKSPACE_BOTTOM - TAB_HEIGHT);
    if (g_tabs_window)
        MoveWindow(g_tabs_window, WORKSPACE_LEFT, WORKSPACE_TOP, width, TAB_HEIGHT, TRUE);
    if (g_content_window)
        MoveWindow(g_content_window, WORKSPACE_LEFT, WORKSPACE_TOP + TAB_HEIGHT, width, content_h, TRUE);
}

void apply_page_visibility_locked() {
    if (g_content_window)
        ShowWindow(g_content_window, g_ui.tab == TAB_CANVAS ? SW_HIDE : SW_SHOW);
    if (g_tabs_window) BringWindowToTop(g_tabs_window);
}

RECT parent_canvas_region(HWND owner) {
    RECT client{};
    GetClientRect(owner, &client);
    RECT region{WORKSPACE_LEFT, WORKSPACE_TOP + TAB_HEIGHT,
                client.right - WORKSPACE_RIGHT, client.bottom - WORKSPACE_BOTTOM};
    if (region.right < region.left) region.right = region.left;
    if (region.bottom < region.top) region.bottom = region.top;
    return region;
}

void draw_tabs(HDC dc, const RECT& client, const UiState& state) {
    fill_rect_ui(dc, client, RGB(14, 18, 20));
    const RECT canvas_tab{8, 4, 112, 40};
    const RECT canvases_tab{118, 4, 242, 40};
    const RECT animation_tab{248, 4, 382, 40};
    auto tab = [&](RECT r, const wchar_t* title, bool selected) {
        fill_rect_ui(dc, r, selected ? RGB(45, 91, 94) : RGB(31, 38, 42));
        FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        text_ui(dc, static_cast<int>(r.left) + 12, static_cast<int>(r.top) + 8, title,
                selected ? RGB(230, 248, 247) : RGB(178, 188, 192));
    };
    tab(canvas_tab, L"Canvas", state.tab == TAB_CANVAS);
    tab(canvases_tab, L"Canvases", state.tab == TAB_CANVASES);
    tab(animation_tab, L"Animation", state.tab == TAB_ANIMATION);

    std::wstring status = state.canvases.empty() ? L"Primary canvas" : std::to_wstring(state.canvases.size()) + L" canvases";
    const auto groups = animation_groups(state);
    if (!groups.empty()) status += L"   " + std::to_wstring(groups.size()) + L" animations";
    if (state.animation_refresh_pending) status += L"   drawing...";
    const int sx = std::min(410, std::max(392, static_cast<int>(client.right) - 250));
    text_ui(dc, sx, 14, status, RGB(145, 155, 160));
}

void draw_canvases(HDC dc, const RECT& client, UiState& state) {
    g_canvas_hits.clear();
    if (state.canvases.empty()) {
        text_ui(dc, 18, 24, L"No additional canvases yet.", RGB(111, 220, 215));
        text_ui(dc, 18, 50, L"Astra-created variants and animation frames will appear here immediately.", RGB(145, 152, 156));
        return;
    }

    const int client_w = static_cast<int>(client.right - client.left);
    const int columns = std::max(2, (client_w - 36) / 170);
    const int cell_w = std::max(140, (client_w - 36) / columns);
    const int cell_h = 170;
    int row = 0;
    int col = 0;
    std::string last_group;
    int y = 18 - state.scroll_y;

    std::vector<PackUiCanvasInfo> ordered = state.canvases;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.frame >= 0 && b.frame >= 0 && a.frame != b.frame) return a.frame < b.frame;
        return a.name < b.name;
    });

    for (const auto& canvas : ordered) {
        if (canvas.group != last_group) {
            if (!last_group.empty()) y += cell_h;
            last_group = canvas.group;
            col = 0;
            row = 0;
            text_ui(dc, 18, y, canvas.group.empty() ? L"UNGROUPED" : utf8_to_wide_ui(canvas.group), RGB(111, 220, 215));
            y += 28;
        }
        const int x = 18 + col * cell_w;
        const int cy = y + row * cell_h;
        const LONG right = std::min<LONG>(client.right - 12, static_cast<LONG>(x + cell_w - 12));
        RECT card{static_cast<LONG>(x), static_cast<LONG>(cy), right, static_cast<LONG>(cy + cell_h - 12)};
        if (card.bottom >= 0 && card.top <= client.bottom) {
            fill_rect_ui(dc, card, canvas.name == state.selected_canvas ? RGB(35, 63, 66) : RGB(24, 29, 33));
            FrameRect(dc, &card, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            RECT image_area{card.left + 8, card.top + 8, card.right - 8, card.bottom - 34};
            const auto it = state.presentations.find(canvas.name);
            bool drawing = false;
            if (it != state.presentations.end() && it->second.authoritative.valid()) {
                const auto& presentation = it->second;
                const auto& pixels = presentation.playback.pixels_or(
                    presentation.authoritative.pixels,
                    presentation.authoritative.width,
                    presentation.authoritative.height);
                draw_pixels_fit(dc, image_area,
                                presentation.authoritative.width,
                                presentation.authoritative.height,
                                pixels);
                drawing = presentation.playback.animating();
            }
            std::wstring label = utf8_to_wide_ui(canvas.name);
            if (drawing) label += L"   drawing...";
            text_ui(dc, static_cast<int>(card.left) + 8, static_cast<int>(card.bottom) - 26,
                    label, drawing ? RGB(111, 220, 215) : RGB(205, 216, 220));
            g_canvas_hits.emplace_back(card, canvas.name);
        }
        ++col;
        if (col >= columns) { col = 0; ++row; }
    }
}

void draw_animation(HDC dc, const RECT& client, UiState& state) {
    g_frame_hits.clear();
    const auto groups = animation_groups(state);
    if (groups.empty()) {
        text_ui(dc, 18, 24, L"No completed animation frames yet.", RGB(111, 220, 215));
        text_ui(dc, 18, 50, L"Animation appears after its canvases finish drawing.", RGB(145, 152, 156));
        return;
    }

    const auto frames = animation_frames(state, state.selected_group);
    if (frames.empty()) return;
    const int frame_index = std::clamp(state.animation_frame, 0, static_cast<int>(frames.size()) - 1);

    text_ui(dc, 18, 18, L"Animation: " + utf8_to_wide_ui(state.selected_group), RGB(111, 220, 215));
    g_prev_group_rect = {18, 48, 84, 78};
    g_play_rect = {92, 48, 184, 78};
    g_next_group_rect = {192, 48, 258, 78};
    g_fps_down_rect = {270, 48, 304, 78};
    g_fps_up_rect = {382, 48, 416, 78};
    fill_rect_ui(dc, g_prev_group_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_play_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_next_group_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_fps_down_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_fps_up_rect, RGB(37, 45, 49));
    text_ui(dc, 35, 55, L"Prev");
    text_ui(dc, 110, 55, state.playing ? L"Pause" : L"Play");
    text_ui(dc, 210, 55, L"Next");
    text_ui(dc, 281, 55, L"-");
    text_ui(dc, 312, 55, std::to_wstring(state.fps) + L" FPS", RGB(190, 204, 208));
    text_ui(dc, 393, 55, L"+");
    std::wstring timing = L"frame " + std::to_wstring(frames[static_cast<std::size_t>(frame_index)].frame);
    if (state.animation_refresh_pending) timing += L"   waiting for canvas drawing";
    text_ui(dc, 430, 55, timing, RGB(170, 180, 184));

    const auto current = state.animation_snapshots.find(frames[static_cast<std::size_t>(frame_index)].name);
    if (current != state.animation_snapshots.end() && current->second.valid()) {
        RECT area{18, 92, client.right - 18, client.bottom - 124};
        draw_pixels_fit(dc, area, current->second.width, current->second.height, current->second.pixels);
    }

    const int thumb_w = 86;
    const int y = static_cast<int>(client.bottom) - 108;
    int x = 18;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (x + thumb_w > static_cast<int>(client.right) - 12) break;
        RECT card{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + thumb_w - 8), client.bottom - 30};
        fill_rect_ui(dc, card, static_cast<int>(i) == frame_index ? RGB(35, 63, 66) : RGB(24, 29, 33));
        const auto snapshot = state.animation_snapshots.find(frames[i].name);
        if (snapshot != state.animation_snapshots.end() && snapshot->second.valid()) {
            RECT area{card.left + 4, card.top + 4, card.right - 4, card.bottom - 22};
            draw_pixels_fit(dc, area, snapshot->second.width, snapshot->second.height, snapshot->second.pixels);
        }
        text_ui(dc, static_cast<int>(card.left) + 6, static_cast<int>(card.bottom) - 18,
                std::to_wstring(frames[i].frame), RGB(190, 200, 204));
        g_frame_hits.emplace_back(card, static_cast<int>(i));
        x += thumb_w;
    }
}

void paint_tabs_buffered(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    HDC target = bitmap ? memory : dc;
    {
        std::lock_guard lock(g_ui_mutex);
        draw_tabs(target, client, g_ui);
    }
    if (bitmap) BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    if (old) SelectObject(memory, old);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

void paint_content_buffered(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    HDC target = bitmap ? memory : dc;
    fill_rect_ui(target, client, RGB(14, 18, 20));
    {
        std::lock_guard lock(g_ui_mutex);
        if (g_ui.tab == TAB_CANVASES) draw_canvases(target, client, g_ui);
        else if (g_ui.tab == TAB_ANIMATION) draw_animation(target, client, g_ui);
        if (!g_ui.project_directory.empty())
            text_ui(target, 12, static_cast<int>(client.bottom) - 22,
                    L"Project: " + g_ui.project_directory, RGB(125, 135, 140));
    }
    if (bitmap) BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    if (old) SelectObject(memory, old);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK tabs_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            HWND owner = nullptr;
            bool show_canvas = false;
            {
                std::lock_guard lock(g_ui_mutex);
                const RECT canvas_tab{8, 4, 112, 40};
                const RECT canvases_tab{118, 4, 242, 40};
                const RECT animation_tab{248, 4, 382, 40};
                if (PtInRect(&canvas_tab, p)) {
                    g_ui.tab = TAB_CANVAS;
                    show_canvas = true;
                } else if (PtInRect(&canvases_tab, p)) {
                    g_ui.tab = TAB_CANVASES;
                } else if (PtInRect(&animation_tab, p)) {
                    g_ui.tab = TAB_ANIMATION;
                    g_ui.last_frame_tick = GetTickCount64();
                }
                apply_page_visibility_locked();
                if (g_content_window && g_ui.tab != TAB_CANVAS) InvalidateRect(g_content_window, nullptr, FALSE);
                owner = g_owner;
            }
            if (show_canvas && owner) {
                const RECT region = parent_canvas_region(owner);
                InvalidateRect(owner, &region, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PACK_REFRESH: {
            {
                std::lock_guard lock(g_ui_mutex);
                apply_page_visibility_locked();
                if (g_content_window && g_ui.tab != TAB_CANVAS) InvalidateRect(g_content_window, nullptr, FALSE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT:
            paint_tabs_buffered(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK content_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, PACK_TIMER, 8, nullptr);
            return 0;
        case WM_TIMER:
            if (wparam == PACK_TIMER) {
                bool repaint_content = false;
                bool repaint_tabs = false;
                {
                    std::lock_guard lock(g_ui_mutex);
                    const ULONGLONG now = GetTickCount64();
                    bool reveal_changed = false;
                    for (auto& [name, presentation] : g_ui.presentations) {
                        (void)name;
                        reveal_changed = presentation.playback.tick(now) || reveal_changed;
                    }

                    bool animation_committed = false;
                    if (g_ui.animation_refresh_pending && !any_canvas_animating_locked()) {
                        commit_animation_snapshot_locked();
                        animation_committed = true;
                        repaint_tabs = true;
                    }

                    bool animation_advanced = false;
                    if (g_ui.tab == TAB_ANIMATION && g_ui.playing) {
                        const auto frames = animation_frames(g_ui, g_ui.selected_group);
                        if (!frames.empty()) {
                            const ULONGLONG interval = std::max<ULONGLONG>(1, 1000ull / static_cast<ULONGLONG>(std::max(1, g_ui.fps)));
                            if (now - g_ui.last_frame_tick >= interval) {
                                g_ui.animation_frame = (g_ui.animation_frame + 1) % static_cast<int>(frames.size());
                                g_ui.last_frame_tick = now;
                                animation_advanced = true;
                            }
                        }
                    }

                    if (g_ui.tab == TAB_CANVASES && reveal_changed) repaint_content = true;
                    if (g_ui.tab == TAB_ANIMATION && (animation_committed || animation_advanced)) repaint_content = true;
                }
                if (repaint_content) InvalidateRect(hwnd, nullptr, FALSE);
                if (repaint_tabs && g_tabs_window) InvalidateRect(g_tabs_window, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            std::lock_guard lock(g_ui_mutex);
            if (g_ui.tab == TAB_CANVASES) {
                g_ui.scroll_y = std::max(0, g_ui.scroll_y - GET_WHEEL_DELTA_WPARAM(wparam) / 2);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            std::lock_guard lock(g_ui_mutex);
            if (g_ui.tab == TAB_CANVASES) {
                for (const auto& hit : g_canvas_hits) {
                    if (PtInRect(&hit.first, p)) {
                        g_ui.selected_canvas = hit.second;
                        break;
                    }
                }
            } else if (g_ui.tab == TAB_ANIMATION) {
                const auto groups = animation_groups(g_ui);
                if (PtInRect(&g_play_rect, p)) {
                    g_ui.playing = !g_ui.playing;
                    g_ui.last_frame_tick = GetTickCount64();
                } else if (PtInRect(&g_fps_down_rect, p)) {
                    g_ui.fps = std::max(1, g_ui.fps - 1);
                    g_ui.last_frame_tick = GetTickCount64();
                } else if (PtInRect(&g_fps_up_rect, p)) {
                    if (g_ui.fps < INT_MAX) ++g_ui.fps;
                    g_ui.last_frame_tick = GetTickCount64();
                } else if (!groups.empty() && (PtInRect(&g_prev_group_rect, p) || PtInRect(&g_next_group_rect, p))) {
                    auto it = std::find(groups.begin(), groups.end(), g_ui.selected_group);
                    std::size_t index = it == groups.end() ? 0 : static_cast<std::size_t>(it - groups.begin());
                    if (PtInRect(&g_prev_group_rect, p)) index = (index + groups.size() - 1) % groups.size();
                    else index = (index + 1) % groups.size();
                    g_ui.selected_group = groups[index];
                    g_ui.animation_frame = 0;
                    g_ui.last_frame_tick = GetTickCount64();
                } else {
                    for (const auto& hit : g_frame_hits) {
                        if (PtInRect(&hit.first, p)) {
                            g_ui.animation_frame = hit.second;
                            g_ui.playing = false;
                            break;
                        }
                    }
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT:
            paint_content_buffered(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd, PACK_TIMER);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool is_main_pixelforge_window(HWND hwnd) {
    wchar_t name[96]{};
    if (!GetClassNameW(hwnd, name, 96)) return false;
    return wcscmp(name, L"PixelForgeAutomaticWindow") == 0 || wcscmp(name, L"PixelForgeWindow") == 0;
}

void register_workspace_classes() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW tabs{};
    tabs.lpfnWndProc = tabs_wndproc;
    tabs.hInstance = instance;
    tabs.lpszClassName = L"PixelForgeWorkspaceTabsV3";
    tabs.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tabs.hbrBackground = nullptr;
    if (!RegisterClassW(&tabs) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    WNDCLASSW content{};
    content.lpfnWndProc = content_wndproc;
    content.hInstance = instance;
    content.lpszClassName = L"PixelForgeWorkspacePageV3";
    content.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    content.hbrBackground = nullptr;
    RegisterClassW(&content);
}

void attach_workspace(HWND owner) {
    std::lock_guard lock(g_ui_mutex);
    if (!owner || !IsWindow(owner)) return;
    if (g_owner == owner && g_tabs_window && IsWindow(g_tabs_window)) {
        layout_workspace_locked();
        return;
    }
    g_owner = owner;

    const LONG_PTR style = GetWindowLongPtrW(owner, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0)
        SetWindowLongPtrW(owner, GWL_STYLE, style | WS_CLIPCHILDREN);

    register_workspace_classes();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    g_tabs_window = CreateWindowExW(0, L"PixelForgeWorkspaceTabsV3", L"",
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                    0, 0, 10, TAB_HEIGHT, owner, nullptr, instance, nullptr);
    g_content_window = CreateWindowExW(0, L"PixelForgeWorkspacePageV3", L"",
                                       WS_CHILD | WS_CLIPSIBLINGS,
                                       0, 0, 10, 10, owner, nullptr, instance, nullptr);
    layout_workspace_locked();
    apply_page_visibility_locked();
    if (g_tabs_window) BringWindowToTop(g_tabs_window);
}

void detach_workspace(HWND owner) {
    std::lock_guard lock(g_ui_mutex);
    if (owner != g_owner) return;
    if (g_content_window && IsWindow(g_content_window)) DestroyWindow(g_content_window);
    if (g_tabs_window && IsWindow(g_tabs_window)) DestroyWindow(g_tabs_window);
    g_content_window = nullptr;
    g_tabs_window = nullptr;
    g_owner = nullptr;
}

LRESULT CALLBACK attach_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message->hwnd && is_main_pixelforge_window(message->hwnd)) {
            if (message->message == WM_CREATE || message->message == WM_SHOWWINDOW) {
                attach_workspace(message->hwnd);
            } else if (message->message == WM_SIZE) {
                std::lock_guard lock(g_ui_mutex);
                if (message->hwnd == g_owner) layout_workspace_locked();
            } else if (message->message == WM_DESTROY) {
                detach_workspace(message->hwnd);
            }
        }
    }
    return CallNextHookEx(g_attach_hook, code, wparam, lparam);
}

struct AutoAttachWorkspace {
    AutoAttachWorkspace() {
        g_attach_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, attach_hook_proc, nullptr, GetCurrentThreadId());
    }
    ~AutoAttachWorkspace() {
        if (g_attach_hook) UnhookWindowsHookEx(g_attach_hook);
        g_attach_hook = nullptr;
    }
};

AutoAttachWorkspace g_auto_attach_workspace;

} // namespace

void pack_workspace_ui_publish(HWND owner,
                               std::uint64_t task_id,
                               std::uint64_t revision,
                               std::wstring project_directory,
                               std::vector<PackUiCanvasInfo> canvases) {
    // Pack autosave is complete before this function is called. Load immutable
    // frame snapshots on the worker thread once; paint/timer paths never touch
    // disk, which removes the old animation flicker and repeated WIC decode cost.
    std::unordered_map<std::string, PixelSnapshot> loaded;
    loaded.reserve(canvases.size());
    for (const auto& canvas : canvases) {
        PixelSnapshot snapshot;
        if (load_canvas_snapshot(project_directory, canvas, snapshot))
            loaded.emplace(canvas.name, std::move(snapshot));
    }

    HWND tabs = nullptr;
    {
        std::lock_guard lock(g_ui_mutex);
        if (owner && !g_owner && is_main_pixelforge_window(owner)) g_owner = owner;

        const bool new_workspace = g_ui.project_directory != project_directory;
        const std::string previous_canvas = new_workspace ? std::string{} : g_ui.selected_canvas;
        const std::string previous_group = new_workspace ? std::string{} : g_ui.selected_group;
        if (new_workspace) {
            g_ui.presentations.clear();
            g_ui.animation_canvases.clear();
            g_ui.animation_snapshots.clear();
            g_ui.animation_refresh_pending = false;
            g_ui.animation_revision = 0;
        }

        g_ui.task_id = task_id;
        g_ui.revision = revision;
        g_ui.project_directory = std::move(project_directory);
        g_ui.canvases = std::move(canvases);

        std::unordered_set<std::string> live_names;
        live_names.reserve(g_ui.canvases.size());
        bool queued_reveal = false;
        for (const auto& canvas : g_ui.canvases) {
            live_names.insert(canvas.name);
            const auto source = loaded.find(canvas.name);
            if (source == loaded.end() || !source->second.valid()) continue;

            auto it = g_ui.presentations.find(canvas.name);
            if (it == g_ui.presentations.end()) {
                CanvasPresentation presentation;
                presentation.authoritative = source->second;
                presentation.playback.reset_to_authoritative(
                    PACK_PRESENTATION_TASK,
                    source->second.width,
                    source->second.height,
                    revision,
                    source->second.pixels);
                g_ui.presentations.emplace(canvas.name, std::move(presentation));
                continue;
            }

            auto& presentation = it->second;
            const bool changed = presentation.playback.observe_authoritative(
                PACK_PRESENTATION_TASK,
                true,
                source->second.width,
                source->second.height,
                revision,
                source->second.pixels);
            presentation.authoritative = source->second;
            queued_reveal = (changed && presentation.playback.animating()) || queued_reveal;
        }

        for (auto it = g_ui.presentations.begin(); it != g_ui.presentations.end();) {
            if (live_names.find(it->first) == live_names.end()) it = g_ui.presentations.erase(it);
            else ++it;
        }

        if (!previous_canvas.empty() && live_names.find(previous_canvas) != live_names.end())
            g_ui.selected_canvas = previous_canvas;
        else
            g_ui.selected_canvas.clear();

        if (queued_reveal || any_canvas_animating_locked()) {
            g_ui.animation_refresh_pending = true;
        } else {
            // Initial project load and metadata-only updates are already stable.
            commit_animation_snapshot_locked();
        }

        if (!previous_group.empty()) {
            const auto groups = animation_groups(g_ui);
            if (std::find(groups.begin(), groups.end(), previous_group) != groups.end())
                g_ui.selected_group = previous_group;
        }
        normalize_animation_selection_locked();

        tabs = g_tabs_window;
    }

    // Pack updates originate on Astra's worker thread. Never call synchronous
    // HWND APIs here; schedule the page refresh on the owning UI thread.
    if (tabs && IsWindow(tabs)) PostMessageW(tabs, WM_PACK_REFRESH, 0, 0);
}

} // namespace pixelforge::win32
