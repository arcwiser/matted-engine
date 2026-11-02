// Matted Engine - Win32/GDI window (interactive demo target).
// Blits the software rasterizer's BGRA framebuffer with StretchDIBits and
// draws a text HUD on top. All Win32 details are contained in the .cpp.
#pragma once
#include "engine/core/mtt_types.h"

#ifdef _WIN32

namespace mtt {

class Window {
public:
    struct Impl; // Win32 details (defined in window.cpp)
    static constexpr u8 kMouseLeft = 0x01;   // VK_LBUTTON
    static constexpr u8 kMouseRight = 0x02;  // VK_RBUTTON

    bool create(const char* title, u32 width, u32 height);
    ~Window();

    // pump OS messages; returns false once the window should close
    bool pump();
    // blit framebuffer (+ optional HUD line drawn on top)
    void present(const u32* pixels_bgra, u32 width, u32 height, const char* hud_text = nullptr);

    // ---- input (polled per frame) ----
    bool key_down(u8 vkey) const;      // held
    bool key_pressed(u8 vkey);         // true once per press
    bool mouse_down(u8 button) const;  // VK_LBUTTON / VK_RBUTTON
    bool mouse_clicked(u8 button);     // true once per click
    void mouse_delta(i32& dx, i32& dy);// accumulated since last call
    bool should_close() const { return should_close_; }
    u32  width()  const { return width_; }
    u32  height() const { return height_; }

private:
    Impl* impl_ = nullptr;
    u32 width_ = 0, height_ = 0;
    bool should_close_ = false;
};

} // namespace mtt

#endif // _WIN32
