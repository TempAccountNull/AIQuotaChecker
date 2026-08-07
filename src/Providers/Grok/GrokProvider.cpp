#include "Global.hpp"
#include "GrokProvider.hpp"

#include "GrokNotifier.hpp"
#include "Network.hpp"

#include <exception>
#include <string>
#include <thread>

namespace
{
    static GrokNotifier::QuotaWarningRule ToNotifierRule(const AppSettings::QuotaWarningRule& src)
    {
        GrokNotifier::QuotaWarningRule rule;
        rule.enabled = src.enabled;
        rule.percent = src.percent;
        return rule;
    }
}

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
    GrokNotifier::SetPosition(position);

    GrokNotifier::Config cfg;
    cfg.enabled = m_notifySettings.enabled;
    cfg.prepareReset = m_notifySettings.prepareReset;
    cfg.exactReset = m_notifySettings.exactReset;
    cfg.prepareMinutes = m_notifySettings.prepareMinutes;
    cfg.weekly = ToNotifierRule(m_quotaWarnings.weekly);

    GrokNotifier::SetConfig(cfg);
    GrokNotifier::SetQuotaNotificationRepeatSeconds(quotaRepeatSeconds);
}

void GrokProvider::HandleRateLimit(const std::string& detail)
{
    if (m_rateLimitCallback) {
        m_rateLimitCallback("Grok", detail);
    }
}

void GrokProvider::RefreshThunk()
{
    GrokProvider::get_instance()->RefreshAsync();
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
    Grok::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_snapshot;
    }

    GrokNotifier::Poll(snapshot, &GrokProvider::RefreshThunk);
}
