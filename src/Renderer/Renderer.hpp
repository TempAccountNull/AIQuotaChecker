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
    };

    void ApplyStyle();
    void ReleaseTabImages();
    void RenderMainUi(State& state);
}
