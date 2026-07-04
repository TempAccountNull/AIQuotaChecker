#include "Global.hpp"
#include "CodexProvider.hpp"

#include "CodexNotifier.hpp"
#include "Network.hpp"

#include <exception>
#include <string>
#include <thread>

namespace
{

    static CodexNotifier::QuotaWarningRule ToNotifierRule(const AppSettings::QuotaWarningRule& src)
    {
        CodexNotifier::QuotaWarningRule rule;
        rule.enabled = src.enabled;
        rule.percent = src.percent;
        return rule;
    }
}

CodexProvider* CodexProvider::get_instance()
{
    static CodexProvider provider;
    return &provider;
}

std::mutex* CodexProvider::StateMutex()
{
    return &m_mutex;
}

Codex::Snapshot* CodexProvider::Snapshot()
{
    return &m_snapshot;
}

std::atomic_bool* CodexProvider::Loading()
{
    return &m_loading;
}

AppSettings::ProviderNotifications* CodexProvider::NotifySettings()
{
    return &m_notifySettings;
}

AppSettings::CodexQuotaWarnings* CodexProvider::QuotaWarnings()
{
    return &m_quotaWarnings;
}

void CodexProvider::SetRateLimitCallback(RateLimitCallback callback)
{
    m_rateLimitCallback = callback;
}

void CodexProvider::LoadSettings(const AppSettings::Settings& settings)
{
    m_notifySettings = settings.codex;
    m_quotaWarnings = settings.codexQuotaWarnings;
}

void CodexProvider::SaveSettings(AppSettings::Settings& settings) const
{
    settings.codex = m_notifySettings;
    settings.codexQuotaWarnings = m_quotaWarnings;
}

void CodexProvider::ApplyRuntime(NotifyPosition position, int quotaRepeatSeconds)
{
    CodexNotifier::SetPosition(position);

    CodexNotifier::Config cfg;
    cfg.enabled = m_notifySettings.enabled;
    cfg.resetCredits = m_notifySettings.resetCredits;
    cfg.prepareReset = m_notifySettings.prepareReset;
    cfg.exactReset = m_notifySettings.exactReset;
    cfg.prepareMinutes = m_notifySettings.prepareMinutes;
    cfg.fiveHour = ToNotifierRule(m_quotaWarnings.fiveHour);
    cfg.weekly = ToNotifierRule(m_quotaWarnings.weekly);

    CodexNotifier::SetConfig(cfg);
    CodexNotifier::SetQuotaNotificationRepeatSeconds(quotaRepeatSeconds);
}

void CodexProvider::HandleRateLimit(const std::string& detail)
{
    if (m_rateLimitCallback) {
        m_rateLimitCallback("Codex", detail);
    }
}

void CodexProvider::RefreshThunk()
{
    CodexProvider::get_instance()->RefreshAsync();
}

void CodexProvider::RefreshAsync()
{
    if (m_loading.exchange(true)) {
        return;
    }

    std::thread([] {
        CodexProvider* self = CodexProvider::get_instance();

        try {
            Codex::Snapshot snapshot = Codex::FetchSnapshot();

            if (Network::get_instance()->IsRateLimitText(snapshot.statusText)) {
                self->HandleRateLimit(snapshot.statusText);
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = snapshot;
        }
        catch (const std::exception& e) {
            std::string error = std::string("Codex error: ") + e.what();

            if (Network::get_instance()->IsRateLimitText(error)) {
                self->HandleRateLimit(error);
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            self->Snapshot()->statusText = error;
        }

        *self->Loading() = false;
    }).detach();
}

void CodexProvider::PollNotifications()
{
    Codex::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_snapshot;
    }

    CodexNotifier::Poll(snapshot, &CodexProvider::RefreshThunk);
}
