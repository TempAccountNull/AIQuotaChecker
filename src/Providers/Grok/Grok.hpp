#pragma once

#include <string>
#include <vector>

#include "../UsageTelemetry.hpp"

namespace Grok
{
    struct UsageWindow
    {
        std::string title;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
    };

    // Auto top-up of prepaid credits. Read only while the account holds
    // credits, as the CLI does; invalid when unknown.
    struct AutoTopUp
    {
        bool valid = false;
        bool enabled = false;
        std::string amountText;
        std::string monthlyLimitText; // Empty: no monthly maximum.
    };

    // Money beyond the plan allowance: prepaid credits and the legacy
    // pay-as-you-go cap. Empty texts are the parts the account doesn't have.
    struct ExtraCredits
    {
        bool valid = false;
        std::string balanceText;
        std::string usedText;
        std::string capText;
        AutoTopUp autoTopUp;
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
        // Past billing periods, newest first, as display lines.
        std::vector<std::string> history;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
    };

    Snapshot FetchSnapshot();
}
