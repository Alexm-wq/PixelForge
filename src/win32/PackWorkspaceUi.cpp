#include "PackWorkspaceUi.hpp"

#include "ImageIO.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pixelforge::win32 {
namespace {

constexpr UINT WM_PACK_REFRESH = WM_APP + 91;
constexpr UINT_PTR PACK_TIMER = 91;
constexpr int TAB_CANVAS = 0;
constexpr int TAB_CANVASES = 1;
constexpr int TAB_ANIMATION = 2;

struct UiState {
    std::uint64_t task_id = 0;
    std::uint64_t revision = 0;
    std::wstring project_directory;
    std::vector<PackUiCanvasInfo> canvases;
    std::string selected_canvas;
    std::string selected_group;
    int tab = TAB_CANVASES;
    int fps = 10;
    bool playing = true;
    int animation_frame = 0;
    ULONGLONG last_frame_tick = 0;
    int scroll_y = 0;
};

std::mutex g_ui_mutex;
UiState g_ui;
HWND g_window = nullptr;
bool g_starting = false;
std::vector<std::pair<RECT, std::string>> g_canvas_hits;
RECT g_play_rect{};
RECT g_prev_group_rect{};
RECT g_next_group_rect{};
RECT g_prev_canvas_rect{};
RECT g_next_canvas_rect{};

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

std::filesystem::path canvas_path(const UiState& state, const PackUiCanvasInfo& canvas) {
    const std::wstring folder = canvas.group.empty() ? L"ungrouped" : utf8_to_wide_ui(canvas.group);
    return std::filesystem::path(state.project_directory) / L"canvases" / folder /
           (utf8_to_wide_ui(canvas.name) + L".png");
}

bool load_canvas_image(const UiState& state, const PackUiCanvasInfo& canvas, ImageData& image) {
    std::wstring error;
    return load_image_wic(canvas_path(state, canvas).wstring(), image, error);
}

void draw_image_fit(HDC dc, const RECT& area, const ImageData& image) {
    if (!image.valid()) {
        text_ui(dc, area.left + 10, area.top + 10, L"Canvas snapshot is not available yet.", RGB(145, 152, 156));
        return;
    }
    const int aw = std::max(1L, area.right - area.left);
    const int ah = std::max(1L, area.bottom - area.top);
    const double fit = std::min(static_cast<double>(aw) / image.width, static_cast<double>(ah) / image.height);
    int scale = fit >= 1.0 ? std::max(1, static_cast<int>(fit)) : 0;
    int dw = scale ? image.width * scale : std::max(1, static_cast<int>(image.width * fit));
    int dh = scale ? image.height * scale : std::max(1, static_cast<int>(image.height * fit));
    const int dx = area.left + (aw - dw) / 2;
    const int dy = area.top + (ah - dh) / 2;

    std::vector<std::uint32_t> composited(static_cast<std::size_t>(image.width) * image.height);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4u;
            const unsigned b = image.bgra[i + 0];
            const unsigned g = image.bgra[i + 1];
            const unsigned r = image.bgra[i + 2];
            const unsigned a = image.bgra[i + 3];
            const unsigned bg = ((x + y) & 1) ? 45 : 58;
            auto blend = [&](unsigned c, unsigned base) { return (c * a + base * (255 - a) + 127) / 255; };
            const unsigned rr = blend(r, bg);
            const unsigned gg = blend(g, bg + 3);
            const unsigned bb = blend(b, bg + 6);
            composited[static_cast<std::size_t>(y) * image.width + x] = (rr << 16) | (gg << 8) | bb;
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = image.width;
    bmi.bmiHeader.biHeight = -image.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, image.width, image.height,
                  composited.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

std::vector<std::string> animation_groups(const UiState& state) {
    std::vector<std::string> groups;
    for (const auto& canvas : state.canvases) {
        if (canvas.frame < 0 || canvas.group.empty()) continue;
        if (std::find(groups.begin(), groups.end(), canvas.group) == groups.end()) groups.push_back(canvas.group);
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

std::vector<PackUiCanvasInfo> animation_frames(const UiState& state, std::string_view group) {
    std::vector<PackUiCanvasInfo> frames;
    for (const auto& canvas : state.canvases)
        if (canvas.frame >= 0 && canvas.group == group) frames.push_back(canvas);
    std::stable_sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return a.name < b.name;
    });
    return frames;
}

const PackUiCanvasInfo* selected_canvas(const UiState& state) {
    if (!state.selected_canvas.empty()) {
        for (const auto& canvas : state.canvases)
            if (canvas.name == state.selected_canvas) return &canvas;
    }
    return state.canvases.empty() ? nullptr : &state.canvases.front();
}

void draw_tab(HDC dc, RECT rect, const wchar_t* title, bool selected) {
    fill_rect_ui(dc, rect, selected ? RGB(45, 91, 94) : RGB(31, 38, 42));
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    text_ui(dc, rect.left + 12, rect.top + 8, title,
            selected ? RGB(230, 248, 247) : RGB(178, 188, 192));
}

void draw_canvas_tab(HDC dc, const RECT& client, const UiState& state) {
    const auto* canvas = selected_canvas(state);
    if (!canvas) {
        text_ui(dc, 24, 92, L"No pack canvases yet.", RGB(145, 152, 156));
        return;
    }
    text_ui(dc, 24, 78, utf8_to_wide_ui(canvas->name), RGB(111, 220, 215));
    std::wstring meta = std::to_wstring(canvas->width) + L"x" + std::to_wstring(canvas->height);
    if (!canvas->group.empty()) meta += L"   group " + utf8_to_wide_ui(canvas->group);
    if (canvas->frame >= 0) meta += L"   frame " + std::to_wstring(canvas->frame);
    text_ui(dc, 24, 102, meta, RGB(156, 164, 168));

    g_prev_canvas_rect = {24, 130, 96, 160};
    g_next_canvas_rect = {104, 130, 176, 160};
    fill_rect_ui(dc, g_prev_canvas_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_next_canvas_rect, RGB(37, 45, 49));
    text_ui(dc, 42, 137, L"Prev");
    text_ui(dc, 122, 137, L"Next");

    ImageData image;
    if (load_canvas_image(state, *canvas, image)) {
        RECT area{24, 176, client.right - 24, client.bottom - 58};
        draw_image_fit(dc, area, image);
    }
}

void draw_canvases_tab(HDC dc, const RECT& client, const UiState& state) {
    g_canvas_hits.clear();
    const int columns = std::max(2, static_cast<int>((client.right - 48) / 170));
    const int cell_w = std::max(140, static_cast<int>((client.right - 48) / columns));
    const int cell_h = 170;
    int row = 0;
    int col = 0;
    std::string last_group;
    int y = 78 - state.scroll_y;

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
            text_ui(dc, 24, y, canvas.group.empty() ? L"UNGROUPED" : utf8_to_wide_ui(canvas.group), RGB(111, 220, 215));
            y += 28;
        }
        const int x = 24 + col * cell_w;
        const int cy = y + row * cell_h;
        RECT card{x, cy, std::min(client.right - 16, x + cell_w - 12), cy + cell_h - 12};
        if (card.bottom >= 68 && card.top <= client.bottom - 42) {
            fill_rect_ui(dc, card, canvas.name == state.selected_canvas ? RGB(35, 63, 66) : RGB(24, 29, 33));
            FrameRect(dc, &card, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            RECT image_area{card.left + 8, card.top + 8, card.right - 8, card.bottom - 34};
            ImageData image;
            if (load_canvas_image(state, canvas, image)) draw_image_fit(dc, image_area, image);
            text_ui(dc, card.left + 8, card.bottom - 26, utf8_to_wide_ui(canvas.name), RGB(205, 216, 220));
            g_canvas_hits.emplace_back(card, canvas.name);
        }
        ++col;
        if (col >= columns) { col = 0; ++row; }
    }
}

void draw_animation_tab(HDC dc, const RECT& client, UiState& state) {
    const auto groups = animation_groups(state);
    if (groups.empty()) {
        text_ui(dc, 24, 92, L"No animation groups yet. Frames with a non-negative frame number appear here automatically.",
                RGB(145, 152, 156));
        return;
    }
    if (std::find(groups.begin(), groups.end(), state.selected_group) == groups.end()) {
        state.selected_group = groups.front();
        state.animation_frame = 0;
    }
    auto frames = animation_frames(state, state.selected_group);
    if (frames.empty()) return;
    if (state.animation_frame < 0 || state.animation_frame >= static_cast<int>(frames.size())) state.animation_frame = 0;

    const ULONGLONG now = GetTickCount64();
    if (state.playing && now - state.last_frame_tick >= static_cast<ULONGLONG>(1000 / std::max(1, state.fps))) {
        state.animation_frame = (state.animation_frame + 1) % static_cast<int>(frames.size());
        state.last_frame_tick = now;
    }

    text_ui(dc, 24, 78, L"Animation: " + utf8_to_wide_ui(state.selected_group), RGB(111, 220, 215));
    g_prev_group_rect = {24, 108, 90, 138};
    g_play_rect = {98, 108, 190, 138};
    g_next_group_rect = {198, 108, 264, 138};
    fill_rect_ui(dc, g_prev_group_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_play_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_next_group_rect, RGB(37, 45, 49));
    text_ui(dc, 41, 115, L"Prev");
    text_ui(dc, 116, 115, state.playing ? L"Pause" : L"Play");
    text_ui(dc, 216, 115, L"Next");
    text_ui(dc, 282, 115, std::to_wstring(state.fps) + L" FPS", RGB(170, 180, 184));

    ImageData image;
    if (load_canvas_image(state, frames[static_cast<std::size_t>(state.animation_frame)], image)) {
        RECT area{24, 154, client.right - 24, client.bottom - 150};
        draw_image_fit(dc, area, image);
    }

    const int thumb_w = 86;
    const int y = client.bottom - 132;
    int x = 24;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (x + thumb_w > client.right - 18) break;
        RECT card{x, y, x + thumb_w - 8, client.bottom - 54};
        fill_rect_ui(dc, card, static_cast<int>(i) == state.animation_frame ? RGB(35, 63, 66) : RGB(24, 29, 33));
        ImageData thumb;
        if (load_canvas_image(state, frames[i], thumb)) {
            RECT area{card.left + 4, card.top + 4, card.right - 4, card.bottom - 22};
            draw_image_fit(dc, area, thumb);
        }
        text_ui(dc, card.left + 6, card.bottom - 18, std::to_wstring(frames[i].frame), RGB(190, 200, 204));
        x += thumb_w;
    }
}

