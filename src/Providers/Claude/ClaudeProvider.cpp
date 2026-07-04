#include "Global.hpp"
#include "ClaudeProvider.hpp"

#include "ClaudeNotifier.hpp"
#include "Network.hpp"

#include <exception>
#include <string>
#include <thread>

namespace
{

    static ClaudeNotifier::QuotaWarningRule ToNotifierRule(const AppSettings::QuotaWarningRule& src)
    {
        ClaudeNotifier::QuotaWarningRule rule;
        rule.enabled = src.enabled;
        rule.percent = src.percent;
        return rule;
    }
}

ClaudeProvider* ClaudeProvider::get_instance()
{
    static ClaudeProvider provider;
    return &provider;
}

std::mutex* ClaudeProvider::StateMutex()
{
    return &m_mutex;
}

Claude::Snapshot* ClaudeProvider::Snapshot()
{
    return &m_snapshot;
}

std::atomic_bool* ClaudeProvider::Loading()
{
    return &m_loading;
}

AppSettings::ProviderNotifications* ClaudeProvider::NotifySettings()
{
    return &m_notifySettings;
}

AppSettings::ClaudeQuotaWarnings* ClaudeProvider::QuotaWarnings()
{
    return &m_quotaWarnings;
}

void ClaudeProvider::SetRateLimitCallback(RateLimitCallback callback)
{
    m_rateLimitCallback = callback;
}

void ClaudeProvider::LoadSettings(const AppSettings::Settings& settings)
{
    m_notifySettings = settings.claude;
    m_quotaWarnings = settings.claudeQuotaWarnings;
}

void ClaudeProvider::SaveSettings(AppSettings::Settings& settings) const
{
    settings.claude = m_notifySettings;
    settings.claudeQuotaWarnings = m_quotaWarnings;
}

void ClaudeProvider::ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds)
{
    ClaudeNotifier::SetPosition(position);

    ClaudeNotifier::Config cfg;
    cfg.enabled = m_notifySettings.enabled;
    cfg.prepareReset = m_notifySettings.prepareReset;
    cfg.exactReset = m_notifySettings.exactReset;
    cfg.prepareMinutes = m_notifySettings.prepareMinutes;
    cfg.currentSession = ToNotifierRule(m_quotaWarnings.currentSession);
    cfg.allModels = ToNotifierRule(m_quotaWarnings.allModels);
    cfg.sonnet = ToNotifierRule(m_quotaWarnings.sonnet);
    cfg.fable = ToNotifierRule(m_quotaWarnings.fable);
    cfg.credits = ToNotifierRule(m_quotaWarnings.credits);

    ClaudeNotifier::SetConfig(cfg);
    ClaudeNotifier::SetQuotaNotificationRepeatSeconds(quotaRepeatSeconds);
}

void ClaudeProvider::HandleRateLimit(const std::string& detail)
{
    if (m_rateLimitCallback) {
        m_rateLimitCallback("Claude", detail);
    }
}

void ClaudeProvider::RefreshThunk()
{
    ClaudeProvider::get_instance()->RefreshAsync();
}

void ClaudeProvider::RefreshAsync()
{
    if (m_loading.exchange(true)) {
        return;
    }

    std::thread([] {
        ClaudeProvider* self = ClaudeProvider::get_instance();

        try {
            Claude::Snapshot snapshot = Claude::FetchSnapshot();

            if (Network::get_instance()->IsRateLimitText(snapshot.statusText)) {
                self->HandleRateLimit(snapshot.statusText);
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = snapshot;
        }
        catch (const std::exception& e) {
            std::string error = std::string("Claude error: ") + e.what();

            if (Network::get_instance()->IsRateLimitText(error)) {
                self->HandleRateLimit(error);
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            self->Snapshot()->statusText = error;
        }

        *self->Loading() = false;
    }).detach();
}

void ClaudeProvider::PollNotifications()
{
    Claude::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_snapshot;
    }

    ClaudeNotifier::Poll(snapshot, &ClaudeProvider::RefreshThunk);
}
