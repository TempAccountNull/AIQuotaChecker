#pragma once

#include <string>
#include <vector>

#include "../UsageTelemetry.hpp"

namespace Claude {

    enum class AccountSource {
        Auto = 0,
        Desktop = 1,
        CredentialsFile = 2,
        EnvironmentToken = 3
    };

    struct UsageWindow {
        std::string title;
        std::string subtitle;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
    };

    struct UsageCredits {
        bool reported = false;
        bool valid = false;
        std::string label = "Usage credits";
        bool enabled = false;
        bool hasSpentAmount = false;
        bool hasMonthlyLimit = false;
        std::string spentText;
        std::string resetText;
        std::string limitText;
        std::string monthlyLimitText;
        std::string currentBalanceText;
        float usedPercent = 0.0f;
        bool hasUsedPercent = false;
        long long resetAtUnixSeconds = 0;
    };

    struct Snapshot {
        // Internal identity key used only to reset notification state when the
        // selected Claude account changes. It is never rendered or persisted.
        std::string accountKey;
        std::string usageHeading = "Claude usage";
        std::string plan = "Claude";
        std::string statusText = "Waiting for Claude auto refresh";
        std::string lastUpdated = "never";

        UsageWindow currentSession;
        UsageWindow weeklyAllModels;
        UsageWindow weeklySonnet;
        UsageWindow weeklyFable;
        std::vector<UsageWindow> additionalLimits;

        UsageCredits credits;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
    };

    Snapshot FetchSnapshot(AccountSource source);
    UsageTelemetry::ContextUsage ReadLocalContextUsage();

}
