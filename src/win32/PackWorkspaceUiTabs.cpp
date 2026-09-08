#include "PackWorkspaceUi.hpp"

#include "ImageIO.hpp"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
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

struct UiState {
    std::uint64_t task_id = 0;
    std::uint64_t revision = 0;
    std::wstring project_directory;
    std::vector<PackUiCanvasInfo> canvases;
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
    if (state.project_directory.empty()) return false;
    std::wstring error;
    return load_image_wic(canvas_path(state, canvas).wstring(), image, error);
}

void draw_image_fit(HDC dc, const RECT& area, const ImageData& image) {
    if (!image.valid()) return;
    const int aw = std::max(1, static_cast<int>(area.right - area.left));
    const int ah = std::max(1, static_cast<int>(area.bottom - area.top));
    const double fit = std::min(static_cast<double>(aw) / image.width, static_cast<double>(ah) / image.height);
    const int scale = fit >= 1.0 ? std::max(1, static_cast<int>(fit)) : 0;
    const int dw = scale ? image.width * scale : std::max(1, static_cast<int>(image.width * fit));
    const int dh = scale ? image.height * scale : std::max(1, static_cast<int>(image.height * fit));
    const int dx = static_cast<int>(area.left) + (aw - dw) / 2;
    const int dy = static_cast<int>(area.top) + (ah - dh) / 2;

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
            composited[static_cast<std::size_t>(y) * image.width + x] =
                (blend(r, bg) << 16) | (blend(g, bg + 3) << 8) | blend(b, bg + 6);
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
            ImageData image;
            if (load_canvas_image(state, canvas, image)) draw_image_fit(dc, image_area, image);
            text_ui(dc, static_cast<int>(card.left) + 8, static_cast<int>(card.bottom) - 26,
                    utf8_to_wide_ui(canvas.name), RGB(205, 216, 220));
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
        text_ui(dc, 18, 24, L"No animation groups yet.", RGB(111, 220, 215));
        text_ui(dc, 18, 50, L"Frames with a group and non-negative frame number will play here automatically.", RGB(145, 152, 156));
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

    text_ui(dc, 18, 18, L"Animation: " + utf8_to_wide_ui(state.selected_group), RGB(111, 220, 215));
    g_prev_group_rect = {18, 48, 84, 78};
    g_play_rect = {92, 48, 184, 78};
    g_next_group_rect = {192, 48, 258, 78};
    fill_rect_ui(dc, g_prev_group_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_play_rect, RGB(37, 45, 49));
    fill_rect_ui(dc, g_next_group_rect, RGB(37, 45, 49));
    text_ui(dc, 35, 55, L"Prev");
    text_ui(dc, 110, 55, state.playing ? L"Pause" : L"Play");
    text_ui(dc, 210, 55, L"Next");
    text_ui(dc, 276, 55,
            std::to_wstring(state.fps) + L" FPS   frame " +
            std::to_wstring(frames[static_cast<std::size_t>(state.animation_frame)].frame), RGB(170, 180, 184));

    ImageData image;
    if (load_canvas_image(state, frames[static_cast<std::size_t>(state.animation_frame)], image)) {
        RECT area{18, 92, client.right - 18, client.bottom - 124};
        draw_image_fit(dc, area, image);
    }

    const int thumb_w = 86;
    const int y = static_cast<int>(client.bottom) - 108;
    int x = 18;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (x + thumb_w > static_cast<int>(client.right) - 12) break;
        RECT card{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + thumb_w - 8), client.bottom - 30};
        fill_rect_ui(dc, card, static_cast<int>(i) == state.animation_frame ? RGB(35, 63, 66) : RGB(24, 29, 33));
        ImageData thumb;
        if (load_canvas_image(state, frames[i], thumb)) {
            RECT area{card.left + 4, card.top + 4, card.right - 4, card.bottom - 22};
            draw_image_fit(dc, area, thumb);
        }
        text_ui(dc, static_cast<int>(card.left) + 6, static_cast<int>(card.bottom) - 18,
                std::to_wstring(frames[i].frame), RGB(190, 200, 204));
        g_frame_hits.emplace_back(card, static_cast<int>(i));
        x += thumb_w;
    }
}

