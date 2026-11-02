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
        std::string disabledReason; // server code when credits are off; empty when none given
    };

    // <userData>\ccd-limit-auto-resume.json: Desktop sessions parked until the
    // 5-hour limit resets (written only while auto-resume is on).
    struct AutoResume {
        bool valid = false; // at least one session is waiting
        long long resetAtUnixSeconds = 0; // soonest future resetsAt
        int waitingSessions = 0;
        bool optedIn = false;
    };

    // 3P builds' usage ledger, last 30 days:
    // local-agent-mode-sessions\<acct>\<org>\usage-ledger\YYYY-MM-DD[.N].ndjson
    struct UsageLedger {
        bool valid = false;
        int windowDays = 30;
        long long turns = 0;
        long long errorTurns = 0;
        long long inputTokens = 0;
        long long outputTokens = 0;
        long long cacheReadTokens = 0;
        long long cacheWriteTokens = 0;
        bool costValid = false;
        double costUsd = 0.0;
        // surface (chat / cowork / code) -> input + output tokens
        std::vector<std::pair<std::string, long long>> surfaceTokens;
    };

    // Free limit resets on the account (usage `cedar_ember`, sent when the
    // request carries ?cedar_ember=1). Read-only: claiming one is a POST that
    // is never sent from here.
    struct LimitReset {
        std::string id;
        std::string title;   // "Weekly reset", "Session reset", else the server label
        std::string type;    // server type: weekly / session / both / ...
        int left = 0;        // resets left, at least 1 while it can be used
        int total = 0;
        long long endsAtUnixSeconds = 0; // expiry
        bool usableNow = false;
        bool paused = false;
    };

    struct LimitResets {
        bool valid = false; // the server sent the block
        bool eligible = false;
        bool atLimit = false;
        long long cooldownUntilUnixSeconds = 0;
        std::vector<LimitReset> grants; // unexpired, soonest expiry first
    };

    // One-time grants that never reset, e.g. the cloud session credit
    // (`iguana_necktie`: "$250 of $250 left, Expires 1:59 AM CST, November 5").
    struct PromoCredit {
        std::string key;   // usage field name; stable id for pinning
        std::string title; // "Cloud session credits"
        std::string label; // row label, "Included credit"
        float usedPercent = 0.0f;
        bool hasDollars = false;
        double limitDollars = 0.0;
        double remainingDollars = 0.0;
        long long expiresAtUnixSeconds = 0;
        std::string lockedReason;
    };

    // Desktop's weekly pace forecast, computed from the weekly limit.
    struct Pace {
        enum class State { None, Zero, Fresh, Early, OnTrack, HeadsUp, LimitHit };
        State state = State::None;
        int percent = 0;
        long long runsOutAtUnixSeconds = 0; // HeadsUp only
        long long resetsAtUnixSeconds = 0;
    };

    // Weekly usage split by surface (`seven_day_breakdown`).
    struct BreakdownRow {
        std::string name; // "Claude Code", "Chats", "Cowork", "Other"
        float percent = 0.0f;
    };

    struct Snapshot {
        // Internal identity key used only to reset notification state when the
        // selected Claude account changes. It is never rendered or persisted.
        std::string accountKey;
        std::string usageHeading = "Claude usage";
        std::string plan = "Claude";
        // subscription_details, Desktop cookie sessions only: the next renewal,
        // or (planEnds) when a plan that will not renew ends. 0 = unknown.
        long long planRenewsAtUnixSeconds = 0;
        bool planEnds = false;
        std::string statusText = "Waiting for Claude auto refresh";
        std::string lastUpdated = "never";

        UsageWindow currentSession;
        UsageWindow weeklyAllModels;
        UsageWindow weeklySonnet;
        UsageWindow weeklyFable;
        std::vector<UsageWindow> additionalLimits;

        UsageCredits credits;
        LimitResets limitResets;
        std::vector<PromoCredit> promoCredits;
        std::vector<BreakdownRow> weeklyBreakdown;
        AutoResume autoResume;
        UsageLedger ledger;
        UsageTelemetry::AccessStatus access;
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    struct LocalTelemetry {
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
    };

    Snapshot FetchSnapshot(AccountSource source);

    // Pure: pace state of a weekly limit at `now`. Not cached in the
    // snapshot because the forecast moves with the clock.
    Pace ComputePace(float usedPercent, long long resetAtUnixSeconds, long long nowUnixSeconds);
    // Desktop's wording in local time, e.g. "Heads up. At this pace you'll run
    // out Wednesday night, before Thursday's reset."
    std::string PaceText(const Pace& pace, long long nowUnixSeconds);
    LocalTelemetry ReadLocalTelemetry();

}
