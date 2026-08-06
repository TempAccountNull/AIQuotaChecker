#include "Global.hpp"
#include "ClaudeNotifier.hpp"
#include "KSharedClock.hpp"
#include "Math.hpp"
#include "Format.hpp"

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    static constexpr std::uint32_t kNotifyAmber = NOTIFY_COL32(255, 200, 64, 255);
    static constexpr std::uint32_t kNotifyGreen = NOTIFY_COL32(70, 230, 90, 255);
    static constexpr std::uint32_t kNotifyRed = NOTIFY_COL32(255, 90, 90, 255);

    static LONGLONG g_quotaWarningRepeatSeconds = 60;
    static LONGLONG g_quotaExhaustedRepeatSeconds = 60;

    static constexpr LONGLONG kConfigChangeQuietSeconds = 3;

    struct WatchPoint
    {
        std::string key;
        std::string label;
        std::string notificationName;
        float usedPercent = 0.0f;
        LONGLONG resetAtUnixSeconds = 0;
        ClaudeNotifier::QuotaWarningRule quotaRule;
    };

    struct WatchState
    {
        std::string label;
        LONGLONG resetAtUnixSeconds = 0;
        bool quotaWarningSent = false;
        bool quotaExhaustedSent = false;
        bool prepareSent = false;
        bool resetSent = false;
        int lastPrepareMinutesRemaining = -1;
        bool lastQuotaRuleEnabled = true;
        int lastQuotaRulePercent = -1;
        LONGLONG nextQuotaWarningUnixSeconds = 0;
        LONGLONG nextQuotaExhaustedUnixSeconds = 0;
    };

    struct PendingNotification
    {
        std::string message;
        std::uint32_t color = NOTIFY_COL32(255, 255, 255, 255);
        float duration = 8.0f;
    };

    static std::mutex g_mutex;
    static NotifyPosition g_position = NotifyPosition::BOTTOM_RIGHT;
    static ClaudeNotifier::Config g_config;
    static std::vector<WatchState> g_watches;


    static WatchState& GetWatchLocked(const std::string& key, LONGLONG resetAtUnixSeconds)
    {
        for (WatchState& watch : g_watches) {
            if (watch.label == key) {
                if (watch.resetAtUnixSeconds != resetAtUnixSeconds) {
                    watch.resetAtUnixSeconds = resetAtUnixSeconds;
                    watch.quotaWarningSent = false;
                    watch.quotaExhaustedSent = false;
                    watch.prepareSent = false;
                    watch.resetSent = false;
                    watch.lastPrepareMinutesRemaining = -1;
                    watch.nextQuotaWarningUnixSeconds = 0;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }

                return watch;
            }
        }

        WatchState watch;
        watch.label = key;
        watch.resetAtUnixSeconds = resetAtUnixSeconds;
        watch.quotaWarningSent = false;
        watch.quotaExhaustedSent = false;
        watch.prepareSent = false;
        watch.resetSent = false;
        watch.lastPrepareMinutesRemaining = -1;
        watch.nextQuotaWarningUnixSeconds = 0;
        watch.nextQuotaExhaustedUnixSeconds = 0;

        g_watches.push_back(watch);
        return g_watches.back();
    }


    static std::string BuildQuotaWarningMessage(const std::string& quotaName, float usedPercent, LONGLONG resetAtUnixSeconds)
    {
        return "Claude quota warning: You are about to reach your quota max for Claude " + quotaName + ". " +
            Format::get_instance()->QuotaLeft(usedPercent) + ". Resets at: " + Format::get_instance()->UnixResetTime(resetAtUnixSeconds) + ".";
    }

    static std::string BuildQuotaExhaustedMessage(const std::string& quotaName, float usedPercent, LONGLONG resetAtUnixSeconds)
    {
        return "Claude quota exhausted: You have exhausted your remaining quota for Claude " + quotaName + ". " +
            Format::get_instance()->QuotaLeft(usedPercent) + ". Try again at: " + Format::get_instance()->UnixResetTime(resetAtUnixSeconds) + ".";
    }

    static std::string BuildPrepareResetMessage(const std::string& quotaName, int minutesRemaining)
    {
        if (minutesRemaining < 0) {
            minutesRemaining = 0;
        }

        std::string minuteText = std::to_string(minutesRemaining) + " minute";

        if (minutesRemaining != 1) {
            minuteText += "s";
        }

        return "Claude: " + minuteText + " until your quota resets for " + quotaName + ".";
    }

    static std::string BuildExactResetMessage(const std::string& quotaName)
    {
        return "Claude: Your quota has been reset for " + quotaName + ".";
    }

    static void AddUsageWindow(
        std::vector<WatchPoint>& points,
        const Claude::UsageWindow& window,
        const ClaudeNotifier::QuotaWarningRule& rule,
        const char* key,
        const char* notificationName
    )
    {
        if (!window.valid) {
            return;
        }

        WatchPoint point;
        point.key = key;
        point.label = window.title;
        point.notificationName = notificationName;
        point.usedPercent = window.usedPercent;
        point.resetAtUnixSeconds = static_cast<LONGLONG>(window.resetAtUnixSeconds);
        point.quotaRule = rule;
        points.push_back(point);
    }

    static void BuildWatchPoints(const Claude::Snapshot& snapshot, const ClaudeNotifier::Config& config, std::vector<WatchPoint>& points)
    {
        points.clear();

        AddUsageWindow(points, snapshot.currentSession, config.currentSession, "claude_5_hour", "5-hour limit/session");
        AddUsageWindow(points, snapshot.weeklyAllModels, config.allModels, "claude_weekly_all_models", "weekly - all models");
        AddUsageWindow(points, snapshot.weeklySonnet, config.sonnet, "claude_weekly_sonnet", "weekly - Sonnet");
        AddUsageWindow(points, snapshot.weeklyFable, config.fable, "claude_weekly_fable", "weekly - Fable");

        if (snapshot.credits.valid && snapshot.credits.enabled && snapshot.credits.hasUsedPercent) {
            WatchPoint point;
            point.key = "claude_usage_credits";
            point.label = "Usage credits";
            point.notificationName = "usage credits/extra usage";
            point.usedPercent = snapshot.credits.usedPercent;
            point.resetAtUnixSeconds = static_cast<LONGLONG>(snapshot.credits.resetAtUnixSeconds);
            point.quotaRule = config.credits;
            points.push_back(point);
        }
    }
}