LRESULT CALLBACK tabs_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            HWND owner = nullptr;
            {
                std::lock_guard lock(g_ui_mutex);
                const RECT canvas_tab{8, 4, 112, 40};
                const RECT canvases_tab{118, 4, 242, 40};
                const RECT animation_tab{248, 4, 382, 40};
                if (PtInRect(&canvas_tab, p)) {
                    g_ui.tab = TAB_CANVAS;
                } else if (PtInRect(&canvases_tab, p)) {
                    g_ui.tab = TAB_CANVASES;
                } else if (PtInRect(&animation_tab, p)) {
                    g_ui.tab = TAB_ANIMATION;
                    g_ui.last_frame_tick = GetTickCount64();
                }
                apply_page_visibility_locked();
                if (g_content_window) InvalidateRect(g_content_window, nullptr, FALSE);
                owner = g_owner;
            }
            if (owner) InvalidateRect(owner, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PACK_REFRESH: {
            HWND owner = nullptr;
            {
                std::lock_guard lock(g_ui_mutex);
                apply_page_visibility_locked();
                if (g_content_window) InvalidateRect(g_content_window, nullptr, FALSE);
                owner = g_owner;
            }
            if (owner) InvalidateRect(owner, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            std::lock_guard lock(g_ui_mutex);
            draw_tabs(dc, client, g_ui);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK content_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, PACK_TIMER, 30, nullptr);
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
            if (g_tabs_window) InvalidateRect(g_tabs_window, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            fill_rect_ui(dc, client, RGB(14, 18, 20));
            std::lock_guard lock(g_ui_mutex);
            if (g_ui.tab == TAB_CANVASES) draw_canvases(dc, client, g_ui);
            else if (g_ui.tab == TAB_ANIMATION) draw_animation(dc, client, g_ui);
            if (!g_ui.project_directory.empty())
                text_ui(dc, 12, static_cast<int>(client.bottom) - 22,
                        L"Project: " + g_ui.project_directory, RGB(125, 135, 140));
            EndPaint(hwnd, &ps);
            return 0;
        }
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
    if (!GetClassNameW(hwnd, name, static_cast<int>(std::size(name)))) return false;
    return wcscmp(name, L"PixelForgeAutomaticWindow") == 0 || wcscmp(name, L"PixelForgeWindow") == 0;
}

void register_workspace_classes() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW tabs{};
    tabs.lpfnWndProc = tabs_wndproc;
    tabs.hInstance = instance;
    tabs.lpszClassName = L"PixelForgeWorkspaceTabsV2";
    tabs.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tabs.hbrBackground = nullptr;
    if (!RegisterClassW(&tabs) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    WNDCLASSW content{};
    content.lpfnWndProc = content_wndproc;
    content.hInstance = instance;
    content.lpszClassName = L"PixelForgeWorkspacePageV2";
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

    // The primary canvas is painted by the legacy parent window. WS_CLIPCHILDREN
    // makes the Canvases/Animation page a real exclusive page: the parent is
    // physically prevented from painting through the visible child page.
    const LONG_PTR style = GetWindowLongPtrW(owner, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0)
        SetWindowLongPtrW(owner, GWL_STYLE, style | WS_CLIPCHILDREN);

    register_workspace_classes();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    g_tabs_window = CreateWindowExW(0, L"PixelForgeWorkspaceTabsV2", L"",
                                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                    0, 0, 10, TAB_HEIGHT, owner, nullptr, instance, nullptr);
    g_content_window = CreateWindowExW(0, L"PixelForgeWorkspacePageV2", L"",
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
    HWND tabs = nullptr;
    {
        std::lock_guard lock(g_ui_mutex);
        if (owner && !g_owner && is_main_pixelforge_window(owner)) g_owner = owner;

        const bool new_task = g_ui.task_id != 0 && task_id != g_ui.task_id;
        const std::string previous_canvas = new_task ? std::string{} : g_ui.selected_canvas;
        const std::string previous_group = new_task ? std::string{} : g_ui.selected_group;
        g_ui.task_id = task_id;
        g_ui.revision = revision;
        g_ui.project_directory = std::move(project_directory);
        g_ui.canvases = std::move(canvases);

        if (!previous_canvas.empty()) {
            const auto it = std::find_if(g_ui.canvases.begin(), g_ui.canvases.end(), [&](const auto& c) {
                return c.name == previous_canvas;
            });
            g_ui.selected_canvas = it == g_ui.canvases.end() ? std::string{} : previous_canvas;
        } else {
            g_ui.selected_canvas.clear();
        }

        const auto groups = animation_groups(g_ui);
        if (!previous_group.empty() && std::find(groups.begin(), groups.end(), previous_group) != groups.end())
            g_ui.selected_group = previous_group;
        else
            g_ui.selected_group = groups.empty() ? std::string{} : groups.front();

        tabs = g_tabs_window;
    }

    // Pack updates originate on Astra's worker thread. Never call synchronous
    // HWND APIs there; schedule the page refresh on the owning UI thread.
    if (tabs && IsWindow(tabs)) PostMessageW(tabs, WM_PACK_REFRESH, 0, 0);
}

} // namespace pixelforge::win32
