#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "Codex.hpp"
#include "Claude.hpp"
#include "ZAi.hpp"
#include "NotifyGUI.hpp"
#include "AppSettings.hpp"

namespace Renderer
{
    struct State
    {
        bool* shouldClose = nullptr;

        std::mutex* codexMutex = nullptr;
        std::mutex* claudeMutex = nullptr;
        std::mutex* zaiMutex = nullptr;
        Codex::Snapshot* codexState = nullptr;
        Claude::Snapshot* claudeState = nullptr;
        ZAi::Snapshot* zaiState = nullptr;

        std::atomic_bool* codexLoading = nullptr;
        std::atomic_bool* claudeLoading = nullptr;
        std::atomic_bool* zaiLoading = nullptr;

        bool* showRemaining = nullptr;
        bool* showResetDateDetails = nullptr;
        int* resetDisplayMode = nullptr;
        bool* showNotificationsInsideWindow = nullptr;
        bool* autoRefreshEnabled = nullptr;
        int* autoRefreshMinutes = nullptr;

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

        AppSettings::CodexQuotaWarnings* codexQuotaWarnings = nullptr;
        AppSettings::ClaudeQuotaWarnings* claudeQuotaWarnings = nullptr;
        AppSettings::ZAiQuotaWarnings* zaiQuotaWarnings = nullptr;

        void (*refreshCodexAsync)() = nullptr;
        void (*refreshClaudeAsync)() = nullptr;
        void (*refreshZAiAsync)() = nullptr;
        bool (*saveAppSettings)() = nullptr;
        void (*applySettingsToRuntime)() = nullptr;
    };

    void ApplyStyle();
    void RenderMainUi(State& state);
}
