#include "Global.hpp"
#include "CodexNotifier.hpp"
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
    static constexpr std::uint32_t kNotifyCyan = NOTIFY_COL32(0, 220, 255, 255);
    static constexpr std::uint32_t kNotifyAmber = NOTIFY_COL32(255, 200, 64, 255);
    static constexpr std::uint32_t kNotifyGreen = NOTIFY_COL32(70, 230, 90, 255);
    static constexpr std::uint32_t kNotifyRed = NOTIFY_COL32(255, 90, 90, 255);

    static LONGLONG g_quotaWarningRepeatSeconds = 60;
    static LONGLONG g_quotaExhaustedRepeatSeconds = 60;

    static constexpr LONGLONG kConfigChangeQuietSeconds = 3;

    struct ResetPoint
    {
        std::string key;
        std::string label;
        std::string notificationName;
        float usedPercent = 0.0f;
        LONGLONG resetAtUnixSeconds = 0;
    };

    struct ResetWatch
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
    static CodexNotifier::Config g_config;

    static size_t g_lastResetCreditCount = 0;
    static bool g_resetCreditNoticeSent = false;
    static std::vector<ResetWatch> g_resetWatches;


    static ResetWatch& GetResetWatchLocked(const std::string& key, LONGLONG resetAtUnixSeconds)
    {
        for (ResetWatch& watch : g_resetWatches) {
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

        ResetWatch watch;
        watch.label = key;
        watch.resetAtUnixSeconds = resetAtUnixSeconds;
        watch.quotaWarningSent = false;
        watch.quotaExhaustedSent = false;
        watch.prepareSent = false;
        watch.resetSent = false;
        watch.lastPrepareMinutesRemaining = -1;
        watch.nextQuotaWarningUnixSeconds = 0;
        watch.nextQuotaExhaustedUnixSeconds = 0;

        g_resetWatches.push_back(watch);
        return g_resetWatches.back();
    }

    static std::string BuildResetCreditMessage(size_t count)
    {
        std::string message = "Codex reset credits: You have " + std::to_string(count) + " ";

        if (count == 1) {
            message += "reset available to use in Codex. Make sure to use it!";
        }
        else {
            message += "resets available to use in Codex. Make sure to use them!";
        }

        return message;
    }


    static std::string BuildQuotaWarningMessage(const std::string& quotaName, float usedPercent, LONGLONG resetAtUnixSeconds)
    {
        return "Codex quota warning: You are about to reach your quota max for Codex " + quotaName + ". " +
            Format::get_instance()->QuotaLeft(usedPercent) + ". Resets at: " + Format::get_instance()->UnixResetTime(resetAtUnixSeconds) + ".";
    }

    static std::string BuildQuotaExhaustedMessage(const std::string& quotaName, float usedPercent, LONGLONG resetAtUnixSeconds)
    {
        return "Codex quota exhausted: You have exhausted your remaining quota for Codex " + quotaName + ". " +
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

        return "Codex: " + minuteText + " until your quota resets for " + quotaName + ".";
    }

    static std::string BuildExactResetMessage(const std::string& quotaName)
    {
        return "Codex: Your quota has been reset for " + quotaName + ".";
    }

    static CodexNotifier::QuotaWarningRule GetQuotaRuleForLabel(const CodexNotifier::Config& config, const std::string& label)
    {
        if (label.find("5-hour") != std::string::npos || label.find("5h") != std::string::npos || label.find("Session") != std::string::npos || label.find("session") != std::string::npos) {
            return config.fiveHour;
        }

        return config.weekly;
    }

    static void FillCodexPointIdentity(const std::string& label, ResetPoint& point)
    {
        if (label.find("Session") != std::string::npos || label.find("session") != std::string::npos || label.find("5-hour") != std::string::npos || label.find("5h") != std::string::npos) {
            point.key = "codex_5_hour";
            point.notificationName = "5-hour limit/session";
            return;
        }

        point.key = "codex_weekly_all_models";
        point.notificationName = "weekly - all models";
    }

    static void BuildResetPoints(const Codex::Snapshot& snapshot, std::vector<ResetPoint>& resetPoints)
    {
        resetPoints.clear();

        for (const Codex::UsageBar& bar : snapshot.bars) {
            if (!bar.valid || !bar.quotaNotificationEligible) {
                continue;
            }

            ResetPoint point;
            point.label = bar.label;
            FillCodexPointIdentity(bar.label, point);
            point.usedPercent = bar.usedPercent;
            point.resetAtUnixSeconds = static_cast<LONGLONG>(bar.resetAtUnixSeconds);
            resetPoints.push_back(point);
        }
    }
}