namespace ClaudeNotifier
{
    void SetPosition(NotifyPosition position)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_position = position;
    }

    NotifyPosition GetPosition()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_position;
    }

    void SetConfig(const Config& config)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = config;

        g_config.prepareMinutes = Math::get_instance()->ClampPrepareMinutes(g_config.prepareMinutes);
        g_config.currentSession.percent = Math::get_instance()->ClampPercent(g_config.currentSession.percent);
        g_config.allModels.percent = Math::get_instance()->ClampPercent(g_config.allModels.percent);
        g_config.sonnet.percent = Math::get_instance()->ClampPercent(g_config.sonnet.percent);
        g_config.fable.percent = Math::get_instance()->ClampPercent(g_config.fable.percent);
        g_config.credits.percent = Math::get_instance()->ClampPercent(g_config.credits.percent);
    }

    Config GetConfig()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_config;
    }

    void ResetState()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_watches.clear();
    }

    void SetQuotaNotificationRepeatSeconds(int seconds)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (seconds < 60) {
            seconds = 60;
        }

        if (seconds > 7200) {
            seconds = 7200;
        }

        LONGLONG newValue = static_cast<LONGLONG>(seconds);

        if (g_quotaWarningRepeatSeconds == newValue && g_quotaExhaustedRepeatSeconds == newValue) {
            return;
        }

        g_quotaWarningRepeatSeconds = newValue;
        g_quotaExhaustedRepeatSeconds = newValue;

        LONGLONG nowUnix = KSharedClock::SystemUnixSeconds();
        LONGLONG nextUnix = nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;

        for (WatchState& watch : g_watches) {
            watch.nextQuotaWarningUnixSeconds = nextUnix;
            watch.nextQuotaExhaustedUnixSeconds = nextUnix;
        }
    }

    void Poll(const Claude::Snapshot& snapshot, void (*refreshClaudeAsync)())
    {
        std::vector<WatchPoint> points;
        std::vector<PendingNotification> pending;
        NotifyPosition position;
        bool refreshAfterReset = false;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            position = g_position;

            ClaudeNotifier::Config config = g_config;

            if (!config.enabled) {
                return;
            }

            BuildWatchPoints(snapshot, config, points);

            for (const WatchPoint& point : points) {
                WatchState& watch = GetWatchLocked(point.key, point.resetAtUnixSeconds);

                LONGLONG nowUnix = KSharedClock::SystemUnixSeconds();

                if (watch.lastQuotaRuleEnabled != point.quotaRule.enabled || watch.lastQuotaRulePercent != point.quotaRule.percent) {
                    watch.quotaWarningSent = false;
                    watch.quotaExhaustedSent = false;
                    watch.nextQuotaWarningUnixSeconds = point.quotaRule.enabled && nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;
                    watch.nextQuotaExhaustedUnixSeconds = point.quotaRule.enabled && nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;
                    watch.lastQuotaRuleEnabled = point.quotaRule.enabled;
                    watch.lastQuotaRulePercent = point.quotaRule.percent;
                }

                bool exhausted = point.usedPercent >= 100.0f;

                if (!point.quotaRule.enabled) {
                    watch.quotaWarningSent = false;
                    watch.quotaExhaustedSent = false;
                    watch.nextQuotaWarningUnixSeconds = 0;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }
                else if (exhausted && !watch.resetSent) {
                    if (watch.nextQuotaExhaustedUnixSeconds == 0 || nowUnix >= watch.nextQuotaExhaustedUnixSeconds) {
                        PendingNotification n;
                        n.message = BuildQuotaExhaustedMessage(point.notificationName, point.usedPercent, point.resetAtUnixSeconds);
                        n.color = kNotifyRed;
                        n.duration = 10.0f;
                        pending.push_back(n);

                        watch.quotaExhaustedSent = true;
                        watch.nextQuotaExhaustedUnixSeconds = nowUnix + g_quotaExhaustedRepeatSeconds;
                    }
                }
                else if (point.usedPercent >= static_cast<float>(point.quotaRule.percent) && !watch.resetSent) {
                    if (watch.nextQuotaWarningUnixSeconds == 0 || nowUnix >= watch.nextQuotaWarningUnixSeconds) {
                        PendingNotification n;
                        n.message = BuildQuotaWarningMessage(point.notificationName, point.usedPercent, point.resetAtUnixSeconds);
                        n.color = kNotifyAmber;
                        n.duration = 8.0f;
                        pending.push_back(n);

                        watch.quotaWarningSent = true;
                        watch.nextQuotaWarningUnixSeconds = nowUnix + g_quotaWarningRepeatSeconds;
                    }

                    watch.quotaExhaustedSent = false;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }
                else {
                    watch.quotaWarningSent = false;
                    watch.quotaExhaustedSent = false;
                    watch.nextQuotaWarningUnixSeconds = 0;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }

                if (point.resetAtUnixSeconds <= 0) {
                    continue;
                }

                LONGLONG secondsUntilReset = KSharedClock::SecondsUntilUnix(point.resetAtUnixSeconds);

                if (config.prepareReset && secondsUntilReset > 0 && secondsUntilReset <= config.prepareMinutes * 60) {
                    int minutesRemaining = static_cast<int>((secondsUntilReset + 59) / 60);

                    if (minutesRemaining < 1) {
                        minutesRemaining = 1;
                    }

                    if (watch.lastPrepareMinutesRemaining != minutesRemaining) {
                        PendingNotification n;
                        n.message = BuildPrepareResetMessage(point.notificationName, minutesRemaining);
                        n.color = kNotifyAmber;
                        n.duration = 8.0f;
                        pending.push_back(n);

                        watch.prepareSent = true;
                        watch.lastPrepareMinutesRemaining = minutesRemaining;
                    }
                }
                else if (secondsUntilReset > config.prepareMinutes * 60) {
                    watch.prepareSent = false;
                    watch.lastPrepareMinutesRemaining = -1;
                }

                if (config.exactReset && secondsUntilReset <= 0 && !watch.resetSent) {
                    PendingNotification n;
                    n.message = BuildExactResetMessage(point.notificationName);
                    n.color = kNotifyGreen;
                    n.duration = 8.0f;
                    pending.push_back(n);

                    watch.resetSent = true;
                    watch.lastPrepareMinutesRemaining = 0;
                    refreshAfterReset = true;
                }
            }
        }

        for (const PendingNotification& n : pending) {
            NotifyGUI::Add(n.message.c_str(), position, n.duration, n.color);
        }

        if (refreshAfterReset && refreshClaudeAsync) {
            refreshClaudeAsync();
        }
    }
}
