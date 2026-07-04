#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "AppSettings.hpp"
#include "Codex.hpp"
#include "NotifyGUI.hpp"

class CodexProvider
{
public:
    using RateLimitCallback = void (*)(const char* provider, const std::string& detail);

    static CodexProvider* get_instance();

    std::mutex* StateMutex();
    Codex::Snapshot* Snapshot();
    std::atomic_bool* Loading();

    AppSettings::ProviderNotifications* NotifySettings();
    AppSettings::CodexQuotaWarnings* QuotaWarnings();

    void SetRateLimitCallback(RateLimitCallback callback);

    void LoadSettings(const AppSettings::Settings& settings);
    void SaveSettings(AppSettings::Settings& settings) const;

    void ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds);
    void RefreshAsync();
    void PollNotifications();

private:
    CodexProvider() = default;

    static void RefreshThunk();
    void HandleRateLimit(const std::string& detail);

    std::mutex m_mutex;
    Codex::Snapshot m_snapshot;
    std::atomic_bool m_loading = false;

    AppSettings::ProviderNotifications m_notifySettings;
    AppSettings::CodexQuotaWarnings m_quotaWarnings;

    RateLimitCallback m_rateLimitCallback = nullptr;
};
