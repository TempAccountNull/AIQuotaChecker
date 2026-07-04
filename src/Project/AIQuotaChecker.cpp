#include "Global.hpp"

#include <windows.h>
#include <d3d11.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
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

    static constexpr int kTitleBarHeight = 38;
    static constexpr int kResizeBorder = 6;

    static bool g_showRemaining = false;
    static bool g_showResetDateDetails = false;
    static int g_resetDisplayMode = ResetTime::Static;
    static bool g_showNotificationsInsideWindow = false;
    static bool g_autoRefreshEnabled = true;
    static int g_autoRefreshMinutes = 1;

    static std::atomic_bool g_autoRefreshDisableRequested = false;
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

    static void RequestAutoRefreshDisableForRateLimit(const char* provider, const std::string& detail)
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

        g_autoRefreshWarning += ". Increase the refresh interval before enabling it again.";
        g_autoRefreshDisableRequested = true;
    }

    static std::string GetAutoRefreshWarning()
    {
        std::lock_guard<std::mutex> lock(g_autoRefreshWarningMutex);
        return g_autoRefreshWarning;
    }

    static void ApplyPendingAutoRefreshDisable()
    {
        if (!g_autoRefreshDisableRequested.exchange(false)) {
            return;
        }

        g_autoRefreshEnabled = false;

        std::string warning = GetAutoRefreshWarning();

        if (warning.empty()) {
            warning = "Warning: Auto refresh disabled after a provider rate limit response.";
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
        CodexProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
        ClaudeProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
        ZAiProvider::get_instance()->ApplyRuntime(g_notifyPosition, repeatSeconds);
    }

    static void LoadAppSettings()
    {
        AppSettings::Settings settings;
        AppSettings::Load(settings);

        g_showRemaining = settings.showRemaining;
        g_showResetDateDetails = settings.showResetDateDetails;
        g_resetDisplayMode = AppSettings::ClampResetDisplayMode(settings.resetDisplayMode);
        g_showNotificationsInsideWindow = settings.notificationsInsideWindow;
        g_notifyPositionIndex = settings.notificationPositionIndex;
        g_autoRefreshEnabled = settings.autoRefreshEnabled;
        g_autoRefreshMinutes = AppSettings::ClampAutoRefreshMinutes(settings.autoRefreshMinutes);

        CodexProvider::get_instance()->LoadSettings(settings);
        ClaudeProvider::get_instance()->LoadSettings(settings);
        ZAiProvider::get_instance()->LoadSettings(settings);

        ApplySettingsToRuntime();
    }

    static bool SaveAppSettings()
    {
        AppSettings::Settings settings;
        settings.showRemaining = g_showRemaining;
        settings.showResetDateDetails = g_showResetDateDetails;
        settings.resetDisplayMode = AppSettings::ClampResetDisplayMode(g_resetDisplayMode);
        settings.notificationsInsideWindow = g_showNotificationsInsideWindow;
        settings.notificationPositionIndex = g_notifyPositionIndex;
        settings.autoRefreshEnabled = g_autoRefreshEnabled;
        settings.autoRefreshMinutes = AppSettings::ClampAutoRefreshMinutes(g_autoRefreshMinutes);

        CodexProvider::get_instance()->SaveSettings(settings);
        ClaudeProvider::get_instance()->SaveSettings(settings);
        ZAiProvider::get_instance()->SaveSettings(settings);

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

    static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
            return true;
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

            bool inCloseButtonArea = y >= 0 && y < kTitleBarHeight && x >= w - 52 && x < w;

            if (!inCloseButtonArea && y >= 0 && y < kTitleBarHeight) {
                return HTCAPTION;
            }

            return HTCLIENT;
        }

        case WM_SIZE:
            if (g_device && wparam != SIZE_MINIMIZED) {
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

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    static void PollAutoRefresh()
    {
        static LONGLONG nextCodexRefresh = 0;
        static LONGLONG nextClaudeRefresh = 0;
        static LONGLONG nextZAiRefresh = 0;

        if (!g_autoRefreshEnabled) {
            nextCodexRefresh = 0;
            nextClaudeRefresh = 0;
            nextZAiRefresh = 0;
            return;
        }

        LONGLONG now = KSharedClock::SystemUnixSeconds();

        if (now <= 0) {
            return;
        }

        LONGLONG intervalSeconds = static_cast<LONGLONG>(AutoRefreshIntervalSeconds());

        if (nextCodexRefresh == 0 || nextCodexRefresh - now > intervalSeconds) {
            nextCodexRefresh = now + intervalSeconds;
        }

        if (nextClaudeRefresh == 0 || nextClaudeRefresh - now > intervalSeconds) {
            nextClaudeRefresh = now + intervalSeconds;
        }

        if (nextZAiRefresh == 0 || nextZAiRefresh - now > intervalSeconds) {
            nextZAiRefresh = now + intervalSeconds;
        }

        CodexProvider* codex = CodexProvider::get_instance();
        ClaudeProvider* claude = ClaudeProvider::get_instance();
        ZAiProvider* zai = ZAiProvider::get_instance();

        if (now >= nextCodexRefresh) {
            if (!codex->Loading()->load()) {
                codex->RefreshAsync();
                nextCodexRefresh = now + intervalSeconds;
            }
            else {
                nextCodexRefresh = now + 5;
            }
        }

        if (now >= nextClaudeRefresh) {
            if (!claude->Loading()->load()) {
                claude->RefreshAsync();
                nextClaudeRefresh = now + intervalSeconds;
            }
            else {
                nextClaudeRefresh = now + 5;
            }
        }

        if (now >= nextZAiRefresh) {
            if (!zai->Loading()->load()) {
                zai->RefreshAsync();
                nextZAiRefresh = now + intervalSeconds;
            }
            else {
                nextZAiRefresh = now + 5;
            }
        }
    }

    static void PollAppNotifications()
    {
        ApplyPendingAutoRefreshDisable();
        PollAutoRefresh();

        CodexProvider::get_instance()->PollNotifications();
        ClaudeProvider::get_instance()->PollNotifications();
        ZAiProvider::get_instance()->PollNotifications();
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
    }

    static void InitializeRendererState()
    {
        CodexProvider* codex = CodexProvider::get_instance();
        ClaudeProvider* claude = ClaudeProvider::get_instance();
        ZAiProvider* zai = ZAiProvider::get_instance();

        g_rendererState.shouldClose = &g_shouldClose;

        g_rendererState.codexMutex = codex->StateMutex();
        g_rendererState.claudeMutex = claude->StateMutex();
        g_rendererState.zaiMutex = zai->StateMutex();
        g_rendererState.codexState = codex->Snapshot();
        g_rendererState.claudeState = claude->Snapshot();
        g_rendererState.zaiState = zai->Snapshot();

        g_rendererState.codexLoading = codex->Loading();
        g_rendererState.claudeLoading = claude->Loading();
        g_rendererState.zaiLoading = zai->Loading();

        g_rendererState.showRemaining = &g_showRemaining;
        g_rendererState.showResetDateDetails = &g_showResetDateDetails;
        g_rendererState.resetDisplayMode = &g_resetDisplayMode;
        g_rendererState.showNotificationsInsideWindow = &g_showNotificationsInsideWindow;
        g_rendererState.autoRefreshEnabled = &g_autoRefreshEnabled;
        g_rendererState.autoRefreshMinutes = &g_autoRefreshMinutes;
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
        g_rendererState.codexQuotaWarnings = codex->QuotaWarnings();
        g_rendererState.claudeQuotaWarnings = claude->QuotaWarnings();
        g_rendererState.zaiQuotaWarnings = zai->QuotaWarnings();

        g_rendererState.refreshCodexAsync = RefreshCodexAsync;
        g_rendererState.refreshClaudeAsync = RefreshClaudeAsync;
        g_rendererState.refreshZAiAsync = RefreshZAiAsync;
        g_rendererState.saveAppSettings = SaveAppSettings;
        g_rendererState.applySettingsToRuntime = ApplySettingsToRuntime;
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
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

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"AI Quota Checker", WS_POPUP, 100, 100, 1040, 760, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    InitializeCoreServices();
    InitializeProviders();
    InitializeRendererState();
    Renderer::ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    LoadAppSettings();

    CodexProvider::get_instance()->RefreshAsync();
    ClaudeProvider::get_instance()->RefreshAsync();
    ZAiProvider::get_instance()->RefreshAsync();

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
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();

    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
