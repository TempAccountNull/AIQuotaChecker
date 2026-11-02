#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace CodexAppServer {

    enum class ResultKind {
        Success,
        Unavailable,
        Error
    };

    struct Result {
        ResultKind kind = ResultKind::Unavailable;
        nlohmann::json accountResult;
        nlohmann::json rateLimitsResult;
        // Optional effective config returned by config/read. This is kept
        // separate from quota/account state because older app-server builds
        // may not expose the method.
        nlohmann::json configResult;
        // ChatGPT bearer token from getAuthStatus (memory only), so the
        // backend-only extras - monthly usage, profile stats, workspace
        // credits - also work for the app-server account. Empty for API-key
        // logins or older app-servers.
        std::string authToken;
        // accountSession/list: signed-in sessions (0 when unsupported).
        int sessionCount = 0;
        std::string detail;
    };

    // Reads the account and quota state through the installed Codex app-server.
    // This deliberately delegates credential-store and token-refresh handling to
    // Codex itself so account changes cannot be masked by an old auth.json file.
    Result ReadCurrentAccountRateLimits();

}
