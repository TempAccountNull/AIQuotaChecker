#pragma once

#include "ZAiCredentialFormat.hpp"
#include <functional>

namespace ZAi::Client
{
    struct Request
    {
        std::string url;
        std::string token;
        bool bearer = true;
        std::vector<std::pair<std::string, std::string>> extraHeaders;
    };
    struct Response
    {
        int status = 0;
        std::string body;
        long long serverUnixSeconds = 0;
    };
    struct Options
    {
        std::string balanceUrl = Protocol::BalanceUrl;
        // Only an explicit legacy/current URL override triggers this extra request.
        std::string currentUrl;
        long long now = 0;
    };
    using Transport = std::function<Response(const Request&)>;

    inline bool Https(const std::string& url)
    {
        if (url.size() < 9 || Protocol::Lower(url.substr(0, 8)) != "https://") return false;
        for (unsigned char ch : url) if (ch < 33 || ch == 127) return false;
        return true;
    }

    inline Snapshot Fetch(const CredentialFormat::Selection& credentials, const Options& options,
        const Transport& transport)
    {
        Snapshot snapshot;
        snapshot.lastUpdated = "now";
        std::string error;
        int errorRank = -1;
        UsageTelemetry::AccessStatus failure;
        auto fail = [&](const std::string& text, int status = 0, int rank = 1) {
            if (status == 401 || status == 403 || status == 429 || status == 402) rank = 3;
            if (rank <= errorRank) return;
            errorRank = rank;
            error = text;
            failure = UsageTelemetry::FromHttpFailure(status, {}, text);
        };
        auto request = [&](const Request& query, const std::string& name, bool optional,
            long long* serverTime = nullptr) -> Protocol::Json {
            if (!Https(query.url)) {
                if (!optional) fail(name + " URL must use HTTPS", 0, 2);
                return nullptr;
            }
            try {
                auto response = transport(query);
                if (response.status < 200 || response.status >= 300) {
                    if (!optional) {
                        const std::string detail = response.status == 401 || response.status == 403
                            ? "; credentials expired or rejected. Sign in again in ZCode."
                            : response.status == 429 ? "; refresh rate limited. Try again later." : "";
                        fail(name + " HTTP " + std::to_string(response.status) + detail, response.status, 2);
                    }
                    return nullptr;
                }
                auto root = Protocol::Json::parse(response.body, nullptr, false);
                if (!root.is_object()) {
                    if (!optional) fail(name + " returned invalid JSON", 0, 2);
                    return nullptr;
                }
                const auto businessCode = Protocol::Number(root, "code");
                if (businessCode && (*businessCode == 401 || *businessCode == 403 ||
                    *businessCode == 402 || *businessCode == 429)) {
                    const int code = static_cast<int>(*businessCode);
                    if (!optional) fail(name + " failed (API code " + std::to_string(code) + ")" +
                        (code == 401 || code == 403 ? "; sign in again in ZCode." : ""), code);
                    return nullptr;
                }
                if (serverTime) *serverTime = response.serverUnixSeconds;
                return root;
            }
            catch (...) {
                // Transport errors may contain URLs/headers; do not surface secrets.
                if (!optional) fail(name + " request failed. Check the connection and refresh again.", 0, 2);
                return nullptr;
            }
        };
        auto officialMcp = [&](Snapshot& result) {
            // ZCode 3.14.0 zcodeMcpQuotaProvider requires an exact Coding Plan
            // scope before querying usage. Start Plan's identity-only MCP access
            // does not supply that scope and must not trigger a quota request.
            if (!credentials.currentZaiAccount || !credentials.personalCodingSelected ||
                credentials.startPlanJwt.empty() || credentials.oauthAccessToken.empty()) return;
            Request query{ Protocol::McpUrl, credentials.startPlanJwt, true, {
                { "X-Bigmodel-Authorization", "Bearer " + credentials.oauthAccessToken },
                { "Bigmodel-Target-Type", "PERSONAL" }
            } };
            Protocol::ApplyMcpUsage(result, request(query, "MCP usage", true));
        };
        auto start = [&]() -> bool {
            if (credentials.startPlanJwt.empty()) return false;
            long long serverTime = 0;
            auto root = request({ options.balanceUrl, credentials.startPlanJwt, true, {} },
                "Start Plan balance", false, &serverTime);
            if (!root.is_object()) return false;
            // Legacy installations may supply plans through an explicitly configured
            // current URL. Never call the obsolete /billing endpoints by default.
            auto* data = Protocol::Data(root, true);
            if (data && data->is_object() && !data->contains("plans") && !options.currentUrl.empty()) {
                const auto current = request({ options.currentUrl, credentials.startPlanJwt, true, {} },
                    "Start Plan current", false);
                const auto* currentData = Protocol::Data(current, true);
                if (currentData && currentData->is_object() && currentData->contains("plans") &&
                    currentData->at("plans").is_array()) root["data"]["plans"] = currentData->at("plans");
            }
            Snapshot candidate;
            candidate.lastUpdated = "now";
            const auto parsed = Protocol::ApplyStartPlan(candidate, root, options.now, serverTime);
            if (!parsed.usable) { fail(parsed.error, 0, Protocol::Data(root, true) ? 1 : 2); return false; }
            candidate.statusText = "Source: active ZCode Start Plan";
            Protocol::FinalizeAccess(candidate);
            snapshot = std::move(candidate);
            return true;
        };
        auto coding = [&]() -> bool {
            for (const auto& apiKey : credentials.codingApiKeys) {
                const auto root = request({ Protocol::QuotaUrl, apiKey, false, {} }, "Coding Plan quota", false);
                if (!root.is_object()) continue;
                Snapshot candidate;
                candidate.lastUpdated = "now";
                if (!Protocol::ApplyCodingQuota(candidate, root)) {
                    fail(Protocol::Data(root) ? "Coding Plan returned no usable quota windows"
                        : "Coding Plan returned an unsuccessful quota envelope", 0, Protocol::Data(root) ? 1 : 2);
                    continue;
                }
                Protocol::ApplySubscription(candidate, request({ Protocol::SubscriptionUrl, apiKey, false, {} },
                    "Coding Plan subscription", true));
                candidate.statusText = "Source: Z.Ai Coding Plan quota";
                officialMcp(candidate);
                Protocol::FinalizeAccess(candidate);
                snapshot = std::move(candidate);
                return true;
            }
            return false;
        };
        if (credentials.personalCodingSelected ? (coding() || start()) : (start() || coding())) return snapshot;
        if (error.empty()) {
            error = credentials.diagnostic.empty()
                ? "Z.Ai credentials not found. Sign in and open the plan in ZCode, or set ZCODE_JWT_TOKEN / ZAI_CODING_API_KEY."
                : credentials.diagnostic;
            failure.state = UsageTelemetry::AccessState::Unavailable;
            failure.detail = error;
        }
        else if (!credentials.diagnostic.empty()) error += " " + credentials.diagnostic;
        snapshot.statusText = "Z.Ai usage unavailable: " + error;
        snapshot.access = std::move(failure);
        return snapshot;
    }
}
