#pragma once

#include <string>
#include <vector>

namespace ClaudeDesktopAuth {

    enum class AuthSource {
        None,
        BrowserCookies,
        OAuthCache
    };

    enum class ResultKind {
        Success,
        NoDesktopSession,
        Error
    };

    struct Result {
        ResultKind kind = ResultKind::NoDesktopSession;
        AuthSource source = AuthSource::None;
        std::string organizationId;
        std::string baseUrl;
        std::string cookieHeader;
        std::string accessToken;
        std::string subscriptionType;
        std::string rateLimitTier;
        std::string detail;
    };

    // Reads the account currently active in Claude Desktop. The cookie store is
    // read on every refresh so switching accounts cannot reuse a stale CLI token.
    Result AcquireCurrentSession();

    // Claude Desktop samples its own plan usage every 4.5 minutes into
    // userData/plan-usage-history.json (schema v2 in the shipped ASAR:
    // { version, samples: [{ t, org, u: { <code>: percent } }] }). Reading it is
    // pure file I/O, so it keeps working while Desktop holds its Cookies
    // database open, and the newest sample names the organization that is
    // active *now* - which is what makes an account switch visible before
    // .credentials.json has been rewritten.
    struct PlanUsageWindow {
        std::string key;
        double usedPercent = 0.0;
        // Derived, not reported: the history file carries no reset instants, so
        // this is recovered from where the series itself resets.
        long long resetAtUnixSeconds = 0;
    };

    struct PlanUsageSample {
        bool valid = false;
        std::string organizationId;
        long long sampledAtUnixMs = 0;
        std::vector<PlanUsageWindow> windows;
        bool hasExtraUsage = false;
        double extraUsagePercent = 0.0;
    };

    PlanUsageSample ReadLatestPlanUsage();

}
