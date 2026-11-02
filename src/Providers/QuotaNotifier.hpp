#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "AppSettings.hpp"
#include "NotifyGUI.hpp"

// The quota toasts every provider shares. For each watched limit window:
//   - a warning at the rule's percent and an "exhausted" toast at 100%, each
//     repeating every refresh interval while it still applies;
//   - a countdown toast each minute inside the "prepare" window before a reset;
//   - a "has been reset" toast when the reset time passes, after which the
//     provider refreshes to pick up the new window.
// Providers only turn their snapshot into Points.
class QuotaNotifier final
{
public:
    struct Point
    {
        std::string key;          // unique per window within the provider
        std::string name;         // as worded in toasts: "weekly - all models"
        std::string leftNoun = "quota"; // "N% quota left"; Z.Ai says "balance"
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        AppSettings::QuotaWarningRule rule;
    };

    explicit QuotaNotifier(std::string provider); // "Codex", "Claude", "Z.Ai", "Grok"

    // From Settings: where toasts go, which kinds are on, and how often a
    // still-true warning repeats (the auto refresh interval, 60..7200 s).
    void Configure(NotifyPosition position, const AppSettings::ProviderNotifications& settings, int repeatSeconds);

    // The account changed: forget every window.
    void Reset();

    // Returns true when a window has just reset, so the caller refreshes.
    bool Poll(const std::vector<Point>& points);

    // A one-off toast at the configured position, unless toasts are off.
    void Notify(const std::string& message, std::uint32_t color, float duration) const;

private:
    struct Watch
    {
        std::string key;
        long long resetAtUnixSeconds = 0;
        long long nextWarningUnixSeconds = 0;
        long long nextExhaustedUnixSeconds = 0;
        int lastPrepareMinutes = -1;
        bool resetSent = false;
        bool ruleEnabled = true;
        int rulePercent = -1;
    };

    Watch& WatchLocked(const std::string& key, long long resetAtUnixSeconds);

    std::string WarningMessage(const Point& point) const;
    std::string ExhaustedMessage(const Point& point) const;
    std::string PrepareMessage(const Point& point, int minutes) const;
    std::string ResetMessage(const Point& point) const;

    const std::string m_provider;
    mutable std::mutex m_mutex;
    NotifyPosition m_position = NotifyPosition::BOTTOM_RIGHT;
    AppSettings::ProviderNotifications m_settings;
    long long m_repeatSeconds = 60;
    std::vector<Watch> m_watches;
};
