#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "AppSettings.hpp"
#include "NotifyGUI.hpp"
#include "ZAi.hpp"

class ZAiProvider
{
public:
    using RateLimitCallback = void (*)(const char* provider, const std::string& detail);

    static ZAiProvider* get_instance();

    std::mutex* StateMutex();
    ZAi::Snapshot* Snapshot();
    std::atomic_bool* Loading();

    AppSettings::ProviderNotifications* NotifySettings();
    AppSettings::ZAiQuotaWarnings* QuotaWarnings();

    void SetRateLimitCallback(RateLimitCallback callback);

    void LoadSettings(const AppSettings::Settings& settings);
    void SaveSettings(AppSettings::Settings& settings) const;

    void ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds);
    void RefreshAsync();
    void PollNotifications();

private:
    ZAiProvider() = default;

    void HandleRateLimit(const std::string& detail);

    std::mutex m_mutex;
    ZAi::Snapshot m_snapshot;
    std::atomic_bool m_loading = false;

    AppSettings::ProviderNotifications m_notifySettings;
    AppSettings::ZAiQuotaWarnings m_quotaWarnings;

    RateLimitCallback m_rateLimitCallback = nullptr;
};
