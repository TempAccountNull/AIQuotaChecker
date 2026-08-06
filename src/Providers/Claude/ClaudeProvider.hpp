#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "AppSettings.hpp"
#include "Claude.hpp"
#include "NotifyGUI.hpp"

class ClaudeProvider
{
public:
    using RateLimitCallback = void (*)(const char* provider, const std::string& detail);

    static ClaudeProvider* get_instance();

    std::mutex* StateMutex();
    Claude::Snapshot* Snapshot();
    std::atomic_bool* Loading();

    AppSettings::ProviderNotifications* NotifySettings();
    AppSettings::ClaudeQuotaWarnings* QuotaWarnings();

    void SetAccountSource(int source);
    int AccountSource() const;

    void SetRateLimitCallback(RateLimitCallback callback);

    void LoadSettings(const AppSettings::Settings& settings);
    void SaveSettings(AppSettings::Settings& settings) const;

    void ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds);
    void RefreshAsync();
    void PollNotifications();

private:
    ClaudeProvider() = default;

    static void RefreshThunk();
    void HandleRateLimit(const std::string& detail);

    std::mutex m_mutex;
    Claude::Snapshot m_snapshot;
    std::string m_lastSuccessfulAccountKey;
    std::atomic_bool m_loading = false;

    AppSettings::ProviderNotifications m_notifySettings;
    AppSettings::ClaudeQuotaWarnings m_quotaWarnings;

    std::atomic_int m_accountSource = 0;
    std::atomic<std::uint64_t> m_sourceGeneration = 0;
    mutable std::mutex m_sourceMutex;

    RateLimitCallback m_rateLimitCallback = nullptr;
};
