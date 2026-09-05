#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "Codex.hpp"
#include "Claude.hpp"
#include "ZAi.hpp"
#include "Grok.hpp"
#include "NotifyGUI.hpp"
#include "AppSettings.hpp"

struct ID3D11Device;

namespace Renderer
{
    struct State
    {
        bool* shouldClose = nullptr;
        bool* minimizeRequest = nullptr;
        bool* widgetMode = nullptr;
        bool* widgetPinned = nullptr;
        bool* widgetAlwaysOnTop = nullptr;
        // AppSettings::WidgetAnchor: which edge the panel parks against.
        int* widgetAnchor = nullptr;
        // Which monitor it parks on, plus the picker's labels.
        int* widgetMonitor = nullptr;
        const char* const* widgetMonitorNames = nullptr;
        int widgetMonitorCount = 0;
        std::string* widgetOrder = nullptr;
        // Ordered "<provider>:<window>" keys shown in the collapsed bar.
        std::string* widgetBarRows = nullptr;
        int* uiFontSize = nullptr;
        int* widgetSections = nullptr;
        // Raised by the widget footer; the app opens a separate settings window.
        bool* settingsWindowOpen = nullptr;
        ID3D11Device* device = nullptr;

        std::mutex* codexMutex = nullptr;
        std::mutex* claudeMutex = nullptr;
        std::mutex* zaiMutex = nullptr;
        std::mutex* grokMutex = nullptr;
        Codex::Snapshot* codexState = nullptr;
        Claude::Snapshot* claudeState = nullptr;
        ZAi::Snapshot* zaiState = nullptr;
        Grok::Snapshot* grokState = nullptr;

        std::atomic_bool* codexLoading = nullptr;
        std::atomic_bool* claudeLoading = nullptr;
        std::atomic_bool* zaiLoading = nullptr;
        std::atomic_bool* grokLoading = nullptr;

        bool* showRemaining = nullptr;
        bool* showResetDateDetails = nullptr;
        int* resetDisplayMode = nullptr;
        bool* showNotificationsInsideWindow = nullptr;
        bool* autoRefreshEnabled = nullptr;
        int* autoRefreshMinutes = nullptr;
        bool* codexAutoRefreshEnabled = nullptr;
        bool* claudeAutoRefreshEnabled = nullptr;
        bool* zaiAutoRefreshEnabled = nullptr;
        bool* grokAutoRefreshEnabled = nullptr;
        int* claudeAccountSource = nullptr;
        int* claudeThinkingShimmerSpeedPercent = nullptr;
        int* codexAccountSource = nullptr;
        std::string* codexCustomAuthPath = nullptr;

        std::string* autoRefreshWarning = nullptr;
        std::mutex* autoRefreshWarningMutex = nullptr;

        int* notifyPositionIndex = nullptr;
        NotifyPosition* notifyPosition = nullptr;
        const char* const* notifyPositionNames = nullptr;
        int notifyPositionCount = 0;
        NotifyPosition* notifyPositions = nullptr;

        AppSettings::ProviderNotifications* codexNotifySettings = nullptr;
        AppSettings::ProviderNotifications* claudeNotifySettings = nullptr;
        AppSettings::ProviderNotifications* zaiNotifySettings = nullptr;
        AppSettings::ProviderNotifications* grokNotifySettings = nullptr;

        AppSettings::CodexQuotaWarnings* codexQuotaWarnings = nullptr;
        AppSettings::ClaudeQuotaWarnings* claudeQuotaWarnings = nullptr;
        AppSettings::ZAiQuotaWarnings* zaiQuotaWarnings = nullptr;
        AppSettings::GrokQuotaWarnings* grokQuotaWarnings = nullptr;

        void (*refreshCodexAsync)() = nullptr;
        void (*refreshClaudeAsync)() = nullptr;
        void (*refreshZAiAsync)() = nullptr;
        void (*refreshGrokAsync)() = nullptr;
        bool (*saveAppSettings)() = nullptr;
        void (*applySettingsToRuntime)() = nullptr;
        // Re-park the widget after a monitor/anchor change.
        void (*repositionWidget)() = nullptr;
    };

    void ApplyStyle();
    // Style only (no font atlas work) - for a second ImGui context whose
    // atlas is separate from the widget's.
    void ApplyStyleColorsOnly();
    // Full-window settings UI, rendered into its own window/context.
    void RenderSettingsUi(State& state);
    // Rebuild the interface font atlas at the configured size. The caller
    // must invalidate/recreate backend device objects around this.
    void ReloadFonts();
    // Install the interface font as the current context's default. For the
    // settings window, which has its own atlas.
    void LoadSettingsFont(int size);
    bool ConsumeFontReloadRequest();
    void ReleaseTabImages();
    void RenderMainUi(State& state);
    // Height in pixels the widget's content wants, measured during the last
    // DrawWidgetUi. Zero until the widget has drawn at least one frame.
    int WidgetDesiredHeight();
    // Width the widest bar row needs to render untrimmed. Zero until measured.
    int WidgetDesiredWidth();
    // Height of the bottom summary strip. The widget retracts to exactly this,
    // so the selected host's quota stays readable while rolled up.
    int WidgetPeekHeight();
}
