#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "../UsageTelemetry.hpp"


namespace Codex {

    enum class AccountSource {
        Auto = 0,
        ActiveAccount = 1,
        AuthFile = 2,
        CustomAuthFile = 3
    };

    struct UsageBar {
        std::string label;
        std::string sublabel;
        std::string rightText;
        float usedPercent = 0.0f;
        std::chrono::system_clock::time_point resetAt{};
        long long resetAtUnixSeconds = 0;
        bool valid = false;
        bool quotaNotificationEligible = true;
        bool blocksProvider = true;
    };

    struct ResetCredit {
        std::string title;
        std::string status;
        std::chrono::system_clock::time_point grantedAt{};
        std::chrono::system_clock::time_point expiresAt{};
    };

    struct ExtraUsage {
        bool valid = false;
        std::string label = "Monthly usage limit";
        std::string spentText;
        std::string limitText;
        std::string remainingText;
        std::string resetText;
        float usedPercent = 0.0f;
        bool hasUsedPercent = false;
        long long resetAtUnixSeconds = 0;
    };

    struct CreditBalance {
        bool valid = false;
        bool hasCredits = false;
        bool unlimited = false;
        std::string balanceText;
    };

    struct Snapshot {
        std::string plan = "Codex";
        std::string statusText = "Waiting for Codex auto refresh";
        std::string lastUpdated = "never";
        std::string usageNotice;

        std::vector<UsageBar> bars;

        int resetCreditsAvailableCount = -1;
        std::vector<ResetCredit> resetCredits;
        std::vector<ResetCredit> resetCreditLedger;
        ExtraUsage extraUsage;
        CreditBalance creditBalance;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
    };

    Snapshot FetchSnapshot(AccountSource source, const std::string& customAuthPath);
    UsageTelemetry::ContextUsage ReadLocalContextUsage();

}
