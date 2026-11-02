#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "AppSettings.hpp"
#include "Codex.hpp"
#include "NotifyGUI.hpp"
#include "QuotaNotifier.hpp"

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

    void SetAccountSource(int source, const std::string& customAuthPath);

    void SetRateLimitCallback(RateLimitCallback callback);

    void LoadSettings(const AppSettings::Settings& settings);
    void SaveSettings(AppSettings::Settings& settings) const;

    void ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds);
    void RefreshAsync();
    void RefreshContextAsync();
    void PollNotifications();

private:
    CodexProvider() = default;

    void HandleRateLimit(const std::string& detail);

    std::mutex m_mutex;
    Codex::Snapshot m_snapshot;
    std::atomic_bool m_loading = false;
    std::atomic_bool m_contextLoading = false;

    AppSettings::ProviderNotifications m_notifySettings;
    AppSettings::CodexQuotaWarnings m_quotaWarnings;
    QuotaNotifier m_notifier{ "Codex" };
    size_t m_resetCreditsNoticed = 0; // reset-credit count last toasted

    std::atomic_int m_accountSource = 0;
    std::atomic<std::uint64_t> m_sourceGeneration = 0;
    mutable std::mutex m_sourceMutex;
    std::string m_customAuthPath;

    RateLimitCallback m_rateLimitCallback = nullptr;
};
