#include <windows.h>

#include <cwchar>
#include <initializer_list>

namespace {

constexpr UINT WM_AGENT_UPDATED = WM_APP + 1;
constexpr UINT_PTR CODEX_REFRESH_TIMER = 77;
constexpr UINT_PTR DISPLAY_ANIMATION_TIMER = 78;
constexpr int ID_INTELLIGENCE = 1021;
constexpr int ID_REVIEW_ACCEPT = 1031;
constexpr int ID_REVIEW_CHANGES = 1032;
constexpr int ID_REVIEW_REVISION_LEVEL = 1091;

HWND g_automatic_window = nullptr;
WNDPROC g_original_proc = nullptr;
HHOOK g_attach_hook = nullptr;

HWND g_review_window = nullptr;
HWND g_revision_level = nullptr;
WNDPROC g_review_original_proc = nullptr;

bool window_has_class(HWND hwnd, const wchar_t* wanted) {
    wchar_t class_name[96]{};
    if (!GetClassNameW(hwnd, class_name, 96)) return false;
    return std::wcscmp(class_name, wanted) == 0;
}

bool is_automatic_window(HWND hwnd) {
    return window_has_class(hwnd, L"PixelForgeAutomaticWindow");
}

bool is_review_window(HWND hwnd) {
    return window_has_class(hwnd, L"PixelForgeUserReviewWindow");
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

LRESULT CALLBACK review_intelligence_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const auto original = g_review_original_proc;
    if (!original) return DefWindowProcW(hwnd, msg, wparam, lparam);

    // The normal revision path reads the owner's LEVEL combo immediately after
    // the review window accepts the change request. Mirror the popup's selection
    // into that existing control before main_auto.cpp handles Request changes.
    if (msg == WM_COMMAND && LOWORD(wparam) == ID_REVIEW_CHANGES) {
        const LRESULT selection = g_revision_level
            ? SendMessageW(g_revision_level, CB_GETCURSEL, 0, 0)
            : CB_ERR;
        const HWND owner = GetWindow(hwnd, GW_OWNER);
        const HWND owner_level = owner ? GetDlgItem(owner, ID_INTELLIGENCE) : nullptr;
        if (selection != CB_ERR && owner_level)
            SendMessageW(owner_level, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }

    const LRESULT result = CallWindowProcW(original, hwnd, msg, wparam, lparam);
    if (msg == WM_NCDESTROY && hwnd == g_review_window) {
        g_review_window = nullptr;
        g_revision_level = nullptr;
        g_review_original_proc = nullptr;
    }
    return result;
}

void attach_review_intelligence(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || hwnd == g_review_window) return;

    const HWND owner = GetWindow(hwnd, GW_OWNER);
    const HWND owner_level = owner ? GetDlgItem(owner, ID_INTELLIGENCE) : nullptr;
    LRESULT initial_selection = owner_level ? SendMessageW(owner_level, CB_GETCURSEL, 0, 0) : 1;
    if (initial_selection == CB_ERR) initial_selection = 1;

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    // Make room below the feedback box for a per-revision intelligence choice.
    RECT window_rect{};
    if (GetWindowRect(hwnd, &window_rect)) {
        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        SetWindowPos(hwnd, nullptr, 0, 0, width, height + 54,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (HWND accept = GetDlgItem(hwnd, ID_REVIEW_ACCEPT))
        MoveWindow(accept, 18, 326, 140, 32, TRUE);
    if (HWND changes = GetDlgItem(hwnd, ID_REVIEW_CHANGES))
        MoveWindow(changes, 170, 326, 302, 32, TRUE);

    HWND label = CreateWindowW(L"STATIC", L"REVISION LEVEL", WS_CHILD | WS_VISIBLE,
                               18, 274, 104, 20, hwnd, nullptr, nullptr, nullptr);
    if (label) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    g_revision_level = CreateWindowW(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        126, 270, 150, 160, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REVIEW_REVISION_LEVEL)), nullptr, nullptr);
    if (g_revision_level) {
        SendMessageW(g_revision_level, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        for (const wchar_t* level : {L"Low", L"Medium", L"High", L"Extra High", L"Max"})
            SendMessageW(g_revision_level, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(level));
        SendMessageW(g_revision_level, CB_SETDROPPEDWIDTH, 170, 0);
        SendMessageW(g_revision_level, CB_SETCURSEL, static_cast<WPARAM>(initial_selection), 0);
    }

    HWND hint = CreateWindowW(L"STATIC", L"Use Low/Medium for small targeted fixes.",
                              WS_CHILD | WS_VISIBLE, 288, 274, 184, 36,
                              hwnd, nullptr, nullptr, nullptr);
    if (hint) SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    g_review_window = hwnd;
    g_review_original_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(review_intelligence_proc)));
}

LRESULT CALLBACK attach_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const auto* message = reinterpret_cast<const CWPRETSTRUCT*>(lparam);
        if (message->hwnd && is_automatic_window(message->hwnd) &&
            (message->message == WM_CREATE || message->message == WM_SHOWWINDOW)) {
            attach_repaint_guard(message->hwnd);
        }
        if (message->hwnd && is_review_window(message->hwnd) &&
            (message->message == WM_CREATE || message->message == WM_SHOWWINDOW)) {
            attach_review_intelligence(message->hwnd);
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