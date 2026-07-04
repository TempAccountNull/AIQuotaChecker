#include "Global.hpp"

#include "Claude.hpp"
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
#include <fstream>
#include <iomanip>
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

    static std::optional<ClaudeCredentials> LoadCredentials() {
        std::string envToken = Network::get_instance()->GetEnvText("CLAUDE_CODE_OAUTH_TOKEN");

        if (!envToken.empty()) {
            ClaudeCredentials creds;
            creds.oauth.accessToken = envToken;
            creds.canSave = false;
            return creds;
        }

        std::filesystem::path path = ClaudeCredentialsPath();

        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }

        json root = JsonUtils::get_instance()->ParseRequired(Network::get_instance()->ReadRequiredTextFile(path));

        if (!root.contains("claudeAiOauth") || !root.at("claudeAiOauth").is_object()) {
            return std::nullopt;
        }

        const json& oauthJson = root.at("claudeAiOauth");

        ClaudeOAuth oauth;
        oauth.accessToken = JsonUtils::get_instance()->String(oauthJson, "accessToken");
        oauth.refreshToken = JsonUtils::get_instance()->String(oauthJson, "refreshToken");
        oauth.expiresAtMs = JsonUtils::get_instance()->Number(oauthJson, "expiresAt", 0.0);
        oauth.subscriptionType = JsonUtils::get_instance()->String(oauthJson, "subscriptionType");
        oauth.rateLimitTier = JsonUtils::get_instance()->String(oauthJson, "rateLimitTier");

        if (oauth.accessToken.empty()) {
            return std::nullopt;
        }

        ClaudeCredentials creds;
        creds.oauth = oauth;
        creds.root = root;
        creds.path = path;
        creds.canSave = true;
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

    static bool IsClaudeProOrMax(const ClaudeOAuth& oauth) {
        std::string sub = ToLowerAscii(oauth.subscriptionType);
        return sub == "pro" || sub == "max";
    }

    static bool ShouldShowSonnetLimit(const ClaudeOAuth& oauth) {
        std::string sub = ToLowerAscii(oauth.subscriptionType);

        // Matches OpenClaude: only Max and Team plans have a Sonnet limit that
        // differs from the weekly limit. If the plan is unknown, show it.
        return sub.empty() || sub == "max" || sub == "team";
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

        double used = JsonUtils::get_instance()->Number(object, "utilization", 0.0);
        used = Math::get_instance()->ClampPercentDouble(used);

        std::chrono::system_clock::time_point resetAt{};

        if (object.contains("resets_at")) {
            resetAt = ParseTimeFlexible(object.at("resets_at"));
        }

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

        if (title == "Fable") {
            return UsageWindowModel::FablePlaceholder();
        }

        return UsageWindowModel::Placeholder(title, subtitle);
    }

    static UsageCredits ParseCredits(const json& root, const ClaudeOAuth& oauth) {
        (void)oauth;

        UsageCredits credits;
        credits.valid = true;
        credits.enabled = true;
        credits.spentText = "$0.00 spent";
        credits.limitText = "";
        credits.monthlyLimitText = "Unavailable";
        credits.currentBalanceText = "Unavailable";
        credits.resetText = "";
        credits.usedPercent = 0.0f;
        credits.resetAtUnixSeconds = 0;

        if (!root.contains("extra_usage") || !root.at("extra_usage").is_object()) {
            // Claude Desktop always reserves a plan-usage row for extra_usage,
            // but the current endpoint did not return official spend data.
            // Keep the row visible and do not fall back to local log estimates.
            return credits;
        }

        const json& object = root.at("extra_usage");

        std::optional<double> usedCents = JsonUtils::get_instance()->NumberOpt(object, "used_credits");
        std::optional<double> limitCents = JsonUtils::get_instance()->NumberOpt(object, "monthly_limit");
        std::optional<double> utilization = JsonUtils::get_instance()->NumberOpt(object, "utilization");

        double usedDollars = usedCents.value_or(0.0) / 100.0;
        std::string usedText = FormatDollars(usedDollars);

        credits.spentText = usedText + " spent";

        if (limitCents) {
            double limitDollars = *limitCents / 100.0;
            double balanceDollars = std::max(0.0, limitDollars - usedDollars);

            credits.monthlyLimitText = FormatDollars(limitDollars);
            credits.currentBalanceText = FormatDollars(balanceDollars);

            if (utilization) {
                credits.usedPercent = static_cast<float>(Math::get_instance()->ClampPercentDouble(*utilization));
            }
            else if (*limitCents == 0.0) {
                // Claude Desktop treats a zero cap as fully used.
                credits.usedPercent = 100.0f;
            }
            else {
                credits.usedPercent = static_cast<float>(Math::get_instance()->ClampPercentDouble((usedCents.value_or(0.0) / *limitCents) * 100.0));
            }
        }
        else if (usedCents || utilization) {
            credits.monthlyLimitText = "Unlimited";
            credits.currentBalanceText = "Unlimited";
            credits.usedPercent = utilization ? static_cast<float>(Math::get_instance()->ClampPercentDouble(*utilization)) : 0.0f;
        }

        std::chrono::system_clock::time_point resetAt{};

        if (object.contains("resets_at")) {
            resetAt = ParseTimeFlexible(object.at("resets_at"));
        }

        if (resetAt.time_since_epoch().count() == 0) {
            resetAt = FirstDayOfNextMonth();
        }

        credits.resetAtUnixSeconds = TimePointToUnixSeconds(resetAt);

        std::string reset = FormatResetShort(resetAt);

        if (!reset.empty()) {
            credits.resetText = "Resets " + reset;
        }

        return credits;
    }

    static Snapshot ParseSnapshot(const json& root, const ClaudeOAuth& oauth) {
        Snapshot snapshot;

        snapshot.plan = FormatPlan(oauth);
        snapshot.statusText = "Plan: " + snapshot.plan;
        snapshot.lastUpdated = "just now";

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

        if (ShouldShowSonnetLimit(oauth)) {
            snapshot.weeklySonnet = ParseWindow(
                root,
                "seven_day_sonnet",
                "Sonnet",
                "Sonnet only"
            );
        }

        snapshot.weeklyFable = ParseFirstAvailableWindow(
            root,
            { "seven_day_omelette", "omelette_promotional", "seven_day_fable" },
            "Fable",
            "Fable"
        );

        snapshot.credits = ParseCredits(root, oauth);
        return snapshot;
    }

    Snapshot FetchSnapshot() {
        auto credsOpt = LoadCredentials();

        if (!credsOpt) {
            Snapshot snapshot;
            snapshot.statusText = "Not logged in. Could not find Claude credentials";
            return snapshot;
        }

        ClaudeCredentials creds = *credsOpt;

        if (IsTokenNearExpiry(creds.oauth.expiresAtMs)) {
            if (!RefreshToken(creds)) {
                Snapshot snapshot;
                snapshot.statusText = "Claude token expired and refresh failed";
                return snapshot;
            }
        }

        Network::HttpResponse response = FetchUsage(creds.oauth.accessToken);

        if ((response.statusCode == 401 || response.statusCode == 403) && !creds.oauth.refreshToken.empty()) {
            if (RefreshToken(creds)) {
                response = FetchUsage(creds.oauth.accessToken);
            }
        }

        if (response.statusCode == 429) {
            Snapshot snapshot;
            snapshot.statusText = "Claude usage request is rate limited";
            return snapshot;
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            Snapshot snapshot;
            snapshot.statusText = "Claude usage failed: HTTP " + std::to_string(response.statusCode);
            return snapshot;
        }

        return ParseSnapshot(JsonUtils::get_instance()->ParseRequired(response.body), creds.oauth);
    }

}
