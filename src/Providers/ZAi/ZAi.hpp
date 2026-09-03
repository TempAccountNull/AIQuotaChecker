#pragma once

#include <string>
#include <vector>

#include "../UsageTelemetry.hpp"

namespace ZAi
{
    struct UsageBar
    {
        std::string label;
        std::string sublabel;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
        bool red = false;
        bool white = false;
        bool green = false;
        bool thin = false;
        bool spendBalance = false;
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

    struct Snapshot
    {
        std::string plan = "Z.Ai";
        std::string statusText = "Waiting for Z.Ai auto refresh";
        std::string lastUpdated = "never";
        std::vector<UsageBar> bars;
        std::vector<DetailRow> details;
        McpUsage mcp;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    LocalTelemetry ReadLocalTelemetry();
    Snapshot FetchSnapshot();
}
