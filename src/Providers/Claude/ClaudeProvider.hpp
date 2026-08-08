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
    void RefreshContextAsync();
    void PollNotifications();

private:
    ClaudeProvider() = default;

    static void RefreshThunk();
    void HandleRateLimit(const std::string& detail);
    void ApplyLocalTelemetryLocked(Claude::LocalTelemetry local, Claude::Snapshot& target);

    std::mutex m_mutex;
    Claude::Snapshot m_snapshot;
    std::string m_lastSuccessfulAccountKey;
    std::atomic_bool m_loading = false;
    std::atomic_bool m_contextLoading = false;

    // Compaction is intentionally latched because Claude Desktop can start
    // showing its compaction UI before compact_boundary is flushed to the
    // JSONL transcript. The boundary/new low-context value clears the latch.
    bool m_compactionLatched = false;
    long long m_compactionStartedAtUnixSeconds = 0;
    // Last time the foreground JSONL positively showed an active user turn.
    // Compaction can stop transcript writes for minutes, so this lets the
    // provider bridge the 0%-until-auto-compact edge without polling Claude.
    long long m_lastActiveRunSeenAtUnixSeconds = 0;
    long long m_compactionNoticeAtUnixSeconds = 0;
    bool m_compactionNoticeEligible = false;
    long long m_lastCompactionSavedTokens = 0;
    std::string m_lastCompactionEventId;

    AppSettings::ProviderNotifications m_notifySettings;
    AppSettings::ClaudeQuotaWarnings m_quotaWarnings;

    std::atomic_int m_accountSource = 0;
    std::atomic<std::uint64_t> m_sourceGeneration = 0;
    mutable std::mutex m_sourceMutex;

    RateLimitCallback m_rateLimitCallback = nullptr;
};
