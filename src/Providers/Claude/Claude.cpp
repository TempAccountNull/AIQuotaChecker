#include "Global.hpp"

#include "Claude.hpp"
#include "ClaudeCookieStore.hpp"
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
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <mutex>
#include <regex>
#include <utility>
#include <unordered_map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Claude {

    constexpr std::uint64_t kMegabyte = 1024ULL * 1024ULL;

    struct ClaudeOAuth {
        std::string accessToken;
        std::string refreshToken;
        double expiresAtMs = 0.0;
        std::string subscriptionType;
        std::string rateLimitTier;
        // Server-provided plan name (organization.plan_display_name, e.g.
        // "Ultra"). Desktop shows it in place of the generic tier name.
        std::string displayName;
        // Granted scopes as stored by `claude login`. Older credentials predate
        // the field, so an empty list means "unknown", not "none".
        std::vector<std::string> scopes;
        // A `claude setup-token` / CLAUDE_CODE_OAUTH_TOKEN credential can run
        // inference but is rejected by the usage endpoint.
        bool inferenceOnly = false;
    };

    // /api/oauth/usage requires user:profile. A token without it authenticates
    // fine for inference and then 403s here, which is otherwise indistinguishable
    // from a server-side failure.
    static constexpr const char* kClaudeUsageScope = "user:profile";

    static bool ClaudeTokenCanReadUsage(const ClaudeOAuth& oauth) {
        if (oauth.inferenceOnly) {
            return false;
        }

        if (oauth.scopes.empty()) {
            return true;
        }

        return std::find(oauth.scopes.begin(), oauth.scopes.end(), kClaudeUsageScope) !=
            oauth.scopes.end();
    }

    struct ClaudeCredentials {
        ClaudeOAuth oauth;
        json root;
        std::filesystem::path path;
        bool canSave = false;
    };

    // CLAUDE_CONFIG_DIR, or %USERPROFILE%\.claude.
    static std::filesystem::path ClaudeConfigDir() {
        const std::string configured = Network::get_instance()->GetEnvText("CLAUDE_CONFIG_DIR");

        return configured.empty()
            ? Network::get_instance()->UserProfilePath() / ".claude"
            : std::filesystem::path(configured);
    }

    static std::filesystem::path ClaudeCredentialsPath() {
        return ClaudeConfigDir() / ".credentials.json";
    }

    // Replaces the file atomically. The refresh token inside is single-use and
    // already spent, so a crash or full disk must never leave it truncated.
    static void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::path temp = path;
        temp += ".tmp";

        bool written = false;
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file << text;
            file.flush();
            written = file.good();
        }

        if (!written ||
            !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            throw std::runtime_error("Could not save the refreshed Claude Code credentials");
        }
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

    static std::optional<ClaudeCredentials> LoadClaudeCredentialsFile() {
        std::filesystem::path path = ClaudeCredentialsPath();
        std::error_code ec;

        if (!std::filesystem::exists(path, ec)) {
            return std::nullopt;
        }

        // Fixed messages only: a JSON parse error quotes the bytes it stopped
        // at, and a torn read of this file would put a token in the status.
        const json root = JsonUtils::get_instance()->ParseOrNull(
            ClaudeCookieStore::ReadSharedText(path, kMegabyte)
        );

        if (!root.is_object()) {
            throw std::runtime_error("Claude Code .credentials.json could not be read or is not valid JSON");
        }

        if (!root.contains("claudeAiOauth") || !root.at("claudeAiOauth").is_object()) {
            throw std::runtime_error("Claude Code .credentials.json does not contain claudeAiOauth");
        }

        const json& oauthJson = root.at("claudeAiOauth");

        ClaudeOAuth oauth;
        oauth.accessToken = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(oauthJson, "accessToken"), JsonUtils::get_instance()->String(oauthJson, "access_token") });
        oauth.refreshToken = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(oauthJson, "refreshToken"), JsonUtils::get_instance()->String(oauthJson, "refresh_token") });
        oauth.expiresAtMs = JsonUtils::get_instance()->NumberAny(oauthJson, { "expiresAt", "expires_at" }).value_or(0.0);
        oauth.subscriptionType = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(oauthJson, "subscriptionType"), JsonUtils::get_instance()->String(oauthJson, "subscription_type") });
        oauth.rateLimitTier = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(oauthJson, "rateLimitTier"), JsonUtils::get_instance()->String(oauthJson, "rate_limit_tier") });

        for (const char* key : { "scopes", "scope" }) {
            auto it = oauthJson.find(key);

            if (it == oauthJson.end()) {
                continue;
            }

            if (it->is_array()) {
                for (const json& entry : *it) {
                    if (entry.is_string()) {
                        oauth.scopes.push_back(entry.get<std::string>());
                    }
                }
            }
            else if (it->is_string()) {
                std::istringstream stream(it->get<std::string>());
                std::string scope;

                while (stream >> scope) {
                    oauth.scopes.push_back(scope);
                }
            }

            if (!oauth.scopes.empty()) {
                break;
            }
        }

        if (oauth.accessToken.empty()) {
            throw std::runtime_error("Claude Code accessToken is missing from .credentials.json");
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
        // CLAUDE_CODE_OAUTH_TOKEN is normally a `claude setup-token` token:
        // inference-only by design, so the usage endpoint will 403 it.
        creds.oauth.inferenceOnly = true;
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

    // Requesting scopes the original grant never included can come back
    // downscoped, which then 403s on the usage endpoint. Re-request exactly
    // what the credential already carries, and otherwise ask only for the
    // scopes that are reliably granted to a Claude Code login.
    static std::string RefreshScope(const ClaudeOAuth& oauth) {
        if (!oauth.scopes.empty()) {
            std::string joined;

            for (const std::string& scope : oauth.scopes) {
                if (!joined.empty()) {
                    joined.push_back(' ');
                }

                joined += scope;
            }

            return joined;
        }

        return "user:profile user:inference user:sessions:claude_code";
    }

    static bool RefreshToken(ClaudeCredentials& creds) {
        if (creds.oauth.refreshToken.empty()) {
            return false;
        }

        // The refresh token is single-use. If Claude Code rotated it since the
        // file was loaded, spending the stale copy fails (or signs the CLI out),
        // so adopt the CLI's fresh credentials instead.
        if (creds.canSave) {
            try {
                std::optional<ClaudeCredentials> current = LoadClaudeCredentialsFile();

                if (current &&
                    (current->oauth.refreshToken != creds.oauth.refreshToken ||
                        current->oauth.expiresAtMs != creds.oauth.expiresAtMs)) {
                    creds = std::move(*current);
                    return true;
                }
            }
            catch (const std::exception&) {
                // Unreadable right now: refresh with what was loaded.
            }
        }

        json body;
        body["grant_type"] = "refresh_token";
        body["refresh_token"] = creds.oauth.refreshToken;
        body["client_id"] = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
        body["scope"] = RefreshScope(creds.oauth);

        std::wstring headers =
            std::wstring(L"Content-Type: application/json\r\n") +
            L"Accept: application/json\r\n";

        Network::HttpResponse response = Network::get_instance()->TryRequestUrl(
            "https://platform.claude.com/v1/oauth/token",
            "POST",
            headers,
            body.dump()
        );

        if (response.statusCode < 200 || response.statusCode >= 300) {
            return false;
        }

        const json decoded = JsonUtils::get_instance()->ParseOrNull(response.body);

        if (decoded.contains("scope") && decoded.at("scope").is_string()) {
            std::vector<std::string> refreshed;
            std::istringstream stream(decoded.at("scope").get<std::string>());
            std::string scope;

            while (stream >> scope) {
                refreshed.push_back(scope);
            }

            if (!refreshed.empty()) {
                creds.oauth.scopes = std::move(refreshed);
            }
        }

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

    // Claude Code keeps the signed-in account in <CLAUDE_CONFIG_DIR or
    // %USERPROFILE%>\.claude.json (oauthAccount). Unlike the token, its id
    // survives every refresh. The CLI rewrites the file often, so a torn read
    // keeps the last id seen rather than looking like an account switch.
    static std::string ClaudeCodeAccountId() {
        static std::mutex mutex;
        static std::string lastKnown;

        const std::string configured = Network::get_instance()->GetEnvText("CLAUDE_CONFIG_DIR");
        const std::filesystem::path path = configured.empty()
            ? Network::get_instance()->UserProfilePath() / ".claude.json"
            : std::filesystem::path(configured) / ".claude.json";

        const json config = JsonUtils::get_instance()->ParseOrNull(
            ClaudeCookieStore::ReadSharedText(path, 64 * kMegabyte)
        );
        const json account = config.is_object() && config.contains("oauthAccount")
            ? config.at("oauthAccount")
            : json();
        const std::string id = Text::get_instance()->FirstNonEmpty({
            JsonUtils::get_instance()->String(account, "accountUuid"),
            JsonUtils::get_instance()->String(account, "organizationUuid")
        });

        std::lock_guard<std::mutex> lock(mutex);

        if (!id.empty()) {
            lastKnown = id;
        }

        return lastKnown;
    }

    // Claude Desktop 4.4212.0 identifies itself on every claude.ai request and
    // sends the same anthropic-client-* set to api.anthropic.com.
    static const char* const kDesktopUserAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Claude/4.4212.0 Chrome/152.0.7977.167 Electron/44.9.3 Safari/537.36 MSIX";

    static const std::string& DesktopClientHeaders() {
        static const std::string headers = [] {
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            const double totalBytes = GlobalMemoryStatusEx(&memory)
                ? static_cast<double>(memory.ullTotalPhys)
                : 0.0;
            const char* deviceClass = totalBytes < 6e9 ? "le4"
                : totalBytes < 12e9 ? "8"
                : totalBytes < 2e10 ? "16"
                : "gt16";

            // os.release() equivalent. GetVersionEx reports what the manifest
            // allows, RtlGetVersion the real build.
            using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            RTL_OSVERSIONINFOW os{};
            os.dwOSVersionInfoSize = sizeof(os);

            if (const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
                rtlGetVersion(&os);
            }

            return std::string("anthropic-client-platform: desktop_app\r\n") +
                "anthropic-client-app: com.anthropic.claudefordesktop\r\n"
                "anthropic-client-version: 4.4212.0\r\n"
                "anthropic-client-device-class: " + deviceClass + "\r\n"
                "anthropic-client-total-memory-gb: " +
                std::to_string(std::llround(totalBytes / (1024.0 * 1024.0 * 1024.0))) + "\r\n"
                "anthropic-client-os-platform: win32\r\n"
                "anthropic-client-os-version: " + std::to_string(os.dwMajorVersion) + "." +
                std::to_string(os.dwMinorVersion) + "." + std::to_string(os.dwBuildNumber) + "\r\n";
        }();

        return headers;
    }

    // Status 0 when the request could not be sent. desktop: the token came
    // from Claude Desktop, so send what Desktop sends.
    static Network::HttpResponse FetchUsage(const std::string& accessToken, bool desktop) {
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

        if (desktop) {
            headers += Network::get_instance()->Utf8ToWide(DesktopClientHeaders());
        }

        return Network::get_instance()->TryRequestUrl(
            "https://api.anthropic.com/api/oauth/usage",
            "GET",
            headers
        );
    }

    // A GET on claude.ai / claude.com with Desktop's cookie session, sent the
    // way Desktop sends it. Status 0 when it could not be sent.
    static Network::HttpResponse DesktopGet(
        const ClaudeDesktopAuth::Result& desktop,
        const std::string& base,
        const std::string& path
    ) {
        const std::string cookie = Network::get_instance()->StripHeaderValue(desktop.cookieHeader);

        if (cookie.empty()) {
            return {};
        }

        const std::string origin = base.empty() ? "https://claude.ai" : base;
        const std::string headers =
            "Accept: application/json\r\n"
            "Cache-Control: no-cache, no-store, max-age=0\r\n"
            "Pragma: no-cache\r\n"
            "Cookie: " + cookie + "\r\n"
            "Referer: " + origin + "/settings/usage\r\n"
            "Origin: " + origin + "\r\n"
            "User-Agent: " + kDesktopUserAgent + "\r\n" +
            DesktopClientHeaders() +
            "anthropic-desktop-topbar: 1\r\n";

        return Network::get_instance()->TryRequestUrl(
            origin + path,
            "GET",
            Network::get_instance()->Utf8ToWide(headers)
        );
    }

    // claude.ai's Settings > Usage asks for cedar_ember=1 (the free limit
    // resets block) plus skip_spend=1. Spend is wanted here, and there is no
    // skip_spend=0 form, so only cedar_ember is sent. GET only, always.
    static std::string DesktopUsagePath(const ClaudeDesktopAuth::Result& desktop) {
        return "/api/organizations/" + desktop.organizationId + "/usage?cedar_ember=1";
    }

    static std::chrono::system_clock::time_point ParseTimeFlexibleOrThrow(const json& value) {
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

    // A malformed time (stod/stoi on garbage) reads as unknown instead of
    // throwing out of a whole snapshot.
    static std::chrono::system_clock::time_point ParseTimeFlexible(const json& value) {
        try {
            return ParseTimeFlexibleOrThrow(value);
        }
        catch (const std::exception&) {
            return {};
        }
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
        // Let mktime decide: keeping today's flag is an hour off across DST.
        localTime.tm_isdst = -1;

        return std::chrono::system_clock::from_time_t(std::mktime(&localTime));
    }

    static long long TimePointToUnixSeconds(std::chrono::system_clock::time_point tp) {
        if (tp.time_since_epoch().count() == 0) {
            return 0;
        }

        return static_cast<long long>(std::chrono::system_clock::to_time_t(tp));
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
        std::string lower = Text::get_instance()->ToLowerCopy(subscriptionType);

        if (lower.empty()) return "";
        if (lower == "pro") return "Pro";
        if (lower == "max") return "Max";
        if (lower == "team") return "Team";
        if (lower == "enterprise") return "Enterprise";
        if (lower == "free") return "Free";

        return PrettyTitleToken(subscriptionType);
    }

    static std::string ExtractRateLimitMultiplier(const std::string& rateLimitTier) {
        std::string lower = Text::get_instance()->ToLowerCopy(rateLimitTier);

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

    // Desktop shows plan_display_name whitespace-collapsed and capped at 40
    // characters: 39 and an ellipsis.
    static std::string CapPlanDisplayName(const std::string& name) {
        std::string collapsed;

        for (const unsigned char c : name) {
            if (!std::isspace(c)) {
                collapsed.push_back(static_cast<char>(c));
            }
            else if (!collapsed.empty() && collapsed.back() != ' ') {
                collapsed.push_back(' ');
            }
        }

        if (!collapsed.empty() && collapsed.back() == ' ') {
            collapsed.pop_back();
        }

        // Count characters, not UTF-8 bytes, and never cut inside one.
        size_t characters = 0;
        size_t cut = 0;

        for (size_t i = 0; i < collapsed.size(); ++i) {
            if ((static_cast<unsigned char>(collapsed[i]) & 0xC0) == 0x80) {
                continue;
            }

            if (characters++ == 39) {
                cut = i;
            }
        }

        return characters > 40 ? collapsed.substr(0, cut) + "\xE2\x80\xA6" : collapsed;
    }

    static std::string FormatPlan(const ClaudeOAuth& oauth) {
        if (!oauth.displayName.empty()) {
            return "Claude " + CapPlanDisplayName(oauth.displayName);
        }

        // Anthropic renamed the Max plans on 2026-10-30: Max 5x is now "Max"
        // and Max 20x is "Ultra". The tier string still says max_5x/max_20x,
        // so the multiplier picks the name and is no longer shown. Without a
        // subscription type the tier alone decides.
        const std::string lowerSubscription = Text::get_instance()->ToLowerCopy(oauth.subscriptionType);
        const std::string lowerTier = Text::get_instance()->ToLowerCopy(oauth.rateLimitTier);
        const bool maxTier = lowerTier.find("max") != std::string::npos;

        if (lowerSubscription == "ultra" || lowerTier.find("ultra") != std::string::npos ||
            ((lowerSubscription == "max" || maxTier) &&
                ExtractRateLimitMultiplier(oauth.rateLimitTier) == "20x")) {
            return "Claude Ultra";
        }

        if (lowerSubscription == "max" || (lowerSubscription.empty() && maxTier)) {
            return "Claude Max";
        }

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

        std::optional<double> usedValue = JsonUtils::get_instance()->NumberAny(
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
            .Reset(TimePointToUnixSeconds(resetAt), Format::get_instance()->ResetRelative(resetAt))
            .Valid()
            .Build();
    }

    static std::string LimitScopeDisplayName(const json& limit) {
        if (!limit.contains("scope") || !limit.at("scope").is_object()) {
            return {};
        }

        const json& scope = limit.at("scope");

        for (const char* key : { "model", "surface" }) {
            if (scope.contains(key) && scope.at(key).is_object()) {
                std::string displayName = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(scope.at(key), "display_name"), JsonUtils::get_instance()->String(scope.at(key), "displayName") });
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

        std::string kind = JsonUtils::get_instance()->String(limit, "kind");

        if (kind == "session") {
            return "Session";
        }

        if (kind == "weekly_all") {
            return "Weekly";
        }

        std::string group = JsonUtils::get_instance()->String(limit, "group");
        std::string title = HumanizeIdentifier(kind.empty() ? group : kind);
        return title.empty() ? "Usage" : title;
    }

    static std::string ServerLimitSubtitle(const json& limit) {
        const std::string kind = JsonUtils::get_instance()->String(limit, "kind");
        const std::string group = JsonUtils::get_instance()->String(limit, "group");

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

        std::optional<double> percent = JsonUtils::get_instance()->NumberAny(
            limit,
            { "percent", "utilization", "used_percent", "usedPercent" }
        );
        if (!percent) return window.Build();

        const std::chrono::system_clock::time_point resetAt = ParseClaudeResetAt(limit);

        return window
            .UsedPercent(static_cast<float>(Math::get_instance()->ClampPercentDouble(*percent)))
            .Reset(TimePointToUnixSeconds(resetAt), Format::get_instance()->ResetRelative(resetAt))
            .Valid()
            .Build();
    }

    static void AddAdditionalLimit(Snapshot& snapshot, UsageWindow window) {
        if (!window.valid) {
            return;
        }

        auto sameIdentity = [&](const UsageWindow& existing) {
            return existing.valid &&
                Text::get_instance()->ToLowerCopy(existing.title) == Text::get_instance()->ToLowerCopy(window.title) &&
                Text::get_instance()->ToLowerCopy(existing.subtitle) == Text::get_instance()->ToLowerCopy(window.subtitle) &&
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

    // weekly_scoped_shares[] ties a scoped limit, by its limits[] index, to
    // the share of the weekly limit it may use (Fable: "up to half").
    static std::string ScopedShareSubtitle(const json& root, size_t limitIndex) {
        if (!root.contains("weekly_scoped_shares") || !root.at("weekly_scoped_shares").is_array()) {
            return {};
        }

        for (const json& share : root.at("weekly_scoped_shares")) {
            const auto index = JsonUtils::get_instance()->NumberOpt(share, "limit_index");
            const auto allowance = JsonUtils::get_instance()->NumberOpt(share, "allowance_percent_of_weekly");

            if (index && allowance && *index >= 0.0 && static_cast<size_t>(*index) == limitIndex) {
                return "Up to " + Format::get_instance()->Percent(*allowance) + " of weekly limit";
            }
        }

        return {};
    }

    static bool ParseServerDrivenLimits(const json& root, Snapshot& snapshot) {
        if (!root.contains("limits") || !root.at("limits").is_array()) {
            return false;
        }

        const json& limits = root.at("limits");

        if (limits.begin() == limits.end()) {
            // An explicitly empty list carries no rows of its own; only the
            // named session/weekly/Sonnet fields still apply (ParseSnapshot).
            return true;
        }

        bool parsedAny = false;

        for (size_t index = 0; index < limits.size(); ++index) {
            const json& limit = limits.at(index);

            if (!limit.is_object()) continue;

            if (!JsonUtils::get_instance()->NumberAny(limit, { "percent", "utilization", "used_percent", "usedPercent" })) {
                continue;
            }

            const std::string kind = JsonUtils::get_instance()->String(limit, "kind");
            const std::string group = JsonUtils::get_instance()->String(limit, "group");
            const std::string share = ScopedShareSubtitle(root, index);

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
            const std::string lower = Text::get_instance()->ToLowerCopy(displayName);

            if (group == "weekly" && lower.find("sonnet") != std::string::npos) {
                snapshot.weeklySonnet = ParseServerLimitWindow(
                    limit,
                    displayName.empty() ? "Sonnet" : displayName,
                    share.empty() ? "Model-specific weekly limit" : share
                );
                parsedAny = snapshot.weeklySonnet.valid || parsedAny;
            }
            else if (group == "weekly" && lower.find("fable") != std::string::npos) {
                // Live Desktop 4.4212 data has a real per-model window:
                // kind "weekly_scoped", scope.model.display_name "Fable".
                snapshot.weeklyFable = ParseServerLimitWindow(
                    limit,
                    displayName,
                    share.empty() ? "Model-specific weekly limit" : share
                );
                parsedAny = snapshot.weeklyFable.valid || parsedAny;
            }
            else {
                UsageWindow additional = ParseServerLimitWindow(
                    limit,
                    ServerLimitTitle(limit),
                    share.empty() ? ServerLimitSubtitle(limit) : share
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

    // Credit amounts as the credits section shows them: USD (or no currency,
    // Desktop's default) as dollars, anything else with its code.
    static std::string FormatCreditMoney(double value, const std::string& currency) {
        const std::string lower = Text::get_instance()->ToLowerCopy(currency);

        if (lower.empty() || lower == "usd") {
            return Format::get_instance()->Dollars(value);
        }

        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << value << ' ' << currency;
        return out.str();
    }

    // spend: { used: { amount_minor, currency, exponent }, limit, percent,
    //          enabled, disabled_reason, balance, cap, ... }
    // The newer shape both usage bodies send beside extra_usage, and the one
    // claude.ai's Settings > Usage reads. It always carries the spent amount,
    // so it is final: no supplementary request is made for it.
    static UsageCredits ParseSpend(const json& spend) {
        struct Money {
            double amount = 0.0;
            std::string currency;
        };
        const auto money = [](const json& object, const char* key) -> std::optional<Money> {
            const json* value = FindClaudeObject(object, { key });
            const std::optional<double> minor = value
                ? JsonUtils::get_instance()->NumberOpt(*value, "amount_minor")
                : std::nullopt;

            if (!minor) {
                return std::nullopt;
            }

            return Money{
                std::max(0.0, *minor) / std::pow(10.0, JsonUtils::get_instance()->Number(*value, "exponent")),
                JsonUtils::get_instance()->String(*value, "currency")
            };
        };

        // The page shows an absent amount as nothing spent.
        const Money used = money(spend, "used").value_or(Money{});
        const std::optional<Money> limit = money(spend, "limit");
        const Money balance = money(spend, "balance").value_or(Money{ 0.0, used.currency });

        UsageCredits credits;
        credits.reported = true;
        credits.valid = true;
        credits.enabled = JsonUtils::get_instance()->Bool(spend, "enabled");
        credits.disabledReason = JsonUtils::get_instance()->String(spend, "disabled_reason");
        credits.hasSpentAmount = true;
        credits.hasMonthlyLimit = limit.has_value();
        credits.spentText = FormatCreditMoney(used.amount, used.currency) + " spent";
        credits.currentBalanceText = FormatCreditMoney(balance.amount, balance.currency);

        if (!credits.enabled) {
            credits.limitText = "Off";
            credits.monthlyLimitText = "Off";
            return credits;
        }

        if (limit) {
            credits.limitText = FormatCreditMoney(limit->amount, limit->currency) + " monthly limit";
            credits.monthlyLimitText = FormatCreditMoney(limit->amount, limit->currency);
            credits.hasUsedPercent = true;

            const std::optional<double> percent = JsonUtils::get_instance()->NumberOpt(spend, "percent");
            credits.usedPercent = static_cast<float>(Math::get_instance()->ClampPercentDouble(
                percent ? *percent
                : limit->amount > 0.0 ? used.amount / limit->amount * 100.0
                : 100.0
            ));
        }
        else {
            credits.limitText = "Unlimited";
            credits.monthlyLimitText = "Unlimited";
        }

        const std::chrono::system_clock::time_point resetAt = FirstDayOfNextMonth();
        credits.resetAtUnixSeconds = TimePointToUnixSeconds(resetAt);
        credits.resetText = Format::get_instance()->ResetRelative(resetAt);
        return credits;
    }

    static UsageCredits ParseCredits(const json& root) {
        if (const json* spend = FindClaudeObject(root, { "spend" })) {
            return ParseSpend(*spend);
        }

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
                enabled = JsonUtils::get_instance()->Bool(object, key);
                break;
            }
        }

        const std::optional<double> usedCents = JsonUtils::get_instance()->NumberAny(
            object,
            { "used_credits", "usedCredits", "spent_credits", "spentCredits" }
        );
        const std::optional<double> limitCents = JsonUtils::get_instance()->NumberAny(
            object,
            { "monthly_limit", "monthlyLimit", "spend_limit", "spendLimit" }
        );
        const std::optional<double> utilization = JsonUtils::get_instance()->NumberAny(
            object,
            { "utilization", "percent", "used_percent", "usedPercent" }
        );
        const std::optional<double> balanceCents = JsonUtils::get_instance()->NumberAny(
            object,
            {
                "current_balance", "currentBalance",
                "credit_balance", "creditBalance", "balance"
            }
        );
        // /api/oauth/usage now names the currency; Desktop defaults to USD.
        const std::string currency = JsonUtils::get_instance()->String(object, "currency");
        const auto money = [&](double value) {
            return FormatCreditMoney(value, currency);
        };

        credits.disabledReason = JsonUtils::get_instance()->String(object, "disabled_reason");
        credits.hasSpentAmount = usedCents.has_value();
        credits.hasMonthlyLimit = limitCents.has_value();

        // Desktop parity: an explicit is_enabled is final, whatever spend
        // figures sit next to it; only without one does spend data imply on.
        credits.enabled = enabled.value_or(
            usedCents.has_value() || limitCents.has_value() || utilization.has_value()
        );

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
            credits.limitText = limitCents ? money(std::max(0.0, *limitCents) / 100.0) : "Unavailable";
            credits.monthlyLimitText = credits.limitText;
            credits.currentBalanceText = balanceCents
                ? money(std::max(0.0, *balanceCents) / 100.0)
                : money(0.0);
            credits.hasUsedPercent = false;
            return credits;
        }

        const double safeUsedCents = std::max(0.0, *usedCents);
        const double usedDollars = safeUsedCents / 100.0;
        credits.spentText = money(usedDollars) + " spent";

        if (limitCents) {
            const double safeLimitCents = std::max(0.0, *limitCents);
            const double limitDollars = safeLimitCents / 100.0;
            credits.limitText = money(limitDollars) + " monthly limit";
            credits.monthlyLimitText = money(limitDollars);
            // The monthly limit is a spending cap, not a wallet balance.
            // Claude Desktop renders an omitted prepaid-credit balance as
            // $0.00, so mirror that exact UI behavior instead of showing an
            // ambiguous "Not reported" value.
            credits.currentBalanceText = balanceCents
                ? money(std::max(0.0, *balanceCents) / 100.0)
                : money(0.0);
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
                ? money(std::max(0.0, *balanceCents) / 100.0)
                : money(0.0);
            credits.hasUsedPercent = false;
        }

        std::chrono::system_clock::time_point resetAt = ParseClaudeResetAt(object);

        if (resetAt.time_since_epoch().count() == 0) {
            resetAt = FirstDayOfNextMonth();
        }

        credits.resetAtUnixSeconds = TimePointToUnixSeconds(resetAt);
        // Already "Resets in ..." / "Reset available".
        credits.resetText = Format::get_instance()->ResetRelative(resetAt);
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

        // Desktop treats is_enabled:false as final; only skip_spend=1 replies
        // (which this app never sends) omit spend data. A full-spend response
        // always supplies used_credits when the feature is enabled, and a
        // spend block always counts as spent data (ParseSpend).
        return credits.enabled && !credits.hasSpentAmount;
    }

    // Desktop keeps claude-code-sessions\<acct>\<org>\local_<uuid>.json and
    // local-agent-mode-sessions\<acct>\<org>\ under its userData: %APPDATA%\Claude
    // for a normal install, Claude-3p for the 3P build, or an MSIX package
    // cache. Scan every candidate; the newest file wins. Read-only.
    static std::vector<std::filesystem::path> ClaudeDesktopDataDirs(const char* child) {
        std::vector<std::filesystem::path> dirs;

        for (const std::filesystem::path& root : ClaudeDesktopAuth::UserDataCandidates()) {
            std::error_code ec;

            if (std::filesystem::is_directory(root / child, ec)) {
                dirs.push_back(root / child);
            }
        }

        return dirs;
    }

    struct ClaudeFile {
        std::filesystem::path path;
        std::filesystem::file_time_type writeTime{};
    };

    // Files under the roots (recursively) that keep() accepts, newest first,
    // at most limit. Never throws: an unreadable entry is skipped and an
    // iteration error ends that root's scan. keep() sees the native wide path,
    // so a name outside the code page is never converted.
    static std::vector<ClaudeFile> NewestClaudeFiles(
        const std::vector<std::filesystem::path>& roots,
        const std::function<bool(const std::filesystem::path&)>& keep,
        size_t limit
    ) {
        std::vector<ClaudeFile> files;

        for (const std::filesystem::path& root : roots) {
            std::error_code ec;

            for (std::filesystem::recursive_directory_iterator it(
                     root,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec), end;
                 !ec && it != end;
                 it.increment(ec)) {
                std::error_code entryError;

                if (!it->is_regular_file(entryError) || entryError || !keep(it->path())) {
                    continue;
                }

                const auto writeTime = it->last_write_time(entryError);

                if (!entryError) {
                    files.push_back({ it->path(), writeTime });
                }
            }
        }

        std::sort(files.begin(), files.end(), [](const ClaudeFile& a, const ClaudeFile& b) {
            return a.writeTime > b.writeTime;
        });

        if (files.size() > limit) {
            files.resize(limit);
        }

        return files;
    }

    struct ClaudeDesktopSessionHint {
        bool valid = false;
        std::filesystem::file_time_type writeTime{};
        std::string cliSessionId;
        long long lastActivityAtMs = 0;
        long long lastFocusedAtMs = 0;
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

    // The Desktop ASAR stores account settings under
    // userData/local-agent-mode-sessions/<account>/<org>/; newest first.
    static std::vector<ClaudeFile> ClaudeCoworkSettingsFiles() {
        return NewestClaudeFiles(
            ClaudeDesktopDataDirs("local-agent-mode-sessions"),
            [](const std::filesystem::path& path) {
                return _wcsicmp(path.filename().c_str(), L"cowork_account_settings.json") == 0;
            },
            16
        );
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

        for (const ClaudeFile& file : ClaudeCoworkSettingsFiles()) {
            const json value = JsonUtils::get_instance()->ParseOrNull(
                ClaudeCookieStore::ReadSharedText(file.path, 4 * kMegabyte)
            );
            std::string selected = ClaudeSelectedModelFromJson(value);
            if (!selected.empty()) {
                cache.model = std::move(selected);
                break;
            }
        }
        return cache.model;
    }

    // An explicit 1M-context selection: "[1m]", context-1m / context_1m, or a
    // "-1m" suffix (Desktop matches /-1m\b/i).
    static bool HasOneMillionMarker(const std::string& model) {
        static const std::regex kSuffix("-1m\\b");
        const std::string lower = Text::get_instance()->ToLowerCopy(model);

        return lower.find("[1m]") != std::string::npos ||
            lower.find("context-1m") != std::string::npos ||
            lower.find("context_1m") != std::string::npos ||
            std::regex_search(lower, kSuffix);
    }

    static long long ClaudeDesktopSelectedContextWindowHint(
        const ClaudeDesktopSessionHint& hint
    ) {
        const std::string selected = hint.selectedModel.empty()
            ? LatestClaudeDesktopSelectedModel()
            : hint.selectedModel;

        return HasOneMillionMarker(selected) ? 1000000 : 0;
    }

    static ClaudeDesktopSessionHint LatestClaudeDesktopSessionHint() {
        const std::vector<ClaudeFile> files = NewestClaudeFiles(
            ClaudeDesktopDataDirs("claude-code-sessions"),
            [](const std::filesystem::path& path) {
                return path.extension() == ".json" &&
                    _wcsnicmp(path.filename().c_str(), L"local_", 6) == 0;
            },
            64
        );

        ClaudeDesktopSessionHint best;
        for (const ClaudeFile& file : files) {
            const json value = JsonUtils::get_instance()->ParseOrNull(
                ClaudeCookieStore::ReadSharedText(file.path, 10 * kMegabyte)
            );
            if (!value.is_object() || JsonUtils::get_instance()->Bool(value, "isArchived")) {
                continue;
            }

            ClaudeDesktopSessionHint hint;
            hint.valid = true;
            hint.writeTime = file.writeTime;
            hint.cliSessionId = JsonUtils::get_instance()->String(value, "cliSessionId");
            hint.lastActivityAtMs = static_cast<long long>(std::max(
                0.0,
                JsonUtils::get_instance()->Number(value, "lastActivityAt")
            ));
            hint.lastFocusedAtMs = static_cast<long long>(std::max(
                0.0,
                JsonUtils::get_instance()->Number(value, "lastFocusedAt")
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

    static bool IsClaudeBackgroundSessionPath(const std::filesystem::path& path) {
        for (const auto& component : path) {
            if (_wcsicmp(component.c_str(), L"subagents") == 0) {
                return true;
            }
        }

        return _wcsnicmp(path.filename().c_str(), L"agent-", 6) == 0;
    }

    static std::vector<ClaudeFile> LatestClaudeSessionFiles() {
        return NewestClaudeFiles(
            { ClaudeConfigDir() / "projects" },
            [](const std::filesystem::path& path) {
                // Task/subagent transcripts have independent context windows and
                // must never replace the visible foreground conversation meter.
                return path.extension() == ".jsonl" && !IsClaudeBackgroundSessionPath(path);
            },
            64
        );
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

    // One usage counter, snake_case or camelCase. Some Claude SDK snapshots
    // fill usage through iterations[] first, so the newest iteration stands in
    // until the top-level count is persisted.
    static long long ClaudeUsageTokens(const json& usage, const char* snakeKey, const char* camelKey) {
        std::optional<double> value = JsonUtils::get_instance()->NumberAny(usage, { snakeKey, camelKey });

        if (!value && usage.is_object() && usage.contains("iterations") && usage.at("iterations").is_array()) {
            const json& iterations = usage.at("iterations");

            for (auto it = iterations.rbegin(); it != iterations.rend() && !value; ++it) {
                value = JsonUtils::get_instance()->NumberAny(*it, { snakeKey, camelKey });
            }
        }

        return static_cast<long long>(std::max(0.0, value.value_or(0.0)));
    }

    // What the context window holds: fresh input plus both cache sides.
    static long long ClaudeInputTokensFromUsage(const json& usage) {
        return ClaudeUsageTokens(usage, "input_tokens", "inputTokens") +
            ClaudeUsageTokens(usage, "cache_creation_input_tokens", "cacheCreationInputTokens") +
            ClaudeUsageTokens(usage, "cache_read_input_tokens", "cacheReadInputTokens");
    }

    // Cheap pre-filter before a full parse: does the line carry
    // "type":"<type>" in either spacing JSON writers use?
    static bool LineHasType(const std::string& line, const char* type) {
        const std::string value = std::string("\"") + type + "\"";

        return line.find("\"type\":" + value) != std::string::npos ||
            line.find("\"type\": " + value) != std::string::npos;
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
            const json* current = FindClaudeObject(
                *contextWindow,
                { "current_usage", "currentUsage" }
            );
            if (current) {
                const long long currentTokens = ClaudeInputTokensFromUsage(*current);
                if (currentTokens > 0) {
                    return currentTokens;
                }
            }

            const auto total = JsonUtils::get_instance()->NumberAny(
                *contextWindow,
                { "total_input_tokens", "totalInputTokens" }
            );
            if (total && *total > 0.0) {
                return static_cast<long long>(*total);
            }
        }

        if (LineHasType(line, "assistant")) {
            if (auto usage = ExtractClaudeMetadataObject(line, "usage")) {
                return ClaudeInputTokensFromUsage(*usage);
            }
        }

        return 0;
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

    // Mirrors Claude Code's own context-window resolution (KT()/esc() in the
    // CLI bundle), verified against the model registry embedded in
    // claude.exe 2.1.220 and Desktop 4.4212.0:
    //
    //   1. a "[1m]" / "-1m" model-id suffix       -> 1,000,000
    //   2. a bare first-party alias (opus, best)  -> 1,000,000, its 1M 5.x target
    //   3. registry entry with context.native_1m  -> 1,000,000
    //   4. registry entry context.window          -> that value
    //   5. any other recognizable Claude model    -> 200,000 (the CLI's default)
    //
    // Entries are longest-prefix matched so dated ids (claude-opus-4-8-20260101)
    // and vendor-prefixed ids resolve to their base model.
    struct ClaudeContextWindow {
        long long tokens = 0;
        // False only for the last-resort 200K default, where the model was not
        // in the registry. A registry hit is as authoritative as persisted
        // metadata and must not suppress auto-compact telemetry.
        bool fromRegistry = false;
    };

    // ---------------------------------------------------------------------
    // Claude Code statusLine payload
    //
    // Claude Code hands its statusLine command a JSON document on stdin that
    // contains the values AQC otherwise has to infer:
    //
    //   context_window: { context_window_size, current_usage,
    //                     total_input_tokens, used_percentage, ... }
    //   model: { id, display_name }, exceeds_200k_tokens, cost, session_id
    //
    // Running `AIQuotaChecker.exe --statusline` as that command parks the
    // document in AQC_CLAUDE_STATUSLINE_FILE (default
    // %LOCALAPPDATA%\AIQuotaChecker\claude-statusline.json). Everything below
    // is a plain read of that file - no request, no token, so no 401/403 - and
    // it is authoritative because Claude Code computed it itself.
    // ---------------------------------------------------------------------
    struct ClaudeStatusLinePayload {
        bool valid = false;
        std::string sessionId;
        long long contextWindowTokens = 0;
        bool hasCost = false;
        double totalCostUsd = 0.0;
        long long linesAdded = 0;
        long long linesRemoved = 0;
        long long durationMs = 0;
        long long apiDurationMs = 0;
        std::vector<std::string> details;
    };

    static std::filesystem::path ClaudeStatusLineFilePath() {
        const std::string configured =
            Network::get_instance()->GetEnvText("AQC_CLAUDE_STATUSLINE_FILE");

        if (!configured.empty()) {
            return std::filesystem::path(configured);
        }

        const std::string localAppData = Network::get_instance()->GetEnvText("LOCALAPPDATA");

        if (localAppData.empty()) {
            return {};
        }

        return std::filesystem::path(localAppData) / "AIQuotaChecker" / "claude-statusline.json";
    }

    static ClaudeStatusLinePayload ReadClaudeStatusLinePayload() {
        ClaudeStatusLinePayload payload;
        const std::filesystem::path path = ClaudeStatusLineFilePath();

        if (path.empty()) {
            return payload;
        }

        const json root = JsonUtils::get_instance()->ParseOrNull(
            ClaudeCookieStore::ReadSharedText(path, 4 * kMegabyte)
        );

        if (!root.is_object()) {
            return payload;
        }

        payload.sessionId = JsonUtils::get_instance()->String(root, "session_id");

        if (const json* context = FindClaudeObject(root, { "context_window" })) {
            const auto size = JsonUtils::get_instance()->NumberAny(
                *context,
                { "context_window_size", "contextWindowSize" }
            );

            if (size && *size > 0.0) {
                payload.contextWindowTokens = static_cast<long long>(*size);
            }
        }

        if (const json* cost = FindClaudeObject(root, { "cost" })) {
            const auto total = JsonUtils::get_instance()->NumberAny(*cost, { "total_cost_usd", "totalCostUsd" });

            if (total) {
                payload.hasCost = true;
                payload.totalCostUsd = *total;
            }

            payload.linesAdded = static_cast<long long>(JsonUtils::get_instance()->Number(*cost, "total_lines_added"));
            payload.linesRemoved = static_cast<long long>(JsonUtils::get_instance()->Number(*cost, "total_lines_removed"));
            payload.durationMs = static_cast<long long>(JsonUtils::get_instance()->Number(*cost, "total_duration_ms"));
            payload.apiDurationMs = static_cast<long long>(JsonUtils::get_instance()->Number(*cost, "total_api_duration_ms"));
        }

        // Session facts published only here. Each is optional.
        const auto field = [&](const char* object, const char* key) {
            const json* holder = FindClaudeObject(root, { object });
            return holder ? JsonUtils::get_instance()->String(*holder, key) : std::string();
        };
        const auto add = [&](const char* label, const std::string& value) {
            if (!value.empty()) {
                payload.details.push_back(label + value);
            }
        };

        add("Model: ", field("model", "display_name"));
        add("Output style: ", field("output_style", "name"));

        if (JsonUtils::get_instance()->Bool(root, "exceeds_200k_tokens")) {
            payload.details.push_back("Long-context tier (>200k)");
        }

        // The folder name only; npos + 1 keeps the whole string.
        const std::string dir = Text::get_instance()->FirstNonEmpty({
            field("workspace", "project_dir"),
            field("workspace", "current_dir")
        });
        add("Project: ", dir.substr(dir.find_last_of("/\\") + 1));
        add("Repo: ", field("workspace", "repo"));
        add("Agent: ", field("agent", "name"));

        if (const json* pr = FindClaudeObject(root, { "pr" })) {
            if (const auto number = JsonUtils::get_instance()->NumberOpt(*pr, "number")) {
                payload.details.push_back("PR #" + std::to_string(static_cast<long long>(*number)));
            }
        }

        add("Branch: ", field("worktree", "branch"));
        add("Vim: ", field("vim", "mode"));

        payload.valid = payload.contextWindowTokens > 0 || payload.hasCost || !payload.details.empty();
        return payload;
    }

    static ClaudeContextWindow ResolveClaudeContextWindow(const std::string& model) {
        const std::string lower = Text::get_instance()->ToLowerCopy(model);

        if (lower.empty()) {
            return {};
        }

        if (HasOneMillionMarker(lower)) {
            return { 1000000, true };
        }

        for (const char* alias : { "opus", "sonnet", "haiku", "fable", "mythos", "best", "opusplan" }) {
            if (lower == alias) {
                return { 1000000, true };
            }
        }

        struct Entry { const char* id; long long window; };
        static const Entry kRegistry[] = {
            // Native 1M-context models (Desktop 4.4212 catalog max_input_tokens).
            // Prefixes: claude-opus-5-5, claude-fable-5-1 and claude-mythos-5-1
            // resolve through their family entry.
            { "claude-opus-5",     1000000 },
            { "claude-sonnet-5",   1000000 },
            { "claude-fable-5",    1000000 },
            { "claude-haiku-5",    1000000 },
            { "claude-mythos-5",   1000000 },
            { "claude-opus-4-8",   1000000 },
            { "claude-opus-4-7",   1000000 },
            { "claude-opus-4-6",   1000000 },
            { "claude-sonnet-4-6", 1000000 },
            // 200K models. Listed explicitly so a sibling 1M model in the same
            // family can never be matched by a shorter prefix.
            { "claude-opus-4-5",    200000 },
            { "claude-opus-4-1",    200000 },
            { "claude-opus-4-0",    200000 },
            { "claude-sonnet-4-5",  200000 },
            { "claude-sonnet-4-0",  200000 },
            { "claude-haiku-4-5",   200000 },
            { "claude-3-7-sonnet",  200000 },
            { "claude-3-5-sonnet",  200000 },
            { "claude-3-5-haiku",   200000 },
        };

        const Entry* best = nullptr;
        size_t bestLength = 0;

        for (const Entry& entry : kRegistry) {
            const size_t length = std::strlen(entry.id);

            if (length > bestLength && lower.find(entry.id) != std::string::npos) {
                best = &entry;
                bestLength = length;
            }
        }

        if (best) {
            return { best->window, true };
        }

        // Claude Code honors CLAUDE_CODE_MAX_CONTEXT_TOKENS only for models it
        // does not recognize as first-party, so apply it at the same point.
        const std::string configured =
            Network::get_instance()->GetEnvText("CLAUDE_CODE_MAX_CONTEXT_TOKENS");

        if (!configured.empty()) {
            char* end = nullptr;
            const long long parsed = std::strtoll(configured.c_str(), &end, 10);

            if (end != configured.c_str() && parsed > 0) {
                return { parsed, true };
            }
        }

        // The CLI's own fallback for anything still unresolved.
        if (lower.rfind("claude-", 0) == 0 || lower == "default") {
            return { 200000, false };
        }

        return {};
    }

    static bool ClaudeEnvTruthy(const std::string& value) {
        const std::string lower = Text::get_instance()->ToLowerCopy(value);
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
        const std::string lower = Text::get_instance()->ToLowerCopy(line);
        if (lower.find("compact") == std::string::npos) {
            return false;
        }

        const std::string type = Text::get_instance()->ToLowerCopy(ExtractClaudeMetadataString(line, "type"));
        const std::string subtype = Text::get_instance()->ToLowerCopy(ExtractClaudeMetadataString(line, "subtype"));
        const std::string status = Text::get_instance()->ToLowerCopy(ExtractClaudeMetadataString(line, "status"));
        const std::string phase = Text::get_instance()->ToLowerCopy(ExtractClaudeMetadataString(line, "phase"));
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

        return Text::get_instance()->ToLowerCopy(ExtractClaudeMetadataString(line, "type")) == "system" &&
            Text::get_instance()->ToLowerCopy(ExtractClaudeMetadataString(line, "subtype")) == "compact_boundary";
    }

    static bool ClaudeUserRecordStartsRun(const json& record) {
        if (!record.is_object() || JsonUtils::get_instance()->String(record, "type") != "user" ||
            JsonUtils::get_instance()->Bool(record, "isSidechain") || JsonUtils::get_instance()->Bool(record, "isSynthetic") ||
            JsonUtils::get_instance()->Bool(record, "isMeta") || JsonUtils::get_instance()->Bool(record, "isCompactSummary") ||
            JsonUtils::get_instance()->Bool(record, "isVisibleInTranscriptOnly")) {
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

            const std::string type = JsonUtils::get_instance()->String(block, "type");
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

    // Whether any real turn activity was written after `index`. Attachments and
    // the assorted bookkeeping records the CLI emits do not count - only model
    // output or a tool result means the turn is still going.
    static bool ClaudeActivityFollows(
        const std::vector<std::string>& lines,
        size_t index
    ) {
        for (size_t i = index + 1; i < lines.size(); ++i) {
            const std::string& line = lines[i];

            if (LooksLikeClaudeSidechainRecord(line)) {
                continue;
            }

            json record = json::parse(line, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const std::string type = JsonUtils::get_instance()->String(record, "type");

            if (type == "assistant" || type == "user") {
                return true;
            }
        }

        return false;
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

            if (!LineHasType(line, "user") && !LineHasType(line, "result") &&
                !LineHasType(line, "assistant")) {
                continue;
            }

            json record = json::parse(line, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const std::string type = JsonUtils::get_instance()->String(record, "type");

            // Sidechain records are subagent turns interleaved into the same
            // transcript. Their end_turn is not the main turn ending, and
            // treating it as one made the scan stop early and report "idle"
            // for the whole time any subagent was running.
            const bool sidechain =
                JsonUtils::get_instance()->Bool(record, "isSidechain") ||
                JsonUtils::get_instance()->Bool(record, "isMeta") ||
                JsonUtils::get_instance()->Bool(record, "isSynthetic");

            if (type == "result" && !sidechain) {
                return run;
            }

            if (type == "assistant" && !sidechain && record.contains("message") &&
                record.at("message").is_object()) {
                const json& message = record.at("message");

                // end_turn closes an API BLOCK, not the user's turn - the record
                // carries an apiBlockIndex and a single turn spans several of
                // them. It only means the turn is over when nothing came after
                // it; otherwise the model is still working and stopping here
                // reported "idle" for the whole rest of the turn.
                if (JsonUtils::get_instance()->String(message, "stop_reason") == "end_turn" &&
                    !ClaudeActivityFollows(lines, i)) {
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

        // Subagent tasks still in flight: Task tool calls issued since the turn
        // began, minus the ones whose result has already come back.
        std::vector<std::string> pendingTasks;

        for (size_t i = runStart + 1; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (LooksLikeClaudeSidechainRecord(line)) {
                continue;
            }

            if (line.find("\"Task\"") != std::string::npos ||
                line.find("tool_result") != std::string::npos) {
                json probe = json::parse(line, nullptr, false);

                if (!probe.is_discarded() && probe.is_object() &&
                    probe.contains("message") && probe.at("message").is_object()) {
                    const json& message = probe.at("message");

                    if (message.contains("content") && message.at("content").is_array()) {
                        for (const json& block : message.at("content")) {
                            if (!block.is_object()) continue;

                            const std::string blockType = JsonUtils::get_instance()->String(block, "type");

                            if (blockType == "tool_use" &&
                                JsonUtils::get_instance()->String(block, "name") == "Task") {
                                pendingTasks.push_back(JsonUtils::get_instance()->String(block, "id"));
                            }
                            else if (blockType == "tool_result") {
                                const std::string id =
                                    JsonUtils::get_instance()->String(block, "tool_use_id");
                                pendingTasks.erase(
                                    std::remove(pendingTasks.begin(), pendingTasks.end(), id),
                                    pendingTasks.end());
                            }
                        }
                    }
                }
            }

            const bool mayAffectRun =
                LineHasType(line, "assistant") ||
                LineHasType(line, "tool_progress") ||
                LineHasType(line, "tool_use_summary") ||
                LineHasType(line, "stream_event") ||
                LineHasType(line, "user");

            if (!mayAffectRun) {
                continue;
            }

            json record = json::parse(line, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const std::string type = JsonUtils::get_instance()->String(record, "type");

            if (type == "stream_event") {
                // Claude Desktop enables partial SDK messages. When those raw
                // stream events are present in the transcript they provide the
                // closest passive equivalent to Desktop's live thinking state.
                // Fall back to completed assistant content below when a build
                // does not persist partial events.
                if (record.contains("event") && record.at("event").is_object()) {
                    const json& event = record.at("event");
                    const std::string eventType = JsonUtils::get_instance()->String(event, "type");

                    if (eventType == "content_block_delta" &&
                        event.contains("delta") && event.at("delta").is_object()) {
                        const std::string deltaType =
                            JsonUtils::get_instance()->String(event.at("delta"), "type");

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
                            JsonUtils::get_instance()->String(event.at("content_block"), "type");
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
                        if (block.is_object() && JsonUtils::get_instance()->String(block, "type") == "tool_result") {
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
            const std::string messageId = JsonUtils::get_instance()->String(message, "id");

            if (message.contains("usage") && message.at("usage").is_object()) {
                const std::string dedupeId = messageId.empty()
                    ? JsonUtils::get_instance()->String(record, "uuid")
                    : messageId;
                const json& usage = message.at("usage");
                const long long output = ClaudeUsageTokens(usage, "output_tokens", "outputTokens");

                run.tokenStatsValid = true;
                run.currentTokens = output;
                run.inputTokens = ClaudeInputTokensFromUsage(usage);
                run.rawInputTokens = ClaudeUsageTokens(usage, "input_tokens", "inputTokens");
                run.cacheCreationInputTokens = ClaudeUsageTokens(usage, "cache_creation_input_tokens", "cacheCreationInputTokens");
                run.cacheReadInputTokens = ClaudeUsageTokens(usage, "cache_read_input_tokens", "cacheReadInputTokens");

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

                    const std::string blockType = JsonUtils::get_instance()->String(block, "type");
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

        run.activeTaskCount = static_cast<int>(pendingTasks.size());
        return run;
    }

    static LocalTelemetry ReadClaudeLocalTelemetryFromSession(
        const std::filesystem::path& session,
        long long contextWindowHint = 0
    ) {
        LocalTelemetry local;
        UsageTelemetry::ContextUsage& context = local.context;
        const std::string text = ClaudeCookieStore::ReadSharedText(session, 8 * kMegabyte, true);

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
                    const auto maximum = JsonUtils::get_instance()->NumberAny(
                        *contextWindow,
                        { "context_window_size", "contextWindowSize" }
                    );

                    if (maximum && *maximum > 0.0) {
                        contextWindowTokens = static_cast<long long>(*maximum);
                    }
                }

                if (usedTokens <= 0) {
                    const json* current = FindClaudeObject(
                        *contextWindow,
                        { "current_usage", "currentUsage" }
                    );

                    if (current) {
                        usedTokens = ClaudeInputTokensFromUsage(*current);
                    }

                    if (usedTokens <= 0) {
                        const auto total = JsonUtils::get_instance()->NumberAny(
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
                        const auto maximum = JsonUtils::get_instance()->NumberAny(
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

        // Claude Code's own statusLine payload beats every inference below it:
        // it is the exact number the CLI is using, including a 1M beta session
        // and any CLAUDE_CODE_MAX_CONTEXT_TOKENS override. It is also the only
        // place the CLI publishes session spend. Trust it only for the session
        // it was actually written for.
        {
            const ClaudeStatusLinePayload statusLine = ReadClaudeStatusLinePayload();
            const bool matchesSession = statusLine.valid &&
                (statusLine.sessionId.empty() ||
                    statusLine.sessionId == session.stem().string());

            if (matchesSession && contextWindowTokens <= 0 &&
                statusLine.contextWindowTokens > 0) {
                contextWindowTokens = statusLine.contextWindowTokens;
                modelWindowFallback = false;
            }

            if (matchesSession && !statusLine.details.empty()) {
                local.run.sessionDetails = statusLine.details;
            }

            if (matchesSession && statusLine.hasCost) {
                local.run.costValid = true;
                local.run.sessionCostUsd = statusLine.totalCostUsd;
                local.run.sessionLinesAdded = statusLine.linesAdded;
                local.run.sessionLinesRemoved = statusLine.linesRemoved;
                local.run.sessionDurationMs = statusLine.durationMs;
                local.run.sessionApiDurationMs = statusLine.apiDurationMs;
            }
        }

        if (contextWindowTokens <= 0 && contextWindowHint > 0) {
            contextWindowTokens = contextWindowHint;
            modelWindowFallback = false;
        }

        if (contextWindowTokens <= 0) {
            const ClaudeContextWindow resolved = ResolveClaudeContextWindow(model);
            contextWindowTokens = resolved.tokens;
            modelWindowFallback = contextWindowTokens > 0 && !resolved.fromRegistry;
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
        std::vector<ClaudeFile> candidates = LatestClaudeSessionFiles();

        // The supplied Desktop ASAR persists cliSessionId in
        // LOCALAPPDATA\\Claude-3p\\claude-code-sessions even though isRunning
        // itself is intentionally NOT serialized. Use the ID only to pick the
        // correct foreground transcript; never treat the session JSON as an
        // authoritative running flag.
        if (desktopHint.valid && !desktopHint.cliSessionId.empty()) {
            // Only let the hint win while the session it names is still the one
            // being written to. Desktop keeps pointing at the last CLI session
            // it launched, so an old hint would pin us to a stale transcript and
            // report "nothing running" while another session is mid-turn - which
            // is exactly what it did.
            std::filesystem::file_time_type newestWrite{};
            std::filesystem::file_time_type hintWrite{};
            bool hintSeen = false;

            for (const ClaudeFile& candidate : candidates) {
                if (candidate.writeTime > newestWrite) newestWrite = candidate.writeTime;

                if (candidate.path.stem().string() == desktopHint.cliSessionId) {
                    hintWrite = candidate.writeTime;
                    hintSeen = true;
                }
            }

            const auto behindSeconds = hintSeen
                ? std::chrono::duration_cast<std::chrono::seconds>(newestWrite - hintWrite).count()
                : 0LL;

            // A minute of slack covers the CLI trailing Desktop's own write
            // without letting a session from an hour ago hold the slot.
            constexpr long long kHintStaleSeconds = 60;

            if (hintSeen && behindSeconds <= kHintStaleSeconds) {
                std::stable_sort(
                    candidates.begin(),
                    candidates.end(),
                    [&](const ClaudeFile& left, const ClaudeFile& right) {
                        const bool leftMatch =
                            left.path.stem().string() == desktopHint.cliSessionId;
                        const bool rightMatch =
                            right.path.stem().string() == desktopHint.cliSessionId;
                        return leftMatch && !rightMatch;
                    }
                );
            }
        }

        for (const ClaudeFile& candidate : candidates) {
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

            // How long the transcript has been quiet. While a turn is running,
            // a file that has stopped growing means the request is in flight
            // and we are waiting on the API, not working locally.
            if (local.run.running) {
                const auto quiet = std::filesystem::file_time_type::clock::now() -
                    candidate.writeTime;
                const long long quietSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(quiet).count();
                local.run.idleSeconds = (std::max)(0LL, quietSeconds);
            }

            if (local.context.valid || local.context.compacting || local.run.running) {
                return local;
            }
        }

        return {};
    }

    LocalTelemetry ReadLocalTelemetry() {
        return ReadLatestClaudeLocalTelemetry();
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

    // An ISO or epoch time field as unix seconds; 0 when absent or unreadable.
    static long long ClaudeUnixSeconds(const json& object, const char* key) {
        return object.is_object() && object.contains(key) && !object.at(key).is_null()
            ? TimePointToUnixSeconds(ParseTimeFlexible(object.at(key)))
            : 0;
    }

    // cedar_ember: free limit resets, ported from claude.ai's own parser.
    // Read-only - claiming a reset is a POST that is never sent from here.
    static LimitResets ParseLimitResets(const json& root, long long now) {
        // The limit types the page knows; any other blocker is ignored.
        static const char* const kLimitTypes[] = {
            "five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet",
            "seven_day_oauth_apps", "seven_day_cowork", "seven_day_omelette",
            "seven_day_overage_included"
        };
        static const std::regex kGrantId("^[a-z0-9_-]{1,40}$");

        const auto isTrue = [](const json& object, const char* key) {
            const auto it = object.find(key);
            return it != object.end() && it->is_boolean() && it->get<bool>();
        };
        const auto integer = [](const json& object, const char* key) -> std::optional<long long> {
            const auto it = object.find(key);
            if (it == object.end() || !it->is_number() || std::floor(it->get<double>()) != it->get<double>()) {
                return std::nullopt;
            }
            return static_cast<long long>(it->get<double>());
        };
        const auto strings = [](const json& object, const char* key) {
            std::vector<std::string> values;
            const auto it = object.find(key);
            if (it != object.end() && it->is_array()) {
                for (const json& value : *it) {
                    if (value.is_string()) values.push_back(value.get<std::string>());
                }
            }
            return values;
        };

        LimitResets resets;
        const json* block = FindClaudeObject(root, { "cedar_ember" });

        if (!block || !block->contains("eligible") || !block->at("eligible").is_boolean()) {
            return resets;
        }

        resets.valid = true;
        resets.eligible = block->at("eligible").get<bool>();
        resets.atLimit = isTrue(*block, "at_limit") && block->contains("exhausted") &&
            block->at("exhausted").is_array() && !block->at("exhausted").empty();
        resets.cooldownUntilUnixSeconds = ClaudeUnixSeconds(*block, "cooldown_until");

        if (!resets.eligible || !block->contains("grants") || !block->at("grants").is_array()) {
            return resets;
        }

        for (const json& grant : block->at("grants")) {
            const std::string id = JsonUtils::get_instance()->String(grant, "id");
            const std::optional<long long> total = integer(grant, "resets_total");
            const std::optional<long long> left = integer(grant, "resets_left");
            const long long endsAt = ClaudeUnixSeconds(grant, "ends_at");

            // Malformed, expired, or a duplicate id: skipped.
            if (!std::regex_match(id, kGrantId) || !total || *total < 1 || !left || *left < 0 ||
                endsAt <= now ||
                std::any_of(resets.grants.begin(), resets.grants.end(), [&](const LimitReset& kept) {
                    return kept.id == id;
                })) {
                continue;
            }

            const std::vector<std::string> clears = strings(grant, "clears");
            bool blocked = false;

            for (const std::string& type : strings(grant, "blocking")) {
                blocked = blocked ||
                    (std::find(std::begin(kLimitTypes), std::end(kLimitTypes), type) != std::end(kLimitTypes) &&
                        std::find(clears.begin(), clears.end(), type) == clears.end());
            }

            LimitReset reset;
            reset.id = id;
            reset.total = static_cast<int>(*total);
            reset.paused = isTrue(grant, "paused");
            reset.usableNow = isTrue(grant, "usable_now") && !reset.paused && !blocked;
            reset.endsAtUnixSeconds = endsAt;

            // Shown as the resets left, or 1 when none are counted but it is
            // usable right now.
            const int remaining = static_cast<int>(std::min(*left, *total));
            if (remaining <= 0 && !reset.usableNow) {
                continue;
            }
            reset.left = remaining > 0 ? remaining : 1;

            reset.type = JsonUtils::get_instance()->String(grant, "type");
            if (reset.type.empty()) {
                const auto fiveHour = [](const std::string& type) { return type == "five_hour"; };
                reset.type = std::none_of(clears.begin(), clears.end(), fiveHour) ? "weekly"
                    : std::all_of(clears.begin(), clears.end(), fiveHour) ? "session"
                    : "both";
            }

            reset.title = isTrue(grant, "gives_back_weekly") ? "Fable reset"
                : reset.type == "weekly" ? "Weekly reset"
                : reset.type == "session" ? "Session reset"
                : reset.type == "both" ? "Session and weekly reset"
                : Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(grant, "label"), "Limit reset" });

            resets.grants.push_back(std::move(reset));
        }

        std::sort(resets.grants.begin(), resets.grants.end(), [](const LimitReset& a, const LimitReset& b) {
            return a.endsAtUnixSeconds < b.endsAtUnixSeconds;
        });

        return resets;
    }

    // One-time credits that never reset. iguana_necktie is the cloud session
    // credit; the rest share one window shape, and their real titles live in
    // a server message catalog, so they get a generic one.
    static std::vector<PromoCredit> ParsePromoCredits(const json& root, long long now) {
        // limit_dollars > 0 with remaining_dollars, or else limit - used.
        const auto readDollars = [](const json& object, PromoCredit& credit) {
            const std::optional<double> limit = JsonUtils::get_instance()->NumberOpt(object, "limit_dollars");
            std::optional<double> remaining = JsonUtils::get_instance()->NumberOpt(object, "remaining_dollars");
            const std::optional<double> used = JsonUtils::get_instance()->NumberOpt(object, "used_dollars");

            if (!limit || *limit <= 0.0 || (!remaining && !used)) {
                return false;
            }

            credit.hasDollars = true;
            credit.limitDollars = *limit;
            credit.remainingDollars = remaining ? *remaining : *limit - *used;
            return true;
        };
        const auto spentPercent = [](const PromoCredit& credit) {
            return static_cast<float>(Math::get_instance()->ClampPercentDouble(
                (credit.limitDollars - credit.remainingDollars) / credit.limitDollars * 100.0));
        };

        std::vector<PromoCredit> credits;

        // Whole dollars; the page hides the card below $1 left.
        if (const json* object = FindClaudeObject(root, { "iguana_necktie" })) {
            PromoCredit credit{ "iguana_necktie", "Cloud session credits", "Included credit" };

            if (readDollars(*object, credit)) {
                credit.remainingDollars = std::floor(credit.remainingDollars);

                if (credit.remainingDollars >= 1.0) {
                    credit.usedPercent = spentPercent(credit);
                    credit.expiresAtUnixSeconds = ClaudeUnixSeconds(*object, "resets_at");
                    credit.lockedReason = JsonUtils::get_instance()->String(*object, "locked_reason");
                    credits.push_back(std::move(credit));
                }
            }
        }

        static const std::pair<const char*, const char*> kCredits[] = {
            { "cinder_cove", "Claude Code and Cowork credit" },
            { "copper_kite", "Promotional credit" },
            { "harbor_lantern", "Promotional credit" },
            { "wattle_ember", "Promotional credit" },
            { "tangelo", "Promotional credit" },
            { "kumquat_keel", "Promotional credit" },
            { "nimbus_quill", "Promotional credit" },
            { "brass_thimble", "Promotional credit" },
            { "promo_credits", "Promotional credit" },
            { "amber_ladder", "Promotional credit" },
            { "granite_teacup", "Promotional credit" },
            { "amber_cistern", "Promotional credit" },
            { "juniper_tide", "Promotional credit" },
            { "amber_gauge", "Promotional credit" },
            { "starry_waterfall", "Promotional credit" },
        };

        for (const auto& [key, title] : kCredits) {
            const json* object = FindClaudeObject(root, { key });

            if (!object) {
                continue;
            }

            PromoCredit credit{ key, title, "Included credit" };
            const std::optional<double> utilization = JsonUtils::get_instance()->NumberOpt(*object, "utilization");

            if (!readDollars(*object, credit) && !utilization) {
                continue;
            }

            credit.usedPercent = utilization
                ? static_cast<float>(Math::get_instance()->ClampPercentDouble(*utilization))
                : spentPercent(credit);

            const long long expires = ClaudeUnixSeconds(*object, "resets_at");
            credit.expiresAtUnixSeconds = expires > now ? expires : 0;
            credit.lockedReason = JsonUtils::get_instance()->String(*object, "locked_reason");

            const std::string models = JsonUtils::get_instance()->String(*object, "covered_models_label");
            if (credit.key == "wattle_ember" && !models.empty()) {
                credit.title += " \xC2\xB7 " + models;
            }

            credits.push_back(std::move(credit));
        }

        return credits;
    }

    // seven_day_breakdown.rows[]: this week's usage by surface.
    static std::vector<BreakdownRow> ParseWeeklyBreakdown(const json& root) {
        std::vector<BreakdownRow> rows;
        const json* breakdown = FindClaudeObject(root, { "seven_day_breakdown" });

        if (!breakdown || !breakdown->contains("rows") || !breakdown->at("rows").is_array()) {
            return rows;
        }

        for (const json& row : breakdown->at("rows")) {
            const std::string name = JsonUtils::get_instance()->String(row, "display_name");
            const std::optional<double> percent = JsonUtils::get_instance()->NumberOpt(row, "percent");

            if (!name.empty() && percent) {
                rows.push_back({ name, static_cast<float>(Math::get_instance()->ClampPercentDouble(*percent)) });
            }
        }

        return rows;
    }

    static Snapshot ParseSnapshot(const json& root, const ClaudeOAuth& oauth, const std::string& usageHeading) {
        Snapshot snapshot;

        // Diagnostic only: set AQC_CLAUDE_DUMP_USAGE to a path to capture the
        // raw usage payload. Off unless the variable is set.
        {
            const std::string dumpPath =
                Network::get_instance()->GetEnvText("AQC_CLAUDE_DUMP_USAGE");
            if (!dumpPath.empty()) {
                std::ofstream out(dumpPath, std::ios::binary);
                if (out) out << root.dump(2);
            }
        }

        snapshot.usageHeading = usageHeading;
        snapshot.plan = FormatPlan(oauth);
        snapshot.statusText = "Plan: " + snapshot.plan;
        snapshot.lastUpdated = "just now";

        const bool hasServerDrivenLimits = ParseServerDrivenLimits(root, snapshot);

        // Desktop's Code tab reads 5-hour, weekly and Sonnet from the named
        // fields and only per-model rows from limits[], so a limits[] array
        // (even an empty one) must not hide them. Fill whatever it left empty.
        // A null five_hour (Ultra has no 5-hour limit) leaves Session invalid.
        if (!snapshot.currentSession.valid) {
            snapshot.currentSession = ParseWindow(
                root,
                "five_hour",
                "Session",
                "Starts when a message is sent"
            );
        }

        if (!snapshot.weeklyAllModels.valid) {
            snapshot.weeklyAllModels = ParseWindow(
                root,
                "seven_day",
                "Weekly",
                "All models"
            );
        }

        if (!snapshot.weeklySonnet.valid) {
            snapshot.weeklySonnet = ParseWindow(
                root,
                "seven_day_sonnet",
                "Sonnet",
                "Sonnet only"
            );
        }

        if (!hasServerDrivenLimits) {
            // "omelette" is Claude Design, a product - not the Fable model.
            AddAdditionalLimit(
                snapshot,
                ParseWindow(root, "seven_day_omelette", "Claude Design", "Product-specific weekly limit")
            );

            if (!snapshot.weeklyFable.valid) {
                snapshot.weeklyFable = ParseWindow(root, "seven_day_fable", "Fable", "Model-specific weekly limit");
            }

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

        // Desktop's tray shows this grant beside the limits[] rows.
        AddAdditionalLimit(
            snapshot,
            ParseWindow(root, "omelette_promotional", "Claude Design grant", "Promotional grant")
        );

        const long long now = static_cast<long long>(std::time(nullptr));
        snapshot.limitResets = ParseLimitResets(root, now);
        snapshot.promoCredits = ParsePromoCredits(root, now);
        snapshot.weeklyBreakdown = ParseWeeklyBreakdown(root);
        snapshot.credits = ParseCredits(root);
        FinalizeClaudeAccess(snapshot);
        return snapshot;
    }

    // organization_type is the upstream value Claude Desktop switches on; the
    // web account payload exposes the same fact through rate_limit_tier, so
    // accept either spelling.
    static std::string SubscriptionFromOrganizationType(const std::string& raw) {
        const std::string lower = Text::get_instance()->ToLowerCopy(raw);

        // Before "max": an Ultra tier string can contain both.
        if (lower.find("ultra") != std::string::npos) return "ultra";
        if (lower.find("max") != std::string::npos) return "max";
        if (lower.find("pro") != std::string::npos) return "pro";
        if (lower.find("enterprise") != std::string::npos) return "enterprise";
        if (lower.find("team") != std::string::npos) return "team";
        if (lower.find("free") != std::string::npos) return "free";

        return {};
    }

    // claude.ai organization objects carry capabilities, not organization_type:
    // claude_max / claude_pro, and raven (Team) or raven_enterprise.
    static std::string SubscriptionFromCapabilities(const json& org) {
        if (!org.contains("capabilities") || !org.at("capabilities").is_array()) {
            return {};
        }

        const json& capabilities = org.at("capabilities");
        const auto has = [&](const char* name) {
            return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end();
        };

        if (has("claude_max")) return "max";
        if (has("claude_pro")) return "pro";
        if (has("raven_enterprise")) return "enterprise";
        if (has("raven")) return "team";
        return {};
    }

    // Fills subscriptionType/rateLimitTier from an /api/account body. Returns
    // false when the payload has nothing usable, leaving the plan unresolved
    // rather than inventing one.
    static bool ApplyAccountPlan(
        const std::string& body,
        const std::string& organizationId,
        ClaudeOAuth& plan
    ) {
        json parsed = JsonUtils::get_instance()->ParseOrNull(body);

        if (!parsed.is_object()) {
            return false;
        }

        const json* memberships = parsed.contains("memberships") && parsed.at("memberships").is_array()
            ? &parsed.at("memberships")
            : nullptr;

        if (!memberships) {
            return false;
        }

        const json* chosen = nullptr;

        // Prefer the membership for the organization the usage call used; a
        // machine can be signed into several.
        for (const json& membership : *memberships) {
            if (!membership.is_object()) continue;

            const json* org = membership.contains("organization") && membership.at("organization").is_object()
                ? &membership.at("organization")
                : nullptr;

            if (org && !organizationId.empty() &&
                JsonUtils::get_instance()->String(*org, "uuid") == organizationId) {
                chosen = &membership;
                break;
            }
            if (!chosen && org) {
                chosen = &membership;
            }
        }

        if (!chosen) {
            return false;
        }

        const json& org = chosen->at("organization");

        std::string tier = JsonUtils::get_instance()->String(org, "rate_limit_tier");
        if (tier.empty()) tier = JsonUtils::get_instance()->String(org, "rateLimitTier");

        std::string subscription = SubscriptionFromOrganizationType(
            JsonUtils::get_instance()->String(org, "organization_type"));

        if (subscription.empty()) {
            subscription = SubscriptionFromCapabilities(org);
        }

        if (subscription.empty()) {
            subscription = SubscriptionFromOrganizationType(tier);
        }

        // The tray labels the plan with this when present, e.g. "Ultra".
        const std::string displayName = JsonUtils::get_instance()->String(org, "plan_display_name");

        if (plan.displayName.empty() && !displayName.empty()) {
            plan.displayName = displayName;
        }

        // A team seat refines to Standard/Premium, which is a real distinction
        // in the reference tools rather than cosmetic.
        const std::string seat = Text::get_instance()->ToLowerCopy(
            JsonUtils::get_instance()->String(*chosen, "seat_tier"));

        if (subscription == "team" && !seat.empty() && seat != "unassigned") {
            if (seat.find("tier_1") != std::string::npos || seat.find("premium") != std::string::npos) {
                subscription = "team premium";
            }
            else if (seat.find("standard") != std::string::npos) {
                subscription = "team standard";
            }
        }

        if (subscription.empty() && tier.empty()) {
            return false;
        }

        if (plan.subscriptionType.empty()) plan.subscriptionType = subscription;
        if (plan.rateLimitTier.empty()) plan.rateLimitTier = tier;

        return !plan.subscriptionType.empty() || !plan.rateLimitTier.empty();
    }

    struct PlanRenewal {
        long long atUnixSeconds = 0;
        bool ends = false; // nothing will be charged: the plan ends then
    };

    // subscription_details: { next_charge_at, plan_ending_at, payment_method,
    // ... }. The next charge is the renewal; without one the plan ends at
    // plan_ending_at. payment_method is dropped while parsing, so it is never
    // read or held.
    static PlanRenewal ParsePlanRenewal(const std::string& body) {
        const json details = json::parse(
            body,
            [](int, json::parse_event_t event, json& parsed) {
                return !(event == json::parse_event_t::key && parsed == "payment_method");
            },
            false
        );

        PlanRenewal renewal;
        const long long charge = ClaudeUnixSeconds(details, "next_charge_at");
        renewal.atUnixSeconds = charge > 0 ? charge : ClaudeUnixSeconds(details, "plan_ending_at");
        renewal.ends = charge <= 0 && renewal.atUnixSeconds > 0;
        return renewal;
    }

    // Per-organization answers that rarely change (plan name, renewal):
    // fetched at most once an hour each, so a renamed plan still shows up
    // without a restart. Only a usable answer is kept; a failure is simply
    // asked again on the next refresh, and an older answer stands meanwhile.
    template <class Value>
    static std::optional<Value> HourlyPerOrganization(
        const std::string& organizationId,
        const std::function<std::optional<Value>()>& fetch
    ) {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::pair<Value, long long>> cache;
        std::lock_guard<std::mutex> lock(mutex);

        const long long now = static_cast<long long>(std::time(nullptr));
        auto known = cache.find(organizationId);

        if (known == cache.end() || now - known->second.second >= 60 * 60) {
            if (std::optional<Value> fresh = fetch()) {
                known = cache.insert_or_assign(organizationId, std::make_pair(std::move(*fresh), now)).first;
            }
        }

        return known == cache.end() ? std::nullopt : std::optional<Value>(known->second.first);
    }

    static bool MergeCreditsFromBody(
        Snapshot& snapshot,
        const std::string& body
    ) {
        if (body.empty()) {
            return false;
        }

        json parsed = JsonUtils::get_instance()->ParseOrNull(body);

        if (!parsed.is_object() && !parsed.is_array()) {
            return false;
        }

        UsageCredits credits = ParseCredits(parsed);

        if (credits.reported &&
            CreditsInformationScore(credits) > CreditsInformationScore(snapshot.credits)) {
            snapshot.credits = std::move(credits);
            return true;
        }

        return false;
    }

    // Cookie sessions only: an OAuth-cache snapshot already came from
    // /api/oauth/usage, the one other place spend data lives.
    static void FetchDesktopCreditsSupplement(
        Snapshot& snapshot,
        const ClaudeDesktopAuth::Result& desktop
    ) {
        if (desktop.source != ClaudeDesktopAuth::AuthSource::BrowserCookies ||
            !CreditsNeedSupplement(snapshot.credits)) {
            return;
        }

        auto mergeSuccessful = [&](const Network::HttpResponse& response) {
            if (response.statusCode >= 200 && response.statusCode < 300) {
                MergeCreditsFromBody(snapshot, response.body);
            }
        };

        // The session only carries a cached token of its own organization
        // (AcquireCurrentSession). The token remains in memory only.
        if (!desktop.accessToken.empty()) {
            mergeSuccessful(FetchUsage(desktop.accessToken, true));
        }

        if (!CreditsNeedSupplement(snapshot.credits)) {
            return;
        }

        // Claude has migrated between claude.ai and claude.com. Query the
        // alternate official host with the same in-memory authentication and
        // merge only extra_usage; no credentials are persisted or copied.
        const std::string alternate = desktop.baseUrl.find("claude.com") != std::string::npos
            ? "https://claude.ai"
            : "https://claude.com";

        mergeSuccessful(DesktopGet(desktop, alternate, DesktopUsagePath(desktop)));
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

    // Claude Desktop's tray poller records its own plan usage to disk (see
    // ClaudeDesktopAuth::ReadLatestPlanUsage for its gated 15-minute cadence).
    // When the live request cannot be made (Cookies locked, token expired,
    // offline) that file is still an honest reading, so surface it instead of
    // showing empty bars. It carries percentages only - no reset timestamps -
    // which is why it is a fallback and never preferred over a live response.
    static std::optional<Snapshot> DesktopSnapshotFromPlanUsage(
        const ClaudeOAuth& desktopPlan,
        const std::string& failureDetail
    ) {
        const ClaudeDesktopAuth::PlanUsageSample sample =
            ClaudeDesktopAuth::ReadLatestPlanUsage();

        if (!sample.valid || sample.windows.empty()) {
            return std::nullopt;
        }

        json root = json::object();

        for (const ClaudeDesktopAuth::PlanUsageWindow& window : sample.windows) {
            json entry = json::object({ { "utilization", window.usedPercent } });

            if (window.resetAtUnixSeconds > 0) {
                entry["resets_at"] = window.resetAtUnixSeconds;
            }

            root[window.key] = std::move(entry);
        }

        Snapshot snapshot = ParseSnapshot(root, desktopPlan, "Claude Desktop usage");

        // xu is the extra-usage percentage the history file records alongside
        // the windows; ParseSnapshot has no field for it, so apply it directly
        // and classify access again, since it can exhaust access on its own.
        if (sample.hasExtraUsage) {
            snapshot.credits.reported = true;
            snapshot.credits.valid = true;
            snapshot.credits.enabled = true;
            snapshot.credits.hasUsedPercent = true;
            snapshot.credits.usedPercent = static_cast<float>(
                Math::get_instance()->ClampPercentDouble(sample.extraUsagePercent));
            FinalizeClaudeAccess(snapshot);
        }

        const long long ageSeconds = sample.sampledAtUnixMs > 0
            ? static_cast<long long>(std::time(nullptr)) - sample.sampledAtUnixMs / 1000
            : -1;

        std::string age = "Claude Desktop's own recorded usage";

        if (ageSeconds >= 0) {
            const long long minutes = ageSeconds / 60;
            age += minutes > 0
                ? " (sampled " + std::to_string(minutes) + "m ago)"
                : " (sampled just now)";
        }

        snapshot.lastUpdated = "from Desktop history";
        snapshot.statusText = "Plan: " + snapshot.plan + " | Source: " + age;
        snapshot.access.detail = failureDetail.empty()
            ? age + ". Reset times are derived from the recorded series."
            : age + ". Live request unavailable: " + failureDetail;
        snapshot.accountKey = "desktop:" + sample.organizationId;
        return snapshot;
    }

    static Snapshot FetchDesktopSnapshot(const ClaudeDesktopAuth::Result& initialDesktop) {
        ClaudeDesktopAuth::Result desktop = initialDesktop;
        ClaudeOAuth desktopPlan;
        desktopPlan.subscriptionType = desktop.subscriptionType;
        desktopPlan.rateLimitTier = desktop.rateLimitTier;

        const auto fetchUsage = [](const ClaudeDesktopAuth::Result& session) {
            return session.source == ClaudeDesktopAuth::AuthSource::OAuthCache
                ? FetchUsage(session.accessToken, true)
                : DesktopGet(session, session.baseUrl, DesktopUsagePath(session));
        };

        Network::HttpResponse response = fetchUsage(desktop);

        if (desktop.source == ClaudeDesktopAuth::AuthSource::BrowserCookies &&
            (response.statusCode == 401 || response.statusCode == 403)) {
            // Re-read the cookie DB in case the account changed between the
            // first read and the request.
            ClaudeDesktopAuth::Result current = ClaudeDesktopAuth::AcquireCurrentSession();

            if (current.kind == ClaudeDesktopAuth::ResultKind::Success) {
                response = fetchUsage(current);
                desktop = current;
                desktopPlan.subscriptionType = desktop.subscriptionType;
                desktopPlan.rateLimitTier = desktop.rateLimitTier;
            }
        }

        if (response.statusCode == 429) {
            const std::string detail = "the usage request is rate limited";

            if (auto local = DesktopSnapshotFromPlanUsage(desktopPlan, detail)) {
                return *local;
            }

            return FailedSnapshot(
                "Claude Desktop usage",
                "Claude Desktop usage request is rate limited",
                response.statusCode,
                response.body
            );
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            const std::string detail = response.statusCode > 0
                ? "HTTP " + std::to_string(response.statusCode)
                : "the usage request could not be sent";

            if (auto local = DesktopSnapshotFromPlanUsage(desktopPlan, detail)) {
                return *local;
            }

            return FailedSnapshot(
                "Claude Desktop usage",
                "Claude Desktop usage failed: " + detail,
                response.statusCode,
                response.body
            );
        }

        // The plan is not in the usage payload, so ask for it separately. The
        // OAuth cache usually supplies the tier, but only the account payload
        // carries the display name Desktop shows (e.g. "Ultra"). Best effort:
        // a failure costs the plan label, nothing else.
        const std::optional<ClaudeOAuth> accountPlan = HourlyPerOrganization<ClaudeOAuth>(
            desktop.organizationId,
            [&]() -> std::optional<ClaudeOAuth> {
                const Network::HttpResponse account = DesktopGet(desktop, desktop.baseUrl, "/api/account");
                ClaudeOAuth plan;

                if (account.statusCode >= 200 && account.statusCode < 300 &&
                    ApplyAccountPlan(account.body, desktop.organizationId, plan)) {
                    return plan;
                }

                return std::nullopt;
            }
        );

        if (accountPlan) {
            if (desktopPlan.subscriptionType.empty()) desktopPlan.subscriptionType = accountPlan->subscriptionType;
            if (desktopPlan.rateLimitTier.empty()) desktopPlan.rateLimitTier = accountPlan->rateLimitTier;
            desktopPlan.displayName = accountPlan->displayName;
        }

        Snapshot snapshot = ParseSnapshot(
            JsonUtils::get_instance()->ParseRequired(response.body),
            desktopPlan,
            "Claude Desktop usage"
        );

        // When the plan renews, or ends. The cookie session is what
        // subscription_details accepts; a failure just leaves it unknown.
        if (desktop.source == ClaudeDesktopAuth::AuthSource::BrowserCookies) {
            const std::optional<PlanRenewal> renewal = HourlyPerOrganization<PlanRenewal>(
                desktop.organizationId,
                [&]() -> std::optional<PlanRenewal> {
                    const Network::HttpResponse details = DesktopGet(
                        desktop,
                        desktop.baseUrl,
                        "/api/organizations/" + desktop.organizationId + "/subscription_details"
                    );
                    const PlanRenewal parsed = details.statusCode >= 200 && details.statusCode < 300
                        ? ParsePlanRenewal(details.body)
                        : PlanRenewal{};

                    return parsed.atUnixSeconds > 0 ? std::optional<PlanRenewal>(parsed) : std::nullopt;
                }
            );

            if (renewal) {
                snapshot.planRenewsAtUnixSeconds = renewal->atUnixSeconds;
                snapshot.planEnds = renewal->ends;
            }
        }

        // Keep notification state separate for each Desktop organization.
        // The organization ID is already required for the usage request and is
        // retained only in memory.
        snapshot.accountKey = "desktop:" + desktop.organizationId;
        FetchDesktopCreditsSupplement(snapshot, desktop);
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

        // Fail fast with something the user can act on. Refreshing an
        // inference-only or under-scoped token returns the same token class,
        // so retrying it just turns one 403 into two.
        if (!ClaudeTokenCanReadUsage(codeCredentials.oauth)) {
            return FailedSnapshot(
                usageHeading,
                codeCredentials.oauth.inferenceOnly
                    ? sourceName + " is an inference-only token (CLAUDE_CODE_OAUTH_TOKEN / "
                      "`claude setup-token`). The usage endpoint requires a `claude login` "
                      "session; unset the variable or pick a different Claude account source."
                    : sourceName + " is missing the " + kClaudeUsageScope + " scope required "
                      "to read usage. Run `claude` and sign in again."
            );
        }

        Network::HttpResponse response = FetchUsage(codeCredentials.oauth.accessToken, false);

        // 401 only. A 403 means the token authenticated but may not read
        // usage, and a refresh would spend the single-use refresh token for
        // the same class of token.
        if (response.statusCode == 401 &&
            codeCredentials.canSave &&
            !codeCredentials.oauth.refreshToken.empty() &&
            RefreshToken(codeCredentials)) {
            response = FetchUsage(codeCredentials.oauth.accessToken, false);
        }

        if (response.statusCode == 403) {
            return FailedSnapshot(
                usageHeading,
                sourceName + " usage was rejected (HTTP 403). The token authenticated but is "
                "not allowed to read usage - it most likely lacks the " + kClaudeUsageScope +
                " scope. Run `claude` and sign in again.",
                response.statusCode,
                response.body
            );
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
                sourceName + " usage failed: " + (response.statusCode > 0
                    ? "HTTP " + std::to_string(response.statusCode)
                    : std::string("the usage request could not be sent")),
                response.statusCode,
                response.body
            );
        }

        Snapshot snapshot = ParseSnapshot(
            JsonUtils::get_instance()->ParseRequired(response.body),
            codeCredentials.oauth,
            usageHeading
        );

        // Never the token itself. A credentials-file login is keyed by its
        // account id, which survives the token rotating on every refresh; an
        // environment token never rotates here, and it may belong to another
        // account than .claude.json, so it keeps a process-local hash.
        const std::string accountId = codeCredentials.canSave ? ClaudeCodeAccountId() : std::string();
        snapshot.accountKey = sourceName + ":" + (accountId.empty()
            ? std::to_string(std::hash<std::string>{}(codeCredentials.oauth.accessToken))
            : accountId);
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

                // The credential could not be read, but Desktop's own recorded
                // usage still can be. Show the real numbers rather than an
                // empty panel; the reason stays visible in the status detail.
                ClaudeOAuth unknownPlan;

                if (auto local = DesktopSnapshotFromPlanUsage(unknownPlan, detail)) {
                    return *local;
                }

                return FailedSnapshot(
                    "Claude Desktop usage",
                    "Claude Desktop is selected, but the account could not be read: " + detail
                );
            }

            // A Desktop credential/decryption error must remain visible. Auto mode
            // only falls through when Desktop genuinely has no signed-in session.
            if (desktop.kind == ClaudeDesktopAuth::ResultKind::Error) {
                // Auto is the mode that has to survive an account switch, and
                // the credential is exactly what goes stale across one. Desktop
                // records its own usage to disk either way, so prefer those real
                // numbers over an error panel; the reason stays in the detail.
                ClaudeOAuth unknownPlan;

                if (auto local = DesktopSnapshotFromPlanUsage(unknownPlan, desktop.detail)) {
                    return *local;
                }

                return FailedSnapshot(
                    "Claude Desktop usage",
                    "Claude Desktop account could not be read: " + desktop.detail
                );
            }

            // In Auto mode, a present credentials file is authoritative. If it is
            // malformed or expired, show that error instead of silently switching
            // to a different environment-token account.
            std::error_code ec;

            if (std::filesystem::exists(ClaudeCredentialsPath(), ec)) {
                return FetchCredentialsFileSnapshot(true);
            }

            return FetchEnvironmentSnapshot(true);
        }

        if (source == AccountSource::CredentialsFile) {
            return FetchCredentialsFileSnapshot(false);
        }

        return FetchEnvironmentSnapshot(false);
    }

    // 3P builds can cap tokens per window by admin policy
    // (HKLM/HKCU\SOFTWARE\Policies\Claude inferenceMaxTokensPerWindow and
    // inferenceTokenWindowHours). Desktop counts input + output tokens into
    // cowork_account_settings.json __token_usage { windowStartMs, inputTokens,
    // outputTokens } and starts a fresh window once the old one has elapsed.
    static std::optional<long long> ClaudePolicyNumber(const wchar_t* name) {
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
            DWORD number = 0;
            DWORD size = sizeof(number);

            if (RegGetValueW(root, L"SOFTWARE\\Policies\\Claude", name, RRF_RT_REG_DWORD, nullptr, &number, &size) == ERROR_SUCCESS) {
                return static_cast<long long>(number);
            }

            wchar_t text[32]{};
            size = sizeof(text);

            if (RegGetValueW(root, L"SOFTWARE\\Policies\\Claude", name, RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS) {
                return _wtoll(text);
            }
        }

        return std::nullopt;
    }

    static void AddClaudeTokenCap(Snapshot& snapshot) {
        const std::optional<long long> cap = ClaudePolicyNumber(L"inferenceMaxTokensPerWindow");
        const std::optional<long long> hours = ClaudePolicyNumber(L"inferenceTokenWindowHours");

        if (!cap || !hours || *cap <= 0 || *hours <= 0 || *hours > 720) {
            return;
        }

        json usage;
        const std::vector<ClaudeFile> files = ClaudeCoworkSettingsFiles();

        if (!files.empty()) {
            const json settings = JsonUtils::get_instance()->ParseOrNull(
                ClaudeCookieStore::ReadSharedText(files.front().path, 4 * kMegabyte)
            );

            if (settings.is_object() && settings.contains("__token_usage")) {
                usage = settings.at("__token_usage");
            }
        }

        using namespace std::chrono;
        const double nowMs = static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
        const double windowMs = static_cast<double>(*hours) * 3600000.0;
        const double startMs = JsonUtils::get_instance()->Number(usage, "windowStartMs");
        const bool current = startMs > 0.0 && startMs <= nowMs && nowMs < startMs + windowMs;
        const double used = current
            ? JsonUtils::get_instance()->Number(usage, "inputTokens") + JsonUtils::get_instance()->Number(usage, "outputTokens")
            : 0.0;
        const float percent = static_cast<float>(
            Math::get_instance()->ClampPercentDouble(used / static_cast<double>(*cap) * 100.0));

        UsageWindowModel::Builder window;
        window.Title("Token limit").Subtitle(std::to_string(*hours) + "-hour admin cap").UsedPercent(percent);

        if (current) {
            const system_clock::time_point resetAt{ milliseconds(static_cast<long long>(startMs + windowMs)) };
            window.Reset(TimePointToUnixSeconds(resetAt), Format::get_instance()->ResetRelative(resetAt));
        }

        AddAdditionalLimit(snapshot, window.Valid().Build());

        // In 3P mode this cap is the quota: hitting it stops every request.
        if (UsageTelemetry::IsExhausted(percent)) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = "Token limit reached";
        }
    }

    // <userData>\ccd-limit-auto-resume.json, written while Desktop's limit
    // auto-resume is on:
    //   { choices: { <accountId>: bool }, migrated: [...],
    //     windows: { "<accountId>/<orgId>": { <sessionId>: { resetsAt (epoch
    //       seconds), attempt 0..3, optedIn, armed } } } }
    // Desktop only keeps entries within 6 hours of now; a session is waiting
    // while its resetsAt is still ahead. Given an organization, only its own
    // windows count. Read-only.
    static AutoResume ReadAutoResume(const std::string& organizationId, long long now) {
        constexpr long long kHorizonSeconds = 6 * 60 * 60;
        const std::string suffix = "/" + organizationId;
        AutoResume resume;

        for (const std::filesystem::path& root : ClaudeDesktopAuth::UserDataCandidates()) {
            const json store = JsonUtils::get_instance()->ParseOrNull(
                ClaudeCookieStore::ReadSharedText(root / "ccd-limit-auto-resume.json", 4 * kMegabyte)
            );
            const json* windows = FindClaudeObject(store, { "windows" });

            if (!windows) {
                continue;
            }

            for (auto account = windows->begin(); account != windows->end(); ++account) {
                const std::string& key = account.key();

                if (!account.value().is_object() || (!organizationId.empty() &&
                    (key.size() < suffix.size() || key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0))) {
                    continue;
                }

                for (const json& session : account.value()) {
                    const long long resetsAt = JsonUtils::get_instance()->UnixSecondsField(session, "resetsAt");

                    if (resetsAt <= now || resetsAt > now + kHorizonSeconds) {
                        continue;
                    }

                    ++resume.waitingSessions;
                    resume.optedIn = resume.optedIn || JsonUtils::get_instance()->Bool(session, "optedIn");

                    if (resume.resetAtUnixSeconds == 0 || resetsAt < resume.resetAtUnixSeconds) {
                        resume.resetAtUnixSeconds = resetsAt;
                    }
                }
            }
        }

        resume.valid = resume.waitingSessions > 0;
        return resume;
    }

    // 3P builds log every turn to local-agent-mode-sessions\<acct>\<org>\
    // usage-ledger\YYYY-MM-DD[.N].ndjson, one file set per local date:
    //   { v: 1, ts, surface: chat|cowork|code, origin, sessionId?, isError,
    //     models: { <id>: { inputTokens, outputTokens, cacheReadTokens,
    //       cacheWriteTokens, webSearchRequests, cost?: { usd, basis } } } }
    // Sums the newest ledger's last 30 local days. Only 3P installs keep one;
    // without it this stays silent.
    static UsageLedger ReadUsageLedger(long long now) {
        static const std::wregex kLedgerName(LR"(^(\d{4}-\d{2}-\d{2})(?:\.(\d+))?\.ndjson$)");
        static const char* const kSurfaces[] = { "chat", "cowork", "code" };
        UsageLedger ledger;

        const std::vector<ClaudeFile> files = NewestClaudeFiles(
            ClaudeDesktopDataDirs("local-agent-mode-sessions"),
            [](const std::filesystem::path& path) {
                return _wcsicmp(path.parent_path().filename().c_str(), L"usage-ledger") == 0 &&
                    std::regex_match(path.filename().wstring(), kLedgerName);
            },
            SIZE_MAX
        );

        if (files.empty()) {
            return ledger;
        }

        // The window's first local date. Noon keeps a DST shift from moving it.
        std::time_t start = static_cast<std::time_t>(now);
        std::tm day{};
        localtime_s(&day, &start);
        day.tm_mday -= ledger.windowDays - 1;
        day.tm_hour = 12;
        day.tm_isdst = -1;
        std::mktime(&day);

        wchar_t firstDate[11]{};
        std::wcsftime(firstDate, 11, L"%Y-%m-%d", &day);

        const std::filesystem::path folder = files.front().path.parent_path();
        long long surfaceTokens[3] = {};
        bool surfaceSeen[3] = {};

        for (const ClaudeFile& file : files) {
            if (file.path.parent_path() != folder ||
                file.path.filename().wstring().compare(0, 10, firstDate) < 0) {
                continue;
            }

            std::istringstream lines(ClaudeCookieStore::ReadSharedText(file.path, 64 * kMegabyte));

            for (std::string line; std::getline(lines, line);) {
                const json entry = JsonUtils::get_instance()->ParseOrNull(line);
                const std::string surface = JsonUtils::get_instance()->String(entry, "surface");
                const size_t slot = static_cast<size_t>(
                    std::find(std::begin(kSurfaces), std::end(kSurfaces), surface) - std::begin(kSurfaces));

                if (JsonUtils::get_instance()->Number(entry, "v") != 1.0 || slot >= std::size(kSurfaces)) {
                    continue;
                }

                ledger.valid = true;
                surfaceSeen[slot] = true;

                if (JsonUtils::get_instance()->String(entry, "origin") != "connection_probe") {
                    ++ledger.turns;
                }

                if (JsonUtils::get_instance()->Bool(entry, "isError")) {
                    ++ledger.errorTurns;
                }

                const json* models = FindClaudeObject(entry, { "models" });

                if (!models) {
                    continue;
                }

                for (const json& model : *models) {
                    const auto tokens = [&](const char* key) {
                        return static_cast<long long>(std::max(0.0, JsonUtils::get_instance()->Number(model, key)));
                    };
                    const long long input = tokens("inputTokens");
                    const long long output = tokens("outputTokens");

                    ledger.inputTokens += input;
                    ledger.outputTokens += output;
                    ledger.cacheReadTokens += tokens("cacheReadTokens");
                    ledger.cacheWriteTokens += tokens("cacheWriteTokens");
                    surfaceTokens[slot] += input + output;

                    if (const json* cost = FindClaudeObject(model, { "cost" })) {
                        if (const auto usd = JsonUtils::get_instance()->NumberOpt(*cost, "usd")) {
                            ledger.costValid = true;
                            ledger.costUsd += *usd;
                        }
                    }
                }
            }
        }

        for (size_t slot = 0; slot < std::size(kSurfaces); ++slot) {
            if (surfaceSeen[slot]) {
                ledger.surfaceTokens.emplace_back(kSurfaces[slot], surfaceTokens[slot]);
            }
        }

        return ledger;
    }

    // ---------------------------------------------------------------------
    // Weekly pace, ported from claude.ai's Settings > Usage page.
    // ---------------------------------------------------------------------
    static const char* const kWeekdayNames[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };

    static std::tm LocalTime(long long unixSeconds) {
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        localtime_s(&local, &time);
        return local;
    }

    // The local calendar day as a running day number.
    static long long LocalDay(long long unixSeconds) {
        std::tm local = LocalTime(unixSeconds);
        return static_cast<long long>(_mkgmtime(&local)) / 86400;
    }

    // Small hours count as the night before, unless that night is already
    // behind today.
    static long long PaceShift(long long unixSeconds, long long now) {
        if (LocalTime(unixSeconds).tm_hour >= 5) {
            return unixSeconds;
        }

        const long long earlier = unixSeconds - 5 * 60 * 60;
        return LocalDay(earlier) < LocalDay(now) ? unixSeconds : earlier;
    }

    // "tonight", "tomorrow morning", "Wednesday night"...
    static std::string PaceRunOut(long long runsOut, long long now) {
        const int hour = LocalTime(runsOut).tm_hour;
        const std::string part = hour < 5 ? "night"
            : hour < 12 ? "morning"
            : hour < 17 ? "afternoon"
            : hour < 21 ? "evening"
            : "night";
        const long long shifted = PaceShift(runsOut, now);
        const long long days = LocalDay(shifted) - LocalDay(now);

        if (days == 0) {
            return part == "night" ? "tonight" : "this " + part;
        }

        if (days == 1) {
            return "tomorrow " + part;
        }

        return std::string(kWeekdayNames[LocalTime(shifted).tm_wday]) + " " + part;
    }

    // "today's", "tomorrow's", "Thursday's", "the Nov 5", or "the 11:00 AM"
    // when a heads-up runs out on the reset's own day.
    static std::string PaceResetName(const Pace& pace, long long now) {
        static const char* const kMonths[] = {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        const std::tm reset = LocalTime(pace.resetsAtUnixSeconds);
        char text[32]{};

        if (pace.state == Pace::State::HeadsUp &&
            LocalDay(PaceShift(pace.runsOutAtUnixSeconds, now)) == LocalDay(pace.resetsAtUnixSeconds)) {
            std::snprintf(text, sizeof(text), "the %d:%02d %s",
                reset.tm_hour % 12 == 0 ? 12 : reset.tm_hour % 12, reset.tm_min,
                reset.tm_hour < 12 ? "AM" : "PM");
            return text;
        }

        if (pace.resetsAtUnixSeconds - now >= 6 * 24 * 60 * 60) {
            std::snprintf(text, sizeof(text), "the %s %d", kMonths[reset.tm_mon], reset.tm_mday);
            return text;
        }

        const long long days = LocalDay(pace.resetsAtUnixSeconds) - LocalDay(now);

        return days == 0 ? "today's"
            : days == 1 ? "tomorrow's"
            : std::string(kWeekdayNames[reset.tm_wday]) + "'s";
    }

    Pace ComputePace(float usedPercent, long long resetAtUnixSeconds, long long nowUnixSeconds) {
        constexpr long long kWeekSeconds = 7 * 24 * 60 * 60;
        Pace pace;
        pace.percent = static_cast<int>(std::floor(std::max(0.0, static_cast<double>(usedPercent)) + 1e-9));
        pace.resetsAtUnixSeconds = resetAtUnixSeconds;

        const long long elapsed = nowUnixSeconds - (resetAtUnixSeconds - kWeekSeconds);

        if (pace.percent >= 100) {
            pace.state = Pace::State::LimitHit;
        }
        else if (pace.percent == 0) {
            pace.state = Pace::State::Zero;
        }
        else if (pace.percent < 10) {
            pace.state = Pace::State::Fresh;
        }
        else if (resetAtUnixSeconds <= 0 || elapsed < 12 * 60 * 60) {
            pace.state = Pace::State::Early;
        }
        else {
            const double runsOut = static_cast<double>(nowUnixSeconds) +
                (100.0 - pace.percent) * static_cast<double>(elapsed) / pace.percent;

            if (runsOut < static_cast<double>(resetAtUnixSeconds)) {
                pace.state = Pace::State::HeadsUp;
                pace.runsOutAtUnixSeconds = static_cast<long long>(runsOut);
            }
            else {
                pace.state = Pace::State::OnTrack;
            }
        }

        return pace;
    }

    // Plain ASCII apostrophes: the UI font may lack U+2019.
    std::string PaceText(const Pace& pace, long long nowUnixSeconds) {
        const std::string percent = std::to_string(pace.percent);
        const bool hasReset = pace.resetsAtUnixSeconds > 0;

        switch (pace.state) {
        case Pace::State::Zero:
            return "No usage yet. 100% of your weekly limit is available.";
        case Pace::State::Fresh:
            return "Fresh week. " + percent + "% of your weekly limit used.";
        case Pace::State::Early:
            return percent + "% of your weekly limit used.";
        case Pace::State::OnTrack:
            return hasReset
                ? "On track. You should reach " + PaceResetName(pace, nowUnixSeconds) + " reset with room to spare."
                : "On track. You have room to spare this week.";
        case Pace::State::HeadsUp: {
            const std::string runOut = "Heads up. At this pace you'll run out " +
                PaceRunOut(pace.runsOutAtUnixSeconds, nowUnixSeconds);
            return hasReset
                ? runOut + ", before " + PaceResetName(pace, nowUnixSeconds) + " reset."
                : runOut + ".";
        }
        case Pace::State::LimitHit:
            return hasReset
                ? "Limit hit. Your week resets " + Format::get_instance()->ResetDateTime(pace.resetsAtUnixSeconds) + "."
                : "Limit hit.";
        default:
            return {};
        }
    }

    Snapshot FetchSnapshot(AccountSource source) {
        Snapshot snapshot = FetchSnapshotForSource(source);
        const long long now = static_cast<long long>(std::time(nullptr));
        const std::string organizationId = snapshot.accountKey.rfind("desktop:", 0) == 0
            ? snapshot.accountKey.substr(8)
            : std::string();

        // Local files are best effort: one bad transcript or settings file
        // must never cost the network snapshot, or the other local readers.
        const auto bestEffort = [](const auto& read) {
            try {
                read();
            }
            catch (const std::exception&) {
            }
        };

        bestEffort([&] { AddClaudeTokenCap(snapshot); });
        bestEffort([&] { snapshot.autoResume = ReadAutoResume(organizationId, now); });
        bestEffort([&] { snapshot.ledger = ReadUsageLedger(now); });
        bestEffort([&] {
            LocalTelemetry local = ReadLatestClaudeLocalTelemetry();
            snapshot.context = std::move(local.context);
            snapshot.run = std::move(local.run);
        });
        return snapshot;
    }

}
