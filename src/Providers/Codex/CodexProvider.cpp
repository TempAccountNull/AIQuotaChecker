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

void CodexProvider::SetAccountSource(int source, const std::string& customAuthPath)
{
    source = AppSettings::ClampCodexAccountSource(source);
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        int previousSource = m_accountSource.load();
        bool sourceChanged = previousSource != source;
        bool relevantPathChanged =
            (source == 3 || previousSource == 3) && m_customAuthPath != customAuthPath;

        m_accountSource = source;
        m_customAuthPath = customAuthPath;
        changed = sourceChanged || relevantPathChanged;

        if (changed) {
            m_sourceGeneration.fetch_add(1);
        }
    }

    if (!changed) {
        return;
    }

    Codex::Snapshot switching;

    switch (source) {
    case 1:
        switching.statusText = "Switching to the active Codex app-server account";
        break;
    case 2:
        switching.statusText = "Switching to CODEX_HOME\\auth.json";
        break;
    case 3:
        switching.statusText = customAuthPath.empty()
            ? "Custom Codex auth.json selected, but no path is configured"
            : "Switching to the selected custom Codex auth.json";
        break;
    default:
        switching.statusText = "Selecting the active Codex account automatically";
        break;
    }

    switching.lastUpdated = "now";

    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = switching;
}

int CodexProvider::AccountSource() const
{
    return AppSettings::ClampCodexAccountSource(m_accountSource.load());
}

std::string CodexProvider::CustomAuthPath() const
{
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    return m_customAuthPath;
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

    int selectedSource = 0;
    std::string customAuthPath;
    std::uint64_t sourceGeneration = 0;

    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        selectedSource = AppSettings::ClampCodexAccountSource(m_accountSource.load());
        customAuthPath = m_customAuthPath;
        sourceGeneration = m_sourceGeneration.load();
    }

    std::thread([selectedSource, customAuthPath, sourceGeneration] {
        CodexProvider* self = CodexProvider::get_instance();
        bool sourceChanged = false;

        try {
            Codex::AccountSource source = Codex::AccountSource::Auto;

            if (selectedSource == 1) {
                source = Codex::AccountSource::ActiveAccount;
            }
            else if (selectedSource == 2) {
                source = Codex::AccountSource::AuthFile;
            }
            else if (selectedSource == 3) {
                source = Codex::AccountSource::CustomAuthFile;
            }

            Codex::Snapshot snapshot = Codex::FetchSnapshot(source, customAuthPath);
            sourceChanged = self->m_sourceGeneration.load() != sourceGeneration;

            if (!sourceChanged) {
                if (Network::get_instance()->IsRateLimitText(snapshot.statusText)) {
                    self->HandleRateLimit(snapshot.statusText);
                }

                std::lock_guard<std::mutex> lock(*self->StateMutex());
                *self->Snapshot() = snapshot;
            }
        }
        catch (const std::exception& e) {
            std::string error = std::string("Codex error: ") + e.what();
            sourceChanged = self->m_sourceGeneration.load() != sourceGeneration;

            if (!sourceChanged) {
                if (Network::get_instance()->IsRateLimitText(error)) {
                    self->HandleRateLimit(error);
                }

                Codex::Snapshot failedSnapshot;
                failedSnapshot.statusText = error;
                failedSnapshot.lastUpdated = "now";
                failedSnapshot.access = UsageTelemetry::FromText(error);

                std::lock_guard<std::mutex> lock(*self->StateMutex());
                *self->Snapshot() = failedSnapshot;
            }
        }

        *self->Loading() = false;

        if (sourceChanged) {
            self->RefreshAsync();
        }
    }).detach();
}

void CodexProvider::RefreshContextAsync()
{
    if (m_contextLoading.exchange(true)) {
        return;
    }

    std::thread([] {
        CodexProvider* self = CodexProvider::get_instance();

        try {
            UsageTelemetry::ContextUsage context = Codex::ReadLocalContextUsage();
            std::lock_guard<std::mutex> lock(*self->StateMutex());

            if (context.valid) {
                self->Snapshot()->context = std::move(context);
            }
            else if (context.compacting) {
                self->Snapshot()->context.compacting = true;
            }
            else {
                self->Snapshot()->context.compacting = false;
            }
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(*self->StateMutex());
            self->Snapshot()->context.compacting = false;
        }

        self->m_contextLoading = false;
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