LRESULT CALLBACK pack_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, PACK_TIMER, 30, nullptr);
            return 0;
        case WM_PACK_REFRESH:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wparam == PACK_TIMER) {
                std::lock_guard lock(g_ui_mutex);
                if (g_ui.tab == TAB_ANIMATION && g_ui.playing) InvalidateRect(hwnd, nullptr, FALSE);
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
            RECT canvas_tab{12, 12, 116, 48};
            RECT canvases_tab{122, 12, 246, 48};
            RECT animation_tab{252, 12, 386, 48};
            if (PtInRect(&canvas_tab, p)) g_ui.tab = TAB_CANVAS;
            else if (PtInRect(&canvases_tab, p)) g_ui.tab = TAB_CANVASES;
            else if (PtInRect(&animation_tab, p)) g_ui.tab = TAB_ANIMATION;
            else if (g_ui.tab == TAB_CANVASES) {
                for (const auto& hit : g_canvas_hits) {
                    if (PtInRect(&hit.first, p)) {
                        g_ui.selected_canvas = hit.second;
                        g_ui.tab = TAB_CANVAS;
                        break;
                    }
                }
            } else if (g_ui.tab == TAB_CANVAS) {
                if (!g_ui.canvases.empty() && (PtInRect(&g_prev_canvas_rect, p) || PtInRect(&g_next_canvas_rect, p))) {
                    std::size_t current = 0;
                    for (std::size_t i = 0; i < g_ui.canvases.size(); ++i)
                        if (g_ui.canvases[i].name == g_ui.selected_canvas) { current = i; break; }
                    if (PtInRect(&g_prev_canvas_rect, p))
                        current = (current + g_ui.canvases.size() - 1) % g_ui.canvases.size();
                    else current = (current + 1) % g_ui.canvases.size();
                    g_ui.selected_canvas = g_ui.canvases[current].name;
                }
            } else if (g_ui.tab == TAB_ANIMATION) {
                const auto groups = animation_groups(g_ui);
                if (PtInRect(&g_play_rect, p)) g_ui.playing = !g_ui.playing;
                else if (!groups.empty() && (PtInRect(&g_prev_group_rect, p) || PtInRect(&g_next_group_rect, p))) {
                    auto it = std::find(groups.begin(), groups.end(), g_ui.selected_group);
                    std::size_t index = it == groups.end() ? 0 : static_cast<std::size_t>(it - groups.begin());
                    if (PtInRect(&g_prev_group_rect, p)) index = (index + groups.size() - 1) % groups.size();
                    else index = (index + 1) % groups.size();
                    g_ui.selected_group = groups[index];
                    g_ui.animation_frame = 0;
                    g_ui.last_frame_tick = GetTickCount64();
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            fill_rect_ui(dc, client, RGB(14, 18, 20));
            std::lock_guard lock(g_ui_mutex);
            RECT canvas_tab{12, 12, 116, 48};
            RECT canvases_tab{122, 12, 246, 48};
            RECT animation_tab{252, 12, 386, 48};
            draw_tab(dc, canvas_tab, L"Canvas", g_ui.tab == TAB_CANVAS);
            draw_tab(dc, canvases_tab, L"Canvases", g_ui.tab == TAB_CANVASES);
            draw_tab(dc, animation_tab, L"Animation", g_ui.tab == TAB_ANIMATION);
            text_ui(dc, 410, 23,
                    L"task " + std::to_wstring(g_ui.task_id) + L"   pack revision " + std::to_wstring(g_ui.revision),
                    RGB(150, 160, 164));
            if (g_ui.tab == TAB_CANVAS) draw_canvas_tab(dc, client, g_ui);
            else if (g_ui.tab == TAB_CANVASES) draw_canvases_tab(dc, client, g_ui);
            else draw_animation_tab(dc, client, g_ui);
            if (!g_ui.project_directory.empty())
                text_ui(dc, 16, client.bottom - 30, L"Project: " + g_ui.project_directory, RGB(132, 142, 146));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, PACK_TIMER);
            {
                std::lock_guard lock(g_ui_mutex);
                g_window = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void run_pack_window(HWND owner) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = pack_wndproc;
    wc.hInstance = instance;
    wc.lpszClassName = L"PixelForgePackWorkspaceWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                wc.lpszClassName,
                                L"PixelForge Workspace",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1040, 780,
                                owner, nullptr, instance, nullptr);
    {
        std::lock_guard lock(g_ui_mutex);
        g_window = hwnd;
        g_starting = false;
    }
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace

void pack_workspace_ui_publish(HWND owner,
                               std::uint64_t task_id,
                               std::uint64_t revision,
                               std::wstring project_directory,
                               std::vector<PackUiCanvasInfo> canvases) {
    HWND window = nullptr;
    bool start = false;
    {
        std::lock_guard lock(g_ui_mutex);
        const std::string old_selected = g_ui.selected_canvas;
        const std::string old_group = g_ui.selected_group;
        g_ui.task_id = task_id;
        g_ui.revision = revision;
        g_ui.project_directory = std::move(project_directory);
        g_ui.canvases = std::move(canvases);
        if (!old_selected.empty() && std::any_of(g_ui.canvases.begin(), g_ui.canvases.end(), [&](const auto& c) { return c.name == old_selected; }))
            g_ui.selected_canvas = old_selected;
        else g_ui.selected_canvas = g_ui.canvases.empty() ? std::string{} : g_ui.canvases.front().name;
        const auto groups = animation_groups(g_ui);
        if (!old_group.empty() && std::find(groups.begin(), groups.end(), old_group) != groups.end()) g_ui.selected_group = old_group;
        else g_ui.selected_group = groups.empty() ? std::string{} : groups.front();
        window = g_window;
        if (!window && !g_starting) {
            g_starting = true;
            start = true;
        }
    }
    if (window) PostMessageW(window, WM_PACK_REFRESH, 0, 0);
    if (start) std::thread([owner] { run_pack_window(owner); }).detach();
}

} // namespace pixelforge::win32
