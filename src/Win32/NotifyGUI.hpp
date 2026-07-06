#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// NotifyGUI — lightweight ImGui overlay notification system.
//
// Thread-safe: Add() may be called from any thread (background scanner, etc.).
// Render() must be called from the main/render thread each frame, after
// ImGui::NewFrame() and before Renderer::RenderFrame().
//
// Usage:
//   NotifyGUI::Add("Skipped: foo.sys  [UPX]",
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
    bool GetInsideWindow();

    // Enqueue a notification. Thread-safe.
    //   message  — text to display (truncated to 255 chars)
    //   position — corner to stack notifications in
    //   duration — seconds before the notification disappears (fades last 1 s)
    //   color    — text color; build with NOTIFY_COL32(r,g,b,a)
    void Add(const char* message,
        NotifyPosition position = NotifyPosition::BOTTOM_RIGHT,
        float          duration = 5.0f,
        std::uint32_t  color = NOTIFY_COL32(255, 255, 255, 255));

    // Draw/process all active notifications.
    // Call once per frame: after ImGui::NewFrame(), before Renderer::RenderFrame().
    void Render();
}