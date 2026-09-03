#include "Global.hpp"

#include <windows.h>
#include <d3d11.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "Resource.h"
#include "AppSettings.hpp"
#include "CodexProvider.hpp"
#include "ClaudeProvider.hpp"
#include "KSharedClock.hpp"
#include "NotifyGUI.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Renderer.hpp"
#include "ResetTime.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"
#include "GrokProvider.hpp"
#include "ZAiProvider.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

#pragma comment(lib, "d3d11.lib")

#ifndef IDI_AIQUOTACHECKER
#define IDI_AIQUOTACHECKER 107
#endif

namespace
{
    static ID3D11Device* g_device = nullptr;
    static ID3D11DeviceContext* g_context = nullptr;
    static IDXGISwapChain* g_swapChain = nullptr;
    static ID3D11RenderTargetView* g_renderTarget = nullptr;

    static HWND g_hwnd = nullptr;
    static bool g_shouldClose = false;

    // Settings lives in its own top-level window with its own ImGui context, so
    // the widget keeps rendering behind it.
    static bool g_settingsWindowOpen = false;
    static HWND g_settingsHwnd = nullptr;
    static IDXGISwapChain* g_settingsSwapChain = nullptr;
    static ID3D11RenderTargetView* g_settingsRenderTarget = nullptr;
    static ImGuiContext* g_mainImGuiContext = nullptr;
    static ImGuiContext* g_settingsImGuiContext = nullptr;

    static constexpr int kTitleBarHeight = 38;
    static constexpr int kResizeBorder = 6;
    // Width of the [-][widget][X] strip drawn by Renderer::DrawCustomTitleBar.
    // Everything else in the title bar is HTCAPTION, and Windows consumes those
    // clicks for dragging before ImGui can see them.
    static constexpr int kTitleBarButtonStrip = 120;

    static constexpr int kWidgetWidth = 360;
    static constexpr int kWidgetHeight = 620;
    static constexpr int kWidgetMargin = 12;
    // Fallback for how much of the widget stays on screen once it retracts,
    // used only before the first frame has been measured. Normally the sliver
    // is exactly the renderer's summary strip so the selected host's quota and
    // reset stay readable while rolled up.
    static constexpr int kWidgetSliver = 30;

    static int WidgetSliverHeight()
    {
        const int measured = Renderer::WidgetPeekHeight();
        return measured > 0 ? measured : kWidgetSliver;
    }
    // Auto-sizing bounds. The floor keeps the header, tab strip and footer
    // usable even when every data card is switched off.
    static constexpr int kWidgetMinHeight = 190;
    static constexpr int kWidgetResizeDeadband = 3;

    // Keep in sync with the pill row drawn by Renderer::DrawWidgetUi.
    static constexpr int kWidgetButtonStrip = 136;
    static constexpr double kWidgetRetractAfterSeconds = 1.2;
    // Retracting is a deliberate, watchable slide; dropping back down has to
    // feel immediate because the pointer is already reaching for the panel.
    static constexpr double kWidgetRetractPixelsPerSecond = 170.0;
    static constexpr double kWidgetExpandPixelsPerSecond = 2600.0;

    static bool g_minimizeRequest = false;
    static bool g_widgetMode = false;
    // Pinned keeps the panel down: no auto-retract, only the hover slide is
    // suppressed - the pill still toggles it back.
    static bool g_widgetPinned = false;
    // True once the slide has reached its target, so a later position change
    // can be attributed to the user dragging the panel rather than to us.
    static bool g_widgetSettled = false;
    static int g_widgetAnchor = AppSettings::WidgetAnchorTopRight;
    // Resting top-left for the current anchor, and where the slide is heading.
    // Panel height when fully out; the retracted rect is derived from it.
    static int g_widgetFullHeight = kWidgetHeight;
    // Work area of the monitor the panel lives on, captured while it is still
    // on screen. Re-deriving it from the window each frame walks the panel onto
    // a neighbouring monitor as it slides off the edge of this one.
    static RECT g_widgetWork{};

    // ---- monitors -------------------------------------------------------
    //
    // The panel parks on one chosen monitor. Snapping is always relative to
    // that monitor's work area, never the virtual desktop: on a multi-monitor
    // desktop the latter rolls the panel off one screen and onto the next.

    struct MonitorEntry
    {
        RECT work{};
        std::string label;
    };

    static std::vector<MonitorEntry> g_monitors;
    static int g_widgetMonitor = 0;
    static std::vector<std::string> g_monitorLabels;
    static std::vector<const char*> g_monitorLabelPtrs;

