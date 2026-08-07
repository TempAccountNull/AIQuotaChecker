#include "Global.hpp"
#include "ZAiProvider.hpp"

#include "ZAiNotifier.hpp"
#include "Network.hpp"

#include <exception>
#include <string>
#include <thread>

namespace
{

    static ZAiNotifier::QuotaWarningRule ToNotifierRule(const AppSettings::QuotaWarningRule& src)
    {
        ZAiNotifier::QuotaWarningRule rule;
        rule.enabled = src.enabled;
        rule.percent = src.percent;
        return rule;
    }
}

ZAiProvider* ZAiProvider::get_instance()
{
    static ZAiProvider provider;
    return &provider;
}

std::mutex* ZAiProvider::StateMutex()
{
    return &m_mutex;
}

ZAi::Snapshot* ZAiProvider::Snapshot()
{
    return &m_snapshot;
}

std::atomic_bool* ZAiProvider::Loading()
{
    return &m_loading;
}

AppSettings::ProviderNotifications* ZAiProvider::NotifySettings()
{
    return &m_notifySettings;
}

AppSettings::ZAiQuotaWarnings* ZAiProvider::QuotaWarnings()
{
    return &m_quotaWarnings;
}

void ZAiProvider::SetRateLimitCallback(RateLimitCallback callback)
{
    m_rateLimitCallback = callback;
}

void ZAiProvider::LoadSettings(const AppSettings::Settings& settings)
{
    m_notifySettings = settings.zai;
    m_quotaWarnings = settings.zaiQuotaWarnings;
}

void ZAiProvider::SaveSettings(AppSettings::Settings& settings) const
{
    settings.zai = m_notifySettings;
    settings.zaiQuotaWarnings = m_quotaWarnings;
}

void ZAiProvider::ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds)
{
    ZAiNotifier::SetPosition(position);

    ZAiNotifier::Config cfg;
    cfg.enabled = m_notifySettings.enabled;
    cfg.prepareReset = m_notifySettings.prepareReset;
    cfg.exactReset = m_notifySettings.exactReset;
    cfg.prepareMinutes = m_notifySettings.prepareMinutes;
    cfg.glm52 = ToNotifierRule(m_quotaWarnings.glm52);
    cfg.turbo = ToNotifierRule(m_quotaWarnings.turbo);

    ZAiNotifier::SetConfig(cfg);
    ZAiNotifier::SetQuotaNotificationRepeatSeconds(quotaRepeatSeconds);
}

void ZAiProvider::HandleRateLimit(const std::string& detail)
{
    if (m_rateLimitCallback) {
        m_rateLimitCallback("Z.Ai", detail);
    }
}

void ZAiProvider::RefreshAsync()
{
    if (m_loading.exchange(true)) {
        return;
    }

    std::thread([] {
        ZAiProvider* self = ZAiProvider::get_instance();

        try {
            ZAi::Snapshot snapshot = ZAi::FetchSnapshot();

            if (Network::get_instance()->IsRateLimitText(snapshot.statusText)) {
                self->HandleRateLimit(snapshot.statusText);
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = snapshot;
        }
        catch (const std::exception& e) {
            std::string error = std::string("Z.Ai error: ") + e.what();

            if (Network::get_instance()->IsRateLimitText(error)) {
                self->HandleRateLimit(error);
            }

            ZAi::Snapshot failedSnapshot;
            failedSnapshot.statusText = error;
            failedSnapshot.lastUpdated = "now";
            failedSnapshot.access = UsageTelemetry::FromText(error);

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = failedSnapshot;
        }

        *self->Loading() = false;
    }).detach();
}

void ZAiProvider::PollNotifications()
{
    ZAi::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_snapshot;
    }

    ZAiNotifier::Poll(snapshot);
}
