#pragma once

#include <chrono>
#include <string>
#include <vector>


namespace Codex {

    struct UsageBar {
        std::string label;
        std::string rightText;
        float usedPercent = 0.0f;
        std::chrono::system_clock::time_point resetAt{};
        long long resetAtUnixSeconds = 0;
    };

    struct ResetCredit {
        std::string title;
        std::string status;
        std::chrono::system_clock::time_point grantedAt{};
        std::chrono::system_clock::time_point expiresAt{};
    };

    struct ExtraUsage {
        bool valid = true;
        std::string spentText = "$0.00";
        std::string balanceText = "0 credits";
        float usedPercent = 0.0f;
    };

    struct Snapshot {
        std::string plan = "Codex";
        std::string statusText = "Waiting for Codex auto refresh";
        std::string lastUpdated = "never";

        std::vector<UsageBar> bars = {
            { "Session", "Waiting", 0.0f },
            { "Weekly", "Waiting", 0.0f }
        };

        int resetCreditsAvailableCount = -1;
        std::vector<ResetCredit> resetCredits;
        std::vector<ResetCredit> resetCreditLedger;
        ExtraUsage extraUsage;
    };

    Snapshot FetchSnapshot();

}