namespace CodexNotifier
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
        g_config.fiveHour.percent = Math::get_instance()->ClampPercent(g_config.fiveHour.percent);
        g_config.weekly.percent = Math::get_instance()->ClampPercent(g_config.weekly.percent);
    }

    Config GetConfig()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_config;
    }

    void ResetState()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        g_lastResetCreditCount = 0;
        g_resetCreditNoticeSent = false;
        g_resetWatches.clear();
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

        for (ResetWatch& watch : g_resetWatches) {
            watch.nextQuotaWarningUnixSeconds = nextUnix;
            watch.nextQuotaExhaustedUnixSeconds = nextUnix;
        }
    }

    void Poll(const Codex::Snapshot& snapshot, void (*refreshCodexAsync)())
    {
        std::vector<ResetPoint> resetPoints;
        BuildResetPoints(snapshot, resetPoints);

        size_t resetCreditCount = snapshot.resetCreditsAvailableCount >= 0
            ? static_cast<size_t>(snapshot.resetCreditsAvailableCount)
            : snapshot.resetCredits.size();

        std::vector<PendingNotification> pending;
        NotifyPosition position;
        bool refreshAfterReset = false;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            position = g_position;

            CodexNotifier::Config config = g_config;

            if (!config.enabled) {
                return;
            }

            if (config.resetCredits && resetCreditCount > 0) {
                if (!g_resetCreditNoticeSent || g_lastResetCreditCount != resetCreditCount) {
                    PendingNotification n;
                    n.message = BuildResetCreditMessage(resetCreditCount);
                    n.color = kNotifyCyan;
                    n.duration = 8.0f;
                    pending.push_back(n);

                    g_resetCreditNoticeSent = true;
                    g_lastResetCreditCount = resetCreditCount;
                }
            }
            else if (resetCreditCount == 0) {
                g_resetCreditNoticeSent = false;
                g_lastResetCreditCount = 0;
            }

            for (const ResetPoint& point : resetPoints) {
                ResetWatch& watch = GetResetWatchLocked(point.key, point.resetAtUnixSeconds);
                CodexNotifier::QuotaWarningRule quotaRule = GetQuotaRuleForLabel(config, point.label);

                LONGLONG nowUnix = KSharedClock::SystemUnixSeconds();

                if (watch.lastQuotaRuleEnabled != quotaRule.enabled || watch.lastQuotaRulePercent != quotaRule.percent) {
                    watch.quotaWarningSent = false;
                    watch.quotaExhaustedSent = false;
                    watch.nextQuotaWarningUnixSeconds = quotaRule.enabled && nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;
                    watch.nextQuotaExhaustedUnixSeconds = quotaRule.enabled && nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;
                    watch.lastQuotaRuleEnabled = quotaRule.enabled;
                    watch.lastQuotaRulePercent = quotaRule.percent;
                }

                bool exhausted = point.usedPercent >= 100.0f;

                if (!quotaRule.enabled) {
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
                else if (point.usedPercent >= static_cast<float>(quotaRule.percent) && !watch.resetSent) {
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

        if (refreshAfterReset && refreshCodexAsync) {
            refreshCodexAsync();
        }
    }
}
