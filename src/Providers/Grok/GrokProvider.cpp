#include "Global.hpp"
#include "GrokProvider.hpp"

#include "Network.hpp"
#include "Text.hpp"

#include <exception>
#include <string>
#include <thread>

GrokProvider* GrokProvider::get_instance()
{
    static GrokProvider provider;
    return &provider;
}

std::mutex* GrokProvider::StateMutex()
{
    return &m_mutex;
}

Grok::Snapshot* GrokProvider::Snapshot()
{
    return &m_snapshot;
}

std::atomic_bool* GrokProvider::Loading()
{
    return &m_loading;
}

AppSettings::ProviderNotifications* GrokProvider::NotifySettings()
{
    return &m_notifySettings;
}

AppSettings::GrokQuotaWarnings* GrokProvider::QuotaWarnings()
{
    return &m_quotaWarnings;
}

void GrokProvider::SetRateLimitCallback(RateLimitCallback callback)
{
    m_rateLimitCallback = callback;
}

void GrokProvider::LoadSettings(const AppSettings::Settings& settings)
{
    m_notifySettings = settings.grok;
    m_quotaWarnings = settings.grokQuotaWarnings;
}

void GrokProvider::SaveSettings(AppSettings::Settings& settings) const
{
    settings.grok = m_notifySettings;
    settings.grokQuotaWarnings = m_quotaWarnings;
}

void GrokProvider::ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds)
{
    m_notifier.Configure(position, m_notifySettings, quotaRepeatSeconds);
}

void GrokProvider::HandleRateLimit(const std::string& detail)
{
    if (m_rateLimitCallback) {
        m_rateLimitCallback("Grok", detail);
    }
}

void GrokProvider::RefreshAsync()
{
    if (m_loading.exchange(true)) {
        return;
    }

    std::thread([] {
        GrokProvider* self = GrokProvider::get_instance();

        try {
            Grok::Snapshot snapshot = Grok::FetchSnapshot();

            if (Network::get_instance()->IsRateLimitText(snapshot.statusText)) {
                self->HandleRateLimit(snapshot.statusText);
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = snapshot;
        }
        catch (const std::exception& e) {
            std::string error = std::string("Grok error: ") + e.what();

            if (Network::get_instance()->IsRateLimitText(error)) {
                self->HandleRateLimit(error);
            }

            Grok::Snapshot failedSnapshot;
            failedSnapshot.statusText = error;
            failedSnapshot.lastUpdated = "now";
            failedSnapshot.access = UsageTelemetry::FromText(error);

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = failedSnapshot;
        }

        *self->Loading() = false;
    }).detach();
}

void GrokProvider::PollNotifications()
{
    std::vector<QuotaNotifier::Point> points;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_snapshot.weeklyLimit.valid) {
            QuotaNotifier::Point point;
            point.key = "weekly";
            // "weekly limit", or "monthly limit" on a monthly billing period.
            point.name = Text::get_instance()->ToLowerCopy(m_snapshot.weeklyLimit.title);
            point.usedPercent = m_snapshot.weeklyLimit.usedPercent;
            point.resetAtUnixSeconds = m_snapshot.weeklyLimit.resetAtUnixSeconds;
            point.rule = m_quotaWarnings.weekly;
            points.push_back(std::move(point));
        }
    }

    if (m_notifier.Poll(points)) {
        RefreshAsync();
    }
}
