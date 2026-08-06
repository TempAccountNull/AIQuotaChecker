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

void ClaudeProvider::SetAccountSource(int source)
{
    source = AppSettings::ClampClaudeAccountSource(source);
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        int previous = m_accountSource.load();

        if (previous != source) {
            m_accountSource = source;
            m_sourceGeneration.fetch_add(1);
            changed = true;
        }
    }

    if (changed) {
        // Warning state belongs to the selected account. Never let a warning
        // already sent for one account suppress the same threshold on another.
        ClaudeNotifier::ResetState();

        Claude::Snapshot switching;

        if (source == 1) {
            switching.usageHeading = "Claude Desktop usage";
            switching.statusText = "Switching to Claude Desktop account";
        }
        else if (source == 2) {
            switching.usageHeading = "Claude Code credentials file usage";
            switching.statusText = "Switching to Claude Code .credentials.json";
        }
        else if (source == 3) {
            switching.usageHeading = "Claude Code environment token usage";
            switching.statusText = "Switching to CLAUDE_CODE_OAUTH_TOKEN";
        }
        else {
            switching.usageHeading = "Claude usage";
            switching.statusText = "Selecting Claude Desktop, credentials file, or environment token automatically";
        }

        switching.lastUpdated = "now";

        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = switching;
        m_lastSuccessfulAccountKey.clear();
    }
}

int ClaudeProvider::AccountSource() const
{
    return AppSettings::ClampClaudeAccountSource(m_accountSource.load());
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

    int selectedSource = 0;
    std::uint64_t sourceGeneration = 0;

    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        selectedSource = AppSettings::ClampClaudeAccountSource(m_accountSource.load());
        sourceGeneration = m_sourceGeneration.load();
    }

    std::thread([selectedSource, sourceGeneration] {
        ClaudeProvider* self = ClaudeProvider::get_instance();
        bool sourceChanged = false;

        try {
            Claude::AccountSource source = Claude::AccountSource::Auto;

            if (selectedSource == 1) {
                source = Claude::AccountSource::Desktop;
            }
            else if (selectedSource == 2) {
                source = Claude::AccountSource::CredentialsFile;
            }
            else if (selectedSource == 3) {
                source = Claude::AccountSource::EnvironmentToken;
            }

            Claude::Snapshot snapshot = Claude::FetchSnapshot(source);
            sourceChanged = self->m_sourceGeneration.load() != sourceGeneration;

            if (!sourceChanged) {
                if (Network::get_instance()->IsRateLimitText(snapshot.statusText)) {
                    self->HandleRateLimit(snapshot.statusText);
                }

                bool accountChanged = false;

                {
                    std::lock_guard<std::mutex> lock(*self->StateMutex());
                    accountChanged = !self->m_lastSuccessfulAccountKey.empty() &&
                        !snapshot.accountKey.empty() &&
                        self->m_lastSuccessfulAccountKey != snapshot.accountKey;

                    if (!snapshot.accountKey.empty()) {
                        self->m_lastSuccessfulAccountKey = snapshot.accountKey;
                    }

                    *self->Snapshot() = snapshot;
                }

                if (accountChanged) {
                    ClaudeNotifier::ResetState();
                }
            }
        }
        catch (const std::exception& e) {
            std::string error = std::string("Claude error: ") + e.what();

            sourceChanged = self->m_sourceGeneration.load() != sourceGeneration;

            if (!sourceChanged) {
                if (Network::get_instance()->IsRateLimitText(error)) {
                    self->HandleRateLimit(error);
                }

                Claude::Snapshot failedSnapshot;
                failedSnapshot.statusText = error;
                failedSnapshot.lastUpdated = "now";

                std::lock_guard<std::mutex> lock(*self->StateMutex());
                *self->Snapshot() = failedSnapshot;
            }
        }

        *self->Loading() = false;

        // The user changed account source while this request was in flight.
        // Never commit the old account; immediately query the new selection.
        if (sourceChanged) {
            self->RefreshAsync();
        }
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