    static BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param)
    {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return TRUE;
        }

        MonitorEntry entry;
        entry.work = info.rcWork;

        char label[128]{};
        std::snprintf(label, sizeof(label), "%dx%d%s",
            static_cast<int>(info.rcMonitor.right - info.rcMonitor.left),
            static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top),
            (info.dwFlags & MONITORINFOF_PRIMARY) ? "  (primary)" : "");
        entry.label = label;

        auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(param);
        if (info.dwFlags & MONITORINFOF_PRIMARY) {
            out->insert(out->begin(), std::move(entry));
        }
        else {
            out->push_back(std::move(entry));
        }
        return TRUE;
    }

    static void RefreshMonitors()
    {
        std::vector<MonitorEntry> found;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
            reinterpret_cast<LPARAM>(&found));

        if (found.empty()) {
            RECT primary{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            found.push_back({ primary, "Primary" });
        }

        // Primary first (CollectMonitor front-inserts it), then left to right so
        // the picker order matches how the screens are actually arranged.
        std::stable_sort(found.begin() + 1, found.end(),
            [](const MonitorEntry& a, const MonitorEntry& b) { return a.work.left < b.work.left; });

        g_monitors = std::move(found);

        g_monitorLabels.clear();
        g_monitorLabelPtrs.clear();
        for (size_t i = 0; i < g_monitors.size(); ++i) {
            g_monitorLabels.push_back(
                "Monitor " + std::to_string(i + 1) + " - " + g_monitors[i].label);
        }
        for (const std::string& label : g_monitorLabels) {
            g_monitorLabelPtrs.push_back(label.c_str());
        }
    }

    static int ClampWidgetMonitor(int index)
    {
        if (g_monitors.empty()) RefreshMonitors();
        if (index < 0 || index >= static_cast<int>(g_monitors.size())) return 0;
        return index;
    }

    static RECT WidgetMonitorWork()
    {
        if (g_monitors.empty()) RefreshMonitors();
        return g_monitors[static_cast<size_t>(ClampWidgetMonitor(g_widgetMonitor))].work;
    }

    // Which monitor a dropped window landed on, by its centre point.
    static int MonitorIndexForRect(const RECT& rect)
    {
        if (g_monitors.empty()) RefreshMonitors();

        const POINT center{
            static_cast<int>((rect.left + rect.right) / 2),
            static_cast<int>((rect.top + rect.bottom) / 2)
        };

        for (size_t i = 0; i < g_monitors.size(); ++i) {
            if (PtInRect(&g_monitors[i].work, center)) return static_cast<int>(i);
        }
        return ClampWidgetMonitor(g_widgetMonitor);
    }
    static std::string g_widgetOrder = "codex,claude,zai,grok";
    static int g_uiFontSize = 14;
    static int g_widgetSections = AppSettings::kWidgetSectionAll;

    static bool g_showRemaining = false;
    static bool g_showResetDateDetails = false;
    static int g_resetDisplayMode = ResetTime::Static;
    static bool g_showNotificationsInsideWindow = false;
    static bool g_autoRefreshEnabled = true;
    static int g_autoRefreshMinutes = 1;
    static bool g_codexAutoRefreshEnabled = true;
    static bool g_claudeAutoRefreshEnabled = true;
    static bool g_zaiAutoRefreshEnabled = true;
    static bool g_grokAutoRefreshEnabled = true;
    static int g_claudeAccountSource = 0;
    static int g_claudeThinkingShimmerSpeedPercent = 100;
    static int g_codexAccountSource = 0;
    static std::string g_codexCustomAuthPath;

    enum AutoRefreshProviderMask : unsigned int
    {
        AutoRefreshProviderCodex = 1u << 0,
        AutoRefreshProviderClaude = 1u << 1,
        AutoRefreshProviderZAi = 1u << 2,
        AutoRefreshProviderGrok = 1u << 3
    };

    static std::atomic_uint g_autoRefreshDisableMask = 0;
    static std::mutex g_autoRefreshWarningMutex;
    static std::string g_autoRefreshWarning;

    static int g_notifyPositionIndex = 3;
    static NotifyPosition g_notifyPosition = NotifyPosition::BOTTOM_RIGHT;

    static Renderer::State g_rendererState;

    static const char* g_notifyPositionNames[] = {
        "Top Left",
        "Top Right",
        "Bottom Left",
        "Bottom Right",
        "Center"
    };

    static NotifyPosition g_notifyPositions[] = {
        NotifyPosition::TOP_LEFT,
        NotifyPosition::TOP_RIGHT,
        NotifyPosition::BOTTOM_LEFT,
        NotifyPosition::BOTTOM_RIGHT,
        NotifyPosition::CENTER
    };

    static unsigned int ProviderAutoRefreshMask(const char* provider)
    {
        if (!provider) return 0;
        if (_stricmp(provider, "Codex") == 0) return AutoRefreshProviderCodex;
        if (_stricmp(provider, "Claude") == 0) return AutoRefreshProviderClaude;
        if (_stricmp(provider, "Z.Ai") == 0 || _stricmp(provider, "ZAi") == 0) return AutoRefreshProviderZAi;
        if (_stricmp(provider, "Grok") == 0) return AutoRefreshProviderGrok;
        return 0;
    }

    static void RequestAutoRefreshDisableForRateLimit(const char* provider, const std::string& detail)
    {
        const unsigned int mask = ProviderAutoRefreshMask(provider);

        if (mask == 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_autoRefreshWarningMutex);

            g_autoRefreshWarning = "Warning: Auto refresh disabled for ";
            g_autoRefreshWarning += provider;
            g_autoRefreshWarning += " because that provider was rate limited";

            if (!detail.empty()) {
                g_autoRefreshWarning += " (";
                g_autoRefreshWarning += detail;
                g_autoRefreshWarning += ")";
            }

            g_autoRefreshWarning += ". Other providers remain enabled.";
        }

        g_autoRefreshDisableMask.fetch_or(mask);
    }

    static std::string GetAutoRefreshWarning()
    {
        std::lock_guard<std::mutex> lock(g_autoRefreshWarningMutex);
        return g_autoRefreshWarning;
    }

    static void ApplyPendingAutoRefreshDisable()
    {
        const unsigned int mask = g_autoRefreshDisableMask.exchange(0);

        if (mask == 0) {
            return;
        }

        if ((mask & AutoRefreshProviderCodex) != 0) g_codexAutoRefreshEnabled = false;
        if ((mask & AutoRefreshProviderClaude) != 0) g_claudeAutoRefreshEnabled = false;
        if ((mask & AutoRefreshProviderZAi) != 0) g_zaiAutoRefreshEnabled = false;
        if ((mask & AutoRefreshProviderGrok) != 0) g_grokAutoRefreshEnabled = false;

        std::string warning = GetAutoRefreshWarning();

        if (warning.empty()) {
            warning = "Warning: Auto refresh disabled for the rate-limited provider.";
        }

        NotifyGUI::Add(warning.c_str(), g_notifyPosition, 10.0f, NOTIFY_COL32(255, 200, 64, 255));
    }

    static int AutoRefreshIntervalSeconds()
    {
        return AppSettings::ClampAutoRefreshMinutes(g_autoRefreshMinutes) * 60;
    }

    static void ApplySettingsToRuntime()
    {
        g_notifyPositionIndex = AppSettings::ClampNotificationPositionIndex(g_notifyPositionIndex);
        g_notifyPosition = g_notifyPositions[g_notifyPositionIndex];

        NotifyGUI::SetInsideWindow(g_showNotificationsInsideWindow);

        int repeatSeconds = AutoRefreshIntervalSeconds();
        CodexProvider::get_instance()->SetAccountSource(g_codexAccountSource, g_codexCustomAuthPath);
        CodexProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
        ClaudeProvider::get_instance()->SetAccountSource(g_claudeAccountSource);
        ClaudeProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
        ZAiProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
        GrokProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
    }

    static void LoadAppSettings()
    {
        AppSettings::Settings settings;
        AppSettings::Load(settings);

        g_showRemaining = settings.showRemaining;
#if defined(GUI_AND_WIDGET)
        g_widgetMode = settings.widgetMode;
#elif defined(GUI_ONLY)
        g_widgetMode = false;
#else
        g_widgetMode = true;
#endif
        g_widgetPinned = settings.widgetPinned;
        g_widgetAnchor = AppSettings::ClampWidgetAnchor(settings.widgetAnchor);
        g_widgetMonitor = ClampWidgetMonitor(settings.widgetMonitor);
        g_widgetOrder = settings.widgetOrder;
        g_uiFontSize = AppSettings::ClampUiFontSize(settings.uiFontSize);
        g_widgetSections = settings.widgetSections;
        g_showResetDateDetails = settings.showResetDateDetails;
        g_resetDisplayMode = AppSettings::ClampResetDisplayMode(settings.resetDisplayMode);
        g_showNotificationsInsideWindow = settings.notificationsInsideWindow;
        g_notifyPositionIndex = settings.notificationPositionIndex;
        g_autoRefreshEnabled = settings.autoRefreshEnabled;
        g_autoRefreshMinutes = AppSettings::ClampAutoRefreshMinutes(settings.autoRefreshMinutes);
        g_codexAutoRefreshEnabled = settings.codexAutoRefreshEnabled;
        g_claudeAutoRefreshEnabled = settings.claudeAutoRefreshEnabled;
        g_zaiAutoRefreshEnabled = settings.zaiAutoRefreshEnabled;
        g_grokAutoRefreshEnabled = settings.grokAutoRefreshEnabled;
        g_claudeAccountSource = AppSettings::ClampClaudeAccountSource(settings.claudeAccountSource);
        g_claudeThinkingShimmerSpeedPercent = AppSettings::ClampClaudeThinkingShimmerSpeedPercent(
            settings.claudeThinkingShimmerSpeedPercent
        );
        g_codexAccountSource = AppSettings::ClampCodexAccountSource(settings.codexAccountSource);
        g_codexCustomAuthPath = settings.codexCustomAuthPath;

        CodexProvider::get_instance()->LoadSettings(settings);
        ClaudeProvider::get_instance()->LoadSettings(settings);
        ZAiProvider::get_instance()->LoadSettings(settings);
        GrokProvider::get_instance()->LoadSettings(settings);

        ApplySettingsToRuntime();
    }

    static bool SaveAppSettings()
    {
        AppSettings::Settings settings;
        settings.showRemaining = g_showRemaining;
        settings.widgetMode = g_widgetMode;
        settings.widgetPinned = g_widgetPinned;
        settings.widgetAnchor = AppSettings::ClampWidgetAnchor(g_widgetAnchor);
        settings.widgetMonitor = g_widgetMonitor;
        settings.widgetOrder = g_widgetOrder;
        settings.uiFontSize = AppSettings::ClampUiFontSize(g_uiFontSize);
        settings.widgetSections = g_widgetSections;
        settings.showResetDateDetails = g_showResetDateDetails;
        settings.resetDisplayMode = AppSettings::ClampResetDisplayMode(g_resetDisplayMode);
        settings.notificationsInsideWindow = g_showNotificationsInsideWindow;
        settings.notificationPositionIndex = g_notifyPositionIndex;
        settings.autoRefreshEnabled = g_autoRefreshEnabled;
        settings.autoRefreshMinutes = AppSettings::ClampAutoRefreshMinutes(g_autoRefreshMinutes);
        settings.codexAutoRefreshEnabled = g_codexAutoRefreshEnabled;
        settings.claudeAutoRefreshEnabled = g_claudeAutoRefreshEnabled;
        settings.zaiAutoRefreshEnabled = g_zaiAutoRefreshEnabled;
        settings.grokAutoRefreshEnabled = g_grokAutoRefreshEnabled;
        settings.claudeAccountSource = AppSettings::ClampClaudeAccountSource(g_claudeAccountSource);
        settings.claudeThinkingShimmerSpeedPercent = AppSettings::ClampClaudeThinkingShimmerSpeedPercent(
            g_claudeThinkingShimmerSpeedPercent
        );
        settings.codexAccountSource = AppSettings::ClampCodexAccountSource(g_codexAccountSource);
        settings.codexCustomAuthPath = g_codexCustomAuthPath;

        CodexProvider::get_instance()->SaveSettings(settings);
        ClaudeProvider::get_instance()->SaveSettings(settings);
        ZAiProvider::get_instance()->SaveSettings(settings);
        GrokProvider::get_instance()->SaveSettings(settings);

        return AppSettings::Save(settings);
    }

    static void RefreshCodexAsync()
    {
        CodexProvider::get_instance()->RefreshAsync();
    }

    static void RefreshClaudeAsync()
    {
        ClaudeProvider::get_instance()->RefreshAsync();
    }

    static void RefreshZAiAsync()
    {
        ZAiProvider::get_instance()->RefreshAsync();
    }

    static void RefreshGrokAsync()
    {
        GrokProvider::get_instance()->RefreshAsync();
    }

    static void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;

        if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
            backBuffer->Release();
        }
    }

    static void CleanupRenderTarget()
    {
        if (g_renderTarget) {
            g_renderTarget->Release();
            g_renderTarget = nullptr;
        }
    }

    static bool CreateDeviceD3D(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT flags = 0;

#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevel{};
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_swapChain,
            &g_device,
            &featureLevel,
            &g_context
        );

        if (FAILED(hr)) {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    static void CleanupDeviceD3D()
    {
        CleanupRenderTarget();

        if (g_swapChain) {
            g_swapChain->Release();
            g_swapChain = nullptr;
        }

        if (g_context) {
            g_context->Release();
            g_context = nullptr;
        }

        if (g_device) {
            g_device->Release();
            g_device = nullptr;
        }
    }

    static void DestroySettingsWindow();
    static void RepositionWidget();
    static RECT WidgetRestRect(bool retracted, int fullHeight);

    static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        // Two windows, two ImGui contexts: the backend reads state from the
        // CURRENT context, so select the one that owns this hwnd first.
        if (hwnd == g_settingsHwnd && g_settingsImGuiContext) {
            ImGui::SetCurrentContext(g_settingsImGuiContext);
        }
        else if (g_mainImGuiContext) {
            ImGui::SetCurrentContext(g_mainImGuiContext);
        }

        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
            return true;
        }

        if (hwnd == g_settingsHwnd) {
            switch (msg) {
            case WM_SIZE:
                if (g_settingsSwapChain && wparam != SIZE_MINIMIZED) {
                    if (g_settingsRenderTarget) {
                        g_settingsRenderTarget->Release();
                        g_settingsRenderTarget = nullptr;
                    }
                    g_settingsSwapChain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam),
                        DXGI_FORMAT_UNKNOWN, 0);
                    ID3D11Texture2D* back = nullptr;
                    if (SUCCEEDED(g_settingsSwapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                        g_device->CreateRenderTargetView(back, nullptr, &g_settingsRenderTarget);
                        back->Release();
                    }
                }
                return 0;

            case WM_CLOSE:
                g_settingsWindowOpen = false;
                return 0;

            case WM_NCHITTEST: {
                // Drag by the title strip, but leave the Close button clickable.
                POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                RECT rc{};
                GetWindowRect(hwnd, &rc);

                const int x = pt.x - rc.left;
                const int y = pt.y - rc.top;
                const int w = rc.right - rc.left;

                if (y >= 0 && y < 40 && x >= 0 && x < w - 96) {
                    return HTCAPTION;
                }

                return HTCLIENT;
            }
            }

            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        switch (msg) {
        case WM_NCHITTEST:
        {
            POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

            RECT rc{};
            GetWindowRect(hwnd, &rc);

            int x = pt.x - rc.left;
            int y = pt.y - rc.top;

            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            bool left = x >= 0 && x < kResizeBorder;
            bool right = x <= w && x >= w - kResizeBorder;
            bool top = y >= 0 && y < kResizeBorder;
            bool bottom = y <= h && y >= h - kResizeBorder;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            const int captionHeight = g_widgetMode ? 34 : kTitleBarHeight;
            // The widget header carries four pills (pin / refresh / restore /
            // close) spanning ~114px from the right edge. Anything narrower
            // reports HTCAPTION over them and Windows consumes the click as a
            // window drag before ImGui ever sees a button press.
            const int buttonStrip = g_widgetMode ? kWidgetButtonStrip : kTitleBarButtonStrip;
            bool inButtonArea = y >= 0 && y < captionHeight && x >= w - buttonStrip && x < w;

            if (!inButtonArea && y >= 0 && y < captionHeight) {
                return HTCAPTION;
            }

            return HTCLIENT;
        }

        case WM_SIZE:
            // Scope to the main window explicitly: a newly created window
            // receives WM_SIZE during CreateWindowExW, before its handle has
            // been stored, so it would otherwise resize the widget's swapchain.
            if (hwnd == g_hwnd && g_device && wparam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            // Monitors added, removed or rearranged: the stored index and the
            // cached work area both go stale, so rebuild and re-park.
            RefreshMonitors();
            g_widgetMonitor = ClampWidgetMonitor(g_widgetMonitor);
            g_rendererState.widgetMonitorNames = g_monitorLabelPtrs.data();
            g_rendererState.widgetMonitorCount = static_cast<int>(g_monitorLabelPtrs.size());
            RepositionWidget();
            return 0;

        case WM_DESTROY:
            // Only the widget closing ends the app; destroying the settings
            // window must not post WM_QUIT.
            if (hwnd == g_hwnd) {
                PostQuitMessage(0);
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // -----------------------------------------------------------------------
    // Widget mode
    //
    // Snap to the nearest work-area corner, float above other windows, and
    // slide off the nearest horizontal screen edge when the pointer leaves,
    // leaving a hoverable sliver behind. Hover is polled from the cursor
    // position rather than WM_MOUSELEAVE so it also works unfocused.
    // -----------------------------------------------------------------------
    static RECT g_widgetRestoreRect{};
    static bool g_widgetRestoreValid = false;
    static bool g_widgetApplied = false;
    static bool g_widgetRetracted = false;
    static double g_widgetLastHoverSeconds = 0.0;
    static double g_widgetLastAnimationSeconds = 0.0;

    static double MonotonicSeconds()
    {
        static LARGE_INTEGER frequency{};

        if (frequency.QuadPart == 0) {
            QueryPerformanceFrequency(&frequency);
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
    }

    static RECT WorkAreaForWindow(HWND hwnd)
    {
        MONITORINFO info{};
        info.cbSize = sizeof(info);

        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
            return info.rcWork;
        }

        RECT fallback{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        return fallback;
    }

    // Top-left corner for a given anchor, inset by the margin. Centre columns
    // and the middle row centre on the work area rather than an edge.
    static POINT AnchorOrigin(int anchor, const RECT& work, int width, int height)
    {
        const int row = AppSettings::WidgetAnchorRow(anchor);
        const int col = AppSettings::WidgetAnchorCol(anchor);
        const int spanX = static_cast<int>(work.right - work.left);

        POINT p{};
        p.x = col == 0 ? static_cast<int>(work.left) + kWidgetMargin
            : col == 1 ? static_cast<int>(work.left) + (spanX - width) / 2
                       : static_cast<int>(work.right) - width - kWidgetMargin;
        p.y = row == 0 ? static_cast<int>(work.top) + kWidgetMargin
                       : static_cast<int>(work.bottom) - height - kWidgetMargin;
        return p;
    }

    // Nearest parking spot for a dropped window, from where its centre landed.
    static int AnchorForRect(const RECT& rect, const RECT& work)
    {
        const int spanX = (std::max)(1, static_cast<int>(work.right - work.left));
        const int spanY = (std::max)(1, static_cast<int>(work.bottom - work.top));
        const int cx = static_cast<int>((rect.left + rect.right) / 2 - work.left);
        const int cy = static_cast<int>((rect.top + rect.bottom) / 2 - work.top);

        const int col = cx < spanX / 3 ? 0 : (cx < spanX * 2 / 3 ? 1 : 2);
        const int row = cy < spanY / 2 ? 0 : 1;

        return AppSettings::WidgetAnchorFrom(row, col);
    }

    static void EnterWidgetMode()
    {
        if (GetWindowRect(g_hwnd, &g_widgetRestoreRect)) {
            g_widgetRestoreValid = true;
        }

        SetWindowLongPtrW(
            g_hwnd,
            GWL_EXSTYLE,
            GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_TOPMOST
        );

        // Park at the stored anchor and roll into whichever edge it sits on,
        // macOS drop-down style for the top row and the mirror of that for the
        // bottom row and the sides.
        g_widgetWork = WidgetMonitorWork();
        g_widgetFullHeight = kWidgetHeight;
        g_widgetRetracted = false;
        g_widgetSettled = false;
        g_widgetLastHoverSeconds = MonotonicSeconds();

        const RECT rest = WidgetRestRect(false, g_widgetFullHeight);

        SetWindowPos(
            g_hwnd,
            HWND_TOPMOST,
            rest.left,
            rest.top,
            rest.right - rest.left,
            rest.bottom - rest.top,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED
        );
    }

    static void LeaveWidgetMode()
    {
        SetWindowLongPtrW(
            g_hwnd,
            GWL_EXSTYLE,
            GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & ~(WS_EX_TOOLWINDOW | WS_EX_TOPMOST)
        );

        const RECT r = g_widgetRestoreValid
            ? g_widgetRestoreRect
            : RECT{ 100, 100, 1140, 860 };

        SetWindowPos(
            g_hwnd,
            HWND_NOTOPMOST,
            r.left,
            r.top,
            r.right - r.left,
            r.bottom - r.top,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED
        );

        g_widgetRetracted = false;
    }

    // Where the panel goes when it rolls away.
    //
    // It COLLAPSES into its anchor edge rather than sliding off the screen.
    // Sliding hides nothing when another monitor borders that edge - the part
    // meant to be off-screen simply appears on the neighbour - and this user
    // has a second display hard against the left edge of the primary. Shrinking
    // to the peek strip works on every edge regardless of what is next to it.
    static RECT WidgetRestRect(bool retracted, int fullHeight)
    {
        const RECT work = g_widgetWork;
        const int height = (std::max)(1, fullHeight);
        POINT origin = AnchorOrigin(g_widgetAnchor, work, kWidgetWidth, height);

        RECT out{ origin.x, origin.y, origin.x + kWidgetWidth, origin.y + height };
        if (!retracted) {
            return out;
        }

        const int strip = (std::min)(height, WidgetSliverHeight());

        if (AppSettings::WidgetAnchorRow(g_widgetAnchor) == 0) {
            out.bottom = out.top + strip;
        }
        else {
            out.top = out.bottom - strip;
        }

        return out;
    }

    // Track the content height the renderer measured, so turning cards off in
    // Settings actually shortens the panel instead of leaving dead space.
    static void UpdateWidgetContentHeight()
    {
        const int measured = Renderer::WidgetDesiredHeight();
        if (measured <= 0) {
            return;
        }

        const int maxHeight = (std::max)(kWidgetMinHeight,
            static_cast<int>(g_widgetWork.bottom - g_widgetWork.top) - kWidgetMargin * 2);
        const int wanted = std::clamp(measured, kWidgetMinHeight, maxHeight);

        // Deadband: sub-pixel text metrics jitter by a pixel or two between
        // frames, and resizing every frame would flicker the swapchain.
        if (std::abs(wanted - g_widgetFullHeight) > kWidgetResizeDeadband) {
            g_widgetFullHeight = wanted;
        }
    }

    // Move the panel to the current monitor/anchor now, rather than waiting for
    // the user to drag it. Called when the Settings pickers change and when the
    // display layout does.
    static void RepositionWidget()
    {
        if (!g_widgetMode || !g_hwnd) {
            return;
        }

        g_widgetWork = WidgetMonitorWork();
        g_widgetRetracted = false;
        g_widgetSettled = false;
        g_widgetLastHoverSeconds = MonotonicSeconds();

        const RECT rest = WidgetRestRect(false, g_widgetFullHeight);
        SetWindowPos(g_hwnd, HWND_TOPMOST, rest.left, rest.top,
            rest.right - rest.left, rest.bottom - rest.top, SWP_NOACTIVATE);
    }

    static void PollWidgetWindow()
    {
        if (g_widgetMode != g_widgetApplied) {
            g_widgetApplied = g_widgetMode;

            if (g_widgetMode) {
                EnterWidgetMode();
            }
            else {
                LeaveWidgetMode();
            }
            return;
        }

        if (!g_widgetMode || IsIconic(g_hwnd)) {
            return;
        }

        RECT rect{};
        if (!GetWindowRect(g_hwnd, &rect)) {
            return;
        }

        g_widgetWork = WidgetMonitorWork();

        POINT cursor{};
        const bool hovered = GetCursorPos(&cursor) && PtInRect(&rect, cursor);
        const double now = MonotonicSeconds();

        if (hovered || g_widgetPinned) {
            g_widgetLastHoverSeconds = now;
            g_widgetRetracted = false;
        }
        else if (!g_widgetRetracted &&
            now - g_widgetLastHoverSeconds >= kWidgetRetractAfterSeconds) {
            g_widgetRetracted = true;
        }

        // Dragging the panel by its header moves the window out from under us.
        // Re-snap to whichever spot - and monitor - it was dropped nearest,
        // instead of yanking it back to the old one. Only trusted once the
        // slide has settled, otherwise mid-animation frames look like drags.
        if (!g_widgetRetracted && g_widgetSettled) {
            const RECT settledRest = WidgetRestRect(false, g_widgetFullHeight);
            if (rect.left != settledRest.left || rect.top != settledRest.top) {
                const int monitor = MonitorIndexForRect(rect);
                const RECT dropWork = g_monitors[static_cast<size_t>(monitor)].work;
                const int dropped = AnchorForRect(rect, dropWork);

                if (dropped != g_widgetAnchor || monitor != g_widgetMonitor) {
                    g_widgetAnchor = dropped;
                    g_widgetMonitor = monitor;
                    SaveAppSettings();
                }
                g_widgetWork = dropWork;
            }
        }

        if (!g_widgetRetracted) {
            UpdateWidgetContentHeight();
        }

        const RECT target = WidgetRestRect(g_widgetRetracted, g_widgetFullHeight);

        if (rect.left == target.left && rect.top == target.top &&
            rect.right == target.right && rect.bottom == target.bottom) {
            g_widgetLastAnimationSeconds = now;
            g_widgetSettled = true;
            return;
        }

        g_widgetSettled = false;

        // Time-scaled so the roll runs at a real speed rather than a fraction
        // of the remaining distance - a proportional step is fast at the start
        // and crawls at the end, which is the opposite of what reads well here.
        const double elapsed = g_widgetLastAnimationSeconds > 0.0
            ? std::min(0.1, now - g_widgetLastAnimationSeconds)
            : 1.0 / 60.0;
        g_widgetLastAnimationSeconds = now;

        const double speed = g_widgetRetracted
            ? kWidgetRetractPixelsPerSecond
            : kWidgetExpandPixelsPerSecond;

        const int travel = (std::max)(1, static_cast<int>(speed * elapsed + 0.5));

        const auto step = [travel](LONG from, LONG to) {
            const LONG delta = to - from;
            if (std::abs(delta) <= travel) return to;
            return from + (delta > 0 ? travel : -travel);
        };

        const LONG left = step(rect.left, target.left);
        const LONG top = step(rect.top, target.top);
        const LONG right = step(rect.right, target.right);
        const LONG bottom = step(rect.bottom, target.bottom);

        SetWindowPos(
            g_hwnd,
            HWND_TOPMOST,
            static_cast<int>(left),
            static_cast<int>(top),
            static_cast<int>((std::max)(1L, right - left)),
            static_cast<int>((std::max)(1L, bottom - top)),
            SWP_NOACTIVATE
        );
    }

    static void DestroySettingsWindow()
    {
        if (!g_settingsHwnd) {
            return;
        }

        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g_settingsImGuiContext);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_settingsImGuiContext);
        g_settingsImGuiContext = nullptr;
        ImGui::SetCurrentContext(previous == nullptr ? g_mainImGuiContext : g_mainImGuiContext);

        if (g_settingsRenderTarget) {
            g_settingsRenderTarget->Release();
            g_settingsRenderTarget = nullptr;
        }
        if (g_settingsSwapChain) {
            g_settingsSwapChain->Release();
            g_settingsSwapChain = nullptr;
        }

        DestroyWindow(g_settingsHwnd);
        g_settingsHwnd = nullptr;
    }

    static bool CreateSettingsWindow(HINSTANCE instance)
    {
        if (g_settingsHwnd) {
            SetForegroundWindow(g_settingsHwnd);
            return true;
        }

        const RECT work = WorkAreaForWindow(g_hwnd);
        const int width = 900;
        const int height = 660;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;

        g_settingsHwnd = CreateWindowExW(
            0, L"AIQuotaCheckerDX11", L"AI Quota Checker - Settings",
            WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);

        if (!g_settingsHwnd) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = g_settingsHwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory* factory = nullptr;
        bool ok = false;

        if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetParent(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            ok = SUCCEEDED(factory->CreateSwapChain(g_device, &sd, &g_settingsSwapChain));
        }

        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDevice) dxgiDevice->Release();

        if (!ok) {
            DestroyWindow(g_settingsHwnd);
            g_settingsHwnd = nullptr;
            return false;
        }

        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(g_settingsSwapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
            g_device->CreateRenderTargetView(back, nullptr, &g_settingsRenderTarget);
            back->Release();
        }

        // Its own context: a separate font atlas and input state from the widget.
        ImGuiContext* previous = ImGui::GetCurrentContext();
        g_settingsImGuiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_settingsImGuiContext);
        ImGui::GetIO().IniFilename = nullptr;
        Renderer::ApplyStyleColorsOnly();
        Renderer::LoadSettingsFont(g_uiFontSize);
        ImGui_ImplWin32_Init(g_settingsHwnd);
        ImGui_ImplDX11_Init(g_device, g_context);
        // ShowWindow/UpdateWindow/SetForegroundWindow dispatch messages
        // synchronously, and WndProc switches the current context to whichever
        // window they target. Restore AFTER them, not before.
        ShowWindow(g_settingsHwnd, SW_SHOW);
        UpdateWindow(g_settingsHwnd);
        SetForegroundWindow(g_settingsHwnd);

        ImGui::SetCurrentContext(previous);
        return true;
    }

    static void RenderSettingsWindow()
    {
        if (!g_settingsHwnd || !g_settingsImGuiContext || !g_settingsRenderTarget) {
            return;
        }

        ImGui::SetCurrentContext(g_settingsImGuiContext);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Renderer::RenderSettingsUi(g_rendererState);

        ImGui::Render();

        const float clear[4] = { 0.06f, 0.065f, 0.08f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_settingsRenderTarget, nullptr);
        g_context->ClearRenderTargetView(g_settingsRenderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_settingsSwapChain->Present(1, 0);

        ImGui::SetCurrentContext(g_mainImGuiContext);
    }

    static void PollMinimizeRequest()
    {
        if (!g_minimizeRequest) {
            return;
        }

        g_minimizeRequest = false;
        ShowWindow(g_hwnd, SW_MINIMIZE);
    }

    static void PollAutoRefresh()
    {
        static LONGLONG nextCodexRefresh = 0;
        static LONGLONG nextClaudeRefresh = 0;
        static LONGLONG nextZAiRefresh = 0;
        static LONGLONG nextGrokRefresh = 0;

        if (!g_autoRefreshEnabled) {
            nextCodexRefresh = 0;
            nextClaudeRefresh = 0;
            nextZAiRefresh = 0;
            nextGrokRefresh = 0;
            return;
        }

        if (!g_codexAutoRefreshEnabled) nextCodexRefresh = 0;
        if (!g_claudeAutoRefreshEnabled) nextClaudeRefresh = 0;
        if (!g_zaiAutoRefreshEnabled) nextZAiRefresh = 0;
        if (!g_grokAutoRefreshEnabled) nextGrokRefresh = 0;

        LONGLONG now = KSharedClock::SystemUnixSeconds();

        if (now <= 0) {
            return;
        }

        LONGLONG intervalSeconds = static_cast<LONGLONG>(AutoRefreshIntervalSeconds());

        if (g_codexAutoRefreshEnabled && (nextCodexRefresh == 0 || nextCodexRefresh - now > intervalSeconds)) {
            nextCodexRefresh = now + intervalSeconds;
        }

        if (g_claudeAutoRefreshEnabled && (nextClaudeRefresh == 0 || nextClaudeRefresh - now > intervalSeconds)) {
            nextClaudeRefresh = now + intervalSeconds;
        }

        if (g_zaiAutoRefreshEnabled && (nextZAiRefresh == 0 || nextZAiRefresh - now > intervalSeconds)) {
            nextZAiRefresh = now + intervalSeconds;
        }

        if (g_grokAutoRefreshEnabled && (nextGrokRefresh == 0 || nextGrokRefresh - now > intervalSeconds)) {
            nextGrokRefresh = now + intervalSeconds;
        }

        CodexProvider* codex = CodexProvider::get_instance();
        ClaudeProvider* claude = ClaudeProvider::get_instance();
        ZAiProvider* zai = ZAiProvider::get_instance();
        GrokProvider* grok = GrokProvider::get_instance();

        if (g_codexAutoRefreshEnabled && nextCodexRefresh > 0 && now >= nextCodexRefresh) {
            if (!codex->Loading()->load()) {
                codex->RefreshAsync();
                nextCodexRefresh = now + intervalSeconds;
            }
            else {
                nextCodexRefresh = now + 5;
            }
        }

        if (g_claudeAutoRefreshEnabled && nextClaudeRefresh > 0 && now >= nextClaudeRefresh) {
            if (!claude->Loading()->load()) {
                claude->RefreshAsync();
                nextClaudeRefresh = now + intervalSeconds;
            }
            else {
                nextClaudeRefresh = now + 5;
            }
        }

        if (g_zaiAutoRefreshEnabled && nextZAiRefresh > 0 && now >= nextZAiRefresh) {
            if (!zai->Loading()->load()) {
                zai->RefreshAsync();
                nextZAiRefresh = now + intervalSeconds;
            }
            else {
                nextZAiRefresh = now + 5;
            }
        }

        if (g_grokAutoRefreshEnabled && nextGrokRefresh > 0 && now >= nextGrokRefresh) {
            if (!grok->Loading()->load()) {
                grok->RefreshAsync();
                nextGrokRefresh = now + intervalSeconds;
            }
            else {
                nextGrokRefresh = now + 5;
            }
        }
    }

    static void PollLocalContextTelemetry()
    {
        static LONGLONG nextContextRefresh = 0;
        const LONGLONG now = KSharedClock::SystemUnixSeconds();

        if (now <= 0 || (nextContextRefresh > 0 && now < nextContextRefresh)) {
            return;
        }

        // Local telemetry is passive file I/O only (no provider request).
        // Poll once per second so Claude's first persisted output_tokens value
        // reaches the UI with as little additional AQC-side lag as possible.
        nextContextRefresh = now + 1;
        CodexProvider::get_instance()->RefreshContextAsync();
        ClaudeProvider::get_instance()->RefreshContextAsync();
        ZAiProvider::get_instance()->RefreshContextAsync();
    }

    static void PollAppNotifications()
    {
        ApplyPendingAutoRefreshDisable();
        PollMinimizeRequest();
        PollWidgetWindow();
        PollAutoRefresh();
        PollLocalContextTelemetry();

        CodexProvider::get_instance()->PollNotifications();
        ClaudeProvider::get_instance()->PollNotifications();
        ZAiProvider::get_instance()->PollNotifications();
        GrokProvider::get_instance()->PollNotifications();
    }

    static void InitializeCoreServices()
    {
        Math::get_instance();
        Text::get_instance();
        Format::get_instance();
        Network::get_instance();
        JsonUtils::get_instance();
        ResetTime::get_instance();
    }

    static void InitializeProviders()
    {
        CodexProvider::get_instance()->SetRateLimitCallback(RequestAutoRefreshDisableForRateLimit);
        ClaudeProvider::get_instance()->SetRateLimitCallback(RequestAutoRefreshDisableForRateLimit);
        ZAiProvider::get_instance()->SetRateLimitCallback(RequestAutoRefreshDisableForRateLimit);
        GrokProvider::get_instance()->SetRateLimitCallback(RequestAutoRefreshDisableForRateLimit);
    }

    static void InitializeRendererState()
    {
        CodexProvider* codex = CodexProvider::get_instance();
        ClaudeProvider* claude = ClaudeProvider::get_instance();
        ZAiProvider* zai = ZAiProvider::get_instance();
        GrokProvider* grok = GrokProvider::get_instance();

        // Enumerate before handing the label pointers over: the renderer keeps
        // the vector's data() pointer, so it has to be populated first.
        RefreshMonitors();

        g_rendererState.shouldClose = &g_shouldClose;
        g_rendererState.minimizeRequest = &g_minimizeRequest;
        g_rendererState.widgetMode = &g_widgetMode;
        g_rendererState.widgetPinned = &g_widgetPinned;
        g_rendererState.widgetAnchor = &g_widgetAnchor;
        g_rendererState.widgetMonitor = &g_widgetMonitor;
        g_rendererState.widgetMonitorNames = g_monitorLabelPtrs.data();
        g_rendererState.widgetMonitorCount = static_cast<int>(g_monitorLabelPtrs.size());
        g_rendererState.repositionWidget = RepositionWidget;
        g_rendererState.widgetOrder = &g_widgetOrder;
        g_rendererState.uiFontSize = &g_uiFontSize;
        g_rendererState.widgetSections = &g_widgetSections;
        g_rendererState.settingsWindowOpen = &g_settingsWindowOpen;
        g_rendererState.device = g_device;

        g_rendererState.codexMutex = codex->StateMutex();
        g_rendererState.claudeMutex = claude->StateMutex();
        g_rendererState.zaiMutex = zai->StateMutex();
        g_rendererState.grokMutex = grok->StateMutex();
        g_rendererState.codexState = codex->Snapshot();
        g_rendererState.claudeState = claude->Snapshot();
        g_rendererState.zaiState = zai->Snapshot();
        g_rendererState.grokState = grok->Snapshot();

        g_rendererState.codexLoading = codex->Loading();
        g_rendererState.claudeLoading = claude->Loading();
        g_rendererState.zaiLoading = zai->Loading();
        g_rendererState.grokLoading = grok->Loading();

        g_rendererState.showRemaining = &g_showRemaining;
        g_rendererState.showResetDateDetails = &g_showResetDateDetails;
        g_rendererState.resetDisplayMode = &g_resetDisplayMode;
        g_rendererState.showNotificationsInsideWindow = &g_showNotificationsInsideWindow;
        g_rendererState.autoRefreshEnabled = &g_autoRefreshEnabled;
        g_rendererState.autoRefreshMinutes = &g_autoRefreshMinutes;
        g_rendererState.codexAutoRefreshEnabled = &g_codexAutoRefreshEnabled;
        g_rendererState.claudeAutoRefreshEnabled = &g_claudeAutoRefreshEnabled;
        g_rendererState.zaiAutoRefreshEnabled = &g_zaiAutoRefreshEnabled;
        g_rendererState.grokAutoRefreshEnabled = &g_grokAutoRefreshEnabled;
        g_rendererState.claudeAccountSource = &g_claudeAccountSource;
        g_rendererState.claudeThinkingShimmerSpeedPercent = &g_claudeThinkingShimmerSpeedPercent;
        g_rendererState.codexAccountSource = &g_codexAccountSource;
        g_rendererState.codexCustomAuthPath = &g_codexCustomAuthPath;
        g_rendererState.autoRefreshWarning = &g_autoRefreshWarning;
        g_rendererState.autoRefreshWarningMutex = &g_autoRefreshWarningMutex;

        g_rendererState.notifyPositionIndex = &g_notifyPositionIndex;
        g_rendererState.notifyPosition = &g_notifyPosition;
        g_rendererState.notifyPositionNames = g_notifyPositionNames;
        g_rendererState.notifyPositionCount = static_cast<int>(sizeof(g_notifyPositionNames) / sizeof(g_notifyPositionNames[0]));
        g_rendererState.notifyPositions = g_notifyPositions;

        g_rendererState.codexNotifySettings = codex->NotifySettings();
        g_rendererState.claudeNotifySettings = claude->NotifySettings();
        g_rendererState.zaiNotifySettings = zai->NotifySettings();
        g_rendererState.grokNotifySettings = grok->NotifySettings();
        g_rendererState.codexQuotaWarnings = codex->QuotaWarnings();
        g_rendererState.claudeQuotaWarnings = claude->QuotaWarnings();
        g_rendererState.zaiQuotaWarnings = zai->QuotaWarnings();
        g_rendererState.grokQuotaWarnings = grok->QuotaWarnings();

        g_rendererState.refreshCodexAsync = RefreshCodexAsync;
        g_rendererState.refreshClaudeAsync = RefreshClaudeAsync;
        g_rendererState.refreshZAiAsync = RefreshZAiAsync;
        g_rendererState.refreshGrokAsync = RefreshGrokAsync;
        g_rendererState.saveAppSettings = SaveAppSettings;
        g_rendererState.applySettingsToRuntime = ApplySettingsToRuntime;
    }
}

