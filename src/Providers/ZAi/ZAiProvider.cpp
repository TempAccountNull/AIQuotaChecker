#include "Global.hpp"
#include "ZAiProvider.hpp"

#include "Network.hpp"
#include "Text.hpp"

#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace
{
    // GLM-5-Turbo has its own rule; every Coding Plan window (GLM-5.3,
    // GLM-5.3-Flash, the shared 5-hour and weekly windows) uses the main one.
    std::vector<QuotaNotifier::Point> ZAiPoints(const ZAi::Snapshot& snapshot, const AppSettings::ZAiQuotaWarnings& rules)
    {
        std::vector<QuotaNotifier::Point> points;

        for (const ZAi::UsageBar& bar : snapshot.bars) {
            if (!bar.valid) {
                continue;
            }

            QuotaNotifier::Point point;
            point.key = bar.identity.empty() ? bar.label : bar.identity;
            point.name = bar.label;
            point.leftNoun = "balance";
            point.usedPercent = bar.usedPercent;
            point.resetAtUnixSeconds = bar.resetAtUnixSeconds;
            point.rule = Text::get_instance()->CompactLower(bar.label).find("turbo") != std::string::npos
                ? rules.turbo
                : rules.glm52;
            points.push_back(std::move(point));
        }

        return points;
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
    m_notifier.Configure(position, m_notifySettings, quotaRepeatSeconds);
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
            try {
                ZAi::LocalTelemetry local = ZAi::ReadLocalTelemetry();
                failedSnapshot.context = std::move(local.context);
                failedSnapshot.run = std::move(local.run);
            }
            catch (...) {
            }

            std::lock_guard<std::mutex> lock(*self->StateMutex());
            *self->Snapshot() = failedSnapshot;
        }

        *self->Loading() = false;
    }).detach();
}

void ZAiProvider::RefreshContextAsync()
{
    if (m_contextLoading.exchange(true)) {
        return;
    }

    std::thread([] {
        ZAiProvider* self = ZAiProvider::get_instance();
        try {
            ZAi::LocalTelemetry local = ZAi::ReadLocalTelemetry();
            std::lock_guard<std::mutex> lock(*self->StateMutex());

            // Keep the last valid context meter across a momentary SQLite/WAL
            // read gap, just like the Codex/Claude local telemetry paths. A
            // definite non-compacting read still clears the transient state.
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

            self->Snapshot()->run = std::move(local.run);
        }
        catch (...) {
            // Passive local telemetry must never disturb the quota snapshot or
            // make a valid context meter flicker because the live DB was busy.
        }
        self->m_contextLoading = false;
    }).detach();
}

void ZAiProvider::PollNotifications()
{
    std::vector<QuotaNotifier::Point> points;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        points = ZAiPoints(m_snapshot, m_quotaWarnings);
    }

    if (m_notifier.Poll(points)) {
        RefreshAsync();
    }
}
