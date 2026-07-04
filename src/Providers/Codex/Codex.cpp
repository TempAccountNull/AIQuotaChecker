#include "Global.hpp"

#include "Codex.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Math.hpp"
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
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Codex {


    static std::filesystem::path AuthPath() {
        return Network::get_instance()->UserProfilePath() / ".codex" / "auth.json";
    }
    static json ReadAuthJson() {
        return JsonUtils::get_instance()->ParseRequired(Network::get_instance()->ReadRequiredTextFile(AuthPath()));
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

    static std::string StripLeadingZero(std::string text) {
        if (!text.empty() && text[0] == '0') {
            text.erase(text.begin());
        }

        return text;
    }

    static std::string FormatResetClock(std::chrono::system_clock::time_point tp) {
        if (tp.time_since_epoch().count() == 0) {
            return "";
        }

        std::time_t t = std::chrono::system_clock::to_time_t(tp);

        std::tm localTime{};
        localtime_s(&localTime, &t);

        std::ostringstream out;
        out << std::put_time(&localTime, "%I:%M %p");

        return StripLeadingZero(out.str());
    }

    static std::string FormatResetMonthDay(std::chrono::system_clock::time_point tp) {
        if (tp.time_since_epoch().count() == 0) {
            return "";
        }

        std::time_t t = std::chrono::system_clock::to_time_t(tp);

        std::tm localTime{};
        localtime_s(&localTime, &t);

        std::ostringstream out;
        out << std::put_time(&localTime, "%b %d");

        std::string text = out.str();

        size_t zeroPos = text.find(" 0");

        if (zeroPos != std::string::npos) {
            text.erase(zeroPos + 1, 1);
        }

        return text;
    }

    static std::string FormatDurationUntil(std::chrono::system_clock::time_point tp) {
        using namespace std::chrono;

        if (tp.time_since_epoch().count() == 0) {
            return "";
        }

        auto now = system_clock::now();
        auto totalMinutes = duration_cast<minutes>(tp - now).count();

        if (totalMinutes <= 0) {
            return "now";
        }

        long long days = totalMinutes / (60 * 24);
        totalMinutes %= 60 * 24;

        long long hours = totalMinutes / 60;
        long long minutes = totalMinutes % 60;

        std::ostringstream out;

        if (days > 0) {
            out << days << "d";

            if (hours > 0) {
                out << " " << hours << "h";
            }
        }
        else if (hours > 0) {
            out << hours << "h";

            if (minutes > 0) {
                out << " " << minutes << "m";
            }
        }
        else {
            out << minutes << "m";
        }

        return out.str();
    }

    static std::string FormatCodexResetText(
        const std::string&,
        std::chrono::system_clock::time_point tp
    ) {
        std::string duration = FormatDurationUntil(tp);

        if (duration.empty()) {
            return "Resets unknown";
        }

        return "Resets in " + duration;
    }

    static std::string FormatPercent(float value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << value << "%";
        return out.str();
    }

    static std::string FormatCurrency(double value) {
        std::ostringstream out;
        out << "$" << std::fixed << std::setprecision(2) << value;
        return out.str();
    }

    static double ReadCreditsRemaining(const json& body) {
        if (body.is_object() && body.contains("credits") && body.at("credits").is_object()) {
            const json& credits = body.at("credits");

            if (credits.contains("balance")) {
                const json& balance = credits.at("balance");

                if (balance.is_number()) {
                    return balance.get<double>();
                }

                if (balance.is_string()) {
                    try {
                        return std::stod(balance.get<std::string>());
                    }
                    catch (...) {
                        return 0.0;
                    }
                }
            }

            if (credits.contains("has_credits") && credits.at("has_credits").is_boolean() && !credits.at("has_credits").get<bool>()) {
                return 0.0;
            }
        }

        return 0.0;
    }

    static void ParseCodexExtraUsage(const json& usageJson, Snapshot& snapshot) {
        double remaining = std::max(0.0, ReadCreditsRemaining(usageJson));
        int credits = std::max(0, static_cast<int>(std::floor(remaining)));
        double dollars = static_cast<double>(credits) * 0.04;

        snapshot.extraUsage.valid = true;
        snapshot.extraUsage.spentText = FormatCurrency(dollars);
        snapshot.extraUsage.balanceText = std::to_string(credits) + " credits";
        snapshot.extraUsage.usedPercent = 0.0f;
    }

    static double ParseUsedPercentField(const json& object) {
        if (!object.is_object()) {
            return 0.0;
        }

        if (!object.contains("used_percent")) {
            return 0.0;
        }

        return Math::get_instance()->ClampPercentDouble(JsonUtils::get_instance()->Number(object, "used_percent", 0.0));
    }

    static std::chrono::system_clock::time_point ParseResetAtField(const json& object) {
        if (!object.is_object()) {
            return {};
        }

        if (!object.contains("reset_at")) {
            return {};
        }

        return ParseTimeFlexible(object.at("reset_at"));
    }

    static UsageBar ParseCodexWindow(const json& window, const std::string& label) {
        UsageBar bar;
        bar.label = label;
        bar.usedPercent = static_cast<float>(ParseUsedPercentField(window));

        std::chrono::system_clock::time_point resetAt = ParseResetAtField(window);
        bar.resetAt = resetAt;

        if (resetAt.time_since_epoch().count() != 0) {
            bar.resetAtUnixSeconds = static_cast<long long>(std::chrono::system_clock::to_time_t(resetAt));
        }

        std::string right;

        if (resetAt.time_since_epoch().count() != 0) {
            right = FormatCodexResetText(label, resetAt) + "  ";
        }

        right += FormatPercent(bar.usedPercent);

        bar.rightText = right;
        return bar;
    }

    static const json* GetMainRateLimitObject(const json& usageJson) {
        if (!usageJson.is_object()) {
            return nullptr;
        }

        if (!usageJson.contains("rate_limit")) {
            return nullptr;
        }

        const json& rateLimit = usageJson.at("rate_limit");

        if (!rateLimit.is_object()) {
            return nullptr;
        }

        return &rateLimit;
    }

    static const json* GetWindowObject(const json& rateLimit, const char* key) {
        if (!rateLimit.is_object()) {
            return nullptr;
        }

        if (!rateLimit.contains(key)) {
            return nullptr;
        }

        const json& window = rateLimit.at(key);

        if (!window.is_object()) {
            return nullptr;
        }

        return &window;
    }

    static void AddUnavailableWindow(
        Snapshot& snapshot,
        const std::string& label,
        const std::string& reason
    ) {
        snapshot.bars.push_back({
            label,
            reason,
            0.0f
            });
    }

    static int ReadIntField(const json& object, const char* key, int fallback = -1) {
        if (!object.is_object() || !object.contains(key)) {
            return fallback;
        }

        const json& value = object.at(key);

        if (value.is_number_integer()) {
            return value.get<int>();
        }

        if (value.is_number()) {
            return static_cast<int>(value.get<double>());
        }

        if (value.is_string()) {
            try {
                return std::stoi(value.get<std::string>());
            }
            catch (...) {
                return fallback;
            }
        }

        return fallback;
    }

    static int ParseResetCreditsAvailableCount(const json& usageJson) {
        if (!usageJson.is_object()) {
            return -1;
        }

        if (usageJson.contains("rate_limit_reset_credits") && usageJson.at("rate_limit_reset_credits").is_object()) {
            int count = ReadIntField(usageJson.at("rate_limit_reset_credits"), "available_count", -1);

            if (count >= 0) {
                return count;
            }
        }

        if (usageJson.contains("rate_limit") && usageJson.at("rate_limit").is_object()) {
            const json& rateLimit = usageJson.at("rate_limit");

            if (rateLimit.contains("rate_limit_reset_credits") && rateLimit.at("rate_limit_reset_credits").is_object()) {
                int count = ReadIntField(rateLimit.at("rate_limit_reset_credits"), "available_count", -1);

                if (count >= 0) {
                    return count;
                }
            }
        }

        return -1;
    }

    static ResetCredit ParseResetCreditLedgerEntry(const json& credit) {
        ResetCredit reset;

        if (!credit.is_object()) {
            return reset;
        }

        reset.title = credit.value("title", "");
        reset.status = credit.value("status", "");

        if (credit.contains("granted_at")) {
            reset.grantedAt = ParseTimeFlexible(credit.at("granted_at"));
        }

        if (credit.contains("expires_at")) {
            reset.expiresAt = ParseTimeFlexible(credit.at("expires_at"));
        }

        return reset;
    }

    static void ParseResetCredits(const json& resetJson, Snapshot& snapshot) {
        snapshot.resetCredits.clear();
        snapshot.resetCreditLedger.clear();

        if (!resetJson.contains("credits") || !resetJson.at("credits").is_array()) {
            return;
        }

        for (const json& credit : resetJson.at("credits")) {
            ResetCredit reset = ParseResetCreditLedgerEntry(credit);

            if (reset.expiresAt.time_since_epoch().count() == 0) {
                continue;
            }

            snapshot.resetCreditLedger.push_back(reset);

            if (reset.status == "available") {
                snapshot.resetCredits.push_back(reset);
            }
        }

        auto byExpiry = [](const ResetCredit& a, const ResetCredit& b) {
            return a.expiresAt < b.expiresAt;
        };

        std::sort(snapshot.resetCredits.begin(), snapshot.resetCredits.end(), byExpiry);
        std::sort(snapshot.resetCreditLedger.begin(), snapshot.resetCreditLedger.end(), byExpiry);
    }

    static std::string TitleCasePlanToken(std::string value) {
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

    static std::string ExtractStringRecursive(const json& value, const char* key) {
        if (value.is_object()) {
            if (value.contains(key) && value.at(key).is_string()) {
                return value.at(key).get<std::string>();
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                std::string found = ExtractStringRecursive(it.value(), key);

                if (!found.empty()) {
                    return found;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                std::string found = ExtractStringRecursive(item, key);

                if (!found.empty()) {
                    return found;
                }
            }
        }

        return "";
    }

    static std::string FormatCodexPlan(const json& usageJson) {
        std::string raw = ExtractStringRecursive(usageJson, "plan_type");

        if (raw.empty()) {
            raw = ExtractStringRecursive(usageJson, "plan");
        }

        if (raw.empty()) {
            raw = ExtractStringRecursive(usageJson, "account_plan");
        }

        if (raw.empty()) {
            return "Codex";
        }

        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower == "prolite") {
            return "Codex Pro 5x";
        }

        if (lower == "pro") {
            return "Codex Pro 20x";
        }

        if (lower == "plus") {
            return "Codex Plus";
        }

        if (lower == "team") {
            return "Codex Team";
        }

        if (lower == "enterprise") {
            return "Codex Enterprise";
        }

        return "Codex " + TitleCasePlanToken(raw);
    }

    static Snapshot ParseUsageAndResets(const json& usageJson, const json& resetJson) {
        Snapshot snapshot;
        snapshot.plan = FormatCodexPlan(usageJson);
        snapshot.statusText = "Plan: " + snapshot.plan;
        snapshot.lastUpdated = "just now";
        snapshot.bars.clear();
        snapshot.resetCreditsAvailableCount = ParseResetCreditsAvailableCount(usageJson);
        ParseCodexExtraUsage(usageJson, snapshot);

        const json* rateLimit = GetMainRateLimitObject(usageJson);

        if (!rateLimit) {
            AddUnavailableWindow(snapshot, "Session", "No rate_limit object");
            AddUnavailableWindow(snapshot, "Weekly", "No rate_limit object");
            ParseResetCredits(resetJson, snapshot);
            return snapshot;
        }

        const json* primary = GetWindowObject(*rateLimit, "primary_window");
        const json* secondary = GetWindowObject(*rateLimit, "secondary_window");

        if (primary) {
#ifdef _DEBUG
            OutputDebugStringA(
                (
                    std::string("Codex primary_window raw: ") +
                    primary->dump(2) +
                    "\n"
                    ).c_str()
            );
#endif

            snapshot.bars.push_back(
                ParseCodexWindow(*primary, "Session")
            );
        }
        else {
            AddUnavailableWindow(snapshot, "Session", "No primary_window");
        }

        if (secondary) {
#ifdef _DEBUG
            OutputDebugStringA(
                (
                    std::string("Codex secondary_window raw: ") +
                    secondary->dump(2) +
                    "\n"
                    ).c_str()
            );
#endif

            snapshot.bars.push_back(
                ParseCodexWindow(*secondary, "Weekly")
            );
        }
        else {
            AddUnavailableWindow(snapshot, "Weekly", "No secondary_window");
        }

        ParseResetCredits(resetJson, snapshot);
        return snapshot;
    }

    Snapshot FetchSnapshot() {
        json auth = ReadAuthJson();

        if (!auth.contains("tokens") || !auth.at("tokens").is_object()) {
            throw std::runtime_error("Codex auth.json does not contain tokens");
        }

        const json& tokens = auth.at("tokens");

        std::string accessToken = tokens.value("access_token", "");
        std::string accountId = tokens.value("account_id", "");

        if (accessToken.empty()) {
            throw std::runtime_error("Codex access_token missing");
        }

        if (accountId.empty()) {
            throw std::runtime_error("Codex account_id missing");
        }

        std::wstring headers =
            std::wstring(L"Authorization: Bearer ") +
            Network::get_instance()->Utf8ToWide(Network::get_instance()->StripHeaderValue(accessToken)) +
            L"\r\n"
            L"ChatGPT-Account-ID: " +
            Network::get_instance()->Utf8ToWide(Network::get_instance()->StripHeaderValue(accountId)) +
            L"\r\n"
            L"Accept: application/json\r\n"
            L"Origin: https://chatgpt.com\r\n"
            L"Referer: https://chatgpt.com/codex\r\n"
            L"originator: Codex Desktop\r\n";

        Network::HttpResponse usage = Network::get_instance()->RequestUrl(
            "https://chatgpt.com/backend-api/wham/usage",
            "GET",
            headers
        );

        if (usage.statusCode < 200 || usage.statusCode >= 300) {
            throw std::runtime_error("Codex usage failed: HTTP " + std::to_string(usage.statusCode));
        }

        Network::HttpResponse resets = Network::get_instance()->RequestUrl(
            "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
            "GET",
            headers
        );

        if (resets.statusCode < 200 || resets.statusCode >= 300) {
            throw std::runtime_error("Codex resets failed: HTTP " + std::to_string(resets.statusCode));
        }

        return ParseUsageAndResets(
            JsonUtils::get_instance()->ParseRequired(usage.body),
            JsonUtils::get_instance()->ParseRequired(resets.body)
        );
    }

}
