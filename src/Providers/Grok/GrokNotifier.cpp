#include "Global.hpp"
#include "GrokNotifier.hpp"

#include "Format.hpp"
#include "KSharedClock.hpp"
#include "Math.hpp"

#include <cstdint>
#include <mutex>
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

    struct WatchState
    {
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
    static GrokNotifier::Config g_config;
    static WatchState g_weeklyWatch;

    static void ResetWatchForNewResetLocked(LONGLONG resetAtUnixSeconds)
    {
        if (g_weeklyWatch.resetAtUnixSeconds == resetAtUnixSeconds) {
            return;
        }

        g_weeklyWatch.resetAtUnixSeconds = resetAtUnixSeconds;
        g_weeklyWatch.quotaWarningSent = false;
        g_weeklyWatch.quotaExhaustedSent = false;
        g_weeklyWatch.prepareSent = false;
        g_weeklyWatch.resetSent = false;
        g_weeklyWatch.lastPrepareMinutesRemaining = -1;
        g_weeklyWatch.nextQuotaWarningUnixSeconds = 0;
        g_weeklyWatch.nextQuotaExhaustedUnixSeconds = 0;
    }

    static std::string BuildQuotaWarningMessage(float usedPercent, LONGLONG resetAtUnixSeconds)
    {
        return "Grok quota warning: You are about to reach your weekly Grok quota. " +
            Format::get_instance()->QuotaLeft(usedPercent) + ". Resets at: " +
            Format::get_instance()->UnixResetTime(resetAtUnixSeconds) + ".";
    }

    static std::string BuildQuotaExhaustedMessage(float usedPercent, LONGLONG resetAtUnixSeconds)
    {
        return "Grok quota exhausted: You have exhausted your weekly Grok quota. " +
            Format::get_instance()->QuotaLeft(usedPercent) + ". Try again at: " +
            Format::get_instance()->UnixResetTime(resetAtUnixSeconds) + ".";
    }

    static std::string BuildPrepareResetMessage(int minutesRemaining)
    {
        if (minutesRemaining < 0) {
            minutesRemaining = 0;
        }

        std::string minuteText = std::to_string(minutesRemaining) + " minute";

        if (minutesRemaining != 1) {
            minuteText += "s";
        }

        return "Grok: " + minuteText + " until your weekly quota resets.";
    }

    static std::string BuildExactResetMessage()
    {
        return "Grok: Your weekly quota has been reset.";
    }
}

namespace GrokNotifier
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
        g_weeklyWatch = WatchState{};
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
        g_weeklyWatch.nextQuotaWarningUnixSeconds = nextUnix;
        g_weeklyWatch.nextQuotaExhaustedUnixSeconds = nextUnix;
    }

    void Poll(const Grok::Snapshot& snapshot, void (*refreshGrokAsync)())
    {
        std::vector<PendingNotification> pending;
        NotifyPosition position;
        bool refreshAfterReset = false;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            position = g_position;
            Config config = g_config;

            if (!config.enabled || !snapshot.weeklyLimit.valid) {
                return;
            }

            WatchState& watch = g_weeklyWatch;
            ResetWatchForNewResetLocked(static_cast<LONGLONG>(snapshot.weeklyLimit.resetAtUnixSeconds));

            LONGLONG nowUnix = KSharedClock::SystemUnixSeconds();
            float usedPercent = snapshot.weeklyLimit.usedPercent;
            LONGLONG resetAtUnixSeconds = static_cast<LONGLONG>(snapshot.weeklyLimit.resetAtUnixSeconds);

            if (watch.lastQuotaRuleEnabled != config.weekly.enabled || watch.lastQuotaRulePercent != config.weekly.percent) {
                watch.quotaWarningSent = false;
                watch.quotaExhaustedSent = false;
                watch.nextQuotaWarningUnixSeconds = config.weekly.enabled && nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;
                watch.nextQuotaExhaustedUnixSeconds = config.weekly.enabled && nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;
                watch.lastQuotaRuleEnabled = config.weekly.enabled;
                watch.lastQuotaRulePercent = config.weekly.percent;
            }

            bool exhausted = usedPercent >= 100.0f;

            if (!config.weekly.enabled) {
                watch.quotaWarningSent = false;
                watch.quotaExhaustedSent = false;
                watch.nextQuotaWarningUnixSeconds = 0;
                watch.nextQuotaExhaustedUnixSeconds = 0;
            }
            else if (exhausted && !watch.resetSent) {
                if (watch.nextQuotaExhaustedUnixSeconds == 0 || nowUnix >= watch.nextQuotaExhaustedUnixSeconds) {
                    PendingNotification n;
                    n.message = BuildQuotaExhaustedMessage(usedPercent, resetAtUnixSeconds);
                    n.color = kNotifyRed;
                    n.duration = 10.0f;
                    pending.push_back(n);

                    watch.quotaExhaustedSent = true;
                    watch.nextQuotaExhaustedUnixSeconds = nowUnix + g_quotaExhaustedRepeatSeconds;
                }
            }
            else if (usedPercent >= static_cast<float>(config.weekly.percent) && !watch.resetSent) {
                if (watch.nextQuotaWarningUnixSeconds == 0 || nowUnix >= watch.nextQuotaWarningUnixSeconds) {
                    PendingNotification n;
                    n.message = BuildQuotaWarningMessage(usedPercent, resetAtUnixSeconds);
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

            if (resetAtUnixSeconds > 0) {
                LONGLONG secondsUntilReset = KSharedClock::SecondsUntilUnix(resetAtUnixSeconds);

                if (config.prepareReset && secondsUntilReset > 0 && secondsUntilReset <= config.prepareMinutes * 60) {
                    int minutesRemaining = static_cast<int>((secondsUntilReset + 59) / 60);

                    if (minutesRemaining < 1) {
                        minutesRemaining = 1;
                    }

                    if (watch.lastPrepareMinutesRemaining != minutesRemaining) {
                        PendingNotification n;
                        n.message = BuildPrepareResetMessage(minutesRemaining);
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
                    n.message = BuildExactResetMessage();
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

        if (refreshAfterReset && refreshGrokAsync) {
            refreshGrokAsync();
        }
    }
}
