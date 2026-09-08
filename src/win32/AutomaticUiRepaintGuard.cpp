#include <windows.h>

#include <cwchar>

namespace {

constexpr UINT WM_AGENT_UPDATED = WM_APP + 1;
constexpr UINT_PTR CODEX_REFRESH_TIMER = 77;
constexpr UINT_PTR DISPLAY_ANIMATION_TIMER = 78;

HWND g_automatic_window = nullptr;
WNDPROC g_original_proc = nullptr;
HHOOK g_attach_hook = nullptr;

bool is_automatic_window(HWND hwnd) {
    wchar_t class_name[96]{};
    if (!GetClassNameW(hwnd, class_name, 96)) return false;
    return std::wcscmp(class_name, L"PixelForgeAutomaticWindow") == 0;
}

RECT live_canvas_region(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    // Match the legacy editor's center column, but start below the permanent
    // workspace tab bar so parent repaints never touch the tab chrome.
    RECT region{340, 52, client.right - 330, client.bottom - 8};
    if (region.right < region.left) region.right = region.left;
    if (region.bottom < region.top) region.bottom = region.top;
    return region;
}

RECT task_state_region() {
    return RECT{0, 296, 340, 455};
}

void rescope_full_invalidation(HWND hwnd, bool include_task_state) {
    RECT dirty{};
    if (!GetUpdateRect(hwnd, &dirty, FALSE)) return;

    // The legacy window invalidates its whole client area for every playback
    // tick / agent status update. Clear that broad update and replace it with
    // only the regions whose pixels can actually have changed.
    ValidateRect(hwnd, nullptr);
    const RECT canvas = live_canvas_region(hwnd);
    InvalidateRect(hwnd, &canvas, FALSE);
    if (include_task_state) {
        const RECT task = task_state_region();
        InvalidateRect(hwnd, &task, FALSE);
    }
}

LRESULT CALLBACK repaint_guard_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_original_proc) return DefWindowProcW(hwnd, msg, wparam, lparam);

    if (msg == WM_TIMER && wparam == CODEX_REFRESH_TIMER) {
        RECT before{};
        const BOOL already_dirty = GetUpdateRect(hwnd, &before, FALSE);
        const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
        // CODEX_REFRESH_TIMER exists to service review/status state. It should
        // never force a visual refresh by itself. Preserve any update that was
        // already pending before this timer fired.
        if (!already_dirty) ValidateRect(hwnd, nullptr);
        return result;
    }

    if (msg == WM_TIMER && wparam == DISPLAY_ANIMATION_TIMER) {
        RECT before{};
        const BOOL already_dirty = GetUpdateRect(hwnd, &before, FALSE);
        const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
        if (!already_dirty) rescope_full_invalidation(hwnd, false);
        return result;
    }

    if (msg == WM_AGENT_UPDATED) {
        RECT before{};
        const BOOL already_dirty = GetUpdateRect(hwnd, &before, FALSE);
        const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
        if (!already_dirty) rescope_full_invalidation(hwnd, true);
        return result;
    }

    const LRESULT result = CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
    if (msg == WM_NCDESTROY && hwnd == g_automatic_window) {
        g_automatic_window = nullptr;
        g_original_proc = nullptr;
    }
    return result;
}

void attach_repaint_guard(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || hwnd == g_automatic_window) return;

    // Parent painting must never bleed through the Canvases/Animation child
    // pages. This also prevents needless redraw work behind those pages.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0)
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN);

    g_automatic_window = hwnd;
    g_original_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(repaint_guard_proc)));
}

LRESULT CALLBACK attach_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message->hwnd && is_automatic_window(message->hwnd) &&
            (message->message == WM_CREATE || message->message == WM_SHOWWINDOW)) {
            attach_repaint_guard(message->hwnd);
        }
    }
    return CallNextHookEx(g_attach_hook, code, wparam, lparam);
}

struct AutoInstallRepaintGuard {
    AutoInstallRepaintGuard() {
        g_attach_hook = SetWindowsHookExW(WH_CALLWNDPROCRET, attach_hook_proc, nullptr, GetCurrentThreadId());
    }
    ~AutoInstallRepaintGuard() {
        if (g_attach_hook) UnhookWindowsHookEx(g_attach_hook);
        g_attach_hook = nullptr;
    }
};

AutoInstallRepaintGuard g_auto_install_repaint_guard;

} // namespace
