#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ClaudeDesktopAuth {

    // Every existing folder Claude Desktop 4.4212 may keep its userData in:
    // explicit overrides, the MSIX package cache it redirects %APPDATA%\Claude
    // into, Claude-Data, %APPDATA%\Claude, then the 3P build's Claude-3p.
    std::vector<std::filesystem::path> UserDataCandidates();

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

    // Claude Desktop records its own plan usage into
    // userData/plan-usage-history.json when its tray poller runs (every 15
    // minutes, 5 after recent tray use, skipped while locked or idle, behind a
    // feature gate; 4.5 minutes is only the minimum gap). Schema v2 in the ASAR:
    // { version, samples: [{ t, org, u: { <code>: percent } }] }. Reading it is
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
