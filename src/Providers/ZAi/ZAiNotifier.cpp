#include "Global.hpp"
#include "ZAiNotifier.hpp"
#include "KSharedClock.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cctype>
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

    static constexpr LONGLONG kConfigChangeQuietSeconds = 3;

    static LONGLONG g_quotaWarningRepeatSeconds = 60;
    static LONGLONG g_quotaExhaustedRepeatSeconds = 60;

    struct WatchState
    {
        std::string key;
        LONGLONG resetAtUnixSeconds = 0;
        LONGLONG nextQuotaWarningUnixSeconds = 0;
        LONGLONG nextQuotaExhaustedUnixSeconds = 0;
        int lastPrepareMinutesRemaining = -1;
        bool resetSent = false;
    };

    struct PendingNotification
    {
        std::string message;
        std::uint32_t color = NOTIFY_COL32(255, 255, 255, 255);
        float duration = 8.0f;
    };


    static ZAiNotifier::QuotaWarningRule GetRuleForBar(const ZAiNotifier::Config& config, const ZAi::UsageBar& bar)
    {
        std::string compact = Text::get_instance()->CompactLower(bar.label);

        if (compact.find("turbo") != std::string::npos) {
            return config.turbo;
        }

        return config.glm52;
    }

    static std::mutex g_mutex;
    static NotifyPosition g_position = NotifyPosition::BOTTOM_RIGHT;
    static ZAiNotifier::Config g_config;
    static std::vector<WatchState> g_watches;

    static WatchState& GetWatchLocked(const std::string& key, LONGLONG resetAtUnixSeconds)
    {
        for (WatchState& watch : g_watches) {
            if (watch.key == key) {
                if (watch.resetAtUnixSeconds != resetAtUnixSeconds) {
                    watch.resetAtUnixSeconds = resetAtUnixSeconds;
                    watch.nextQuotaWarningUnixSeconds = 0;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                    watch.lastPrepareMinutesRemaining = -1;
                    watch.resetSent = false;
                }

                return watch;
            }
        }

        WatchState watch;
        watch.key = key;
        watch.resetAtUnixSeconds = resetAtUnixSeconds;
        watch.nextQuotaWarningUnixSeconds = 0;
        watch.nextQuotaExhaustedUnixSeconds = 0;
        watch.lastPrepareMinutesRemaining = -1;
        watch.resetSent = false;
        g_watches.push_back(watch);
        return g_watches.back();
    }


    static std::string BuildQuotaWarningMessage(const ZAi::UsageBar& bar)
    {
        std::string message = "Z.Ai quota warning: You are about to reach your quota max for Z.Ai " + bar.label + ". " + Format::get_instance()->QuotaLeft(bar.usedPercent, "balance") + ".";

        if (bar.resetAtUnixSeconds > 0) {
            message += " Resets at: " + Format::get_instance()->UnixResetTime(static_cast<LONGLONG>(bar.resetAtUnixSeconds)) + ".";
        }

        return message;
    }

    static std::string BuildQuotaExhaustedMessage(const ZAi::UsageBar& bar)
    {
        std::string message = "Z.Ai quota exhausted: You have exhausted your remaining quota for Z.Ai " + bar.label + ". " + Format::get_instance()->QuotaLeft(bar.usedPercent, "balance") + ".";

        if (bar.resetAtUnixSeconds > 0) {
            message += " Try again at: " + Format::get_instance()->UnixResetTime(static_cast<LONGLONG>(bar.resetAtUnixSeconds)) + ".";
        }

        return message;
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

        return "Z.Ai: " + minuteText + " until your quota resets for " + quotaName + ".";
    }

    static std::string BuildExactResetMessage(const std::string& quotaName)
    {
        return "Z.Ai: Your quota has been reset for " + quotaName + ".";
    }
}

