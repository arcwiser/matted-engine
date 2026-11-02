// Matted Engine - Win32/GDI window implementation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "engine/render/window.h"
#include <cstring>
#include <string>

namespace mtt {

struct Window::Impl {
    HWND hwnd = nullptr;
    BITMAPINFO bmi{};
    // input state
    bool keys_prev[256] = {};
    bool keys_now[256] = {};
    bool mouse_prev[3] = {};
    bool mouse_now[3] = {};
    i32  mouse_acc_x = 0, mouse_acc_y = 0;
    bool rmb_captured = false;
    POINT capture_anchor{};
};

static Window::Impl* g_wnd_impl = nullptr; // single-window demo scope

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window::Impl* im = g_wnd_impl;
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_MOUSEMOVE: {
            if (im && im->rmb_captured) {
                const i32 x = (i32)(i16)LOWORD(lp);
                const i32 y = (i32)(i16)HIWORD(lp);
                im->mouse_acc_x += x - im->capture_anchor.x;
                im->mouse_acc_y += y - im->capture_anchor.y;
                // re-anchor so deltas accumulate indefinitely
                POINT p = { im->capture_anchor.x, im->capture_anchor.y };
                ClientToScreen(hwnd, &p);
                SetCursorPos(p.x, p.y);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (im && im->rmb_captured) { SetCursor(nullptr); return TRUE; }
            break;
        default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

bool Window::create(const char* title, u32 width, u32 height) {
    impl_ = new Impl();
    g_wnd_impl = impl_;
    width_ = width; height_ = height;

    WNDCLASSA wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512)); // IDC_ARROW
    wc.lpszClassName = "MattedEngineWindow";
    RegisterClassA(&wc);

    RECT rc = { 0, 0, (LONG)width, (LONG)height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    impl_->hwnd = CreateWindowA(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!impl_->hwnd) return false;

    impl_->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    impl_->bmi.bmiHeader.biWidth = (LONG)width;
    impl_->bmi.bmiHeader.biHeight = -(LONG)height; // top-down
    impl_->bmi.bmiHeader.biPlanes = 1;
    impl_->bmi.bmiHeader.biBitCount = 32;
    impl_->bmi.bmiHeader.biCompression = BI_RGB;

    ShowWindow(impl_->hwnd, SW_SHOW);
    return true;
}

Window::~Window() {
    if (impl_) {
        if (impl_->hwnd) DestroyWindow(impl_->hwnd);
        delete impl_;
        g_wnd_impl = nullptr;
    }
}

bool Window::pump() {
    MSG msg;
    std::memcpy(impl_->keys_prev, impl_->keys_now, sizeof(impl_->keys_prev));
    std::memcpy(impl_->mouse_prev, impl_->mouse_now, sizeof(impl_->mouse_prev));

    // poll current key state
    for (u32 k = 0; k < 256; ++k) impl_->keys_now[k] = (GetAsyncKeyState((int)k) & 0x8000) != 0;
    impl_->mouse_now[VK_LBUTTON] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    impl_->mouse_now[VK_RBUTTON] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    // RMB look-capture
    if (impl_->mouse_now[VK_RBUTTON] && !impl_->mouse_prev[VK_RBUTTON]) {
        RECT rc; GetClientRect(impl_->hwnd, &rc);
        POINT p = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(impl_->hwnd, &p);
        SetCursorPos(p.x, p.y);
        impl_->capture_anchor = { p.x, p.y };
        impl_->rmb_captured = true;
        SetCapture(impl_->hwnd);
    } else if (!impl_->mouse_now[VK_RBUTTON] && impl_->mouse_prev[VK_RBUTTON]) {
        impl_->rmb_captured = false;
        ReleaseCapture();
    }

    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { should_close_ = true; return false; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return !should_close_;
}

void Window::present(const u32* pixels_bgra, u32 width, u32 height, const char* hud_text) {
    HDC dc = GetDC(impl_->hwnd);
    RECT rc; GetClientRect(impl_->hwnd, &rc);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom,
                  0, 0, (LONG)width, (LONG)height,
                  pixels_bgra, &impl_->bmi, DIB_RGB_COLORS, SRCCOPY);
    if (hud_text && *hud_text) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 230, 235));
        SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
        RECT trc = { 12, rc.bottom - 26, rc.right - 12, rc.bottom - 6 };
        DrawTextA(dc, hud_text, -1, &trc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    ReleaseDC(impl_->hwnd, dc);
}

bool Window::key_down(u8 vkey) const { return impl_->keys_now[vkey]; }
bool Window::key_pressed(u8 vkey) { return impl_->keys_now[vkey] && !impl_->keys_prev[vkey]; }
bool Window::mouse_down(u8 button) const { return impl_->mouse_now[button & 1]; }
bool Window::mouse_clicked(u8 button) {
    const u8 b = button & 1;
    return impl_->mouse_now[b] && !impl_->mouse_prev[b];
}
void Window::mouse_delta(i32& dx, i32& dy) {
    dx = impl_->mouse_acc_x; dy = impl_->mouse_acc_y;
    impl_->mouse_acc_x = 0; impl_->mouse_acc_y = 0;
}

} // namespace mtt
