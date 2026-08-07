#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

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

    struct ContextUsage
    {
        bool valid = false;
        // True only while the newest local session metadata explicitly reports
        // an in-progress context compaction. The quota checker never infers
        // this state from token percentages.
        bool compacting = false;
        long long usedTokens = 0;
        long long contextWindowTokens = 0;
        long long inputTokens = 0;
        long long cachedInputTokens = 0;
        long long outputTokens = 0;
        long long reasoningOutputTokens = 0;
        std::string sourceLabel;
        std::string model;
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
