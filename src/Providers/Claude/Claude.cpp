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
#include <utility>
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

        const json* extraUsage = FindClaudeObject(root, { "extra_usage", "extraUsage" });

        if (!extraUsage) {
            return credits;
        }

        const json& object = *extraUsage;

        // Claude Desktop only exposes the spend row when extra usage is
        // enabled and the server supplied an actual used-credit amount.
        // is_enabled by itself is not usage data and must not create a 0% row.
        credits.enabled = ClaudeBoolAny(object, { "is_enabled", "isEnabled" }, false);
        const std::optional<double> usedCents = ClaudeNumberAny(
            object,
            { "used_credits", "usedCredits" }
        );

        if (!credits.enabled || !usedCents) {
            return credits;
        }

        credits.valid = true;

        const std::optional<double> limitCents = ClaudeNumberAny(
            object,
            { "monthly_limit", "monthlyLimit" }
        );
        const std::optional<double> utilization = ClaudeNumberAny(
            object,
            { "utilization", "percent", "used_percent", "usedPercent" }
        );

        const double usedDollars = std::max(0.0, *usedCents) / 100.0;
        credits.spentText = FormatDollars(usedDollars) + " spent";

        if (limitCents) {
            const double safeLimitCents = std::max(0.0, *limitCents);
            const double limitDollars = safeLimitCents / 100.0;
            const double balanceDollars = std::max(0.0, limitDollars - usedDollars);

            credits.limitText = FormatDollars(limitDollars) + " monthly limit";
            credits.monthlyLimitText = FormatDollars(limitDollars);
            credits.currentBalanceText = FormatDollars(balanceDollars);
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
                        (std::max(0.0, *usedCents) / safeLimitCents) * 100.0
                    )
                );
            }
        }
        else {
            // No monthly cap means Claude treats the spend as unlimited. Keep
            // the real spend details, but do not invent a 0% quota bar.
            credits.limitText = "Unlimited";
            credits.monthlyLimitText = "Unlimited";
            credits.currentBalanceText = "Unlimited";
            credits.hasUsedPercent = false;
        }

        std::chrono::system_clock::time_point resetAt = ParseClaudeResetAt(object);

        // Claude Desktop derives the spend reset from the first day of the
        // next month when the response does not include an explicit timestamp.
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
        return snapshot;
    }

    static Network::HttpResponse FetchDesktopUsage(const ClaudeDesktopAuth::Result& desktop) {
        std::string cookie = Network::get_instance()->StripHeaderValue(desktop.cookieHeader);
        std::string baseUrl = desktop.baseUrl.empty() ? "https://claude.ai" : desktop.baseUrl;

        std::string headers;
        headers += "Accept: application/json\r\n";
        headers += "Cookie: " + cookie + "\r\n";
        headers += "Referer: " + baseUrl + "/settings/usage\r\n";
        headers += "Origin: " + baseUrl + "\r\n";
        headers += "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Claude/1.25927.0\r\n";

        return Network::get_instance()->RequestUrl(
            baseUrl + "/api/organizations/" + desktop.organizationId + "/usage",
            "GET",
            Network::get_instance()->Utf8ToWide(headers)
        );
    }

    static Snapshot FailedSnapshot(const std::string& heading, const std::string& status) {
        Snapshot snapshot;
        snapshot.usageHeading = heading;
        snapshot.statusText = status;
        snapshot.lastUpdated = "now";
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
            return FailedSnapshot("Claude Desktop usage", "Claude Desktop usage request is rate limited");
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            return FailedSnapshot(
                "Claude Desktop usage",
                "Claude Desktop usage failed: HTTP " + std::to_string(response.statusCode)
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
            return FailedSnapshot(usageHeading, sourceName + " usage request is rate limited");
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            return FailedSnapshot(
                usageHeading,
                sourceName + " usage failed: HTTP " + std::to_string(response.statusCode)
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

    Snapshot FetchSnapshot(AccountSource source) {
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

}
