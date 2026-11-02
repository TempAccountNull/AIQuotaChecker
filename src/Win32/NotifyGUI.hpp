#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// NotifyGUI - provider toasts, either native Win32 popups or an ImGui overlay
// inside the widget.
//
// Thread-safe: Add() may be called from any thread (provider refreshes).
// Render() must be called from the main thread each frame, between
// ImGui::NewFrame() and ImGui::Render().
//
// Usage:
//   NotifyGUI::Add("Claude: 90% of the weekly limit used",
//                  NotifyPosition::BOTTOM_RIGHT, 6.0f,
//                  NOTIFY_COL32(255, 200, 60, 255));
// ---------------------------------------------------------------------------

enum class NotifyPosition
{
    TOP_LEFT,
    TOP_RIGHT,
    BOTTOM_LEFT,
    BOTTOM_RIGHT,
    CENTER
};

// Pack an RGBA color for NotifyGUI::Add(). Layout matches ImGui's IM_COL32:
//   result = (A << 24) | (B << 16) | (G << 8) | R
#define NOTIFY_COL32(r, g, b, a)                         \
    (  (static_cast<std::uint32_t>(a) << 24)             \
     | (static_cast<std::uint32_t>(b) << 16)             \
     | (static_cast<std::uint32_t>(g) <<  8)             \
     |  static_cast<std::uint32_t>(r)          )

namespace NotifyGUI
{
    // Select where new notifications appear.
    // false = native Win32 toast outside the app window.
    // true  = ImGui overlay inside the app window.
    void SetInsideWindow(bool enabled);

    // Native toasts appear on this window's monitor (the widget).
    void SetAnchorWindow(HWND window);

    // Enqueue a notification. Thread-safe.
    //   message  - text to display (truncated to 255 chars)
    //   position - corner to stack notifications in
    //   duration - seconds before the notification disappears (fades last 1 s)
    //   color    - text color; build with NOTIFY_COL32(r,g,b,a)
    void Add(const char* message,
        NotifyPosition position = NotifyPosition::BOTTOM_RIGHT,
        float          duration = 5.0f,
        std::uint32_t  color = NOTIFY_COL32(255, 255, 255, 255));

    // Draw/process all active notifications. Call once per frame, between
    // ImGui::NewFrame() and ImGui::Render().
    void Render();
}