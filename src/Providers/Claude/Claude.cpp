#include "Global.hpp"

#include "Claude.hpp"
#include "ClaudeDesktopAuth.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Window.hpp"
#include "Math.hpp"
#include "Text.hpp"
#include "Format.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <unordered_map>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Claude {


    struct ClaudeOAuth {
        std::string accessToken;
        std::string refreshToken;
        double expiresAtMs = 0.0;
        std::string subscriptionType;
        std::string rateLimitTier;
    };

    struct ClaudeCredentials {
        ClaudeOAuth oauth;
        json root;
        std::filesystem::path path;
        bool canSave = false;
    };

    static std::filesystem::path ClaudeCredentialsPath() {
        std::string configDir = Network::get_instance()->GetEnvText("CLAUDE_CONFIG_DIR");

        if (!configDir.empty()) {
            return std::filesystem::path(configDir) / ".credentials.json";
        }

        return Network::get_instance()->UserProfilePath() / ".claude" / ".credentials.json";
    }

    static void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);

        if (!file) {
            throw std::runtime_error("Could not write file: " + path.string());
        }

        file << text;
    }

    static bool IsTokenNearExpiry(double expiresAtMs) {
        if (expiresAtMs <= 0.0) {
            return false;
        }

        using namespace std::chrono;

        auto nowMs = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count();

        double remainingMs = expiresAtMs - static_cast<double>(nowMs);
        return remainingMs <= 5.0 * 60.0 * 1000.0;
    }

    static std::string ClaudeStringAny(
        const json& object,
        std::initializer_list<const char*> keys
    );
    static std::optional<double> ClaudeNumberAny(
        const json& object,
        std::initializer_list<const char*> keys
    );

    static std::optional<ClaudeCredentials> LoadClaudeCredentialsFile() {
        std::filesystem::path path = ClaudeCredentialsPath();

        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }

        json root = JsonUtils::get_instance()->ParseRequired(
            Network::get_instance()->ReadRequiredTextFile(path)
        );

        if (!root.contains("claudeAiOauth") || !root.at("claudeAiOauth").is_object()) {
            throw std::runtime_error(
                "Claude Code .credentials.json does not contain claudeAiOauth: " + path.string()
            );
        }

        const json& oauthJson = root.at("claudeAiOauth");

        ClaudeOAuth oauth;
        oauth.accessToken = ClaudeStringAny(oauthJson, { "accessToken", "access_token" });
        oauth.refreshToken = ClaudeStringAny(oauthJson, { "refreshToken", "refresh_token" });
        oauth.expiresAtMs = ClaudeNumberAny(oauthJson, { "expiresAt", "expires_at" }).value_or(0.0);
        oauth.subscriptionType = ClaudeStringAny(oauthJson, { "subscriptionType", "subscription_type" });
        oauth.rateLimitTier = ClaudeStringAny(oauthJson, { "rateLimitTier", "rate_limit_tier" });

        if (oauth.accessToken.empty()) {
            throw std::runtime_error(
                "Claude Code accessToken is missing from .credentials.json: " + path.string()
            );
        }

        ClaudeCredentials creds;
        creds.oauth = oauth;
        creds.root = root;
        creds.path = path;
        creds.canSave = true;
        return creds;
    }

    static std::optional<ClaudeCredentials> LoadClaudeEnvironmentCredentials() {
        std::string envToken = Network::get_instance()->GetEnvText("CLAUDE_CODE_OAUTH_TOKEN");

        if (envToken.empty()) {
            return std::nullopt;
        }

        ClaudeCredentials creds;
        creds.oauth.accessToken = envToken;
        creds.canSave = false;
        return creds;
    }

    static void SaveCredentials(ClaudeCredentials& creds) {
        if (!creds.canSave) {
            return;
        }

        if (!creds.root.is_object()) {
            creds.root = json::object();
        }

        if (!creds.root.contains("claudeAiOauth") || !creds.root.at("claudeAiOauth").is_object()) {
            creds.root["claudeAiOauth"] = json::object();
        }

        creds.root["claudeAiOauth"]["accessToken"] = creds.oauth.accessToken;

        if (!creds.oauth.refreshToken.empty()) {
            creds.root["claudeAiOauth"]["refreshToken"] = creds.oauth.refreshToken;
        }

        if (creds.oauth.expiresAtMs > 0.0) {
            creds.root["claudeAiOauth"]["expiresAt"] = creds.oauth.expiresAtMs;
        }

        if (!creds.oauth.subscriptionType.empty()) {
            creds.root["claudeAiOauth"]["subscriptionType"] = creds.oauth.subscriptionType;
        }

        if (!creds.oauth.rateLimitTier.empty()) {
            creds.root["claudeAiOauth"]["rateLimitTier"] = creds.oauth.rateLimitTier;
        }

        WriteTextFile(creds.path, creds.root.dump(2));
    }

    static std::string RefreshScope() {
        return "user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
    }

    static bool RefreshToken(ClaudeCredentials& creds) {
        if (creds.oauth.refreshToken.empty()) {
            return false;
        }

        json body;
        body["grant_type"] = "refresh_token";
        body["refresh_token"] = creds.oauth.refreshToken;
        body["client_id"] = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
        body["scope"] = RefreshScope();

        std::wstring headers =
            std::wstring(L"Content-Type: application/json\r\n") +
            L"Accept: application/json\r\n";

        Network::HttpResponse response = Network::get_instance()->RequestUrl(
            "https://platform.claude.com/v1/oauth/token",
            "POST",
            headers,
            body.dump()
        );

        if (response.statusCode < 200 || response.statusCode >= 300) {
            return false;
        }

        json decoded = JsonUtils::get_instance()->ParseRequired(response.body);

        std::string accessToken = JsonUtils::get_instance()->String(decoded, "access_token");
        std::string refreshToken = JsonUtils::get_instance()->String(decoded, "refresh_token");
        double expiresIn = JsonUtils::get_instance()->Number(decoded, "expires_in", 0.0);

        if (accessToken.empty()) {
            return false;
        }

        creds.oauth.accessToken = accessToken;

        if (!refreshToken.empty()) {
            creds.oauth.refreshToken = refreshToken;
        }

        if (expiresIn > 0.0) {
            using namespace std::chrono;

            auto nowMs = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()
            ).count();

            creds.oauth.expiresAtMs = static_cast<double>(nowMs) + expiresIn * 1000.0;
        }

        SaveCredentials(creds);
        return true;
    }

    static Network::HttpResponse FetchUsage(const std::string& accessToken) {
        std::string token = Network::get_instance()->StripHeaderValue(accessToken);

        std::wstring headers =
            std::wstring(L"Authorization: Bearer ") +
            Network::get_instance()->Utf8ToWide(token) +
            L"\r\n"
            L"Accept: application/json\r\n"
            L"Content-Type: application/json\r\n"
            L"Cache-Control: no-cache\r\n"
            L"Pragma: no-cache\r\n"
            L"anthropic-beta: oauth-2025-04-20\r\n"
            L"User-Agent: claude-code/2.1.69\r\n";

        return Network::get_instance()->RequestUrl(
            "https://api.anthropic.com/api/oauth/usage",
            "GET",
            headers
        );
    }

    static std::chrono::system_clock::time_point ParseTimeFlexible(const json& value) {
        using namespace std::chrono;

        if (value.is_number()) {
            double seconds = value.get<double>();

            if (std::abs(seconds) > 10000000000.0) {
                seconds /= 1000.0;
            }

            return system_clock::from_time_t(static_cast<time_t>(seconds));
        }

        if (!value.is_string()) {
            return system_clock::time_point{};
        }

        std::string text = value.get<std::string>();

        bool numeric = !text.empty();

        for (char c : text) {
            if (!(c >= '0' && c <= '9') && c != '.') {
                numeric = false;
                break;
            }
        }

        if (numeric) {
            double seconds = std::stod(text);

            if (std::abs(seconds) > 10000000000.0) {
                seconds /= 1000.0;
            }

            return system_clock::from_time_t(static_cast<time_t>(seconds));
        }

        std::string main = text;
        int offsetSeconds = 0;

        if (!main.empty() && main.back() == 'Z') {
            main.pop_back();
        }
        else {
            size_t tzPos = std::string::npos;

            for (size_t i = 19; i < main.size(); ++i) {
                if (main[i] == '+' || main[i] == '-') {
                    tzPos = i;
                    break;
                }
            }

            if (tzPos != std::string::npos) {
                char sign = main[tzPos];
                std::string offset = main.substr(tzPos + 1);
                main = main.substr(0, tzPos);

                int hh = 0;
                int mm = 0;

                if (offset.size() >= 5) {
                    hh = std::stoi(offset.substr(0, 2));
                    mm = std::stoi(offset.substr(3, 2));
                }

                offsetSeconds = hh * 3600 + mm * 60;

                if (sign == '-') {
                    offsetSeconds = -offsetSeconds;
                }
            }
        }

        size_t dot = main.find('.');

        if (dot != std::string::npos) {
            main = main.substr(0, dot);
        }

        std::tm tm{};
        std::istringstream ss(main);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

        if (ss.fail()) {
            return system_clock::time_point{};
        }

        time_t utc = _mkgmtime(&tm) - offsetSeconds;
        return system_clock::from_time_t(utc);
    }

    static std::string FormatResetRelativeText(std::chrono::system_clock::time_point tp) {
        return Format::get_instance()->ResetRelative(tp);
    }

    static std::string FormatResetLong(std::chrono::system_clock::time_point tp) {
        return Format::get_instance()->ResetLong(tp);
    }

    static std::string FormatResetShort(std::chrono::system_clock::time_point tp) {
        return Format::get_instance()->ResetRelative(tp);
    }

    static std::chrono::system_clock::time_point FirstDayOfNextMonth() {
        std::time_t now = std::time(nullptr);
        std::tm localTime{};
        localtime_s(&localTime, &now);

        localTime.tm_sec = 0;
        localTime.tm_min = 0;
        localTime.tm_hour = 0;
        localTime.tm_mday = 1;
        localTime.tm_mon += 1;

        return std::chrono::system_clock::from_time_t(std::mktime(&localTime));
    }

    static long long TimePointToUnixSeconds(std::chrono::system_clock::time_point tp) {
        if (tp.time_since_epoch().count() == 0) {
            return 0;
        }

        return static_cast<long long>(std::chrono::system_clock::to_time_t(tp));
    }

    static std::string FormatDollars(double value) {
        return Format::get_instance()->Dollars(value);
    }

    static std::string ToLowerAscii(std::string value) {
        return Text::get_instance()->ToLowerCopy(value);
    }

    static const json* FindClaudeObject(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        if (!object.is_object()) {
            return nullptr;
        }

        for (const char* key : keys) {
            if (object.contains(key) && object.at(key).is_object()) {
                return &object.at(key);
            }
        }

        return nullptr;
    }

    static const json* FindClaudeObjectRecursive(
        const json& value,
        std::initializer_list<const char*> keys,
        int depth = 0
    ) {
        if (depth > 5) {
            return nullptr;
        }

        if (value.is_object()) {
            if (const json* direct = FindClaudeObject(value, keys)) {
                return direct;
            }

            // Current Claude Desktop returns extra_usage at the top level.
            // These wrappers are accepted for compatible web/API variants,
            // without scanning or persisting any unrelated local data.
            for (const char* wrapper : { "data", "usage", "plan_usage", "planUsage", "result" }) {
                if (value.contains(wrapper)) {
                    if (const json* nested = FindClaudeObjectRecursive(value.at(wrapper), keys, depth + 1)) {
                        return nested;
                    }
                }
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!it.value().is_object() && !it.value().is_array()) {
                    continue;
                }

                if (const json* nested = FindClaudeObjectRecursive(it.value(), keys, depth + 1)) {
                    return nested;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (const json* nested = FindClaudeObjectRecursive(item, keys, depth + 1)) {
                    return nested;
                }
            }
        }

        return nullptr;
    }

    static bool HasClaudeExtraUsageField(const json& object) {
        if (!object.is_object()) {
            return false;
        }

        for (const char* key : {
            "is_enabled", "isEnabled", "enabled",
            "monthly_limit", "monthlyLimit", "spend_limit", "spendLimit",
            "used_credits", "usedCredits", "spent_credits", "spentCredits",
            "utilization", "percent", "used_percent", "usedPercent"
        }) {
            if (object.contains(key)) {
                return true;
            }
        }

        return false;
    }

    static int ClaudeExtraUsageInformationScore(const json& object) {
        if (!HasClaudeExtraUsageField(object)) {
            return -1;
        }

        int score = 1;

        auto hasNonNull = [&](std::initializer_list<const char*> keys) {
            for (const char* key : keys) {
                if (object.contains(key) && !object.at(key).is_null()) {
                    return true;
                }
            }

            return false;
        };

        if (hasNonNull({ "is_enabled", "isEnabled", "enabled" })) {
            score += 5;

            if (JsonUtils::get_instance()->Bool(object, "is_enabled",
                    JsonUtils::get_instance()->Bool(object, "isEnabled",
                        JsonUtils::get_instance()->Bool(object, "enabled", false)))) {
                score += 30;
            }
        }

        if (hasNonNull({ "used_credits", "usedCredits", "spent_credits", "spentCredits" })) {
            score += 45;
        }

        if (hasNonNull({ "monthly_limit", "monthlyLimit", "spend_limit", "spendLimit" })) {
            score += 35;
        }

        if (hasNonNull({ "utilization", "percent", "used_percent", "usedPercent" })) {
            score += 20;
        }

        return score;
    }

    static const json* FindClaudeExtraUsageObject(const json& root) {
        const json* best = nullptr;
        int bestScore = -1;

        // Claude Desktop's authoritative plan-usage schema places
        // extra_usage at the response root. A small set of API wrappers is
        // accepted for compatibility, but unrelated nested objects are never
        // searched because they can contain stale feature/configuration flags.
        std::function<void(const json&, int)> visit = [&](const json& value, int depth) {
            if (!value.is_object() || depth > 3) {
                return;
            }

            for (const char* key : { "extra_usage", "extraUsage" }) {
                if (!value.contains(key) || !value.at(key).is_object()) {
                    continue;
                }

                const json& candidate = value.at(key);
                int score = ClaudeExtraUsageInformationScore(candidate);

                if (score > bestScore) {
                    best = &candidate;
                    bestScore = score;
                }
            }

            for (const char* wrapper : { "data", "usage", "plan_usage", "planUsage", "result" }) {
                if (value.contains(wrapper)) {
                    visit(value.at(wrapper), depth + 1);
                }
            }
        };

        visit(root, 0);
        return best;
    }

    static std::string ClaudeStringAny(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        if (!object.is_object()) {
            return {};
        }

        for (const char* key : keys) {
            std::string value = JsonUtils::get_instance()->String(object, key);

            if (!value.empty()) {
                return value;
            }
        }

        return {};
    }

    static std::optional<double> ClaudeNumberAny(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        return JsonUtils::get_instance()->NumberAny(object, keys);
    }

    static bool ClaudeBoolAny(
        const json& object,
        std::initializer_list<const char*> keys,
        bool fallback = false
    ) {
        if (!object.is_object()) {
            return fallback;
        }

        for (const char* key : keys) {
            if (object.contains(key) && !object.at(key).is_null()) {
                return JsonUtils::get_instance()->Bool(object, key, fallback);
            }
        }

        return fallback;
    }

    static std::chrono::system_clock::time_point ParseClaudeResetAt(const json& object) {
        if (!object.is_object()) {
            return {};
        }

        for (const char* key : { "resets_at", "resetsAt", "reset_at", "resetAt" }) {
            if (object.contains(key) && !object.at(key).is_null()) {
                return ParseTimeFlexible(object.at(key));
            }
        }

        return {};
    }

    static std::string PrettyTitleToken(std::string value) {
        if (value.empty()) {
            return "";
        }

        for (char& c : value) {
            if (c == '_' || c == '-') {
                c = ' ';
            }
        }

        bool capitalizeNext = true;

        for (char& c : value) {
            unsigned char uc = static_cast<unsigned char>(c);

            if (capitalizeNext) {
                c = static_cast<char>(std::toupper(uc));
            }
            else {
                c = static_cast<char>(std::tolower(uc));
            }

            capitalizeNext = c == ' ';
        }

        return value;
    }

    static std::string PrettySubscriptionType(const std::string& subscriptionType) {
        std::string lower = ToLowerAscii(subscriptionType);

        if (lower.empty()) return "";
        if (lower == "pro") return "Pro";
        if (lower == "max") return "Max";
        if (lower == "team") return "Team";
        if (lower == "enterprise") return "Enterprise";
        if (lower == "free") return "Free";

        return PrettyTitleToken(subscriptionType);
    }

    static std::string ExtractRateLimitMultiplier(const std::string& rateLimitTier) {
        std::string lower = ToLowerAscii(rateLimitTier);

        // OpenClaude only appends the multiplier part of rateLimitTier.
        // Examples:
        //   default_claude_ai -> ignored
        //   team_5x           -> 5x
        //   max_20x           -> 20x
        for (size_t i = 0; i < lower.size(); ++i) {
            if (lower[i] != 'x') continue;

            size_t start = i;

            while (start > 0 && lower[start - 1] >= '0' && lower[start - 1] <= '9') {
                --start;
            }

            if (start != i) {
                return lower.substr(start, i - start + 1);
            }
        }

        return "";
    }

    static std::string FormatPlan(const ClaudeOAuth& oauth) {
        std::string plan = "Claude";
        std::string subscription = PrettySubscriptionType(oauth.subscriptionType);
        std::string multiplier = ExtractRateLimitMultiplier(oauth.rateLimitTier);

        if (!subscription.empty()) {
            plan += " ";
            plan += subscription;
        }

        if (!multiplier.empty()) {
            plan += " ";
            plan += multiplier;
        }

        return plan;
    }

    static UsageWindow ParseWindow(
        const json& root,
        const char* key,
        const std::string& title,
        const std::string& subtitle
    ) {
        UsageWindowModel::Builder window;
        window.Title(title).Subtitle(subtitle);

        if (!root.contains(key) || !root.at(key).is_object()) {
            return window.Build();
        }

        const json& object = root.at(key);

        std::optional<double> usedValue = ClaudeNumberAny(
            object,
            { "utilization", "percent", "used_percent", "usedPercent" }
        );

        if (!usedValue) {
            return window.Build();
        }

        double used = Math::get_instance()->ClampPercentDouble(*usedValue);
        const std::chrono::system_clock::time_point resetAt = ParseClaudeResetAt(object);

        return window
            .UsedPercent(static_cast<float>(used))
            .Reset(TimePointToUnixSeconds(resetAt), FormatResetLong(resetAt))
            .Valid()
            .Build();
    }


    static UsageWindow ParseFirstAvailableWindow(
        const json& root,
        std::initializer_list<const char*> keys,
        const std::string& title,
        const std::string& subtitle
    ) {
        for (const char* key : keys) {
            UsageWindow window = ParseWindow(root, key, title, subtitle);

            if (window.valid) {
                return window;
            }
        }

        return UsageWindowModel::Placeholder(title, subtitle);
    }

    static std::string LimitScopeDisplayName(const json& limit) {
        if (!limit.contains("scope") || !limit.at("scope").is_object()) {
            return {};
        }

        const json& scope = limit.at("scope");

        for (const char* key : { "model", "surface" }) {
            if (scope.contains(key) && scope.at(key).is_object()) {
                std::string displayName = ClaudeStringAny(
                    scope.at(key),
                    { "display_name", "displayName" }
                );
                if (!displayName.empty()) return displayName;
            }
        }

        return {};
    }

    static std::string HumanizeIdentifier(std::string text) {
        for (char& c : text) {
            if (c == '_' || c == '-' || c == '/') {
                c = ' ';
            }
        }

        std::string output;
        output.reserve(text.size());
        bool previousSpace = true;

        for (char c : text) {
            const bool isSpace = std::isspace(static_cast<unsigned char>(c)) != 0;

            if (isSpace) {
                if (!output.empty() && output.back() != ' ') {
                    output.push_back(' ');
                }
                previousSpace = true;
                continue;
            }

            if (previousSpace) {
                output.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
            else {
                output.push_back(c);
            }

            previousSpace = false;
        }

        while (!output.empty() && output.back() == ' ') {
            output.pop_back();
        }

        return output;
    }

    static std::string ServerLimitTitle(const json& limit) {
        std::string displayName = LimitScopeDisplayName(limit);

        if (!displayName.empty()) {
            return displayName;
        }

        std::string kind = ClaudeStringAny(limit, { "kind" });

        if (kind == "session") {
            return "Session";
        }

        if (kind == "weekly_all") {
            return "Weekly";
        }

        std::string group = ClaudeStringAny(limit, { "group" });
        std::string title = HumanizeIdentifier(kind.empty() ? group : kind);
        return title.empty() ? "Usage" : title;
    }

    static std::string ServerLimitSubtitle(const json& limit) {
        const std::string kind = ClaudeStringAny(limit, { "kind" });
        const std::string group = ClaudeStringAny(limit, { "group" });

        if (kind == "session") {
            return "Starts when a message is sent";
        }

        if (kind == "weekly_all") {
            return "All models";
        }

        if (limit.contains("scope") && limit.at("scope").is_object()) {
            const json& scope = limit.at("scope");

            if (scope.contains("model") && scope.at("model").is_object()) {
                return group == "weekly" ? "Model-specific weekly limit" : "Model-specific limit";
            }

            if (scope.contains("surface") && scope.at("surface").is_object()) {
                return group == "weekly" ? "Product-specific weekly limit" : "Product-specific limit";
            }
        }

        if (!group.empty()) {
            return HumanizeIdentifier(group) + " limit";
        }

        return "Usage limit";
    }

    static UsageWindow ParseServerLimitWindow(
        const json& limit,
        const std::string& title,
        const std::string& subtitle
    ) {
        UsageWindowModel::Builder window;
        window.Title(title).Subtitle(subtitle);

        std::optional<double> percent = ClaudeNumberAny(
            limit,
            { "percent", "utilization", "used_percent", "usedPercent" }
        );
        if (!percent) return window.Build();

        const std::chrono::system_clock::time_point resetAt = ParseClaudeResetAt(limit);

        return window
            .UsedPercent(static_cast<float>(Math::get_instance()->ClampPercentDouble(*percent)))
            .Reset(TimePointToUnixSeconds(resetAt), FormatResetLong(resetAt))
            .Valid()
            .Build();
    }

    static void AddAdditionalLimit(Snapshot& snapshot, UsageWindow window) {
        if (!window.valid) {
            return;
        }

        auto sameIdentity = [&](const UsageWindow& existing) {
            return existing.valid &&
                ToLowerAscii(existing.title) == ToLowerAscii(window.title) &&
                ToLowerAscii(existing.subtitle) == ToLowerAscii(window.subtitle) &&
                existing.resetAtUnixSeconds == window.resetAtUnixSeconds;
        };

        if (sameIdentity(snapshot.currentSession) ||
            sameIdentity(snapshot.weeklyAllModels) ||
            sameIdentity(snapshot.weeklySonnet) ||
            sameIdentity(snapshot.weeklyFable)) {
            return;
        }

        for (const UsageWindow& existing : snapshot.additionalLimits) {
            if (sameIdentity(existing)) {
                return;
            }
        }

        snapshot.additionalLimits.push_back(std::move(window));
    }

    static bool ParseServerDrivenLimits(const json& root, Snapshot& snapshot) {
        if (!root.contains("limits") || !root.at("limits").is_array()) {
            return false;
        }

        const json& limits = root.at("limits");

        if (limits.begin() == limits.end()) {
            // An explicitly empty list means the server currently exposes no
            // quota windows. Do not fabricate legacy rows.
            return true;
        }

        bool parsedAny = false;

        for (const json& limit : limits) {
            if (!limit.is_object()) continue;

            if (!ClaudeNumberAny(limit, { "percent", "utilization", "used_percent", "usedPercent" })) {
                continue;
            }

            const std::string kind = ClaudeStringAny(limit, { "kind" });
            const std::string group = ClaudeStringAny(limit, { "group" });

            if (kind == "session") {
                snapshot.currentSession = ParseServerLimitWindow(
                    limit,
                    "Session",
                    "Starts when a message is sent"
                );
                parsedAny = snapshot.currentSession.valid || parsedAny;
                continue;
            }

            if (kind == "weekly_all") {
                snapshot.weeklyAllModels = ParseServerLimitWindow(
                    limit,
                    "Weekly",
                    "All models"
                );
                parsedAny = snapshot.weeklyAllModels.valid || parsedAny;
                continue;
            }

            const std::string displayName = LimitScopeDisplayName(limit);
            const std::string lower = ToLowerAscii(displayName);

            if (group == "weekly" && lower.find("sonnet") != std::string::npos) {
                snapshot.weeklySonnet = ParseServerLimitWindow(
                    limit,
                    displayName.empty() ? "Sonnet" : displayName,
                    "Model-specific weekly limit"
                );
                parsedAny = snapshot.weeklySonnet.valid || parsedAny;
            }
            else if (group == "weekly" && (lower.find("fable") != std::string::npos ||
                lower.find("design") != std::string::npos ||
                lower.find("omelette") != std::string::npos)) {
                snapshot.weeklyFable = ParseServerLimitWindow(
                    limit,
                    displayName.empty() ? "Fable" : displayName,
                    "Product-specific weekly limit"
                );
                parsedAny = snapshot.weeklyFable.valid || parsedAny;
            }
            else {
                UsageWindow additional = ParseServerLimitWindow(
                    limit,
                    ServerLimitTitle(limit),
                    ServerLimitSubtitle(limit)
                );
                parsedAny = additional.valid || parsedAny;
                AddAdditionalLimit(snapshot, std::move(additional));
            }
        }

        // Presence of limits[] is authoritative in current Claude Desktop. An
        // empty array means no quota rows are available, not 0% utilization.
        // If every non-empty entry was malformed, fall back to the legacy
        // named fields, matching Claude Desktop's own schema behavior.
        return parsedAny;
    }

    static UsageCredits ParseCredits(const json& root, const ClaudeOAuth& oauth) {
        (void)oauth;

        UsageCredits credits;

        const json* extraUsage = FindClaudeExtraUsageObject(root);

        if (!extraUsage) {
            return credits;
        }

        credits.reported = true;
        credits.valid = true;
        credits.label = "Usage credits";

        const json& object = *extraUsage;
        std::optional<bool> enabled;

        for (const char* key : { "is_enabled", "isEnabled", "enabled" }) {
            if (object.contains(key) && !object.at(key).is_null()) {
                enabled = ClaudeBoolAny(object, { key }, false);
                break;
            }
        }

        const std::optional<double> usedCents = ClaudeNumberAny(
            object,
            { "used_credits", "usedCredits", "spent_credits", "spentCredits" }
        );
        const std::optional<double> limitCents = ClaudeNumberAny(
            object,
            { "monthly_limit", "monthlyLimit", "spend_limit", "spendLimit" }
        );
        const std::optional<double> utilization = ClaudeNumberAny(
            object,
            { "utilization", "percent", "used_percent", "usedPercent" }
        );
        const std::optional<double> balanceCents = ClaudeNumberAny(
            object,
            {
                "current_balance", "currentBalance",
                "credit_balance", "creditBalance", "balance"
            }
        );

        credits.hasSpentAmount = usedCents.has_value();
        credits.hasMonthlyLimit = limitCents.has_value();

        // Spend data is authoritative evidence that usage credits are active.
        // Do not render Off when a stale/contradictory flag is returned next
        // to non-null used_credits, monthly_limit, or utilization values.
        const bool hasSpendData = usedCents.has_value() || limitCents.has_value();
        const bool inferredEnabled = hasSpendData || utilization.has_value();
        credits.enabled = enabled.has_value()
            ? (*enabled || hasSpendData)
            : inferredEnabled;

        if (!credits.enabled) {
            credits.spentText = "Off";
            credits.limitText = "Off";
            credits.monthlyLimitText = "Off";
            credits.currentBalanceText = "Unavailable";
            credits.hasUsedPercent = false;
            return credits;
        }

        if (!usedCents) {
            // The provider reported that usage credits are enabled but withheld
            // spend data. Keep the section visible without fabricating a value.
            credits.spentText = "Spend unavailable";
            credits.limitText = limitCents ? FormatDollars(std::max(0.0, *limitCents) / 100.0) : "Unavailable";
            credits.monthlyLimitText = credits.limitText;
            credits.currentBalanceText = balanceCents
                ? FormatDollars(std::max(0.0, *balanceCents) / 100.0)
                : FormatDollars(0.0);
            credits.hasUsedPercent = false;
            return credits;
        }

        const double safeUsedCents = std::max(0.0, *usedCents);
        const double usedDollars = safeUsedCents / 100.0;
        credits.spentText = FormatDollars(usedDollars) + " spent";

        if (limitCents) {
            const double safeLimitCents = std::max(0.0, *limitCents);
            const double limitDollars = safeLimitCents / 100.0;
            credits.limitText = FormatDollars(limitDollars) + " monthly limit";
            credits.monthlyLimitText = FormatDollars(limitDollars);
            // The monthly limit is a spending cap, not a wallet balance.
            // Claude Desktop renders an omitted prepaid-credit balance as
            // $0.00, so mirror that exact UI behavior instead of showing an
            // ambiguous "Not reported" value.
            credits.currentBalanceText = balanceCents
                ? FormatDollars(std::max(0.0, *balanceCents) / 100.0)
                : FormatDollars(0.0);
            credits.hasUsedPercent = true;

            if (utilization) {
                credits.usedPercent = static_cast<float>(
                    Math::get_instance()->ClampPercentDouble(*utilization)
                );
            }
            else if (safeLimitCents == 0.0) {
                credits.usedPercent = 100.0f;
            }
            else {
                credits.usedPercent = static_cast<float>(
                    Math::get_instance()->ClampPercentDouble(
                        (safeUsedCents / safeLimitCents) * 100.0
                    )
                );
            }
        }
        else {
            credits.limitText = "Unlimited";
            credits.monthlyLimitText = "Unlimited";
            credits.currentBalanceText = balanceCents
                ? FormatDollars(std::max(0.0, *balanceCents) / 100.0)
                : FormatDollars(0.0);
            credits.hasUsedPercent = false;
        }

        std::chrono::system_clock::time_point resetAt = ParseClaudeResetAt(object);

        if (resetAt.time_since_epoch().count() == 0) {
            resetAt = FirstDayOfNextMonth();
        }

        credits.resetAtUnixSeconds = TimePointToUnixSeconds(resetAt);

        const std::string reset = FormatResetShort(resetAt);

        if (!reset.empty()) {
            credits.resetText = "Resets " + reset;
        }

        return credits;
    }

    static int CreditsInformationScore(const UsageCredits& credits) {
        if (!credits.reported) {
            return 0;
        }

        int score = 10;
        score += credits.valid ? 5 : 0;
        score += credits.enabled ? 25 : 0;
        score += credits.hasSpentAmount ? 45 : 0;
        score += credits.hasMonthlyLimit ? 35 : 0;
        score += credits.hasUsedPercent ? 20 : 0;
        score += !credits.currentBalanceText.empty() &&
            credits.currentBalanceText != "Unavailable" &&
            credits.currentBalanceText != "Not reported" ? 5 : 0;
        return score;
    }

    static bool CreditsNeedSupplement(const UsageCredits& credits) {
        if (!credits.reported) {
            return true;
        }

        // A full-spend response always supplies used_credits when the feature
        // is enabled. A reported disabled object with no spend fields can be a
        // lightweight/stale response, so verify it through the remaining
        // official authentication/host paths before displaying Off.
        if (!credits.enabled && !credits.hasSpentAmount && !credits.hasMonthlyLimit) {
            return true;
        }

        return credits.enabled && !credits.hasSpentAmount;
    }

    static std::filesystem::path ClaudeProjectsPath() {
        std::string configDir = Network::get_instance()->GetEnvText("CLAUDE_CONFIG_DIR");

        if (!configDir.empty()) {
            return std::filesystem::path(configDir) / "projects";
        }

        return Network::get_instance()->UserProfilePath() / ".claude" / "projects";
    }

    static std::string ReadClaudeTailSharedReadOnly(
        const std::filesystem::path& path,
        unsigned long long maximumBytes
    ) {
        HANDLE file = CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE) {
            return {};
        }

        LARGE_INTEGER size{};

        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
            CloseHandle(file);
            return {};
        }

        const unsigned long long fileSize = static_cast<unsigned long long>(size.QuadPart);
        const unsigned long long readSize = std::min(fileSize, maximumBytes);
        const unsigned long long start = fileSize - readSize;
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(start);

        if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
            CloseHandle(file);
            return {};
        }

        std::string text(static_cast<size_t>(readSize), '\0');
        size_t totalRead = 0;

        while (totalRead < text.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
                text.size() - totalRead,
                1024 * 1024
            ));
            DWORD read = 0;

            if (!ReadFile(file, text.data() + totalRead, chunk, &read, nullptr) || read == 0) {
                break;
            }

            totalRead += read;
        }

        CloseHandle(file);
        text.resize(totalRead);

        if (start > 0) {
            const size_t newline = text.find('\n');

            if (newline == std::string::npos) {
                return {};
            }

            text.erase(0, newline + 1);
        }

        return text;
    }

    static std::filesystem::path ClaudeDesktopSessionsRoot() {
        // Claude Desktop 3p uses LOCALAPPDATA\\Claude-3p\\claude-code-sessions
        // on Windows. This mirrors the path visible in the supplied unpacked
        // ASAR and is read-only here.
        const std::string customUserData =
            Network::get_instance()->GetEnvText("CLAUDE_USER_DATA_DIR");
        if (!customUserData.empty()) {
            return std::filesystem::path(customUserData) / "claude-code-sessions";
        }

        const std::string localAppData = Network::get_instance()->GetEnvText("LOCALAPPDATA");
        if (localAppData.empty()) {
            return {};
        }
        return std::filesystem::path(localAppData) / "Claude-3p" / "claude-code-sessions";
    }

    struct ClaudeDesktopSessionHint {
        bool valid = false;
        std::filesystem::path path;
        std::filesystem::file_time_type writeTime{};
        std::string sessionId;
        std::string cliSessionId;
        long long lastActivityAtMs = 0;
        long long lastFocusedAtMs = 0;
        long long completedTurns = 0;
        std::string selectedModel;
    };

    static std::string ClaudeSelectedModelFromJson(const json& value, int depth = 0) {
        if (depth > 12) {
            return {};
        }
        if (value.is_object()) {
            // Claude Desktop stores picker state by surface. For a Code/Cowork
            // session prefer those surfaces over chat, but preserve the exact
            // selected value (including an explicit "[1m]" suffix). Returning
            // the standard selection too is important: it prevents a different
            // surface's saved 1M choice from overriding a known 200K session.
            auto selector = value.find("__model_selector_state");
            if (selector != value.end() && selector->is_object()) {
                for (const char* surface : { "code", "cowork", "chat" }) {
                    auto surfaceIt = selector->find(surface);
                    if (surfaceIt == selector->end() || !surfaceIt->is_object()) continue;
                    auto model = surfaceIt->find("model");
                    if (model != surfaceIt->end() && model->is_string()) {
                        const std::string candidate = model->get<std::string>();
                        if (!candidate.empty()) return candidate;
                    }
                }
                for (auto it = selector->begin(); it != selector->end(); ++it) {
                    if (!it.value().is_object()) continue;
                    auto model = it.value().find("model");
                    if (model != it.value().end() && model->is_string()) {
                        const std::string candidate = model->get<std::string>();
                        if (!candidate.empty()) return candidate;
                    }
                }
            }

            for (const char* key : { "selectedModel", "selected_model", "model" }) {
                auto it = value.find(key);
                if (it != value.end() && it->is_string()) {
                    const std::string candidate = it->get<std::string>();
                    if (!candidate.empty()) return candidate;
                }
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.value().is_object() || it.value().is_array()) {
                    std::string candidate = ClaudeSelectedModelFromJson(it.value(), depth + 1);
                    if (!candidate.empty()) return candidate;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                std::string candidate = ClaudeSelectedModelFromJson(item, depth + 1);
                if (!candidate.empty()) return candidate;
            }
        }
        return {};
    }

    static std::filesystem::path ClaudeDesktopUserDataRoot() {
        const std::string customUserData =
            Network::get_instance()->GetEnvText("CLAUDE_USER_DATA_DIR");
        if (!customUserData.empty()) {
            return std::filesystem::path(customUserData);
        }
        const std::string localAppData = Network::get_instance()->GetEnvText("LOCALAPPDATA");
        if (localAppData.empty()) {
            return {};
        }
        return std::filesystem::path(localAppData) / "Claude-3p";
    }

    static std::string LatestClaudeDesktopSelectedModel() {
        struct Cache {
            long long checkedAt = 0;
            std::string model;
        };
        static Cache cache;
        static std::mutex cacheMutex;
        std::lock_guard<std::mutex> lock(cacheMutex);

        const long long now = static_cast<long long>(std::time(nullptr));
        if (cache.checkedAt > 0 && now >= cache.checkedAt && now - cache.checkedAt < 5) {
            return cache.model;
        }
        cache.checkedAt = now;
        cache.model.clear();

        std::filesystem::path root = ClaudeDesktopUserDataRoot();
        std::error_code ec;
        if (root.empty()) {
            return {};
        }
        // The Desktop ASAR stores account settings under
        // userData/local-agent-mode-sessions/<account>/<org>/.
        const std::filesystem::path accountRoot = root / "local-agent-mode-sessions";
        if (std::filesystem::exists(accountRoot, ec) && !ec) {
            root = accountRoot;
        }
        ec.clear();
        if (!std::filesystem::exists(root, ec) || ec) {
            return {};
        }

        struct Candidate {
            std::filesystem::path path;
            std::filesystem::file_time_type writeTime{};
        };
        std::vector<Candidate> files;
        std::filesystem::recursive_directory_iterator it(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            ec
        );
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
            if (ToLowerAscii(entry.path().filename().string()) != "cowork_account_settings.json") {
                continue;
            }
            const auto writeTime = entry.last_write_time(ec);
            if (ec) { ec.clear(); continue; }
            files.push_back({ entry.path(), writeTime });
        }
        std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) {
            return a.writeTime > b.writeTime;
        });
        if (files.size() > 16) files.resize(16);

        for (const Candidate& file : files) {
            const std::string raw = ReadClaudeTailSharedReadOnly(file.path, 4ULL * 1024ULL * 1024ULL);
            if (raw.empty()) continue;
            const json value = json::parse(raw, nullptr, false);
            if (value.is_discarded()) continue;
            std::string selected = ClaudeSelectedModelFromJson(value);
            if (!selected.empty()) {
                cache.model = std::move(selected);
                break;
            }
        }
        return cache.model;
    }

    static long long ClaudeDesktopSelectedContextWindowHint(
        const ClaudeDesktopSessionHint& hint
    ) {
        std::string selected = hint.selectedModel;
        if (selected.empty()) {
            selected = LatestClaudeDesktopSelectedModel();
        }
        const std::string lower = ToLowerAscii(selected);
        if (lower.find("[1m]") != std::string::npos ||
            lower.find("context-1m") != std::string::npos ||
            lower.find("context_1m") != std::string::npos) {
            return 1000000;
        }
        return 0;
    }

    static ClaudeDesktopSessionHint LatestClaudeDesktopSessionHint() {
        const std::filesystem::path root = ClaudeDesktopSessionsRoot();
        std::error_code ec;
        if (root.empty() || !std::filesystem::exists(root, ec) || ec) {
            return {};
        }

        struct Candidate {
            std::filesystem::path path;
            std::filesystem::file_time_type writeTime{};
        };
        std::vector<Candidate> files;
        std::filesystem::recursive_directory_iterator it(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            ec
        );
        const std::filesystem::recursive_directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            const auto& entry = *it;
            if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".json") {
                ec.clear();
                continue;
            }
            const std::string filename = ToLowerAscii(entry.path().filename().string());
            if (filename.rfind("local_", 0) != 0) {
                continue;
            }
            const auto writeTime = entry.last_write_time(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            files.push_back({ entry.path(), writeTime });
        }

        std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) {
            return a.writeTime > b.writeTime;
        });
        if (files.size() > 64) {
            files.resize(64);
        }

        ClaudeDesktopSessionHint best;
        for (const Candidate& file : files) {
            const std::string raw = ReadClaudeTailSharedReadOnly(file.path, 10ULL * 1024ULL * 1024ULL);
            if (raw.empty()) {
                continue;
            }

            json value = json::parse(raw, nullptr, false);
            if (value.is_discarded() || !value.is_object() || value.value("isArchived", false)) {
                continue;
            }

            ClaudeDesktopSessionHint hint;
            hint.valid = true;
            hint.path = file.path;
            hint.writeTime = file.writeTime;
            hint.sessionId = value.value("sessionId", std::string{});
            hint.cliSessionId = value.value("cliSessionId", std::string{});
            hint.lastActivityAtMs = static_cast<long long>(std::max(
                0.0,
                ClaudeNumberAny(value, { "lastActivityAt" }).value_or(0.0)
            ));
            hint.lastFocusedAtMs = static_cast<long long>(std::max(
                0.0,
                ClaudeNumberAny(value, { "lastFocusedAt" }).value_or(0.0)
            ));
            hint.completedTurns = static_cast<long long>(std::max(
                0.0,
                ClaudeNumberAny(value, { "completedTurns" }).value_or(0.0)
            ));
            hint.selectedModel = ClaudeSelectedModelFromJson(value);

            const auto score = [](const ClaudeDesktopSessionHint& h) {
                return std::max(h.lastFocusedAtMs, h.lastActivityAtMs);
            };
            if (!best.valid || score(hint) > score(best) ||
                (score(hint) == score(best) && hint.writeTime > best.writeTime)) {
                best = std::move(hint);
            }
        }

        return best;
    }

    struct ClaudeSessionCandidate {
        std::filesystem::path path;
        std::filesystem::file_time_type writeTime{};
    };

    static bool IsClaudeBackgroundSessionPath(const std::filesystem::path& path) {
        for (const auto& component : path) {
            if (ToLowerAscii(component.string()) == "subagents") {
                return true;
            }
        }

        return ToLowerAscii(path.filename().string()).rfind("agent-", 0) == 0;
    }

    static std::vector<ClaudeSessionCandidate> LatestClaudeSessionFiles() {
        const std::filesystem::path projects = ClaudeProjectsPath();
        std::error_code ec;

        if (!std::filesystem::exists(projects, ec) || ec) {
            return {};
        }

        std::vector<ClaudeSessionCandidate> candidates;
        std::filesystem::recursive_directory_iterator it(
            projects,
            std::filesystem::directory_options::skip_permission_denied,
            ec
        );
        const std::filesystem::recursive_directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }

            const std::filesystem::directory_entry& entry = *it;

            if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".jsonl") {
                ec.clear();
                continue;
            }

            // Task/subagent transcripts have independent context windows and
            // must never replace the visible foreground conversation meter.
            if (IsClaudeBackgroundSessionPath(entry.path())) {
                continue;
            }

            const auto writeTime = entry.last_write_time(ec);

            if (ec) {
                ec.clear();
                continue;
            }

            candidates.push_back({ entry.path(), writeTime });
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const ClaudeSessionCandidate& left, const ClaudeSessionCandidate& right) {
                return left.writeTime > right.writeTime;
            }
        );

        if (candidates.size() > 64) {
            candidates.resize(64);
        }

        return candidates;
    }

    static std::optional<double> FindClaudeNumberRecursive(
        const json& value,
        std::initializer_list<const char*> keys,
        int depth = 0
    ) {
        if (depth > 10) {
            return std::nullopt;
        }

        if (value.is_object()) {
            for (const char* key : keys) {
                if (!value.contains(key) || value.at(key).is_null()) {
                    continue;
                }

                const json& candidate = value.at(key);

                if (candidate.is_number()) {
                    return candidate.get<double>();
                }
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                if (it.value().is_object() || it.value().is_array()) {
                    if (auto result = FindClaudeNumberRecursive(it.value(), keys, depth + 1)) {
                        return result;
                    }
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (auto result = FindClaudeNumberRecursive(item, keys, depth + 1)) {
                    return result;
                }
            }
        }

        return std::nullopt;
    }

    static std::optional<json> ExtractClaudeMetadataObject(
        const std::string& line,
        const std::string& key
    ) {
        const std::string needle = "\"" + key + "\"";
        size_t searchAt = 0;

        while (true) {
            const size_t keyAt = line.find(needle, searchAt);

            if (keyAt == std::string::npos) {
                return std::nullopt;
            }

            const size_t colon = line.find(':', keyAt + needle.size());

            if (colon == std::string::npos) {
                return std::nullopt;
            }

            size_t begin = colon + 1;

            while (begin < line.size() && std::isspace(
                static_cast<unsigned char>(line[begin])
            )) {
                ++begin;
            }

            if (begin >= line.size() || line[begin] != '{') {
                searchAt = keyAt + needle.size();
                continue;
            }

            int depth = 0;
            bool inString = false;
            bool escaped = false;

            for (size_t i = begin; i < line.size(); ++i) {
                const char c = line[i];

                if (inString) {
                    if (escaped) {
                        escaped = false;
                    }
                    else if (c == '\\') {
                        escaped = true;
                    }
                    else if (c == '"') {
                        inString = false;
                    }
                    continue;
                }

                if (c == '"') {
                    inString = true;
                }
                else if (c == '{') {
                    ++depth;
                }
                else if (c == '}') {
                    --depth;

                    if (depth == 0) {
                        json object = json::parse(
                            line.substr(begin, i - begin + 1),
                            nullptr,
                            false
                        );

                        if (!object.is_discarded() && object.is_object()) {
                            return object;
                        }

                        return std::nullopt;
                    }
                }
            }

            return std::nullopt;
        }
    }

    static long long ClaudeInputTokensFromUsage(const json& usage) {
        if (!usage.is_object()) {
            return 0;
        }

        const long long input = static_cast<long long>(std::max(
            0.0,
            ClaudeNumberAny(usage, { "input_tokens", "inputTokens" }).value_or(0.0)
        ));
        const long long cacheCreate = static_cast<long long>(std::max(
            0.0,
            ClaudeNumberAny(usage, {
                "cache_creation_input_tokens", "cacheCreationInputTokens"
            }).value_or(0.0)
        ));
        const long long cacheRead = static_cast<long long>(std::max(
            0.0,
            ClaudeNumberAny(usage, {
                "cache_read_input_tokens", "cacheReadInputTokens"
            }).value_or(0.0)
        ));

        return input + cacheCreate + cacheRead;
    }

    static long long ClaudeOutputTokensFromUsage(const json& usage) {
        if (!usage.is_object()) {
            return 0;
        }

        const auto direct = ClaudeNumberAny(usage, { "output_tokens", "outputTokens" });
        if (direct) {
            return static_cast<long long>(std::max(0.0, *direct));
        }

        // Some Claude SDK snapshots expose usage first through iterations.
        // Use the newest iteration as a fallback so AQC picks up the first
        // persisted token count as early as the transcript allows.
        if (usage.contains("iterations") && usage.at("iterations").is_array()) {
            const json& iterations = usage.at("iterations");
            for (auto it = iterations.rbegin(); it != iterations.rend(); ++it) {
                if (!it->is_object()) {
                    continue;
                }
                const auto value = ClaudeNumberAny(*it, { "output_tokens", "outputTokens" });
                if (value) {
                    return static_cast<long long>(std::max(0.0, *value));
                }
            }
        }

        return 0;
    }

    static long long ClaudeCacheCreationTokensFromUsage(const json& usage) {
        if (!usage.is_object()) {
            return 0;
        }

        const auto direct = ClaudeNumberAny(usage, {
            "cache_creation_input_tokens", "cacheCreationInputTokens"
        });
        if (direct) {
            return static_cast<long long>(std::max(0.0, *direct));
        }

        return 0;
    }

    static long long ClaudeCacheReadTokensFromUsage(const json& usage) {
        if (!usage.is_object()) {
            return 0;
        }

        const auto direct = ClaudeNumberAny(usage, {
            "cache_read_input_tokens", "cacheReadInputTokens"
        });
        if (direct) {
            return static_cast<long long>(std::max(0.0, *direct));
        }

        return 0;
    }

    static long long ClaudeRawInputTokensFromUsage(const json& usage) {
        if (!usage.is_object()) {
            return 0;
        }

        const auto direct = ClaudeNumberAny(usage, { "input_tokens", "inputTokens" });
        if (direct) {
            return static_cast<long long>(std::max(0.0, *direct));
        }

        if (usage.contains("iterations") && usage.at("iterations").is_array()) {
            const json& iterations = usage.at("iterations");
            for (auto it = iterations.rbegin(); it != iterations.rend(); ++it) {
                if (!it->is_object()) {
                    continue;
                }
                const auto value = ClaudeNumberAny(*it, { "input_tokens", "inputTokens" });
                if (value) {
                    return static_cast<long long>(std::max(0.0, *value));
                }
            }
        }

        return 0;
    }

    static long long ClaudeUsedTokensFromRecordLine(const std::string& line) {
        std::optional<json> contextWindow = ExtractClaudeMetadataObject(
            line,
            "context_window"
        );
        if (!contextWindow) {
            contextWindow = ExtractClaudeMetadataObject(line, "contextWindow");
        }

        if (contextWindow) {
            const json* current = FindClaudeObjectRecursive(
                *contextWindow,
                { "current_usage", "currentUsage" }
            );
            if (current) {
                const long long currentTokens = ClaudeInputTokensFromUsage(*current);
                if (currentTokens > 0) {
                    return currentTokens;
                }
            }

            const auto total = ClaudeNumberAny(
                *contextWindow,
                { "total_input_tokens", "totalInputTokens" }
            );
            if (total && *total > 0.0) {
                return static_cast<long long>(*total);
            }
        }

        const bool assistantRecord =
            line.find("\"type\":\"assistant\"") != std::string::npos ||
            line.find("\"type\": \"assistant\"") != std::string::npos;
        if (assistantRecord) {
            if (auto usage = ExtractClaudeMetadataObject(line, "usage")) {
                return ClaudeInputTokensFromUsage(*usage);
            }
        }

        return 0;
    }

    static bool LooksLikeClaudeAssistantRecord(const std::string& line) {
        return line.find("\"type\":\"assistant\"") != std::string::npos ||
            line.find("\"type\": \"assistant\"") != std::string::npos;
    }

    static bool LooksLikeClaudeSidechainRecord(const std::string& line) {
        return line.find("\"isSidechain\":true") != std::string::npos ||
            line.find("\"isSidechain\": true") != std::string::npos;
    }

    static std::string ExtractClaudeMetadataString(
        const std::string& line,
        const std::string& key
    ) {
        const std::string needle = "\"" + key + "\"";
        size_t searchAt = 0;

        while (true) {
            const size_t keyAt = line.find(needle, searchAt);

            if (keyAt == std::string::npos) {
                return {};
            }

            const size_t colon = line.find(':', keyAt + needle.size());

            if (colon == std::string::npos) {
                return {};
            }

            size_t begin = colon + 1;
            while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin]))) ++begin;

            if (begin >= line.size() || line[begin] != '"') {
                searchAt = keyAt + needle.size();
                continue;
            }

            std::string value;
            bool escaped = false;

            for (size_t i = begin + 1; i < line.size(); ++i) {
                const char c = line[i];

                if (escaped) {
                    value.push_back(c);
                    escaped = false;
                }
                else if (c == '\\') {
                    escaped = true;
                }
                else if (c == '"') {
                    return value;
                }
                else {
                    value.push_back(c);
                }
            }

            return {};
        }
    }

    static long long KnownClaudeContextWindowForModel(const std::string& model) {
        const std::string lower = ToLowerAscii(model);

        if (lower.empty()) {
            return 0;
        }

        if (lower.find("[1m]") != std::string::npos ||
            lower.find("context-1m") != std::string::npos ||
            lower.find("context_1m") != std::string::npos) {
            return 1000000;
        }

        // Claude Code's current Claude 3.x and Claude 4.x model IDs use a
        // 200k standard context window. Restrict the fallback to recognized
        // Claude model IDs so an unknown future model is never guessed.
        if (lower.rfind("claude-3", 0) == 0 || lower.rfind("claude-4", 0) == 0 ||
            lower.find("claude-sonnet-4") != std::string::npos ||
            lower.find("claude-opus-4") != std::string::npos ||
            lower.find("claude-haiku-4") != std::string::npos ||
            lower == "sonnet" || lower == "opus" || lower == "haiku" ||
            lower == "default") {
            return 200000;
        }

        return 0;
    }

    static bool ClaudeEnvTruthy(const std::string& value) {
        const std::string lower = ToLowerAscii(value);
        return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
    }

    static UsageTelemetry::ContextUsage ApplyClaudeAutoCompactTelemetry(
        UsageTelemetry::ContextUsage context
    ) {
        if (!context.valid || context.usedTokens < 0 || context.contextWindowTokens <= 0) {
            return context;
        }

        // Respect explicit process-level disable switches. AIQuotaChecker never
        // changes Claude settings; it only reads the environment it inherited.
        if (ClaudeEnvTruthy(Network::get_instance()->GetEnvText("DISABLE_AUTO_COMPACT"))) {
            return context;
        }

        long long compactWindow = context.contextWindowTokens;
        const std::string configuredWindow =
            Network::get_instance()->GetEnvText("CLAUDE_CODE_AUTO_COMPACT_WINDOW");

        if (!configuredWindow.empty()) {
            char* end = nullptr;
            const long long parsed = std::strtoll(configuredWindow.c_str(), &end, 10);
            if (end != configuredWindow.c_str() && parsed > 0) {
                compactWindow = (std::min)(compactWindow, parsed);
            }
        }

        // Claude Code reserves up to 20k output tokens for the compaction
        // summary, then keeps a 13k safety buffer before auto-compaction.
        // With the baseline algorithm this is 167k for a 200k context; Claude
        // may lower the effective trigger through its own runtime feature flags.
        constexpr long long kSummaryOutputReserve = 20000;
        constexpr long long kAutoCompactBuffer = 13000;
        const long long effectiveWindow = compactWindow -
            (std::min)(compactWindow, kSummaryOutputReserve);

        if (effectiveWindow <= kAutoCompactBuffer) {
            return context;
        }

        long long threshold = effectiveWindow - kAutoCompactBuffer;
        const std::string overrideText =
            Network::get_instance()->GetEnvText("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE");

        if (!overrideText.empty()) {
            char* end = nullptr;
            const double percentage = std::strtod(overrideText.c_str(), &end);
            if (end != overrideText.c_str() && percentage > 0.0 && percentage <= 100.0) {
                const long long overrideThreshold = static_cast<long long>(
                    std::floor(static_cast<double>(compactWindow) * percentage / 100.0)
                );
                if (overrideThreshold > 0) {
                    threshold = (std::min)(threshold, overrideThreshold);
                }
            }
        }

        if (threshold <= 0) {
            return context;
        }

        const double percentLeft =
            (static_cast<double>(threshold - context.usedTokens) /
                static_cast<double>(threshold)) * 100.0;

        context.autoCompactPercentValid = true;
        context.autoCompactPercentLeft = static_cast<int>(std::clamp(
            std::llround(percentLeft),
            0LL,
            100LL
        ));
        context.autoCompactThresholdTokens = threshold;
        return context;
    }

    static bool ClaudeCompactionStateFromLine(
        const std::string& line,
        bool& state
    ) {
        const std::string lower = ToLowerAscii(line);
        if (lower.find("compact") == std::string::npos) {
            return false;
        }

        const std::string type = ToLowerAscii(ExtractClaudeMetadataString(line, "type"));
        const std::string subtype = ToLowerAscii(ExtractClaudeMetadataString(line, "subtype"));
        const std::string status = ToLowerAscii(ExtractClaudeMetadataString(line, "status"));
        const std::string phase = ToLowerAscii(ExtractClaudeMetadataString(line, "phase"));
        const std::string combined = type + " " + subtype + " " + status + " " + phase;

        const bool completed =
            combined.find("compact_boundary") != std::string::npos ||
            combined.find("compaction_completed") != std::string::npos ||
            combined.find("compact_completed") != std::string::npos ||
            combined.find("context_compacted") != std::string::npos ||
            lower.find("<local-command-stdout>compacted") != std::string::npos ||
            lower.find("conversation compacted") != std::string::npos ||
            ((combined.find("compact") != std::string::npos) &&
                (status == "completed" || status == "complete" || status == "done" ||
                    phase == "completed" || phase == "complete" || phase == "done")) ||
            lower.find("\"completed\":true") != std::string::npos ||
            lower.find("\"completed\": true") != std::string::npos;

        if (completed) {
            state = false;
            return true;
        }

        // Manual /compact is persisted as a local command before the eventual
        // compact_boundary. Recognize that command so the UI can show the
        // transient state while Claude is actually generating the summary.
        const bool manualCommand =
            lower.find("<command-name>/compact</command-name>") != std::string::npos ||
            lower.find("<command-message>compact</command-message>") != std::string::npos ||
            ((type == "user" || type == "system") && subtype == "local_command" &&
                (lower.find("/compact") != std::string::npos ||
                    lower.find("command\":\"compact") != std::string::npos));

        const bool running = manualCommand ||
            combined.find("compaction_started") != std::string::npos ||
            combined.find("compact_started") != std::string::npos ||
            combined.find("context_compaction") != std::string::npos ||
            combined.find("auto_compact") != std::string::npos ||
            combined.find("autocompact") != std::string::npos ||
            combined.find("pre_compact") != std::string::npos ||
            combined.find("precompact") != std::string::npos ||
            combined.find("compacting") != std::string::npos ||
            ((combined.find("compact") != std::string::npos) &&
                (status == "running" || status == "started" || status == "in_progress" ||
                    phase == "running" || phase == "started" || phase == "in_progress")) ||
            lower.find("\"completed\":false") != std::string::npos ||
            lower.find("\"completed\": false") != std::string::npos;

        if (running) {
            state = true;
            return true;
        }

        return false;
    }

    static bool IsClaudeCompactBoundaryLine(const std::string& line) {
        if (line.find("compact_boundary") == std::string::npos) {
            return false;
        }

        return ToLowerAscii(ExtractClaudeMetadataString(line, "type")) == "system" &&
            ToLowerAscii(ExtractClaudeMetadataString(line, "subtype")) == "compact_boundary";
    }

    static bool ClaudeJsonBool(const json& object, const char* key) {
        return object.is_object() && object.contains(key) &&
            object.at(key).is_boolean() && object.at(key).get<bool>();
    }

    static bool ClaudeUserRecordStartsRun(const json& record) {
        if (!record.is_object() || record.value("type", std::string{}) != "user" ||
            ClaudeJsonBool(record, "isSidechain") || ClaudeJsonBool(record, "isSynthetic") ||
            ClaudeJsonBool(record, "isMeta") || ClaudeJsonBool(record, "isCompactSummary") ||
            ClaudeJsonBool(record, "isVisibleInTranscriptOnly")) {
            return false;
        }

        if (!record.contains("message") || !record.at("message").is_object()) {
            return false;
        }

        const json& message = record.at("message");
        if (!message.contains("content")) {
            return true;
        }

        const json& content = message.at("content");
        if (!content.is_array()) {
            return true;
        }

        bool hasToolResult = false;
        bool hasUserContent = false;

        for (const json& block : content) {
            if (!block.is_object()) {
                continue;
            }

            const std::string type = block.value("type", std::string{});
            if (type == "tool_result") {
                hasToolResult = true;
            }
            else if (type == "text" || type == "image" || type == "document") {
                hasUserContent = true;
            }
        }

        // Tool-result records are emitted as role=user while a turn is already
        // running. They must not reset the start time of the current run.
        return !hasToolResult || hasUserContent;
    }

    static UsageTelemetry::RunUsage ReadClaudeRunFromLines(
        const std::vector<std::string>& lines
    ) {
        UsageTelemetry::RunUsage run;
        size_t runStart = std::string::npos;

        // Claude Desktop does not consistently persist a top-level "result"
        // record. In the captured Desktop transcripts an idle/finished turn is
        // instead terminated by an assistant message whose stop_reason is
        // "end_turn". Check for that marker while walking backwards, otherwise
        // the last user prompt remains "running" forever after Claude is idle.
        for (size_t i = lines.size(); i-- > 0;) {
            const std::string& line = lines[i];

            if (LooksLikeClaudeSidechainRecord(line)) {
                continue;
            }

            const bool mayBeUser = line.find("\"type\":\"user\"") != std::string::npos ||
                line.find("\"type\": \"user\"") != std::string::npos;
            const bool mayBeResult = line.find("\"type\":\"result\"") != std::string::npos ||
                line.find("\"type\": \"result\"") != std::string::npos;
            const bool mayBeAssistant =
                line.find("\"type\":\"assistant\"") != std::string::npos ||
                line.find("\"type\": \"assistant\"") != std::string::npos;

            if (!mayBeUser && !mayBeResult && !mayBeAssistant) {
                continue;
            }

            json record = json::parse(line, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const std::string type = record.value("type", std::string{});
            if (type == "result") {
                return run;
            }

            if (type == "assistant" && record.contains("message") &&
                record.at("message").is_object()) {
                const json& message = record.at("message");
                if (message.value("stop_reason", std::string{}) == "end_turn") {
                    return run;
                }
            }

            if (type == "user" && ClaudeUserRecordStartsRun(record)) {
                runStart = i;
                run.valid = true;
                run.running = true;
                // Claude starts the visible thinking timer as soon as the user
                // submits the turn, before the first assistant usage snapshot
                // is persisted. Treat that pre-snapshot interval as THINKING.
                run.thinking = true;

                if (record.contains("timestamp")) {
                    run.startedAtUnixSeconds = TimePointToUnixSeconds(
                        ParseTimeFlexible(record.at("timestamp"))
                    );
                    run.thinkingStartedAtUnixSeconds = run.startedAtUnixSeconds;
                }
                break;
            }
        }

        if (runStart == std::string::npos) {
            return run;
        }

        // Claude appends repeated snapshots for the same API message while it
        // is being generated. Track the latest output count per message ID:
        // currentTokens mirrors Claude Desktop's live "x.xk tokens", while
        // tokens is the cumulative generated-token total for the whole turn.
        std::unordered_map<std::string, long long> apiMessageOutputTokens;

        auto recordTimestamp = [](const json& record) -> long long {
            if (!record.contains("timestamp")) {
                return 0;
            }
            return TimePointToUnixSeconds(ParseTimeFlexible(record.at("timestamp")));
        };

        auto beginThinking = [&](long long at) {
            if (!run.thinking) {
                run.thinking = true;
                run.thinkingStartedAtUnixSeconds = at > 0 ? at : run.startedAtUnixSeconds;
            }
            else if (run.thinkingStartedAtUnixSeconds <= 0) {
                run.thinkingStartedAtUnixSeconds = at > 0 ? at : run.startedAtUnixSeconds;
            }
        };

        auto finishThinking = [&](long long at) {
            if (run.thinking && run.thinkingStartedAtUnixSeconds > 0 &&
                at >= run.thinkingStartedAtUnixSeconds) {
                // The first thought begins at the user prompt. Later thoughts
                // begin when a tool_result hands control back to the model, so
                // measure each reasoning cycle from its own start instead of
                // accidentally reusing the whole-turn timer.
                run.lastThoughtDurationSeconds = at - run.thinkingStartedAtUnixSeconds;
                run.lastThoughtCompletedAtUnixSeconds = at;
            }
            run.thinking = false;
        };

        for (size_t i = runStart + 1; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (LooksLikeClaudeSidechainRecord(line)) {
                continue;
            }

            const bool mayAffectRun =
                line.find("\"type\":\"assistant\"") != std::string::npos ||
                line.find("\"type\": \"assistant\"") != std::string::npos ||
                line.find("\"type\":\"tool_progress\"") != std::string::npos ||
                line.find("\"type\": \"tool_progress\"") != std::string::npos ||
                line.find("\"type\":\"tool_use_summary\"") != std::string::npos ||
                line.find("\"type\": \"tool_use_summary\"") != std::string::npos ||
                line.find("\"type\":\"stream_event\"") != std::string::npos ||
                line.find("\"type\": \"stream_event\"") != std::string::npos ||
                line.find("\"type\":\"user\"") != std::string::npos ||
                line.find("\"type\": \"user\"") != std::string::npos;

            if (!mayAffectRun) {
                continue;
            }

            json record = json::parse(line, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const std::string type = record.value("type", std::string{});

            if (type == "stream_event") {
                // Claude Desktop enables partial SDK messages. When those raw
                // stream events are present in the transcript they provide the
                // closest passive equivalent to Desktop's live thinking state.
                // Fall back to completed assistant content below when a build
                // does not persist partial events.
                if (record.contains("event") && record.at("event").is_object()) {
                    const json& event = record.at("event");
                    const std::string eventType = event.value("type", std::string{});

                    if (eventType == "content_block_delta" &&
                        event.contains("delta") && event.at("delta").is_object()) {
                        const std::string deltaType =
                            event.at("delta").value("type", std::string{});

                        const long long at = recordTimestamp(record);
                        if (deltaType == "thinking_delta") {
                            beginThinking(at);
                        }
                        else if (deltaType == "text_delta" ||
                            deltaType == "input_json_delta") {
                            finishThinking(at);
                        }
                    }
                    else if (eventType == "content_block_start" &&
                        event.contains("content_block") &&
                        event.at("content_block").is_object()) {
                        const std::string blockType =
                            event.at("content_block").value("type", std::string{});
                        const long long at = recordTimestamp(record);
                        if (blockType == "thinking" || blockType == "redacted_thinking") {
                            beginThinking(at);
                        }
                        else if (blockType == "text" || blockType == "tool_use") {
                            finishThinking(at);
                        }
                    }
                    else if (eventType == "message_stop") {
                        finishThinking(recordTimestamp(record));
                    }
                }
                continue;
            }

            if (type == "tool_progress" || type == "tool_use_summary") {
                finishThinking(recordTimestamp(record));
                continue;
            }

            if (type == "user") {
                // Tool-result records mark the hand-off into Claude's next
                // model cycle. They are the earliest persisted signal that the
                // THOUGHT FOR state should flip back to THINKING.
                const json* message = record.contains("message") && record.at("message").is_object()
                    ? &record.at("message")
                    : nullptr;

                if (message && message->contains("content") && message->at("content").is_array()) {
                    for (const json& block : message->at("content")) {
                        if (block.is_object() && block.value("type", std::string{}) == "tool_result") {
                            // Once the tool result is appended Claude immediately
                            // starts the next model cycle. Current-message token
                            // stats belong to the API message that just ended, so
                            // clear them until Claude persists the next usage
                            // snapshot. The cumulative turn total is retained.
                            run.tokenStatsValid = false;
                            run.currentTokens = 0;
                            run.inputTokens = 0;
                            run.rawInputTokens = 0;
                            run.cacheCreationInputTokens = 0;
                            run.cacheReadInputTokens = 0;
                            beginThinking(recordTimestamp(record));
                            break;
                        }
                    }
                }
                continue;
            }

            if (type != "assistant" || !record.contains("message") ||
                !record.at("message").is_object()) {
                continue;
            }

            const json& message = record.at("message");
            const std::string messageId = message.value("id", std::string{});

            if (message.contains("usage") && message.at("usage").is_object()) {
                const std::string dedupeId = messageId.empty()
                    ? record.value("uuid", std::string{})
                    : messageId;
                const json& usage = message.at("usage");
                const long long output = ClaudeOutputTokensFromUsage(usage);

                run.tokenStatsValid = true;
                run.currentTokens = output;
                run.inputTokens = ClaudeInputTokensFromUsage(usage);
                run.rawInputTokens = ClaudeRawInputTokensFromUsage(usage);
                run.cacheCreationInputTokens = ClaudeCacheCreationTokensFromUsage(usage);
                run.cacheReadInputTokens = ClaudeCacheReadTokensFromUsage(usage);

                if (dedupeId.empty()) {
                    // No stable ID means this record cannot be correlated with
                    // later snapshots. These are rare; count the visible output
                    // once rather than mixing input/cache tokens into the total.
                    run.tokens += output;
                }
                else {
                    long long& previous = apiMessageOutputTokens[dedupeId];
                    if (output > previous) {
                        run.tokens += output - previous;
                        previous = output;
                    }
                }
            }

            bool sawActivityBlock = false;
            bool latestBlockThinking = run.thinking;

            if (message.contains("content") && message.at("content").is_array()) {
                for (const json& block : message.at("content")) {
                    if (!block.is_object()) {
                        continue;
                    }

                    const std::string blockType = block.value("type", std::string{});
                    if (blockType == "thinking" || blockType == "redacted_thinking") {
                        sawActivityBlock = true;
                        latestBlockThinking = true;
                    }
                    else if (blockType == "text" || blockType == "tool_use") {
                        sawActivityBlock = true;
                        latestBlockThinking = false;
                    }
                }
            }

            if (sawActivityBlock) {
                const long long at = recordTimestamp(record);
                if (latestBlockThinking) {
                    beginThinking(at);
                }
                else {
                    finishThinking(at);
                }
            }

            // Do not force thinking=false merely because stop_reason is
            // "tool_use". Claude persists that value on the same assistant
            // snapshots that contain a thinking block. The following text/tool
            // record is the reliable transition out of the thought. end_turn
            // is handled by the reverse scan above.

        }

        return run;
    }

    static LocalTelemetry ReadClaudeLocalTelemetryFromSession(
        const std::filesystem::path& session,
        long long contextWindowHint = 0
    ) {
        LocalTelemetry local;
        UsageTelemetry::ContextUsage& context = local.context;
        const std::string text = ReadClaudeTailSharedReadOnly(
            session,
            8ULL * 1024ULL * 1024ULL
        );

        if (text.empty()) {
            return local;
        }

        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;

        while (std::getline(stream, line)) {
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
        }

        local.run = ReadClaudeRunFromLines(lines);

        long long usedTokens = 0;
        long long contextWindowTokens = 0;
        std::string model;
        bool modelWindowFallback = false;
        bool compacting = false;
        size_t lastCompactBoundary = std::string::npos;

        // Process compaction metadata in file order. Most importantly, remember
        // the last real compact_boundary so context usage before that boundary
        // can never win the reverse scan after compaction completes.
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& candidate = lines[i];
            if (LooksLikeClaudeSidechainRecord(candidate)) {
                continue;
            }

            ClaudeCompactionStateFromLine(candidate, compacting);

            if (IsClaudeCompactBoundaryLine(candidate)) {
                lastCompactBoundary = i;
                context.compactionEventId = ExtractClaudeMetadataString(candidate, "uuid");

                const std::string timestamp = ExtractClaudeMetadataString(candidate, "timestamp");
                if (context.compactionEventId.empty() && !timestamp.empty()) {
                    context.compactionEventId = timestamp;
                }
                if (!timestamp.empty()) {
                    context.compactionCompletedAtUnixSeconds = TimePointToUnixSeconds(
                        ParseTimeFlexible(json(timestamp))
                    );
                }

                std::optional<json> metadata = ExtractClaudeMetadataObject(
                    candidate,
                    "compactMetadata"
                );

                if (!metadata) {
                    metadata = ExtractClaudeMetadataObject(candidate, "compact_metadata");
                }

                if (metadata) {
                    const auto pre = FindClaudeNumberRecursive(
                        *metadata,
                        { "preTokens", "pre_tokens" }
                    );
                    if (pre && *pre > 0.0) {
                        context.compactionPreTokens = static_cast<long long>(*pre);
                    }

                    const auto post = FindClaudeNumberRecursive(
                        *metadata,
                        { "postTokens", "post_tokens" }
                    );

                    const auto saved = FindClaudeNumberRecursive(
                        *metadata,
                        { "savedTokens", "saved_tokens" }
                    );
                    if (saved && *saved > 0.0) {
                        context.compactionSavedTokens = static_cast<long long>(*saved);
                    }
                    else if (pre && post && *pre > *post && *post >= 0.0) {
                        // Current Claude Desktop compact_boundary records persist
                        // both preTokens and postTokens. Prefer that exact pair
                        // over subtracting a later context sample after work has
                        // already resumed.
                        context.compactionSavedTokens =
                            static_cast<long long>(*pre - *post);
                    }
                }
            }
        }

        context.compacting = compacting;
        const size_t scanBegin = lastCompactBoundary == std::string::npos
            ? 0
            : lastCompactBoundary + 1;

        // compactMetadata in current Desktop builds exposes preserved-segment
        // information but does not guarantee a saved-token scalar. Capture the
        // last real context count before the boundary so saved tokens can be
        // calculated as pre - post when the new usage record arrives.
        if (lastCompactBoundary != std::string::npos &&
            context.compactionPreTokens <= 0) {
            for (size_t i = lastCompactBoundary; i-- > 0;) {
                const std::string& candidate = lines[i];
                if (LooksLikeClaudeSidechainRecord(candidate)) {
                    continue;
                }
                const long long previousUsed = ClaudeUsedTokensFromRecordLine(candidate);
                if (previousUsed > 0) {
                    context.compactionPreTokens = previousUsed;
                    break;
                }
            }
        }

        // Claude Desktop's own transcript reader drops pre-boundary messages
        // after a compact_boundary. Do the same for the live context meter so a
        // 157k pre-compaction usage record cannot remain visible while Claude's
        // UI has already fallen back to the new compacted context.
        for (size_t i = lines.size(); i-- > scanBegin;) {
            const std::string& candidate = lines[i];
            const bool relevant =
                candidate.find("\"usage\"") != std::string::npos ||
                candidate.find("context_window") != std::string::npos ||
                candidate.find("contextWindow") != std::string::npos ||
                candidate.find("modelUsage") != std::string::npos ||
                candidate.find("\"model\"") != std::string::npos;

            if (!relevant || LooksLikeClaudeSidechainRecord(candidate)) {
                continue;
            }

            std::optional<json> contextWindow = ExtractClaudeMetadataObject(
                candidate,
                "context_window"
            );

            if (!contextWindow) {
                contextWindow = ExtractClaudeMetadataObject(candidate, "contextWindow");
            }

            if (contextWindow) {
                if (contextWindowTokens <= 0) {
                    const auto maximum = ClaudeNumberAny(
                        *contextWindow,
                        { "context_window_size", "contextWindowSize" }
                    );

                    if (maximum && *maximum > 0.0) {
                        contextWindowTokens = static_cast<long long>(*maximum);
                    }
                }

                if (usedTokens <= 0) {
                    const json* current = FindClaudeObjectRecursive(
                        *contextWindow,
                        { "current_usage", "currentUsage" }
                    );

                    if (current) {
                        usedTokens = ClaudeInputTokensFromUsage(*current);
                    }

                    if (usedTokens <= 0) {
                        const auto total = ClaudeNumberAny(
                            *contextWindow,
                            { "total_input_tokens", "totalInputTokens" }
                        );

                        if (total && *total > 0.0) {
                            usedTokens = static_cast<long long>(*total);
                        }
                    }
                }
            }

            if (usedTokens <= 0) {
                usedTokens = ClaudeUsedTokensFromRecordLine(candidate);
            }

            if (model.empty()) {
                model = ExtractClaudeMetadataString(candidate, "model");
            }

            if (contextWindowTokens <= 0) {
                if (auto modelUsage = ExtractClaudeMetadataObject(candidate, "modelUsage")) {
                    const auto maximum = FindClaudeNumberRecursive(
                        *modelUsage,
                        {
                            "context_window_size", "contextWindowSize",
                            "model_context_window", "modelContextWindow",
                            "contextWindow"
                        }
                    );

                    if (maximum && *maximum > 0.0) {
                        contextWindowTokens = static_cast<long long>(*maximum);
                    }
                }
            }

            if (usedTokens > 0 && contextWindowTokens > 0 && !model.empty()) {
                break;
            }
        }

        // The compacted segment may not repeat model/window metadata. Those are
        // stable session properties, so they are safe to recover from earlier
        // records; only the *used token count* is forbidden from crossing the
        // compact boundary.
        if (model.empty() || contextWindowTokens <= 0) {
            for (size_t i = lines.size(); i-- > 0;) {
                const std::string& candidate = lines[i];
                if (LooksLikeClaudeSidechainRecord(candidate)) {
                    continue;
                }

                if (model.empty()) {
                    model = ExtractClaudeMetadataString(candidate, "model");
                }

                if (contextWindowTokens <= 0) {
                    std::optional<json> contextWindow = ExtractClaudeMetadataObject(
                        candidate,
                        "context_window"
                    );
                    if (!contextWindow) {
                        contextWindow = ExtractClaudeMetadataObject(candidate, "contextWindow");
                    }

                    if (contextWindow) {
                        const auto maximum = ClaudeNumberAny(
                            *contextWindow,
                            { "context_window_size", "contextWindowSize" }
                        );
                        if (maximum && *maximum > 0.0) {
                            contextWindowTokens = static_cast<long long>(*maximum);
                        }
                    }
                }

                if (!model.empty() && contextWindowTokens > 0) {
                    break;
                }
            }
        }

        if (contextWindowTokens <= 0 && contextWindowHint > 0) {
            contextWindowTokens = contextWindowHint;
            modelWindowFallback = false;
        }

        if (contextWindowTokens <= 0) {
            contextWindowTokens = KnownClaudeContextWindowForModel(model);
            modelWindowFallback = contextWindowTokens > 0;
        }

        if (usedTokens <= 0 || contextWindowTokens <= 0) {
            // compact_boundary means compaction has FINISHED. Do not turn it
            // back into an in-progress state merely because the first new
            // usage record has not been flushed yet. The provider preserves
            // the old bar while it latches the completion notice.
            if (lastCompactBoundary != std::string::npos) {
                context.compacting = false;
            }
            return local;
        }

        if (context.compactionSavedTokens <= 0 &&
            context.compactionPreTokens > usedTokens) {
            context.compactionSavedTokens = context.compactionPreTokens - usedTokens;
        }

        context.valid = true;
        context.compacting = compacting;
        context.usedTokens = usedTokens;
        context.contextWindowTokens = contextWindowTokens;
        context.model = model;
        context.sourceLabel = modelWindowFallback
            ? "Claude Code session usage + recognized model context limit"
            : "Claude Code session context metadata / selected model · direct read-only";
        // A guessed model-name fallback is useful for showing a last-resort
        // context bar, but it must never manufacture an auto-compact edge.
        // Claude Desktop's selected [1m] state or persisted context metadata is
        // authoritative; unknown/future model limits remain model-agnostic.
        if (!modelWindowFallback) {
            context = ApplyClaudeAutoCompactTelemetry(std::move(context));
        }
        return local;
    }

    static LocalTelemetry ReadLatestClaudeLocalTelemetry() {
        ClaudeDesktopSessionHint desktopHint = LatestClaudeDesktopSessionHint();
        const long long desktopContextWindowHint =
            ClaudeDesktopSelectedContextWindowHint(desktopHint);
        std::vector<ClaudeSessionCandidate> candidates = LatestClaudeSessionFiles();

        // The supplied Desktop ASAR persists cliSessionId in
        // LOCALAPPDATA\\Claude-3p\\claude-code-sessions even though isRunning
        // itself is intentionally NOT serialized. Use the ID only to pick the
        // correct foreground transcript; never treat the session JSON as an
        // authoritative running flag.
        if (desktopHint.valid && !desktopHint.cliSessionId.empty()) {
            std::stable_sort(
                candidates.begin(),
                candidates.end(),
                [&](const ClaudeSessionCandidate& left, const ClaudeSessionCandidate& right) {
                    const bool leftMatch =
                        left.path.stem().string() == desktopHint.cliSessionId;
                    const bool rightMatch =
                        right.path.stem().string() == desktopHint.cliSessionId;
                    return leftMatch && !rightMatch;
                }
            );
        }

        for (const ClaudeSessionCandidate& candidate : candidates) {
            const bool matchesDesktopHint = desktopHint.valid &&
                !desktopHint.cliSessionId.empty() &&
                candidate.path.stem().string() == desktopHint.cliSessionId;
            LocalTelemetry local = ReadClaudeLocalTelemetryFromSession(
                candidate.path,
                matchesDesktopHint ? desktopContextWindowHint : 0
            );

            if (matchesDesktopHint && !local.run.running) {
                const auto age = std::filesystem::file_time_type::clock::now() -
                    desktopHint.writeTime;
                const auto ageSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(age).count();
                const auto desktopLeadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    desktopHint.writeTime - candidate.writeTime
                ).count();

                // saveSession() runs immediately when Desktop enqueues a turn,
                // while the CLI JSONL can trail that write. The persisted JSON
                // does not contain isRunning, so use this only as a very short
                // bridge until the transcript itself exposes the user/stream
                // records. This is intentionally not latched for normal runs.
                if (ageSeconds >= -2 && ageSeconds <= 8 && desktopLeadMs >= 250) {
                    const long long now = static_cast<long long>(std::time(nullptr));
                    local.run.valid = true;
                    local.run.running = true;
                    local.run.thinking = true;
                    local.run.startedAtUnixSeconds = now - (std::max)(0LL, static_cast<long long>(ageSeconds));
                }
            }

            // Compaction state comes only from explicit Claude transcript/provider
            // markers. Reaching an estimated threshold is not proof that a
            // compact operation is actually running.

            if (local.context.valid || local.context.compacting || local.run.running) {
                return local;
            }
        }

        return {};
    }

    LocalTelemetry ReadLocalTelemetry() {
        return ReadLatestClaudeLocalTelemetry();
    }

    UsageTelemetry::ContextUsage ReadLocalContextUsage() {
        return ReadLatestClaudeLocalTelemetry().context;
    }

    static void FinalizeClaudeAccess(Snapshot& snapshot) {
        std::vector<std::string> blockingRateLimits;
        std::vector<std::string> partialRateLimits;

        auto collect = [&](const UsageWindow& window, bool blocksAllUsage) {
            if (!window.valid || !UsageTelemetry::IsExhausted(window.usedPercent)) {
                return;
            }

            (blocksAllUsage ? blockingRateLimits : partialRateLimits).push_back(
                window.title.empty() ? "Usage" : window.title
            );
        };

        collect(snapshot.currentSession, true);
        collect(snapshot.weeklyAllModels, true);
        collect(snapshot.weeklySonnet, false);
        collect(snapshot.weeklyFable, false);

        for (const UsageWindow& limit : snapshot.additionalLimits) {
            collect(limit, false);
        }

        auto join = [](const std::vector<std::string>& values) {
            std::string text;

            for (size_t i = 0; i < values.size(); ++i) {
                if (i != 0) text += ", ";
                text += values[i];
            }

            return text;
        };

        // A depleted plan window or credit allocation means the account cannot
        // continue until reset/top-up, so it is OUT OF USAGE. RATE LIMITED is
        // reserved for transient request throttling such as HTTP 429.
        if (snapshot.credits.valid && snapshot.credits.enabled &&
            snapshot.credits.hasUsedPercent &&
            UsageTelemetry::IsExhausted(snapshot.credits.usedPercent)) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = snapshot.credits.label.empty()
                ? "Usage credits exhausted"
                : snapshot.credits.label + " exhausted";
            return;
        }

        if (!blockingRateLimits.empty()) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = "Usage exhausted: " + join(blockingRateLimits);
            return;
        }

        UsageTelemetry::SetAvailable(snapshot.access);

        if (!partialRateLimits.empty()) {
            snapshot.access.detail = "Some limits are exhausted: " + join(partialRateLimits);
        }
    }

    static Snapshot ParseSnapshot(const json& root, const ClaudeOAuth& oauth, const std::string& usageHeading) {
        Snapshot snapshot;

        snapshot.usageHeading = usageHeading;
        snapshot.plan = FormatPlan(oauth);
        snapshot.statusText = "Plan: " + snapshot.plan;
        snapshot.lastUpdated = "just now";

        const bool hasServerDrivenLimits = ParseServerDrivenLimits(root, snapshot);

        if (!hasServerDrivenLimits) {
            snapshot.currentSession = ParseWindow(
                root,
                "five_hour",
                "Session",
                "Starts when a message is sent"
            );

            snapshot.weeklyAllModels = ParseWindow(
                root,
                "seven_day",
                "Weekly",
                "All models"
            );

            snapshot.weeklySonnet = ParseWindow(
                root,
                "seven_day_sonnet",
                "Sonnet",
                "Sonnet only"
            );

            snapshot.weeklyFable = ParseFirstAvailableWindow(
                root,
                { "seven_day_omelette", "omelette_promotional", "seven_day_fable" },
                "Fable",
                "Fable"
            );

            AddAdditionalLimit(
                snapshot,
                ParseWindow(root, "seven_day_opus", "Opus", "Opus only")
            );

            AddAdditionalLimit(
                snapshot,
                ParseWindow(root, "seven_day_oauth_apps", "OAuth apps", "App integrations")
            );

            AddAdditionalLimit(
                snapshot,
                ParseWindow(root, "seven_day_cowork", "Cowork", "Cowork only")
            );
        }
        else if (!snapshot.weeklyFable.valid) {
            snapshot.weeklyFable = ParseWindow(
                root,
                "omelette_promotional",
                "Fable",
                "Fable"
            );
        }

        snapshot.credits = ParseCredits(root, oauth);
        FinalizeClaudeAccess(snapshot);
        return snapshot;
    }

    static Network::HttpResponse FetchDesktopOrganizationUsage(
        const ClaudeDesktopAuth::Result& desktop,
        const std::string& baseUrl,
        bool explicitSpend
    ) {
        const std::string cookie = Network::get_instance()->StripHeaderValue(desktop.cookieHeader);

        if (cookie.empty()) {
            return {};
        }

        std::string headers;
        headers += "Accept: application/json\r\n";
        headers += "Cache-Control: no-cache, no-store, max-age=0\r\n";
        headers += "Pragma: no-cache\r\n";
        headers += "Cookie: " + cookie + "\r\n";
        headers += "Referer: " + baseUrl + "/settings/usage\r\n";
        headers += "Origin: " + baseUrl + "\r\n";
        headers += "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Claude/1.25927.0\r\n";

        std::string url = baseUrl + "/api/organizations/" + desktop.organizationId + "/usage";

        // Claude Desktop normally omits the query for a full-spend refresh.
        // The explicit false form is retained as a compatibility retry for
        // deployments that distinguish it from an omitted query parameter.
        if (explicitSpend) {
            url += "?skip_spend=0";
        }

        return Network::get_instance()->RequestUrl(
            url,
            "GET",
            Network::get_instance()->Utf8ToWide(headers)
        );
    }

    static Network::HttpResponse FetchDesktopUsage(const ClaudeDesktopAuth::Result& desktop) {
        const std::string baseUrl = desktop.baseUrl.empty() ? "https://claude.ai" : desktop.baseUrl;
        return FetchDesktopOrganizationUsage(desktop, baseUrl, false);
    }

    static bool MergeCreditsFromBody(
        Snapshot& snapshot,
        const std::string& body,
        const ClaudeOAuth& oauth
    ) {
        if (body.empty()) {
            return false;
        }

        json parsed = JsonUtils::get_instance()->ParseOrNull(body);

        if (!parsed.is_object() && !parsed.is_array()) {
            return false;
        }

        UsageCredits credits = ParseCredits(parsed, oauth);

        if (credits.reported &&
            CreditsInformationScore(credits) > CreditsInformationScore(snapshot.credits)) {
            snapshot.credits = std::move(credits);
            return true;
        }

        return false;
    }

    static Network::HttpResponse FetchDesktopUsageFromBase(
        const ClaudeDesktopAuth::Result& desktop,
        const std::string& baseUrl,
        bool explicitSpend = false
    ) {
        return FetchDesktopOrganizationUsage(
            desktop,
            baseUrl,
            explicitSpend
        );
    }

    static void FetchDesktopCreditsSupplement(
        Snapshot& snapshot,
        const ClaudeDesktopAuth::Result& desktop,
        const ClaudeOAuth& desktopPlan
    ) {
        if (!CreditsNeedSupplement(snapshot.credits)) {
            return;
        }

        const std::string primaryBase = desktop.baseUrl.empty()
            ? "https://claude.ai"
            : desktop.baseUrl;

        auto mergeSuccessful = [&](const Network::HttpResponse& response) {
            if (response.statusCode >= 200 && response.statusCode < 300) {
                MergeCreditsFromBody(snapshot, response.body, desktopPlan);
            }
        };

        if (desktop.source == ClaudeDesktopAuth::AuthSource::BrowserCookies) {
            // A full-spend refresh normally has no query. Retry with an
            // explicit skip_spend=0 only when the successful primary response
            // omitted extra_usage.
            mergeSuccessful(FetchDesktopUsageFromBase(
                desktop,
                primaryBase,
                true
            ));
        }

        // The matching Desktop OAuth cache belongs to the same active
        // organization. The token remains in memory only.
        if (CreditsNeedSupplement(snapshot.credits) &&
            desktop.source == ClaudeDesktopAuth::AuthSource::BrowserCookies &&
            !desktop.accessToken.empty()) {
            mergeSuccessful(FetchUsage(desktop.accessToken));
        }

        if (!CreditsNeedSupplement(snapshot.credits) ||
            desktop.source != ClaudeDesktopAuth::AuthSource::BrowserCookies) {
            return;
        }

        // Claude has migrated between claude.ai and claude.com. Query the
        // alternate official host with the same in-memory authentication and
        // merge only extra_usage; no credentials are persisted or copied.
        const std::string alternate = primaryBase.find("claude.com") != std::string::npos
            ? "https://claude.ai"
            : "https://claude.com";

        mergeSuccessful(FetchDesktopUsageFromBase(
            desktop,
            alternate,
            false
        ));
    }

    static Snapshot FailedSnapshot(
        const std::string& heading,
        const std::string& status,
        int statusCode = 0,
        const std::string& responseBody = {}
    ) {
        Snapshot snapshot;
        snapshot.usageHeading = heading;
        snapshot.statusText = status;
        snapshot.lastUpdated = "now";
        snapshot.access = statusCode > 0
            ? UsageTelemetry::FromHttpFailure(statusCode, responseBody, status)
            : UsageTelemetry::FromText(status);
        return snapshot;
    }

    static Snapshot FetchDesktopSnapshot(const ClaudeDesktopAuth::Result& initialDesktop) {
        ClaudeDesktopAuth::Result desktop = initialDesktop;
        ClaudeOAuth desktopPlan;
        desktopPlan.subscriptionType = desktop.subscriptionType;
        desktopPlan.rateLimitTier = desktop.rateLimitTier;

        Network::HttpResponse response;

        if (desktop.source == ClaudeDesktopAuth::AuthSource::OAuthCache) {
            response = FetchUsage(desktop.accessToken);
        }
        else {
            response = FetchDesktopUsage(desktop);
        }

        if (desktop.source == ClaudeDesktopAuth::AuthSource::BrowserCookies &&
            (response.statusCode == 401 || response.statusCode == 403)) {
            // Re-read the cookie DB in case the account changed between the
            // first read and the request.
            ClaudeDesktopAuth::Result current = ClaudeDesktopAuth::AcquireCurrentSession();

            if (current.kind == ClaudeDesktopAuth::ResultKind::Success) {
                if (current.source == ClaudeDesktopAuth::AuthSource::OAuthCache) {
                    response = FetchUsage(current.accessToken);
                }
                else {
                    response = FetchDesktopUsage(current);
                }

                desktop = current;
                desktopPlan.subscriptionType = desktop.subscriptionType;
                desktopPlan.rateLimitTier = desktop.rateLimitTier;
            }
        }

        if (response.statusCode == 429) {
            return FailedSnapshot(
                "Claude Desktop usage",
                "Claude Desktop usage request is rate limited",
                response.statusCode,
                response.body
            );
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            return FailedSnapshot(
                "Claude Desktop usage",
                "Claude Desktop usage failed: HTTP " + std::to_string(response.statusCode),
                response.statusCode,
                response.body
            );
        }

        Snapshot snapshot = ParseSnapshot(
            JsonUtils::get_instance()->ParseRequired(response.body),
            desktopPlan,
            "Claude Desktop usage"
        );

        // Keep notification state separate for each Desktop organization.
        // The organization ID is already required for the usage request and is
        // retained only in memory.
        snapshot.accountKey = "desktop:" + desktop.organizationId;
        FetchDesktopCreditsSupplement(snapshot, desktop, desktopPlan);
        // Supplemental spend data can change availability independently from
        // Session/Weekly limits, so classify access again after the merge.
        FinalizeClaudeAccess(snapshot);
        snapshot.statusText += desktop.source == ClaudeDesktopAuth::AuthSource::OAuthCache
            ? " | Source: Desktop OAuth cache"
            : " | Source: Desktop live session";
        return snapshot;
    }

    static Snapshot FetchCodeSnapshot(
        ClaudeCredentials codeCredentials,
        const std::string& usageHeading,
        const std::string& sourceName
    ) {
        if (IsTokenNearExpiry(codeCredentials.oauth.expiresAtMs)) {
            if (!codeCredentials.canSave || !RefreshToken(codeCredentials)) {
                return FailedSnapshot(usageHeading, sourceName + " token expired and could not be refreshed");
            }
        }

        Network::HttpResponse response = FetchUsage(codeCredentials.oauth.accessToken);

        if ((response.statusCode == 401 || response.statusCode == 403) &&
            codeCredentials.canSave &&
            !codeCredentials.oauth.refreshToken.empty() &&
            RefreshToken(codeCredentials)) {
            response = FetchUsage(codeCredentials.oauth.accessToken);
        }

        if (response.statusCode == 429) {
            return FailedSnapshot(
                usageHeading,
                sourceName + " usage request is rate limited",
                response.statusCode,
                response.body
            );
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            return FailedSnapshot(
                usageHeading,
                sourceName + " usage failed: HTTP " + std::to_string(response.statusCode),
                response.statusCode,
                response.body
            );
        }

        Snapshot snapshot = ParseSnapshot(
            JsonUtils::get_instance()->ParseRequired(response.body),
            codeCredentials.oauth,
            usageHeading
        );

        // Do not store or display the token. A process-local hash is enough to
        // detect a changed Claude Code login and reset stale warning state.
        snapshot.accountKey = sourceName + ":" + std::to_string(
            std::hash<std::string>{}(codeCredentials.oauth.accessToken)
        );
        return snapshot;
    }

    static Snapshot FetchCredentialsFileSnapshot(bool autoFallback) {
        const std::string heading = "Claude Code credentials file usage";
        std::optional<ClaudeCredentials> credentials;

        try {
            credentials = LoadClaudeCredentialsFile();
        }
        catch (const std::exception& e) {
            return FailedSnapshot(heading, std::string("Claude Code credentials file could not be read: ") + e.what());
        }

        if (!credentials) {
            if (autoFallback) {
                return FailedSnapshot("Claude usage", "No Claude Code .credentials.json file was found");
            }

            return FailedSnapshot(
                heading,
                "Claude Code .credentials.json is selected, but the file was not found at " +
                ClaudeCredentialsPath().string()
            );
        }

        Snapshot snapshot = FetchCodeSnapshot(
            *credentials,
            heading,
            "Claude Code credentials file"
        );

        if (snapshot.statusText.rfind("Plan:", 0) == 0) {
            snapshot.statusText += " | File: " + credentials->path.string();
        }

        return snapshot;
    }

    static Snapshot FetchEnvironmentSnapshot(bool autoFallback) {
        const std::string heading = "Claude Code environment token usage";
        std::optional<ClaudeCredentials> credentials = LoadClaudeEnvironmentCredentials();

        if (!credentials) {
            if (autoFallback) {
                return FailedSnapshot(
                    "Claude usage",
                    "Not logged in. No Claude Desktop session, Claude Code .credentials.json, or CLAUDE_CODE_OAUTH_TOKEN was found"
                );
            }

            return FailedSnapshot(
                heading,
                "CLAUDE_CODE_OAUTH_TOKEN is selected, but the environment variable is not set"
            );
        }

        return FetchCodeSnapshot(*credentials, heading, "Claude Code environment token");
    }

    static Snapshot FetchSnapshotForSource(AccountSource source) {
        if (source == AccountSource::Desktop || source == AccountSource::Auto) {
            ClaudeDesktopAuth::Result desktop = ClaudeDesktopAuth::AcquireCurrentSession();

            if (desktop.kind == ClaudeDesktopAuth::ResultKind::Success) {
                return FetchDesktopSnapshot(desktop);
            }

            if (source == AccountSource::Desktop) {
                std::string detail = desktop.detail.empty()
                    ? "No signed-in Claude Desktop session was found"
                    : desktop.detail;

                return FailedSnapshot(
                    "Claude Desktop usage",
                    "Claude Desktop is selected, but the account could not be read: " + detail
                );
            }

            // A Desktop credential/decryption error must remain visible. Auto mode
            // only falls through when Desktop genuinely has no signed-in session.
            if (desktop.kind == ClaudeDesktopAuth::ResultKind::Error) {
                return FailedSnapshot(
                    "Claude Desktop usage",
                    "Claude Desktop account could not be read: " + desktop.detail
                );
            }

            // In Auto mode, a present credentials file is authoritative. If it is
            // malformed or expired, show that error instead of silently switching
            // to a different environment-token account.
            if (std::filesystem::exists(ClaudeCredentialsPath())) {
                return FetchCredentialsFileSnapshot(true);
            }

            return FetchEnvironmentSnapshot(true);
        }

        if (source == AccountSource::CredentialsFile) {
            return FetchCredentialsFileSnapshot(false);
        }

        return FetchEnvironmentSnapshot(false);
    }

    Snapshot FetchSnapshot(AccountSource source) {
        Snapshot snapshot = FetchSnapshotForSource(source);
        LocalTelemetry local = ReadLatestClaudeLocalTelemetry();
        snapshot.context = std::move(local.context);
        snapshot.run = std::move(local.run);
        return snapshot;
    }


}
