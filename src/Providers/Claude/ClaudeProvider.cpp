#include "Global.hpp"
#include "ClaudeProvider.hpp"

#include "ClaudeNotifier.hpp"
#include "Network.hpp"

#include <exception>
#include <ctime>
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
        m_compactionLatched = false;
        m_compactionStartedAtUnixSeconds = 0;
        m_lastActiveRunSeenAtUnixSeconds = 0;
        m_compactionNoticeAtUnixSeconds = 0;
        m_compactionNoticeEligible = false;
        m_lastCompactionSavedTokens = 0;
        m_lastCompactionEventId.clear();
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

void ClaudeProvider::ApplyLocalTelemetryLocked(
    Claude::LocalTelemetry local,
    Claude::Snapshot& target
)
{
    const long long now = static_cast<long long>(std::time(nullptr));
    UsageTelemetry::ContextUsage& incoming = local.context;

    const bool hasBoundary = !incoming.compactionEventId.empty();
    const bool newBoundary = hasBoundary &&
        incoming.compactionEventId != m_lastCompactionEventId;

    // AQC's parser already knows when a foreground user turn is still alive.
    // Remember that fact independently of the most recent content block: when
    // Claude enters auto-compaction it can stop touching the JSONL for several
    // minutes, which otherwise makes the status fall back to AVAILABLE.
    if (local.run.valid && local.run.running) {
        m_lastActiveRunSeenAtUnixSeconds = now;
    }

    // Never synthesize COMPACTING from context percentage alone. Claude can
    // expose 200K and 1M variants of the same model, and a stale/guessed limit
    // must not latch a false compact state. Only explicit transcript/provider
    // compact markers may enter the latch below.

    if (incoming.compacting) {
        if (!m_compactionLatched) {
            // This timer is the compaction timer, not the whole user-turn timer.
            m_compactionStartedAtUnixSeconds = now;
        }
        m_compactionLatched = true;
    }

    // A boundary or a fresh low-context reading normally clears the latch. If
    // neither ever arrives (aborted/failed compaction), do not leave the app in
    // COMPACTING forever. Ten minutes is comfortably longer than the multi-
    // minute compactions observed in the captured Desktop transcript.
    if (m_compactionLatched && !incoming.compacting && !newBoundary &&
        m_compactionStartedAtUnixSeconds > 0 &&
        now - m_compactionStartedAtUnixSeconds > 10 * 60) {
        m_compactionLatched = false;
        m_compactionStartedAtUnixSeconds = 0;
    }

    if (newBoundary) {
        m_lastCompactionEventId = incoming.compactionEventId;
        m_compactionLatched = false;
        m_compactionStartedAtUnixSeconds = 0;
        m_lastCompactionSavedTokens = incoming.compactionSavedTokens;
        m_compactionNoticeAtUnixSeconds = 0;

        // compact_boundary can be flushed several minutes after Claude Desktop
        // already finished compacting. Give a delayed-but-current boundary its
        // full UI notice when AQC first observes it, but do not resurrect an
        // ancient compaction just because the app was started later.
        const long long boundaryAt = incoming.compactionCompletedAtUnixSeconds;
        m_compactionNoticeEligible = boundaryAt <= 0 ||
            (now >= boundaryAt && now - boundaryAt <= 15 * 60);
        if (m_compactionNoticeEligible && incoming.compactionSavedTokens > 0) {
            m_compactionNoticeAtUnixSeconds = now;
        }
    }

    if (hasBoundary && incoming.compactionEventId == m_lastCompactionEventId &&
        incoming.compactionSavedTokens > 0) {
        const bool learnedSavedAmount = m_lastCompactionSavedTokens <= 0;
        m_lastCompactionSavedTokens = incoming.compactionSavedTokens;
        if (learnedSavedAmount && m_compactionNoticeEligible) {
            // Often the first post-boundary usage record is what lets us
            // calculate pre - post. Give the user the full notice duration
            // from the moment that exact saved amount becomes available.
            m_compactionNoticeAtUnixSeconds = now;
        }
    }

    // A new low-context value is independent confirmation that compaction is
    // no longer active even if an intermediate start marker was inferred.
    if (incoming.valid && !incoming.compacting &&
        incoming.autoCompactPercentValid && incoming.autoCompactPercentLeft > 5) {
        m_compactionLatched = false;
        m_compactionStartedAtUnixSeconds = 0;
    }

    // Preserve the last exact bar across the tiny boundary -> first-post-usage
    // gap. Never put a pre-boundary token count back into a newly valid local
    // context, though.
    if (incoming.valid) {
        target.context = incoming;
    }
    else if (target.context.valid) {
        target.context.compactionPreTokens = incoming.compactionPreTokens;
        if (incoming.compactionSavedTokens > 0) {
            target.context.compactionSavedTokens = incoming.compactionSavedTokens;
        }
        if (!incoming.compactionEventId.empty()) {
            target.context.compactionEventId = incoming.compactionEventId;
        }
    }
    else {
        target.context = incoming;
    }

    target.context.compacting = m_compactionLatched;
    target.context.compactionStartedAtUnixSeconds = m_compactionLatched
        ? m_compactionStartedAtUnixSeconds
        : 0;

    if (!m_lastCompactionEventId.empty()) {
        target.context.compactionEventId = m_lastCompactionEventId;
    }

    if (m_lastCompactionSavedTokens > 0 &&
        m_compactionNoticeAtUnixSeconds > 0 &&
        now >= m_compactionNoticeAtUnixSeconds &&
        now - m_compactionNoticeAtUnixSeconds <= 15) {
        target.context.compactionSavedTokens = m_lastCompactionSavedTokens;
        target.context.compactionCompletedAtUnixSeconds =
            m_compactionNoticeAtUnixSeconds;
    }
    else if (target.context.compactionEventId == m_lastCompactionEventId) {
        // Prevent an old transcript timestamp from resurrecting the notice on
        // every 2-second refresh after the 15-second display window expires.
        target.context.compactionCompletedAtUnixSeconds = 0;
    }

    // During compaction Claude Desktop can stop flushing the transcript for
    // quite a while. If the compaction signal arrived without a usable run,
    // synthesize only the timer carrier; the visible state is COMPACTING, not
    // THINKING. Once real run telemetry resumes it replaces this immediately.
    if (m_compactionLatched && !(local.run.valid && local.run.running) &&
        !(target.run.valid && target.run.running)) {
        local.run.valid = true;
        local.run.running = true;
        local.run.thinking = false;
        local.run.startedAtUnixSeconds = m_compactionStartedAtUnixSeconds > 0
            ? m_compactionStartedAtUnixSeconds
            : now;
    }

    // A real parsed active run is authoritative. In particular, when a
    // tool_result starts a fresh model cycle tokenStatsValid=false is
    // intentional. Never copy the prior API message's counters back into a
    // real run: that was the source of the frozen/stale Current Tokens value.
    if (local.run.valid && local.run.running) {
        target.run = std::move(local.run);
    }
    else if (m_compactionLatched && target.run.valid && target.run.running) {
        target.run.thinking = false;
        if (target.run.startedAtUnixSeconds <= 0) {
            target.run.startedAtUnixSeconds = m_compactionStartedAtUnixSeconds;
        }
    }
    else {
        target.run = std::move(local.run);
    }
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

                    Claude::LocalTelemetry local;
                    local.context = std::move(snapshot.context);
                    local.run = std::move(snapshot.run);

                    // Seed the local-only telemetry with the previous exact
                    // values. ApplyLocalTelemetryLocked will replace them when
                    // the newest transcript data is valid and preserve them
                    // only across the short boundary/post-usage gap.
                    if (accountChanged) {
                        snapshot.context = {};
                        snapshot.run = {};
                        self->m_compactionLatched = false;
                        self->m_compactionStartedAtUnixSeconds = 0;
                        self->m_lastActiveRunSeenAtUnixSeconds = 0;
                        self->m_compactionNoticeAtUnixSeconds = 0;
                        self->m_compactionNoticeEligible = false;
                        self->m_lastCompactionSavedTokens = 0;
                        self->m_lastCompactionEventId.clear();
                    }
                    else {
                        snapshot.context = self->m_snapshot.context;
                        snapshot.run = self->m_snapshot.run;
                    }

                    const bool sameAccount = !snapshot.accountKey.empty() &&
                        snapshot.accountKey == self->m_lastSuccessfulAccountKey;

                    // Claude Desktop itself keeps the last full spend object when
                    // a lightweight usage response omits extra_usage. Preserve it
                    // only for the same account; an explicit reported disabled
                    // state always replaces the old data.
                    if (sameAccount && !snapshot.credits.reported && self->m_snapshot.credits.reported) {
                        snapshot.credits = self->m_snapshot.credits;
                    }

                    self->ApplyLocalTelemetryLocked(std::move(local), snapshot);

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
                failedSnapshot.access = UsageTelemetry::FromText(error);

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

void ClaudeProvider::RefreshContextAsync()
{
    if (m_contextLoading.exchange(true)) {
        return;
    }

    std::thread([] {
        ClaudeProvider* self = ClaudeProvider::get_instance();

        try {
            Claude::LocalTelemetry local = Claude::ReadLocalTelemetry();
            std::lock_guard<std::mutex> lock(*self->StateMutex());
            self->ApplyLocalTelemetryLocked(std::move(local), *self->Snapshot());
        }
        catch (...) {
            // Local telemetry is best-effort and read-only. A transient share/
            // parse failure must not make COMPACTING/THINKING flash back to
            // AVAILABLE between successful reads.
        }

        self->m_contextLoading = false;
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
