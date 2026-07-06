#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "AppSettings.hpp"
#include "Grok.hpp"
#include "NotifyGUI.hpp"

class GrokProvider
{
public:
    using RateLimitCallback = void (*)(const char* provider, const std::string& detail);

    static GrokProvider* get_instance();

    std::mutex* StateMutex();
    Grok::Snapshot* Snapshot();
    std::atomic_bool* Loading();

    AppSettings::ProviderNotifications* NotifySettings();
    AppSettings::GrokQuotaWarnings* QuotaWarnings();

    void SetRateLimitCallback(RateLimitCallback callback);

    void LoadSettings(const AppSettings::Settings& settings);
    void SaveSettings(AppSettings::Settings& settings) const;

    void ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds);
    void RefreshAsync();
    void PollNotifications();

private:
    GrokProvider() = default;

    static void RefreshThunk();
    void HandleRateLimit(const std::string& detail);

    std::mutex m_mutex;
    Grok::Snapshot m_snapshot;
    std::atomic_bool m_loading = false;

    AppSettings::ProviderNotifications m_notifySettings;
    AppSettings::GrokQuotaWarnings m_quotaWarnings;

    RateLimitCallback m_rateLimitCallback = nullptr;
};
