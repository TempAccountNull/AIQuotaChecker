#include "Global.hpp"
#include "QuotaNotifier.hpp"
#include "Format.hpp"
#include "Math.hpp"

namespace
{
    constexpr std::uint32_t kNotifyAmber = NOTIFY_COL32(255, 200, 64, 255);
    constexpr std::uint32_t kNotifyGreen = NOTIFY_COL32(70, 230, 90, 255);
    constexpr std::uint32_t kNotifyRed = NOTIFY_COL32(255, 90, 90, 255);

    // After a rule or the repeat interval changes, wait this long before a
    // toast, so dragging a percent slider does not fire one per step.
    constexpr long long kConfigChangeQuietSeconds = 3;

    long long NowUnixSeconds()
    {
        return static_cast<long long>(std::time(nullptr));
    }
}

QuotaNotifier::QuotaNotifier(std::string provider)
    : m_provider(std::move(provider))
{
}

void QuotaNotifier::Configure(NotifyPosition position, const AppSettings::ProviderNotifications& settings, int repeatSeconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_position = position;
    m_settings = settings;
    m_settings.prepareMinutes = Math::get_instance()->ClampPrepareMinutes(settings.prepareMinutes);

    const long long repeat = std::clamp<long long>(repeatSeconds, 60, 7200);

    if (repeat == m_repeatSeconds) {
        return;
    }

    m_repeatSeconds = repeat;
    const long long quietUntil = NowUnixSeconds() + kConfigChangeQuietSeconds;

    for (Watch& watch : m_watches) {
        watch.nextWarningUnixSeconds = quietUntil;
        watch.nextExhaustedUnixSeconds = quietUntil;
    }
}

void QuotaNotifier::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_watches.clear();
}

QuotaNotifier::Watch& QuotaNotifier::WatchLocked(const std::string& key, long long resetAtUnixSeconds)
{
    for (Watch& watch : m_watches) {
        if (watch.key != key) {
            continue;
        }

        // A new window: everything about the old one is done.
        if (watch.resetAtUnixSeconds != resetAtUnixSeconds) {
            watch.resetAtUnixSeconds = resetAtUnixSeconds;
            watch.nextWarningUnixSeconds = 0;
            watch.nextExhaustedUnixSeconds = 0;
            watch.lastPrepareMinutes = -1;
            watch.resetSent = false;
        }

        return watch;
    }

    Watch watch;
    watch.key = key;
    watch.resetAtUnixSeconds = resetAtUnixSeconds;
    m_watches.push_back(watch);
    return m_watches.back();
}

bool QuotaNotifier::Poll(const std::vector<Point>& points)
{
    struct Toast
    {
        std::string message;
        std::uint32_t color;
        float duration;
    };

    std::vector<Toast> toasts;
    NotifyPosition position = NotifyPosition::BOTTOM_RIGHT;
    bool resetPassed = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_settings.enabled) {
            return false;
        }

        position = m_position;
        const long long now = NowUnixSeconds();

        for (const Point& point : points) {
            Watch& watch = WatchLocked(point.key, point.resetAtUnixSeconds);
            const bool ruleEnabled = point.rule.enabled;
            const int rulePercent = Math::get_instance()->ClampPercent(point.rule.percent);

            if (watch.ruleEnabled != ruleEnabled || watch.rulePercent != rulePercent) {
                const long long quietUntil = ruleEnabled ? now + kConfigChangeQuietSeconds : 0;
                watch.nextWarningUnixSeconds = quietUntil;
                watch.nextExhaustedUnixSeconds = quietUntil;
                watch.ruleEnabled = ruleEnabled;
                watch.rulePercent = rulePercent;
            }

            if (!ruleEnabled) {
                watch.nextWarningUnixSeconds = 0;
                watch.nextExhaustedUnixSeconds = 0;
            }
            else if (point.usedPercent >= 100.0f && !watch.resetSent) {
                if (watch.nextExhaustedUnixSeconds == 0 || now >= watch.nextExhaustedUnixSeconds) {
                    toasts.push_back({ ExhaustedMessage(point), kNotifyRed, 10.0f });
                    watch.nextExhaustedUnixSeconds = now + m_repeatSeconds;
                }
            }
            else if (point.usedPercent >= static_cast<float>(rulePercent) && !watch.resetSent) {
                if (watch.nextWarningUnixSeconds == 0 || now >= watch.nextWarningUnixSeconds) {
                    toasts.push_back({ WarningMessage(point), kNotifyAmber, 8.0f });
                    watch.nextWarningUnixSeconds = now + m_repeatSeconds;
                }

                watch.nextExhaustedUnixSeconds = 0;
            }
            else {
                watch.nextWarningUnixSeconds = 0;
                watch.nextExhaustedUnixSeconds = 0;
            }

            if (point.resetAtUnixSeconds <= 0) {
                continue;
            }

            const long long secondsUntilReset = point.resetAtUnixSeconds - now;
            const long long prepareSeconds = m_settings.prepareMinutes * 60LL;

            if (m_settings.prepareReset && secondsUntilReset > 0 && secondsUntilReset <= prepareSeconds) {
                const int minutes = (std::max)(1, static_cast<int>((secondsUntilReset + 59) / 60));

                if (watch.lastPrepareMinutes != minutes) {
                    toasts.push_back({ PrepareMessage(point, minutes), kNotifyAmber, 8.0f });
                    watch.lastPrepareMinutes = minutes;
                }
            }
            else if (secondsUntilReset > prepareSeconds) {
                watch.lastPrepareMinutes = -1;
            }

            if (m_settings.exactReset && secondsUntilReset <= 0 && !watch.resetSent) {
                toasts.push_back({ ResetMessage(point), kNotifyGreen, 8.0f });
                watch.resetSent = true;
                watch.lastPrepareMinutes = 0;
                resetPassed = true;
            }
        }
    }

    for (const Toast& toast : toasts) {
        NotifyGUI::Add(toast.message.c_str(), position, toast.duration, toast.color);
    }

    return resetPassed;
}

void QuotaNotifier::Notify(const std::string& message, std::uint32_t color, float duration) const
{
    NotifyPosition position = NotifyPosition::BOTTOM_RIGHT;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_settings.enabled) {
            return;
        }

        position = m_position;
    }

    NotifyGUI::Add(message.c_str(), position, duration, color);
}

std::string QuotaNotifier::WarningMessage(const Point& point) const
{
    std::string message = m_provider + " quota warning: You are about to reach your quota max for " +
        m_provider + " " + point.name + ". " + Format::get_instance()->QuotaLeft(point.usedPercent, point.leftNoun) + ".";

    if (point.resetAtUnixSeconds > 0) {
        message += " Resets at: " + Format::get_instance()->UnixResetTime(point.resetAtUnixSeconds) + ".";
    }

    return message;
}

std::string QuotaNotifier::ExhaustedMessage(const Point& point) const
{
    std::string message = m_provider + " quota exhausted: You have exhausted your remaining quota for " +
        m_provider + " " + point.name + ". " + Format::get_instance()->QuotaLeft(point.usedPercent, point.leftNoun) + ".";

    if (point.resetAtUnixSeconds > 0) {
        message += " Try again at: " + Format::get_instance()->UnixResetTime(point.resetAtUnixSeconds) + ".";
    }

    return message;
}

std::string QuotaNotifier::PrepareMessage(const Point& point, int minutes) const
{
    return m_provider + ": " + std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes") +
        " until your quota resets for " + point.name + ".";
}

std::string QuotaNotifier::ResetMessage(const Point& point) const
{
    return m_provider + ": Your quota has been reset for " + point.name + ".";
}
