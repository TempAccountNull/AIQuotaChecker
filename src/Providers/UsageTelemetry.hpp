#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace UsageTelemetry
{
    enum class AccessState
    {
        Unknown = 0,
        Available,
        RateLimited,
        OutOfUsage,
        Unavailable
    };

    struct AccessStatus
    {
        AccessState state = AccessState::Unknown;
        std::string detail;
    };

    struct ContextBreakdownEntry
    {
        std::string label;
        long long value = 0;
        double percent = 0.0;
    };

    struct ContextUsage
    {
        bool valid = false;
        // True only while provider/local session data explicitly reports a
        // compaction operation. Context percentage alone is never sufficient.
        bool compacting = false;
        long long usedTokens = 0;
        long long contextWindowTokens = 0;
        long long inputTokens = 0;
        long long cachedInputTokens = 0;
        long long outputTokens = 0;
        long long reasoningOutputTokens = 0;

        // Optional provider-native cache telemetry. ZCode exposes both the
        // latest request and cumulative cache hit ratio; other providers can
        // leave these unset without changing the existing context card.
        bool cacheStatsValid = false;
        long long cacheInputTokens = 0;
        long long cacheReadTokens = 0;
        long long cacheWriteTokens = 0;
        bool latestCacheHitPercentValid = false;
        double latestCacheHitPercent = 0.0;
        bool averageCacheHitPercentValid = false;
        double averageCacheHitPercent = 0.0;
        long long totalCacheInputTokens = 0;
        long long totalCacheReadTokens = 0;
        long long totalCacheWriteTokens = 0;

        // Provider-native composition data. ZCode reports character counts for
        // messages/system/tools/etc.; keep the raw value and a normalized
        // percentage so the renderer never needs provider-specific parsing.
        std::vector<ContextBreakdownEntry> breakdown;

        // Provider-reported/calculated passive auto-compact telemetry.
        // Providers leave this unset when no trustworthy threshold is known;
        // the UI must never substitute the full context window as the trigger.
        bool autoCompactPercentValid = false;
        int autoCompactPercentLeft = 0;
        long long autoCompactThresholdTokens = 0;

        // Claude writes an explicit compact_boundary record when compaction
        // finishes. Current Desktop builds do not guarantee a persisted
        // saved-token scalar, so AQC can calculate it from pre/post usage.
        // Pre/post values are kept so AQC can calculate the saved amount
        // itself when Claude does not persist a saved-token field.
        long long compactionPreTokens = 0;
        long long compactionSavedTokens = 0;
        // Provider-side timestamp for the beginning of the live compaction
        // latch. This is separate from the completed-boundary timestamp.
        long long compactionStartedAtUnixSeconds = 0;
        long long compactionCompletedAtUnixSeconds = 0;
        std::string compactionEventId;

        std::string sourceLabel;
        std::string model;
    };

    struct RunUsage
    {
        bool valid = false;
        bool running = false;
        bool thinking = false;
        long long startedAtUnixSeconds = 0;
        // True once Claude has persisted a usage snapshot for the current
        // API message. Before this arrives, a zero token count would be
        // misleading because the Desktop UI may already be streaming tokens.
        bool tokenStatsValid = false;
        // Tokens produced by the API message Claude is currently working on.
        // This mirrors the live "x.xk tokens" value shown in Claude Desktop.
        long long currentTokens = 0;
        // Effective input for the current Claude API message: uncached input
        // plus cache creation and cache reads. Claude often reports raw
        // input_tokens as only 1-3 tokens when nearly all context came from
        // cache, so expose the components separately as well.
        long long inputTokens = 0;
        long long rawInputTokens = 0;
        long long cacheCreationInputTokens = 0;
        long long cacheReadInputTokens = 0;
        // Provider-reported reasoning tokens for the latest model request.
        // Claude currently leaves this at zero; Codex exposes it directly.
        long long reasoningOutputTokens = 0;
        // Total generated/spent tokens across the active user turn. Providers
        // define this from their native cumulative counters when available.
        // This stays separate from currentTokens because one turn can span
        // many tool/API cycles before the final end_turn.
        long long tokens = 0;
        // Claude can alternate thinking -> tool/text -> thinking within one
        // user turn. Preserve the latest completed thought duration so the UI
        // can say THOUGHT FOR <time> and flip back on the next thinking block.
        long long thinkingStartedAtUnixSeconds = 0;
        long long lastThoughtDurationSeconds = 0;
        long long lastThoughtCompletedAtUnixSeconds = 0;
    };

    inline std::string LowerCopy(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    }

    inline bool ContainsAny(const std::string& lower, std::initializer_list<const char*> values)
    {
        for (const char* value : values) {
            if (value && lower.find(value) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    inline AccessState ClassifyText(const std::string& text)
    {
        const std::string lower = LowerCopy(text);

        if (ContainsAny(lower, {
            "out of usage", "out of extra usage", "out of usage credits",
            "usage limit reached", "monthly usage limit reached",
            "spend control reached", "insufficient balance",
            "no resource package", "usage exhausted", "quota exhausted", "quota exceeded",
            "credit balance exhausted", "usage allocation has been disabled",
            "usage limit is set to $0", "you've hit your", "you've reached your"
        })) {
            return AccessState::OutOfUsage;
        }

        if (ContainsAny(lower, {
            "http 429", "status 429", "too many requests", "rate limited",
            "rate-limited", "ratelimited", "rate limit reached",
            "rate_limit_reached", "rate limit exceeded"
        })) {
            return AccessState::RateLimited;
        }

        if (ContainsAny(lower, {
            "not logged in", "credentials not found", "usage unavailable",
            "usage failed", "token expired", "authentication failed",
            "unauthorized", "forbidden", "http 401", "http 403",
            "could not be read", "not found"
        })) {
            return AccessState::Unavailable;
        }

        return AccessState::Unknown;
    }

    inline AccessStatus FromHttpFailure(int statusCode, const std::string& body, const std::string& detail)
    {
        AccessStatus status;
        status.detail = detail;

        if (statusCode == 429) {
            status.state = AccessState::RateLimited;
            return status;
        }

        if (statusCode == 402) {
            status.state = AccessState::OutOfUsage;
            return status;
        }

        status.state = ClassifyText(body + " " + detail);

        if (status.state == AccessState::Unknown) {
            status.state = AccessState::Unavailable;
        }

        return status;
    }

    inline AccessStatus FromText(const std::string& detail)
    {
        AccessStatus status;
        status.detail = detail;
        status.state = ClassifyText(detail);

        if (status.state == AccessState::Unknown && !detail.empty()) {
            status.state = AccessState::Unavailable;
        }

        return status;
    }

    inline void SetAvailable(AccessStatus& status, const std::string& detail = {})
    {
        status.state = AccessState::Available;
        status.detail = detail;
    }

    inline bool IsExhausted(float usedPercent)
    {
        return usedPercent >= 99.999f;
    }
}
