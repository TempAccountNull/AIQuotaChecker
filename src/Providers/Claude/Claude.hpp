#pragma once

#include <string>

namespace Claude {

    struct UsageWindow {
        std::string title;
        std::string subtitle;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
    };

    struct UsageCredits {
        bool valid = false;
        bool enabled = false;
        std::string spentText;
        std::string resetText;
        std::string limitText;
        std::string monthlyLimitText;
        std::string currentBalanceText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
    };

    struct Snapshot {
        std::string plan = "Claude";
        std::string statusText = "Waiting for Claude auto refresh";
        std::string lastUpdated = "never";

        UsageWindow currentSession;
        UsageWindow weeklyAllModels;
        UsageWindow weeklySonnet;
        UsageWindow weeklyFable;

        UsageCredits credits;
    };

    Snapshot FetchSnapshot();

}