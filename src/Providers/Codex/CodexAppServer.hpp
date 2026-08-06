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
        std::string detail;
    };

    // Reads the account and quota state through the installed Codex app-server.
    // This deliberately delegates credential-store and token-refresh handling to
    // Codex itself so account changes cannot be masked by an old auth.json file.
    Result ReadCurrentAccountRateLimits();

}