namespace
{
    // `AIQuotaChecker.exe --statusline` used as Claude Code's statusLine
    // command. Claude Code pipes its status JSON on stdin - the only place it
    // publishes the exact context window and the five-hour / seven-day rate
    // limits - so park it on disk for the running AQC to read. Nothing here
    // touches Claude's settings; wiring the command up stays the user's call.
    static std::filesystem::path StatusLineCapturePath()
    {
        wchar_t buffer[MAX_PATH]{};
        DWORD length = GetEnvironmentVariableW(L"AQC_CLAUDE_STATUSLINE_FILE", buffer, MAX_PATH);

        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(buffer);
        }

        length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);

        if (length == 0 || length >= MAX_PATH) {
            return {};
        }

        return std::filesystem::path(buffer) / "AIQuotaChecker" / "claude-statusline.json";
    }

    static int RunStatusLineCapture()
    {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

        if (input == INVALID_HANDLE_VALUE || input == nullptr) {
            return 0;
        }

        std::string payload;
        char chunk[4096];
        DWORD read = 0;

        while (ReadFile(input, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
            payload.append(chunk, read);

            if (payload.size() > 4u * 1024u * 1024u) {
                break;
            }
        }

        if (payload.empty()) {
            return 0;
        }

        const std::filesystem::path path = StatusLineCapturePath();

        if (path.empty()) {
            return 0;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        // Write beside the target and rename, so a reader never sees a
        // half-written document.
        const std::filesystem::path temporary = path.string() + ".tmp";

        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

            if (!file) {
                return 0;
            }

            file << payload;
        }

        std::filesystem::rename(temporary, path, ec);

        if (ec) {
            std::filesystem::copy_file(
                temporary,
                path,
                std::filesystem::copy_options::overwrite_existing,
                ec
            );
            std::filesystem::remove(temporary, ec);
        }

        // Claude Code renders whatever this prints as the status line, so emit
        // something useful rather than leaving it blank. Naive extraction keeps
        // this path dependency-free.
        auto findString = [&](const char* key) -> std::string {
            const std::string needle = std::string("\"") + key + "\":\"";
            const size_t at = payload.find(needle);
            if (at == std::string::npos) return {};
            const size_t begin = at + needle.size();
            const size_t end = payload.find('"', begin);
            return end == std::string::npos ? std::string{} : payload.substr(begin, end - begin);
        };

        auto findNumber = [&](const char* key) -> std::string {
            const std::string needle = std::string("\"") + key + "\":";
            const size_t at = payload.find(needle);
            if (at == std::string::npos) return {};
            size_t begin = at + needle.size();
            while (begin < payload.size() && payload[begin] == ' ') ++begin;
            size_t end = begin;
            while (end < payload.size() &&
                (std::isdigit(static_cast<unsigned char>(payload[end])) || payload[end] == '.')) {
                ++end;
            }
            return end > begin ? payload.substr(begin, end - begin) : std::string{};
        };

        std::string line = findString("display_name");
        const std::string used = findNumber("used_percentage");

        if (!used.empty()) {
            const size_t dot = used.find('.');
            const std::string whole = dot == std::string::npos ? used : used.substr(0, dot);
            line += line.empty() ? "" : "  |  ";
            line += "ctx " + whole + "%";
        }

        if (!line.empty()) {
            const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            if (out && out != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            }
        }

        return 0;
    }

    static bool WantsStatusLineCapture()
    {
        int count = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);

        if (!argv) {
            return false;
        }

        bool wants = false;

        for (int i = 1; i < count; ++i) {
            if (_wcsicmp(argv[i], L"--statusline") == 0) {
                wants = true;
                break;
            }
        }

        LocalFree(argv);
        return wants;
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    if (WantsStatusLineCapture()) {
        return RunStatusLineCapture();
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AIQuotaCheckerDX11";

    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AIQUOTACHECKER));

    if (!appIcon) {
        appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;

    RegisterClassExW(&wc);

    #if defined(GUI_ONLY)
    const int startWidth = 1040;
    const int startHeight = 760;
#else
    const int startWidth = kWidgetWidth;
    const int startHeight = kWidgetHeight;
#endif

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"AI Quota Checker", WS_POPUP | WS_MINIMIZEBOX, 100, 100, startWidth, startHeight, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    g_mainImGuiContext = ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    InitializeCoreServices();
    InitializeProviders();
    InitializeRendererState();
    Renderer::ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    LoadAppSettings();

    // ApplyStyle built the atlas at the default size; rebuild if the saved
    // interface font size differs.
    {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        Renderer::ReloadFonts();
        ImGui_ImplDX11_CreateDeviceObjects();
    }

    if (g_autoRefreshEnabled && g_codexAutoRefreshEnabled) CodexProvider::get_instance()->RefreshAsync();
    if (g_autoRefreshEnabled && g_claudeAutoRefreshEnabled) ClaudeProvider::get_instance()->RefreshAsync();
    if (g_autoRefreshEnabled && g_zaiAutoRefreshEnabled) ZAiProvider::get_instance()->RefreshAsync();
    if (g_autoRefreshEnabled && g_grokAutoRefreshEnabled) GrokProvider::get_instance()->RefreshAsync();

    bool done = false;

    while (!done) {
        MSG msg{};

        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);

            if (msg.message == WM_QUIT) {
                done = true;
            }
        }

        if (g_shouldClose) {
            done = true;
        }

        if (done) {
            break;
        }

        if (g_settingsWindowOpen && !g_settingsHwnd) {
            if (!CreateSettingsWindow(hInstance)) {
                g_settingsWindowOpen = false;
            }
        }
        else if (!g_settingsWindowOpen && g_settingsHwnd) {
            DestroySettingsWindow();
        }

        if (Renderer::ConsumeFontReloadRequest()) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            Renderer::ReloadFonts();
            ImGui_ImplDX11_CreateDeviceObjects();
        }

        // Anything above can dispatch window messages (window creation and
        // destruction both do), and WndProc leaves the current context set to
        // whichever window received them. Select the widget's context here,
        // immediately before its frame, so that can never leak in.
        ImGui::SetCurrentContext(g_mainImGuiContext);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        Renderer::RenderMainUi(g_rendererState);
        PollAppNotifications();
        NotifyGUI::Render();
        ImGui::Render();

        const float clearColor[4] = {
            0.075f,
            0.075f,
            0.075f,
            1.0f
        };

        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapChain->Present(1, 0);

        RenderSettingsWindow();
    }

    DestroySettingsWindow();
    Renderer::ReleaseTabImages();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();

    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();

    return 0;
}
