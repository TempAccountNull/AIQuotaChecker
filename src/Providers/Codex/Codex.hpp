#pragma once

#include <chrono>
#include <string>
#include <utility>
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
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
        // A main plan window: notified about, and exhausting it blocks Codex.
        bool quotaNotificationEligible = true;
    };

    struct ResetCredit {
        std::string title;
        std::string status;
        std::string resetType;
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
        // False for a cap the workspace only warns about: Desktop blocks only
        // on enforcement_mode HARD_CAP.
        bool enforced = true;
        long long resetAtUnixSeconds = 0;
    };

    struct CreditBalance {
        bool valid = false;
        bool hasCredits = false;
        bool unlimited = false;
        std::string balanceText;
        // What the balance buys, as the server estimates it, e.g.
        // "~9,531–49,563 local · ~1,525–9,531 cloud messages". Empty when not reported.
        std::string approxMessagesText;
    };

    // GET /wham/profiles/me, as Codex Desktop's profile page shows it.
    // Counts are -1 when not reported.
    struct ProfileStats {
        bool valid = false;
        std::string displayName;
        std::string username;
        long long lifetimeTokens = -1;
        long long peakDailyTokens = -1;
        long long todayTokens = -1;
        long long longestTaskSeconds = -1;
        int currentStreakDays = -1;
        int longestStreakDays = -1;
        long long totalThreads = -1;
        std::string reasoningEffort;          // most used, e.g. "high"
        double reasoningEffortPercent = -1.0;
        double fastModePercent = -1.0;
        long long skillsUsed = -1;            // skill invocations
        long long uniqueSkills = -1;
        int workspaceRank = -1;               // of workspaceUsers
        int workspaceUsers = -1;
        // Weekly totals (week start YYYY-MM-DD -> tokens), oldest first.
        std::vector<std::pair<std::string, long long>> weeklyTokens;
        // Most used plugins and skills (name -> uses), most used first.
        std::vector<std::pair<std::string, long long>> topInvocations;
        std::string statsAsOf;                // YYYY-MM-DD
        bool statsError = false;              // the server could not compute the stats
    };

    // GET /subscriptions/auto_top_up/settings: credit auto-reload.
    struct AutoTopUp {
        bool valid = false;
        bool enabled = false;
        std::string thresholdText;    // reload below this balance, e.g. "125 credits"
        std::string targetText;       // back up to this
        std::string monthlyLimitText; // empty = no monthly cap
    };

    // GET /wham/usage/plan_limit_history?days=7: one past plan window.
    struct PlanLimitPeriod {
        int windowMinutes = 0;        // 300 (5-hour) or 10080 (weekly)
        long long startsAtUnixSeconds = 0;
        long long endsAtUnixSeconds = 0;
        float usedPercent = -1.0f;    // -1 = not accounted yet
    };

    // GET /wham/usage/credit-usage-events: one day's credit spend on one surface.
    struct CreditEvent {
        std::string date;             // YYYY-MM-DD
        std::string surface;          // Desktop's label, e.g. "Desktop App"
        double credits = 0.0;
    };

    // GET /wham/usage/daily-token-usage-breakdown: a surface's share of the last 7 days.
    struct SurfaceShare {
        std::string surface;          // Desktop's label, e.g. "CLI"
        double percent = 0.0;
    };

    // GET /wham/analytics/daily-workspace-usage-counts: one day of the
    // signed-in user's usage.
    struct DailyUsage {
        std::string date;             // YYYY-MM-DD (UTC)
        double credits = 0.0;
        long long tokens = 0;
        long long turns = 0;
        long long threads = 0;
    };

    // GET /subscriptions?account_id=: the plan's billing state.
    struct Subscription {
        bool valid = false;
        bool renews = false;
        long long atUnixSeconds = 0;  // the renewal, or the end
        std::string text;             // "Renews Oct 5", "Ends Oct 5" or "Ended Oct 5"
        std::string billingPeriod;    // e.g. "monthly"
    };

    struct Snapshot {
        std::string plan = "Codex";
        std::string statusText = "Waiting for Codex auto refresh";
        std::string lastUpdated = "never";
        std::string usageNotice;
        // What Codex is serving requests from: included, credits, reserve or
        // unavailable. Empty when the server does not say.
        std::string usageSource;
        // wham/usage promo.campaign_id, e.g. "plus-1-month-free" (a trial).
        std::string promoCampaign;

        std::vector<UsageBar> bars;

        int resetCreditsAvailableCount = -1;
        int resetCreditsEarnedCount = -1;
        std::vector<ResetCredit> resetCredits;
        std::vector<ResetCredit> resetCreditLedger;
        // Last 30 days of banked-reset activity: title "Reset received" or
        // "Reset used", grantedAt = when.
        std::vector<ResetCredit> resetHistory;
        ExtraUsage extraUsage;
        CreditBalance creditBalance;
        ProfileStats profile;
        // Team workspace credit pool (owners only), e.g. "1,200 workspace credits".
        std::string workspaceCredits;
        AutoTopUp autoTopUp;
        Subscription subscription;
        std::vector<PlanLimitPeriod> planLimitHistory; // oldest first
        std::vector<CreditEvent> creditEvents;         // newest first, at most 10
        std::vector<SurfaceShare> surfaceShare;        // largest first
        std::vector<DailyUsage> dailyUsage;            // last 30 days, oldest first
        // Accounts signed in to Codex Desktop (accountSession/list); 0 = unknown.
        int signedInAccounts = 0;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    struct LocalTelemetry {
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    Snapshot FetchSnapshot(AccountSource source, const std::string& customAuthPath);
    LocalTelemetry ReadLocalTelemetry();

}
