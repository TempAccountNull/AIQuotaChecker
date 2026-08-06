#pragma once

#include <string>

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

}
