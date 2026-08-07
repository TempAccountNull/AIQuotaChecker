#pragma once

#include <string>
#include <vector>

#include "../UsageTelemetry.hpp"

namespace Grok
{
    struct UsageWindow
    {
        std::string title;
        std::string subtitle;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
    };

    struct ExtraCredits
    {
        bool valid = false;
        std::string balanceText = "$0.00";
        std::string usedText = "$0.00";
        float usedPercent = 0.0f;
    };

    struct ProductUsage
    {
        std::string product;
        float usagePercent = 0.0f;
    };

    struct Snapshot
    {
        std::string plan = "Grok";
        std::string statusText = "Waiting for Grok auto refresh";
        std::string lastUpdated = "never";

        UsageWindow weeklyLimit;
        ExtraCredits extraCredits;
        std::vector<ProductUsage> products;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
    };

    Snapshot FetchSnapshot();
}