namespace ZAiNotifier
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
        bool changed = g_config.enabled != config.enabled ||
            g_config.prepareReset != config.prepareReset ||
            g_config.exactReset != config.exactReset ||
            g_config.prepareMinutes != config.prepareMinutes ||
            g_config.glm52.enabled != config.glm52.enabled ||
            g_config.glm52.percent != config.glm52.percent ||
            g_config.turbo.enabled != config.turbo.enabled ||
            g_config.turbo.percent != config.turbo.percent;

        g_config = config;
        g_config.prepareMinutes = Math::get_instance()->ClampPrepareMinutes(g_config.prepareMinutes);
        g_config.glm52.percent = Math::get_instance()->ClampPercent(g_config.glm52.percent);
        g_config.turbo.percent = Math::get_instance()->ClampPercent(g_config.turbo.percent);

        if (changed) {
            LONGLONG nowUnix = KSharedClock::SystemUnixSeconds();
            LONGLONG nextUnix = nowUnix > 0 ? nowUnix + kConfigChangeQuietSeconds : 0;

            for (WatchState& watch : g_watches) {
                watch.nextQuotaWarningUnixSeconds = nextUnix;
                watch.nextQuotaExhaustedUnixSeconds = nextUnix;
                watch.lastPrepareMinutesRemaining = -1;
            }
        }
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

    void Poll(const ZAi::Snapshot& snapshot)
    {
        std::vector<PendingNotification> pending;
        NotifyPosition position;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            position = g_position;
            ZAiNotifier::Config config = g_config;

            if (!config.enabled) {
                return;
            }

            LONGLONG nowUnix = KSharedClock::SystemUnixSeconds();

            if (nowUnix <= 0) {
                return;
            }

            for (const ZAi::UsageBar& bar : snapshot.bars) {
                if (!bar.valid) {
                    continue;
                }

                WatchState& watch = GetWatchLocked(bar.label, static_cast<LONGLONG>(bar.resetAtUnixSeconds));
                ZAiNotifier::QuotaWarningRule rule = GetRuleForBar(config, bar);
                bool exhausted = bar.usedPercent >= 100.0f;

                if (!rule.enabled) {
                    watch.nextQuotaWarningUnixSeconds = 0;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }
                else if (exhausted) {
                    if (watch.nextQuotaExhaustedUnixSeconds == 0 || nowUnix >= watch.nextQuotaExhaustedUnixSeconds) {
                        PendingNotification n;
                        n.message = BuildQuotaExhaustedMessage(bar);
                        n.color = kNotifyRed;
                        n.duration = 10.0f;
                        pending.push_back(n);

                        watch.nextQuotaExhaustedUnixSeconds = nowUnix + g_quotaExhaustedRepeatSeconds;
                    }

                    watch.nextQuotaWarningUnixSeconds = 0;
                }
                else if (bar.usedPercent >= static_cast<float>(rule.percent)) {
                    if (watch.nextQuotaWarningUnixSeconds == 0 || nowUnix >= watch.nextQuotaWarningUnixSeconds) {
                        PendingNotification n;
                        n.message = BuildQuotaWarningMessage(bar);
                        n.color = kNotifyAmber;
                        n.duration = 8.0f;
                        pending.push_back(n);

                        watch.nextQuotaWarningUnixSeconds = nowUnix + g_quotaWarningRepeatSeconds;
                    }

                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }
                else {
                    watch.nextQuotaWarningUnixSeconds = 0;
                    watch.nextQuotaExhaustedUnixSeconds = 0;
                }

                if (bar.resetAtUnixSeconds <= 0) {
                    continue;
                }

                LONGLONG secondsUntilReset = KSharedClock::SecondsUntilUnix(static_cast<LONGLONG>(bar.resetAtUnixSeconds));

                if (config.prepareReset && secondsUntilReset > 0 && secondsUntilReset <= static_cast<LONGLONG>(config.prepareMinutes) * 60) {
                    int minutesRemaining = static_cast<int>((secondsUntilReset + 59) / 60);

                    if (minutesRemaining < 1) {
                        minutesRemaining = 1;
                    }

                    if (watch.lastPrepareMinutesRemaining != minutesRemaining) {
                        PendingNotification n;
                        n.message = BuildPrepareResetMessage(bar.label, minutesRemaining);
                        n.color = kNotifyAmber;
                        n.duration = 8.0f;
                        pending.push_back(n);

                        watch.lastPrepareMinutesRemaining = minutesRemaining;
                    }
                }
                else if (secondsUntilReset > static_cast<LONGLONG>(config.prepareMinutes) * 60) {
                    watch.lastPrepareMinutesRemaining = -1;
                }

                if (config.exactReset && secondsUntilReset <= 0 && !watch.resetSent) {
                    PendingNotification n;
                    n.message = BuildExactResetMessage(bar.label);
                    n.color = kNotifyGreen;
                    n.duration = 8.0f;
                    pending.push_back(n);

                    watch.resetSent = true;
                }
            }
        }

        for (const PendingNotification& n : pending) {
            NotifyGUI::Add(n.message.c_str(), position, n.duration, n.color);
        }
    }
}
