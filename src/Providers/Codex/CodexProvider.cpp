#include "Global.hpp"
#include "CodexProvider.hpp"

#include "Network.hpp"
#include "Text.hpp"

#include <exception>
#include <string>
#include <thread>

namespace
{
    constexpr std::uint32_t kNotifyCyan = NOTIFY_COL32(0, 220, 255, 255);

    // A bar's toast wording, warning rule and watch identity.
    QuotaNotifier::Point CodexPoint(const Codex::UsageBar& bar, const AppSettings::CodexQuotaWarnings& rules)
    {
        QuotaNotifier::Point point;
        point.usedPercent = bar.usedPercent;
        point.resetAtUnixSeconds = bar.resetAtUnixSeconds;
        point.rule = rules.weekly;

        const std::string& label = bar.label;
        const auto has = [&](const char* word) { return label.find(word) != std::string::npos; };

        if (has("Session") || has("session") || has("5-hour") || has("5h")) {
            point.name = "5-hour limit/session";
            point.rule = rules.fiveHour;
        }
        // Free plans get a 30-day main window; it is not the weekly quota.
        else if (has("Monthly")) {
            point.name = "monthly limit";
        }
        else if (has("Daily")) {
            point.name = "daily limit";
        }
        else if (label == "Weekly" && bar.sublabel.empty()) {
            point.name = "weekly - all models";
        }
        else {
            point.name = bar.sublabel.empty() ? label : label + " (" + bar.sublabel + ")";
        }

        // Every bar is its own window. Bars that shared one key flipped the
        // watch between their reset times on each poll and toasted every frame.
        point.key = Text::get_instance()->CompactLower(label + "|" + bar.sublabel, false);
        return point;
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
    m_notifier.Configure(position, m_notifySettings, quotaRepeatSeconds);
}

void CodexProvider::HandleRateLimit(const std::string& detail)
{
    if (m_rateLimitCallback) {
        m_rateLimitCallback("Codex", detail);
    }
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

        // Re-read after clearing Loading: a switch that landed in between saw
        // a refresh in flight, skipped its own, and would otherwise leave the
        // old account on screen until the next auto refresh.
        if (sourceChanged || self->m_sourceGeneration.load() != sourceGeneration) {
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
            Codex::LocalTelemetry local = Codex::ReadLocalTelemetry();
            std::lock_guard<std::mutex> lock(*self->StateMutex());

            if (local.context.valid) {
                self->Snapshot()->context = std::move(local.context);
            }
            else if (local.context.compacting) {
                self->Snapshot()->context.compacting = true;
                self->Snapshot()->context.compactionStartedAtUnixSeconds =
                    local.context.compactionStartedAtUnixSeconds;
            }
            else {
                self->Snapshot()->context.compacting = false;
            }

            // Run telemetry is local-only and updates once per second. Keep the
            // latest parsed state so THINKING/COMPACTING and token spend can be
            // shown without waiting for the much slower quota refresh.
            self->Snapshot()->run = std::move(local.run);
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(*self->StateMutex());
            self->Snapshot()->context.compacting = false;
            self->Snapshot()->run = {};
        }

        self->m_contextLoading = false;
    }).detach();
}

void CodexProvider::PollNotifications()
{
    std::vector<QuotaNotifier::Point> points;
    size_t resetCredits = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (const Codex::UsageBar& bar : m_snapshot.bars) {
            if (bar.valid && bar.quotaNotificationEligible) {
                points.push_back(CodexPoint(bar, m_quotaWarnings));
            }
        }

        resetCredits = m_snapshot.resetCreditsAvailableCount >= 0
            ? static_cast<size_t>(m_snapshot.resetCreditsAvailableCount)
            : m_snapshot.resetCredits.size();
    }

    if (m_notifier.Poll(points)) {
        RefreshAsync();
    }

    // Once per new count, so it reminds again when another reset is earned.
    if (resetCredits == 0) {
        m_resetCreditsNoticed = 0;
    }
    else if (m_notifySettings.resetCredits && resetCredits != m_resetCreditsNoticed) {
        m_notifier.Notify("Codex reset credits: You have " + std::to_string(resetCredits) +
            (resetCredits == 1
                ? " reset available to use in Codex. Make sure to use it!"
                : " resets available to use in Codex. Make sure to use them!"),
            kNotifyCyan, 8.0f);
        m_resetCreditsNoticed = resetCredits;
    }
}
