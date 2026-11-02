#pragma once

#include <string>
#include <vector>

#include "../UsageTelemetry.hpp"

namespace ZAi
{
    struct UsageBar
    {
        std::string identity; // Stable bucket/window key; distinct from the display label.
        std::string label;
        std::string sublabel;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
        bool spendBalance = false;
        bool sharedRateLimit = false; // Coding windows that all gate the same requests.
    };

    struct DetailRow
    {
        std::string leftValue;
        std::string leftLabel;
        std::string rightValue;
        std::string rightLabel;
    };

    struct LocalTelemetry
    {
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    // Official MCP quota, from /api/v1/mcp/usage:
    //   { code, msg, data: { server_time, next_refresh_at, level,
    //                        total_usage: { used, limit, remaining } } }
    struct McpUsage
    {
        bool valid = false;
        long long used = 0;
        long long limit = 0;
        long long remaining = 0;
        std::string level;
        long long nextRefreshAtUnixSeconds = 0;
    };

    // Coding Plan usage history (credit-usage/activity and usage-detail
    // MODEL / MCP), refreshed at most every 10 minutes. -1 = not reported.
    struct UsageHistory
    {
        bool valid = false;
        long long todayTokens = -1;
        long long todayModelCalls = -1;
        long long todayToolCalls = -1;
        long long totalTokens = -1;      // 365-day window
        long long peakDailyTokens = -1;
        std::string peakDate;
        long long totalUsageSeconds = -1; // 365-day window
        int currentStreakDays = -1;
        int longestStreakDays = -1;
        std::vector<std::pair<std::string, long long>> models; // name -> tokens, last 7 days
        std::vector<std::pair<std::string, long long>> tools;  // name -> calls, last 7 days
        // Latest daily decode speed (model-performance-day), tokens/s; -1 = not reported.
        double proDecodeTps = -1.0;  // Pro / Max tier
        double liteDecodeTps = -1.0; // Lite tier
    };

    // An unexpired banked quota reset from /api/v1/coding-plan/reset/status.
    struct QuotaReset
    {
        std::string type; // "5-hour" or "Weekly"
        long long expiresAtUnixSeconds = 0;
    };

    struct Snapshot
    {
        std::string plan = "Z.Ai";
        std::string statusText = "Waiting for Z.Ai auto refresh";
        std::string lastUpdated = "never";
        std::vector<UsageBar> bars;
        std::vector<DetailRow> details;
        McpUsage mcp;
        // Soonest expiry first; resetsReported distinguishes "none banked"
        // from "not asked".
        bool resetsReported = false;
        std::vector<QuotaReset> resets;
        long long lastFiveHourResetUnixSeconds = 0;
        long long lastWeeklyResetUnixSeconds = 0;
        UsageHistory history;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    LocalTelemetry ReadLocalTelemetry();
    Snapshot FetchSnapshot();
}
