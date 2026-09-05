#include "Global.hpp"

#include "Codex.hpp"
#include "CodexAppServer.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Math.hpp"
#include "Format.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <utility>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Codex {


    // Explicit user/effective override returned by Codex app-server config/read.
    // A zero value is normal: Codex usually derives the compaction edge from
    // the active model catalog instead of materializing it in config/read.
    static std::atomic<long long> g_modelAutoCompactTokenLimit{ 0 };
    // Effective context window returned by Codex app-server config/read. This
    // is a model-agnostic fallback when a freshly created local thread has not
    // emitted its first token_count model_context_window yet.
    static std::atomic<long long> g_modelContextWindow{ 0 };
    static std::mutex g_configModelMutex;
    static std::string g_configModel;
    // 0 = total (Codex default), 1 = body_after_prefix. The latter needs a
    // different numerator than total active-context tokens, so AQC only shows
    // the countdown when it has trustworthy accounting for the selected scope.
    static std::atomic<int> g_modelAutoCompactTokenLimitScope{ 0 };

    static std::filesystem::path CodexHomePath() {
        std::string configuredHome = Network::get_instance()->GetEnvText("CODEX_HOME");

        if (!configuredHome.empty()) {
            return std::filesystem::path(configuredHome);
        }

        return Network::get_instance()->UserProfilePath() / ".codex";
    }

    static std::filesystem::path DefaultAuthPath() {
        return CodexHomePath() / "auth.json";
    }

    static std::filesystem::path ExpandCustomAuthPath(const std::string& configuredPath) {
        if (configuredPath.empty()) {
            return {};
        }

        std::wstring input = Network::get_instance()->Utf8ToWide(configuredPath);

        if (input.empty()) {
            throw std::runtime_error("Custom Codex auth.json path is not valid UTF-8");
        }

        DWORD required = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);

        if (required == 0) {
            throw std::runtime_error("Could not expand the custom Codex auth.json path");
        }

        std::wstring expanded(static_cast<size_t>(required), L'\0');
        DWORD written = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), required);

        if (written == 0 || written > required) {
            throw std::runtime_error("Could not expand the custom Codex auth.json path");
        }

        if (!expanded.empty() && expanded.back() == L'\0') {
            expanded.pop_back();
        }

        std::filesystem::path path(expanded);

        if (path.is_relative()) {
            path = std::filesystem::absolute(path);
        }

        return path.lexically_normal();
    }

    static std::string TrimAscii(std::string value) {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };

        while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }

        while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }

        return value;
    }

    static std::string ConfiguredCredentialStore() {
        std::string text = Network::get_instance()->ReadTextFile(CodexHomePath() / "config.toml");
        std::istringstream lines(text);
        std::string line;

        while (std::getline(lines, line)) {
            size_t comment = line.find('#');

            if (comment != std::string::npos) {
                line.resize(comment);
            }

            size_t equals = line.find('=');

            if (equals == std::string::npos) {
                continue;
            }

            std::string key = TrimAscii(line.substr(0, equals));

            if (key != "cli_auth_credentials_store") {
                continue;
            }

            std::string value = TrimAscii(line.substr(equals + 1));

            if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }

            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        return {};
    }

    static json ReadAuthJson(const std::filesystem::path& path, bool enforceCredentialStoreSafety) {
        if (enforceCredentialStoreSafety) {
            std::string store = ConfiguredCredentialStore();

            if (store == "keyring" || store == "auto") {
                throw std::runtime_error(
                    "Codex credentials are configured for the OS credential store. Auto mode will not read a possibly stale auth.json. "
                    "Select Active Codex account only, or explicitly select Codex auth.json only if you intentionally want that file."
                );
            }

            if (store == "ephemeral") {
                throw std::runtime_error(
                    "Codex credentials are configured as ephemeral and cannot be read by another process. "
                    "Select Active Codex account only."
                );
            }
        }

        if (path.empty()) {
            throw std::runtime_error("Codex auth.json path is empty");
        }

        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("Codex auth.json not found at " + path.string());
        }

        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Codex auth.json path is not a file: " + path.string());
        }

        return JsonUtils::get_instance()->ParseRequired(
            Network::get_instance()->ReadRequiredTextFile(path)
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

    static const json* FindObjectField(
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

    static std::optional<double> ReadNumberFlexible(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        if (!object.is_object()) {
            return std::nullopt;
        }

        for (const char* key : keys) {
            if (!object.contains(key) || object.at(key).is_null()) {
                continue;
            }

            const json& value = object.at(key);

            if (value.is_number()) {
                return value.get<double>();
            }

            if (value.is_string()) {
                try {
                    return std::stod(value.get<std::string>());
                }
                catch (...) {
                    continue;
                }
            }
        }

        return std::nullopt;
    }

    static std::optional<double> ReadNumberFlexible(const json& object, const char* key) {
        return ReadNumberFlexible(object, { key });
    }

    static std::optional<bool> ReadBoolFlexible(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        if (!object.is_object()) {
            return std::nullopt;
        }

        for (const char* key : keys) {
            if (!object.contains(key) || object.at(key).is_null()) {
                continue;
            }

            const json& value = object.at(key);

            if (value.is_boolean()) {
                return value.get<bool>();
            }

            if (value.is_number()) {
                return value.get<double>() != 0.0;
            }

            if (value.is_string()) {
                std::string text = value.get<std::string>();
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });

                if (text == "true" || text == "1" || text == "yes") return true;
                if (text == "false" || text == "0" || text == "no") return false;
            }
        }

        return std::nullopt;
    }

    static std::string ReadStringFlexible(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        if (!object.is_object()) {
            return {};
        }

        for (const char* key : keys) {
            if (!object.contains(key) || object.at(key).is_null()) {
                continue;
            }

            const json& value = object.at(key);

            if (value.is_string()) {
                return value.get<std::string>();
            }
        }

        return {};
    }

    static const json* FindCreditsObject(const json& body) {
        if (!body.is_object()) {
            return nullptr;
        }

        if (const json* credits = FindObjectField(body, { "credits" })) {
            return credits;
        }

        if (body.contains("balance") || body.contains("hasCredits") ||
            body.contains("has_credits") || body.contains("unlimited")) {
            return &body;
        }

        return nullptr;
    }

    static std::string FormatCreditAmount(double value) {
        std::ostringstream out;
        const double rounded = std::round(value);

        if (std::abs(value - rounded) < 0.000001) {
            out << static_cast<long long>(rounded);
        }
        else {
            out << std::fixed << std::setprecision(2) << value;
            std::string text = out.str();

            while (!text.empty() && text.back() == '0') text.pop_back();
            if (!text.empty() && text.back() == '.') text.pop_back();
            return text;
        }

        return out.str();
    }

    static std::string EncodeUrlPathSegment(const std::string& value) {
        std::ostringstream out;
        out << std::uppercase << std::hex;

        for (unsigned char c : value) {
            if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
                out << static_cast<char>(c);
            }
            else {
                out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
            }
        }

        return out.str();
    }

    static void ParseCodexCreditBalance(const json& body, Snapshot& snapshot) {
        const json* credits = FindCreditsObject(body);

        if (!credits) {
            return;
        }

        const std::optional<double> balance = ReadNumberFlexible(*credits, { "balance" });
        const std::optional<bool> hasCredits = ReadBoolFlexible(*credits, { "hasCredits", "has_credits" });
        const std::optional<bool> unlimited = ReadBoolFlexible(*credits, { "unlimited" });

        if (!balance && !hasCredits && !unlimited) {
            return;
        }

        CreditBalance parsed;
        parsed.valid = true;
        parsed.hasCredits = hasCredits.value_or(balance.value_or(0.0) > 0.0);
        parsed.unlimited = unlimited.value_or(false);

        if (parsed.unlimited) {
            parsed.balanceText = "Unlimited";
        }
        else if (balance) {
            parsed.balanceText = FormatCreditAmount(std::max(0.0, *balance)) + " credits";
        }
        else if (!parsed.hasCredits) {
            parsed.balanceText = "No credits";
        }
        else {
            parsed.balanceText = "Credits available";
        }

        snapshot.creditBalance = std::move(parsed);
    }

    static std::string HumanizeIdentifier(std::string text);

    static void AppendUsageNotice(Snapshot& snapshot, const std::string& notice) {
        if (notice.empty()) {
            return;
        }

        if (snapshot.usageNotice.empty()) {
            snapshot.usageNotice = notice;
            return;
        }

        if (snapshot.usageNotice.find(notice) == std::string::npos) {
            snapshot.usageNotice += " · " + notice;
        }
    }

    static std::string ReadStatusIdentifier(
        const json& object,
        std::initializer_list<const char*> keys
    ) {
        if (!object.is_object()) {
            return {};
        }

        for (const char* key : keys) {
            if (!object.contains(key) || object.at(key).is_null()) {
                continue;
            }

            const json& value = object.at(key);

            if (value.is_string()) {
                return value.get<std::string>();
            }

            if (value.is_object()) {
                std::string type = ReadStringFlexible(value, { "type", "kind", "reason" });

                if (!type.empty()) {
                    return type;
                }
            }
        }

        return {};
    }

    static const json* FindSpendControlObject(const json& body) {
        return FindObjectField(body, { "spendControl", "spend_control" });
    }

    static void ParseCodexLimitStatus(const json& body, Snapshot& snapshot) {
        if (!body.is_object()) {
            return;
        }

        const std::string reachedType = ReadStatusIdentifier(
            body,
            { "rateLimitReachedType", "rate_limit_reached_type" }
        );

        if (!reachedType.empty()) {
            std::string lower = reachedType;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (lower != "none" && lower != "null" && lower != "rate_limit_reached") {
                AppendUsageNotice(snapshot, "Usage exhausted: " + HumanizeIdentifier(reachedType));
            }
            else if (lower == "rate_limit_reached") {
                AppendUsageNotice(snapshot, "Usage exhausted");
            }
        }

        const std::optional<bool> allowed = ReadBoolFlexible(body, { "allowed" });
        const bool explicitRateLimitReached =
            ReadBoolFlexible(body, { "limitReached", "limit_reached" }).value_or(false) ||
            (allowed.has_value() && !*allowed);

        if (explicitRateLimitReached && reachedType.empty()) {
            AppendUsageNotice(snapshot, "Usage exhausted");
        }

        bool spendReached = ReadBoolFlexible(
            body,
            { "spendControlReached", "spend_control_reached" }
        ).value_or(false);

        if (const json* spendControl = FindSpendControlObject(body)) {
            spendReached = spendReached || ReadBoolFlexible(
                *spendControl,
                { "reached", "spendControlReached", "spend_control_reached" }
            ).value_or(false);
        }

        if (spendReached) {
            AppendUsageNotice(snapshot, "Monthly usage limit reached");
        }
    }

    static void ParseCodexIndividualLimit(const json& body, Snapshot& snapshot) {
        const json* spendControl = FindSpendControlObject(body);
        const json* limit = FindObjectField(body, { "individualLimit", "individual_limit" });

        if (!limit && spendControl) {
            limit = FindObjectField(*spendControl, { "individualLimit", "individual_limit" });
        }

        bool reached = ReadBoolFlexible(
            body,
            { "spendControlReached", "spend_control_reached" }
        ).value_or(false);

        if (spendControl) {
            reached = reached || ReadBoolFlexible(
                *spendControl,
                { "reached", "spendControlReached", "spend_control_reached" }
            ).value_or(false);
        }

        if (limit) {
            reached = reached || ReadBoolFlexible(
                *limit,
                { "spendControlReached", "spend_control_reached", "reached" }
            ).value_or(false);
        }

        if (!limit && !reached) {
            return;
        }

        const std::optional<double> maximum = limit
            ? ReadNumberFlexible(*limit, { "limit" })
            : std::nullopt;
        const std::optional<double> used = limit
            ? ReadNumberFlexible(*limit, { "used" })
            : std::nullopt;
        const std::optional<double> remainingPercent = limit
            ? ReadNumberFlexible(*limit, { "remainingPercent", "remaining_percent" })
            : std::nullopt;
        const std::optional<double> explicitUsedPercent = limit
            ? ReadNumberFlexible(*limit, { "usedPercent", "used_percent" })
            : std::nullopt;

        if (!maximum && !used && !remainingPercent && !explicitUsedPercent && !reached) {
            return;
        }

        ExtraUsage parsed;
        parsed.valid = true;

        if (explicitUsedPercent) {
            parsed.usedPercent = static_cast<float>(
                Math::get_instance()->ClampPercentDouble(*explicitUsedPercent)
            );
            parsed.hasUsedPercent = true;
        }
        else if (remainingPercent) {
            parsed.usedPercent = static_cast<float>(
                Math::get_instance()->ClampPercentDouble(100.0 - *remainingPercent)
            );
            parsed.hasUsedPercent = true;
        }
        else if (maximum && *maximum > 0.0 && used) {
            parsed.usedPercent = static_cast<float>(
                Math::get_instance()->ClampPercentDouble((*used / *maximum) * 100.0)
            );
            parsed.hasUsedPercent = true;
        }
        else if (reached) {
            parsed.usedPercent = 100.0f;
            parsed.hasUsedPercent = true;
        }

        if (used && maximum) {
            parsed.spentText =
                FormatCreditAmount(std::max(0.0, *used)) +
                " of " +
                FormatCreditAmount(std::max(0.0, *maximum)) +
                " credits used";
        }
        else if (used) {
            parsed.spentText = FormatCreditAmount(std::max(0.0, *used)) + " credits used";
        }
        else if (reached) {
            parsed.spentText = "Monthly usage limit reached";
        }

        if (maximum) {
            parsed.limitText = FormatCreditAmount(std::max(0.0, *maximum)) + " credit limit";
        }

        if (maximum && used) {
            parsed.remainingText = FormatCreditAmount(std::max(0.0, *maximum - *used)) + " credits remaining";
        }
        else if (remainingPercent) {
            parsed.remainingText = FormatPercent(
                static_cast<float>(Math::get_instance()->ClampPercentDouble(*remainingPercent))
            ) + " remaining";
        }
        else if (reached) {
            parsed.remainingText = "Limit reached";
        }

        if (limit) {
            for (const char* key : { "resetsAt", "resets_at", "resetAt", "reset_at" }) {
                if (limit->contains(key) && !limit->at(key).is_null()) {
                    const auto resetAt = ParseTimeFlexible(limit->at(key));

                    if (resetAt.time_since_epoch().count() != 0) {
                        parsed.resetAtUnixSeconds = static_cast<long long>(
                            std::chrono::system_clock::to_time_t(resetAt)
                        );
                        parsed.resetText = FormatCodexResetText(parsed.label, resetAt);
                    }
                    break;
                }
            }
        }

        snapshot.extraUsage = std::move(parsed);
    }

    static std::chrono::system_clock::time_point ParseResetAtField(const json& object) {
        if (!object.is_object()) {
            return {};
        }

        for (const char* key : { "reset_at", "resetAt", "resets_at", "resetsAt" }) {
            if (object.contains(key) && !object.at(key).is_null()) {
                return ParseTimeFlexible(object.at(key));
            }
        }

        return {};
    }

    static void ParseCodexMonthlyUsage(const json& body, Snapshot& snapshot) {
        if (!body.is_object() || snapshot.extraUsage.valid) {
            return;
        }

        const json* monthlyUsage = FindObjectField(
            body,
            { "monthlyUsage", "monthly_usage" }
        );
        const json& source = monthlyUsage ? *monthlyUsage : body;
        const json* effectiveLimit = FindObjectField(
            source,
            { "effectiveMonthlyLimit", "effective_monthly_limit" }
        );

        if (!effectiveLimit) {
            return;
        }

        const std::optional<double> maximum = ReadNumberFlexible(
            *effectiveLimit,
            { "limit" }
        );
        const std::optional<double> used = ReadNumberFlexible(
            source,
            { "currentMonthUsage", "current_month_usage" }
        );

        // Codex's usage settings require both values. A missing limit or used
        // amount means the monthly row is unavailable, not zero usage.
        if (!maximum || !used || *maximum < 0.0) {
            return;
        }

        const double safeMaximum = std::max(0.0, *maximum);
        const double safeUsed = std::max(0.0, *used);

        ExtraUsage parsed;
        parsed.valid = true;
        parsed.hasUsedPercent = true;
        parsed.usedPercent = static_cast<float>(
            safeMaximum == 0.0
                ? 100.0
                : Math::get_instance()->ClampPercentDouble((safeUsed / safeMaximum) * 100.0)
        );
        parsed.spentText =
            FormatCreditAmount(safeUsed) +
            " of " +
            FormatCreditAmount(safeMaximum) +
            " credits used";
        parsed.limitText = FormatCreditAmount(safeMaximum) + " credit limit";
        parsed.remainingText =
            FormatCreditAmount(std::max(0.0, safeMaximum - safeUsed)) +
            " credits remaining";

        std::chrono::system_clock::time_point resetAt = ParseResetAtField(source);

        if (resetAt.time_since_epoch().count() == 0) {
            resetAt = ParseResetAtField(*effectiveLimit);
        }

        if (resetAt.time_since_epoch().count() != 0) {
            parsed.resetAtUnixSeconds = static_cast<long long>(
                std::chrono::system_clock::to_time_t(resetAt)
            );
            parsed.resetText = FormatCodexResetText(parsed.label, resetAt);
        }

        snapshot.extraUsage = std::move(parsed);
    }

    static UsageBar ParseCodexWindow(
        const json& window,
        const std::string& label,
        const std::string& sublabel = {},
        bool quotaNotificationEligible = true
    ) {
        UsageBar bar;
        bar.label = label;
        bar.sublabel = sublabel;
        bar.quotaNotificationEligible = quotaNotificationEligible;
        bar.blocksProvider = quotaNotificationEligible;

        if (!window.is_object()) {
            return bar;
        }

        const std::optional<double> used = ReadNumberFlexible(
            window,
            { "used_percent", "usedPercent" }
        );

        // A reset timestamp without utilization is not enough to draw a quota
        // bar. Missing utilization must remain hidden, not become 0% used.
        if (!used) {
            return bar;
        }

        bar.valid = true;
        bar.usedPercent = static_cast<float>(Math::get_instance()->ClampPercentDouble(*used));

        const std::chrono::system_clock::time_point resetAt = ParseResetAtField(window);
        bar.resetAt = resetAt;

        if (resetAt.time_since_epoch().count() != 0) {
            bar.resetAtUnixSeconds = static_cast<long long>(std::chrono::system_clock::to_time_t(resetAt));
            bar.rightText = FormatCodexResetText(label, resetAt) + "  ";
        }

        bar.rightText += FormatPercent(bar.usedPercent);
        return bar;
    }

    static void AddUsageBarIfUnique(Snapshot& snapshot, UsageBar bar) {
        if (!bar.valid) {
            return;
        }

        std::string label = bar.label;
        std::string sublabel = bar.sublabel;

        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::transform(sublabel.begin(), sublabel.end(), sublabel.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        for (const UsageBar& existing : snapshot.bars) {
            std::string existingLabel = existing.label;
            std::string existingSublabel = existing.sublabel;

            std::transform(existingLabel.begin(), existingLabel.end(), existingLabel.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            std::transform(existingSublabel.begin(), existingSublabel.end(), existingSublabel.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (existingLabel == label &&
                existingSublabel == sublabel &&
                existing.resetAtUnixSeconds == bar.resetAtUnixSeconds) {
                return;
            }
        }

        snapshot.bars.push_back(std::move(bar));
    }

    static const json* GetMainRateLimitObject(const json& usageJson) {
        return FindObjectField(usageJson, { "rate_limit", "rateLimit" });
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

    static double ReadWindowDurationMinutes(const json& window) {
        const std::optional<double> minutes = ReadNumberFlexible(
            window,
            { "windowDurationMins", "window_duration_mins" }
        );

        if (minutes && *minutes > 0.0) {
            return *minutes;
        }

        const std::optional<double> seconds = ReadNumberFlexible(
            window,
            { "limitWindowSeconds", "limit_window_seconds" }
        );

        if (seconds && *seconds > 0.0) {
            return *seconds / 60.0;
        }

        return 0.0;
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

        auto readCount = [](const json& object) -> int {
            const std::optional<double> value = ReadNumberFlexible(
                object,
                { "available_count", "availableCount" }
            );

            if (!value || *value < 0.0) {
                return -1;
            }

            return static_cast<int>(*value);
        };

        const int directCount = readCount(usageJson);

        if (directCount >= 0) {
            return directCount;
        }

        for (const char* key : { "rate_limit_reset_credits", "rateLimitResetCredits" }) {
            if (usageJson.contains(key) && usageJson.at(key).is_object()) {
                const int count = readCount(usageJson.at(key));

                if (count >= 0) {
                    return count;
                }
            }
        }

        if (const json* rateLimit = GetMainRateLimitObject(usageJson)) {
            for (const char* key : { "rate_limit_reset_credits", "rateLimitResetCredits" }) {
                if (rateLimit->contains(key) && rateLimit->at(key).is_object()) {
                    const int count = readCount(rateLimit->at(key));

                    if (count >= 0) {
                        return count;
                    }
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

        reset.title = ReadStringFlexible(credit, { "title", "name" });
        reset.status = ReadStringFlexible(credit, { "status" });

        for (const char* key : { "granted_at", "grantedAt" }) {
            if (credit.contains(key) && !credit.at(key).is_null()) {
                reset.grantedAt = ParseTimeFlexible(credit.at(key));
                break;
            }
        }

        for (const char* key : { "expires_at", "expiresAt" }) {
            if (credit.contains(key) && !credit.at(key).is_null()) {
                reset.expiresAt = ParseTimeFlexible(credit.at(key));
                break;
            }
        }

        return reset;
    }

    static void ParseResetCredits(const json& resetJson, Snapshot& snapshot) {
        snapshot.resetCredits.clear();
        snapshot.resetCreditLedger.clear();

        const int endpointCount = ParseResetCreditsAvailableCount(resetJson);

        if (endpointCount >= 0) {
            snapshot.resetCreditsAvailableCount = endpointCount;
        }

        const json* body = &resetJson;

        for (const char* key : { "rate_limit_reset_credits", "rateLimitResetCredits" }) {
            if (resetJson.is_object() && resetJson.contains(key) && resetJson.at(key).is_object()) {
                body = &resetJson.at(key);
                break;
            }
        }

        if (!body->is_object() || !body->contains("credits") || !body->at("credits").is_array()) {
            return;
        }

        for (const json& credit : body->at("credits")) {
            ResetCredit reset = ParseResetCreditLedgerEntry(credit);

            // Preserve every supplied ledger item. Expiry can legitimately be
            // missing while the title/status and available count still exist.
            snapshot.resetCreditLedger.push_back(reset);

            std::string status = reset.status;
            std::transform(status.begin(), status.end(), status.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (status == "available") {
                snapshot.resetCredits.push_back(reset);
            }
        }

        auto byExpiry = [](const ResetCredit& a, const ResetCredit& b) {
            const bool aKnown = a.expiresAt.time_since_epoch().count() != 0;
            const bool bKnown = b.expiresAt.time_since_epoch().count() != 0;

            if (aKnown != bKnown) {
                return aKnown;
            }

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

    static std::string FormatCodexPlanToken(const std::string& raw) {
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

    static std::string FormatCodexPlan(const json& usageJson) {
        std::string raw = ExtractStringRecursive(usageJson, "plan_type");

        if (raw.empty()) {
            raw = ExtractStringRecursive(usageJson, "planType");
        }

        if (raw.empty()) {
            raw = ExtractStringRecursive(usageJson, "plan");
        }

        if (raw.empty()) {
            raw = ExtractStringRecursive(usageJson, "account_plan");
        }

        if (raw.empty()) {
            raw = ExtractStringRecursive(usageJson, "accountPlan");
        }

        return FormatCodexPlanToken(raw);
    }

    static std::string ExtractAppServerPlanType(
        const json& accountResult,
        const json& rateLimit
    ) {
        if (accountResult.is_object() &&
            accountResult.contains("account") &&
            accountResult.at("account").is_object()) {
            const std::string accountPlan = ReadStringFlexible(
                accountResult.at("account"),
                { "planType", "plan_type", "plan" }
            );

            if (!accountPlan.empty()) {
                return accountPlan;
            }
        }

        return ReadStringFlexible(rateLimit, { "planType", "plan_type", "plan" });
    }

    static std::string HumanizeIdentifier(std::string text) {
        // A dot between digits is a version number, not a word break. Splitting
        // on it turned "gpt-5.3-codex-spark" into "Gpt 5 3 Codex Spark".
        for (size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];

            if (c == '_' || c == '-' || c == '/') {
                text[i] = ' ';
                continue;
            }

            if (c != '.') continue;

            const bool digitBefore = i > 0 &&
                std::isdigit(static_cast<unsigned char>(text[i - 1])) != 0;
            const bool digitAfter = i + 1 < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0;

            if (!digitBefore || !digitAfter) {
                text[i] = ' ';
            }
        }

        std::string output;
        output.reserve(text.size());
        bool capitalize = true;

        for (char c : text) {
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                if (!output.empty() && output.back() != ' ') output.push_back(' ');
                capitalize = true;
                continue;
            }

            output.push_back(capitalize
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : c);
            capitalize = false;
        }

        while (!output.empty() && output.back() == ' ') output.pop_back();

        // Word-by-word acronym pass: title casing alone gives "Gpt", "Api".
        static const char* const kAcronyms[] = { "gpt", "api", "gpu", "cpu", "ai", "id", "url" };

        size_t start = 0;
        while (start < output.size()) {
            size_t end = output.find(' ', start);
            if (end == std::string::npos) end = output.size();

            std::string word = output.substr(start, end - start);
            std::string lower = word;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            for (const char* acronym : kAcronyms) {
                if (lower != acronym) continue;
                std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                output.replace(start, end - start, word);
                break;
            }

            start = end + 1;
        }

        return output;
    }

    // "GPT-5.3-Codex-Spark" -> "GPT-5.3 Codex Spark".
    //
    // The model and its version stay hyphenated; everything after becomes
    // words. Anything not GPT-prefixed just loses its hyphens.
    static std::string FormatCodexLimitName(const std::string& displayName) {
        std::vector<std::string> parts;
        std::string current;

        for (char c : displayName) {
            if (c == '-') {
                if (!current.empty()) parts.push_back(current);
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        if (!current.empty()) parts.push_back(current);

        if (parts.size() < 2) {
            return displayName;
        }

        std::string head = parts[0];
        std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        std::string out;

        if (head == "gpt") {
            out = "GPT-" + parts[1];
            for (size_t i = 2; i < parts.size(); ++i) out += " " + parts[i];
            return out;
        }

        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) out.push_back(' ');
            out += parts[i];
        }
        return out;
    }

    static std::string AppServerRateLimitName(const json& rateLimit, const std::string& fallbackKey) {
        // limit_name arrives already display-cased from the server; running it
        // through HumanizeIdentifier was destroying it (hyphens to spaces, then
        // "5.3" split into "5 3"). Only the id/key fallbacks are real slugs.
        const std::string display = ReadStringFlexible(rateLimit, { "limitName", "limit_name" });

        if (!display.empty()) {
            return FormatCodexLimitName(display);
        }

        std::string slug = ReadStringFlexible(rateLimit, { "limitId", "limit_id" });

        if (slug.empty()) {
            slug = fallbackKey;
        }

        const std::string name = HumanizeIdentifier(slug);
        return name.empty() ? "Codex" : name;
    }

    static bool IsCodexMainLimit(const json& rateLimit, const std::string& key) {
        std::string id = ReadStringFlexible(rateLimit, { "limitId", "limit_id" });
        std::string candidate = id.empty() ? key : id;
        std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return candidate == "codex" || candidate == "default" || candidate == "chatgpt_codex";
    }

    struct AppServerRateLimitEntry {
        std::string key;
        const json* value = nullptr;
        bool main = false;
    };

    static std::vector<AppServerRateLimitEntry> CollectAppServerRateLimits(const json& result) {
        std::vector<AppServerRateLimitEntry> entries;

        if (!result.is_object()) {
            return entries;
        }

        std::string canonicalId;

        const json* canonical = FindObjectField(result, { "rateLimits", "rate_limits" });

        if (canonical) {
            canonicalId = ReadStringFlexible(*canonical, { "limitId", "limit_id" });

            AppServerRateLimitEntry entry;
            entry.key = canonicalId.empty() ? "codex" : canonicalId;
            entry.value = canonical;
            entry.main = true;
            entries.push_back(std::move(entry));
        }

        const json* byLimitId = FindObjectField(
            result,
            { "rateLimitsByLimitId", "rate_limits_by_limit_id" }
        );

        if (byLimitId) {
            for (auto it = byLimitId->begin(); it != byLimitId->end(); ++it) {
                if (!it.value().is_object()) {
                    continue;
                }

                const std::string valueId = ReadStringFlexible(it.value(), { "limitId", "limit_id" });
                const bool matchesCanonical = !entries.empty() && (
                    (!canonicalId.empty() && (it.key() == canonicalId || valueId == canonicalId)) ||
                    (canonicalId.empty() && IsCodexMainLimit(it.value(), it.key()))
                );

                if (matchesCanonical) {
                    // Prefer the per-limit entry because that map is the
                    // comprehensive current response, while preserving its
                    // canonical/main identity.
                    entries.front().key = it.key();
                    entries.front().value = &it.value();
                    entries.front().main = true;
                    continue;
                }

                AppServerRateLimitEntry entry;
                entry.key = it.key();
                entry.value = &it.value();
                entry.main = entries.empty() && IsCodexMainLimit(*entry.value, entry.key);
                entries.push_back(std::move(entry));
            }
        }

        if (!entries.empty() &&
            std::none_of(entries.begin(), entries.end(), [](const AppServerRateLimitEntry& entry) { return entry.main; })) {
            auto mainIt = std::find_if(entries.begin(), entries.end(), [](const AppServerRateLimitEntry& entry) {
                return entry.value && IsCodexMainLimit(*entry.value, entry.key);
            });

            if (mainIt != entries.end()) {
                mainIt->main = true;
            }
            else {
                entries.front().main = true;
            }
        }

        std::stable_sort(entries.begin(), entries.end(), [](const AppServerRateLimitEntry& a, const AppServerRateLimitEntry& b) {
            if (a.main != b.main) return a.main;
            return AppServerRateLimitName(*a.value, a.key) < AppServerRateLimitName(*b.value, b.key);
        });

        return entries;
    }

    static std::string FormatWindowDuration(double minutes) {
        if (minutes <= 0.0) {
            return {};
        }

        const long long rounded = static_cast<long long>(std::llround(minutes));

        if (rounded % (24 * 60) == 0) {
            const long long days = rounded / (24 * 60);
            return std::to_string(days) + "-day window";
        }

        if (rounded % 60 == 0) {
            const long long hours = rounded / 60;
            return std::to_string(hours) + "-hour window";
        }

        return std::to_string(rounded) + "-minute window";
    }

    static bool DurationApproximately(double minutes, double targetMinutes) {
        return minutes > 0.0 &&
            std::abs(minutes - targetMinutes) <= targetMinutes * 0.05;
    }

    static std::string WindowTypeLabel(
        double durationMinutes,
        int sourceOrder,
        size_t windowCount
    ) {
        if (DurationApproximately(durationMinutes, 5.0 * 60.0)) {
            return "Session";
        }

        if (DurationApproximately(durationMinutes, 7.0 * 24.0 * 60.0)) {
            return "Weekly";
        }

        if (DurationApproximately(durationMinutes, 24.0 * 60.0)) {
            return "Daily";
        }

        if (DurationApproximately(durationMinutes, 30.0 * 24.0 * 60.0)) {
            return "Monthly";
        }

        if (durationMinutes > 0.0) {
            return "Usage";
        }

        if (windowCount == 1) {
            return sourceOrder == 0 ? "Session" : "Weekly";
        }

        return sourceOrder == 0 ? "Session" : "Weekly";
    }

    struct AppServerWindowCandidate {
        const json* window = nullptr;
        double durationMinutes = 0.0;
        int sourceOrder = 0;
    };

    static void ParseAppServerWindows(
        const json& rateLimit,
        const std::string& rateLimitName,
        bool isMain,
        Snapshot& snapshot
    ) {
        std::vector<AppServerWindowCandidate> windows;

        auto addWindow = [&](std::initializer_list<const char*> keys, int sourceOrder) {
            const json* windowObject = FindObjectField(rateLimit, keys);

            if (!windowObject) {
                return;
            }

            const json& window = *windowObject;

            // Explicit null/missing utilization means the window does not
            // exist for this account. It must stay hidden.
            if (!ReadNumberFlexible(window, { "usedPercent", "used_percent" })) {
                return;
            }

            AppServerWindowCandidate candidate;
            candidate.window = &window;
            candidate.durationMinutes = ReadWindowDurationMinutes(window);
            candidate.sourceOrder = sourceOrder;
            windows.push_back(candidate);
        };

        addWindow({ "primary", "primaryWindow", "primary_window" }, 0);
        addWindow({ "secondary", "secondaryWindow", "secondary_window" }, 1);

        if (windows.size() >= 2) {
            std::stable_sort(
                windows.begin(),
                windows.end(),
                [](const AppServerWindowCandidate& a, const AppServerWindowCandidate& b) {
                    if (a.durationMinutes > 0.0 && b.durationMinutes > 0.0 &&
                        a.durationMinutes != b.durationMinutes) {
                        return a.durationMinutes < b.durationMinutes;
                    }
                    return a.sourceOrder < b.sourceOrder;
                }
            );
        }

        for (size_t i = 0; i < windows.size(); ++i) {
            const std::string windowName = WindowTypeLabel(
                windows[i].durationMinutes,
                windows[i].sourceOrder,
                windows.size()
            );

            const std::string label = isMain
                ? windowName
                : rateLimitName + " · " + windowName;
            const std::string sublabel = FormatWindowDuration(windows[i].durationMinutes);

            UsageBar bar = ParseCodexWindow(
                *windows[i].window,
                label,
                sublabel,
                isMain
            );

            AddUsageBarIfUnique(snapshot, std::move(bar));
        }
    }

    static void ParseAppServerResetCredits(const json& result, Snapshot& snapshot) {
        snapshot.resetCreditsAvailableCount = -1;
        snapshot.resetCredits.clear();
        snapshot.resetCreditLedger.clear();

        if (!result.is_object()) {
            return;
        }

        const json* resets = FindObjectField(
            result,
            { "rateLimitResetCredits", "rate_limit_reset_credits" }
        );

        if (!resets && (
            result.contains("availableCount") ||
            result.contains("available_count") ||
            result.contains("credits")
        )) {
            resets = &result;
        }

        if (!resets) {
            return;
        }

        snapshot.resetCreditsAvailableCount = static_cast<int>(
            ReadNumberFlexible(*resets, { "availableCount", "available_count" }).value_or(-1.0)
        );

        if (!resets->contains("credits") || !resets->at("credits").is_array()) {
            return;
        }

        for (const json& credit : resets->at("credits")) {
            if (!credit.is_object()) {
                continue;
            }

            ResetCredit reset;
            reset.title = ReadStringFlexible(credit, { "title", "name" });
            reset.status = ReadStringFlexible(credit, { "status" });

            for (const char* key : { "grantedAt", "granted_at" }) {
                if (credit.contains(key) && !credit.at(key).is_null()) {
                    reset.grantedAt = ParseTimeFlexible(credit.at(key));
                    break;
                }
            }

            for (const char* key : { "expiresAt", "expires_at" }) {
                if (credit.contains(key) && !credit.at(key).is_null()) {
                    reset.expiresAt = ParseTimeFlexible(credit.at(key));
                    break;
                }
            }

            snapshot.resetCreditLedger.push_back(reset);

            std::string status = reset.status;
            std::transform(status.begin(), status.end(), status.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (status == "available") {
                snapshot.resetCredits.push_back(reset);
            }
        }

        auto byExpiry = [](const ResetCredit& a, const ResetCredit& b) {
            const bool aKnown = a.expiresAt.time_since_epoch().count() != 0;
            const bool bKnown = b.expiresAt.time_since_epoch().count() != 0;

            if (aKnown != bKnown) {
                return aKnown;
            }

            return a.expiresAt < b.expiresAt;
        };

        std::sort(snapshot.resetCredits.begin(), snapshot.resetCredits.end(), byExpiry);
        std::sort(snapshot.resetCreditLedger.begin(), snapshot.resetCreditLedger.end(), byExpiry);
    }

    static Snapshot ParseAppServerSnapshot(
        const json& accountResult,
        const json& result
    ) {
        Snapshot snapshot;
        snapshot.statusText = "Source: Codex app-server active account";
        snapshot.lastUpdated = "just now";
        snapshot.bars.clear();

        const std::vector<AppServerRateLimitEntry> entries = CollectAppServerRateLimits(result);
        const json* mainRateLimit = nullptr;

        for (const AppServerRateLimitEntry& entry : entries) {
            if (!entry.value) {
                continue;
            }

            if (entry.main && !mainRateLimit) {
                mainRateLimit = entry.value;
            }

            const std::string name = AppServerRateLimitName(*entry.value, entry.key);
            ParseAppServerWindows(*entry.value, name, entry.main, snapshot);
            ParseCodexLimitStatus(*entry.value, snapshot);

            // Credits and workspace spend controls are optional. Parse them
            // only when the server actually supplies the corresponding object.
            if (!snapshot.creditBalance.valid) {
                ParseCodexCreditBalance(*entry.value, snapshot);
            }

            if (!snapshot.extraUsage.valid) {
                ParseCodexIndividualLimit(*entry.value, snapshot);
            }

            if (!snapshot.extraUsage.valid) {
                ParseCodexMonthlyUsage(*entry.value, snapshot);
            }
        }

        if (mainRateLimit) {
            snapshot.plan = FormatCodexPlanToken(ExtractAppServerPlanType(accountResult, *mainRateLimit));
        }
        else {
            snapshot.plan = FormatCodexPlanToken(ExtractAppServerPlanType(accountResult, json::object()));
        }

        ParseCodexLimitStatus(result, snapshot);

        // Accept top-level optional fields too, for forward/backward compatible
        // app-server responses where these details are not nested in a bucket.
        if (!snapshot.creditBalance.valid) {
            ParseCodexCreditBalance(result, snapshot);
        }

        if (!snapshot.extraUsage.valid) {
            ParseCodexIndividualLimit(result, snapshot);
        }

        if (!snapshot.extraUsage.valid) {
            ParseCodexMonthlyUsage(result, snapshot);
        }

        ParseAppServerResetCredits(result, snapshot);
        return snapshot;
    }

    static void ParseLegacyRateLimitWindows(
        const json& rateLimit,
        const std::string& rateLimitName,
        bool isMain,
        Snapshot& snapshot
    ) {
        struct Candidate {
            const json* window = nullptr;
            double durationMinutes = 0.0;
            int sourceOrder = 0;
        };

        std::vector<Candidate> windows;

        auto addWindow = [&](std::initializer_list<const char*> keys, int sourceOrder) {
            const json* window = FindObjectField(rateLimit, keys);

            if (!window || !ReadNumberFlexible(*window, { "used_percent", "usedPercent" })) {
                return;
            }

            Candidate candidate;
            candidate.window = window;
            candidate.durationMinutes = ReadWindowDurationMinutes(*window);
            candidate.sourceOrder = sourceOrder;
            windows.push_back(candidate);
        };

        addWindow({ "primary_window", "primary" }, 0);
        addWindow({ "secondary_window", "secondary" }, 1);

        if (windows.size() >= 2) {
            std::stable_sort(windows.begin(), windows.end(), [](const Candidate& a, const Candidate& b) {
                if (a.durationMinutes > 0.0 && b.durationMinutes > 0.0 &&
                    a.durationMinutes != b.durationMinutes) {
                    return a.durationMinutes < b.durationMinutes;
                }
                return a.sourceOrder < b.sourceOrder;
            });
        }

        for (size_t i = 0; i < windows.size(); ++i) {
            const std::string windowName = WindowTypeLabel(
                windows[i].durationMinutes,
                windows[i].sourceOrder,
                windows.size()
            );

            const std::string label = isMain
                ? windowName
                : rateLimitName + " · " + windowName;

            UsageBar bar = ParseCodexWindow(
                *windows[i].window,
                label,
                FormatWindowDuration(windows[i].durationMinutes),
                isMain
            );

            AddUsageBarIfUnique(snapshot, std::move(bar));
        }
    }

    static void ParseLegacyAdditionalRateLimitEntry(
        const json& entry,
        const std::string& fallbackName,
        const json& usageJson,
        Snapshot& snapshot
    ) {
        if (!entry.is_object()) {
            return;
        }

        const json* rateLimit = FindObjectField(entry, { "rate_limit", "rateLimit" });
        const json* body = rateLimit ? rateLimit : &entry;

        std::string name = ReadStringFlexible(entry, { "limit_name", "limitName", "rate_limit_name", "rateLimitName" });

        if (name.empty()) {
            name = AppServerRateLimitName(*body, fallbackName);
        }
        else {
            name = HumanizeIdentifier(TrimAscii(name));
        }

        if (name.empty()) {
            name = "Codex";
        }

        const bool isMain = IsCodexMainLimit(*body, fallbackName) ||
            (!GetMainRateLimitObject(usageJson) &&
                (fallbackName == "codex" || fallbackName == "default"));

        // The canonical rate_limit object is parsed separately. Skip the same
        // main bucket if a compatibility map repeats it.
        if (isMain && GetMainRateLimitObject(usageJson)) {
            return;
        }

        ParseLegacyRateLimitWindows(*body, name, isMain, snapshot);
        ParseCodexLimitStatus(entry, snapshot);
        ParseCodexLimitStatus(*body, snapshot);

        if (!snapshot.creditBalance.valid) {
            ParseCodexCreditBalance(entry, snapshot);
        }
        if (!snapshot.creditBalance.valid) {
            ParseCodexCreditBalance(*body, snapshot);
        }

        if (!snapshot.extraUsage.valid) {
            ParseCodexIndividualLimit(entry, snapshot);
        }
        if (!snapshot.extraUsage.valid) {
            ParseCodexIndividualLimit(*body, snapshot);
        }
    }

    static void ParseLegacyAdditionalRateLimits(const json& usageJson, Snapshot& snapshot) {
        if (!usageJson.is_object()) {
            return;
        }

        // Codex Desktop's legacy wham response uses an array:
        // additional_rate_limits: [{ limit_name, rate_limit }, ...].
        for (const char* key : { "additional_rate_limits", "additionalRateLimits" }) {
            if (!usageJson.contains(key) || !usageJson.at(key).is_array()) {
                continue;
            }

            size_t index = 0;

            for (const json& entry : usageJson.at(key)) {
                ParseLegacyAdditionalRateLimitEntry(
                    entry,
                    "additional_" + std::to_string(index++),
                    usageJson,
                    snapshot
                );
            }
        }

        // Keep compatibility with normalized/experimental responses that
        // expose the same buckets as a map keyed by limit ID.
        for (const char* key : { "rate_limits_by_limit_id", "rateLimitsByLimitId" }) {
            if (!usageJson.contains(key) || !usageJson.at(key).is_object()) {
                continue;
            }

            const json& byLimitId = usageJson.at(key);

            for (auto it = byLimitId.begin(); it != byLimitId.end(); ++it) {
                ParseLegacyAdditionalRateLimitEntry(
                    it.value(),
                    it.key(),
                    usageJson,
                    snapshot
                );
            }
        }
    }

    static Snapshot ParseUsageAndResets(const json& usageJson, const json& resetJson) {
        Snapshot snapshot;
        snapshot.plan = FormatCodexPlan(usageJson);
        snapshot.statusText = "Plan: " + snapshot.plan;
        snapshot.lastUpdated = "just now";
        snapshot.bars.clear();
        snapshot.resetCreditsAvailableCount = ParseResetCreditsAvailableCount(usageJson);

        ParseCodexCreditBalance(usageJson, snapshot);
        ParseCodexIndividualLimit(usageJson, snapshot);
        ParseCodexMonthlyUsage(usageJson, snapshot);
        ParseCodexLimitStatus(usageJson, snapshot);

        const json* rateLimit = GetMainRateLimitObject(usageJson);

        if (rateLimit) {
#ifdef _DEBUG
            OutputDebugStringA(
                (
                    std::string("Codex rate_limit raw: ") +
                    rateLimit->dump(2) +
                    "\n"
                ).c_str()
            );
#endif

            ParseLegacyRateLimitWindows(*rateLimit, "Codex", true, snapshot);
            ParseCodexLimitStatus(*rateLimit, snapshot);

            if (!snapshot.creditBalance.valid) {
                ParseCodexCreditBalance(*rateLimit, snapshot);
            }

            if (!snapshot.extraUsage.valid) {
                ParseCodexIndividualLimit(*rateLimit, snapshot);
            }

            if (!snapshot.extraUsage.valid) {
                ParseCodexMonthlyUsage(*rateLimit, snapshot);
            }
        }

        // Some usage responses expose additional named buckets beside the
        // canonical rate_limit. Parse every bucket that actually has a usable
        // utilization percentage and hide absent/null windows.
        ParseLegacyAdditionalRateLimits(usageJson, snapshot);

        ParseResetCredits(resetJson, snapshot);
        return snapshot;
    }

    // Codex's OAuth contract, taken from the Desktop bundle and OpenUsage:
    // POST auth.openai.com/oauth/token, form-encoded, with the CLI's client id.
    // The refresh token ROTATES - replaying a spent one is rejected as
    // refresh_token_reused - so a refresh must be persisted, and auth.json must
    // be re-read first in case the Codex CLI already rotated it for us.
    static constexpr const char* kCodexOAuthClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
    static constexpr const char* kCodexTokenUrl = "https://auth.openai.com/oauth/token";

    static std::string UrlFormEncode(const std::string& value) {
        static const char* kHex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 3);

        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            }
            else {
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0x0F]);
            }
        }

        return out;
    }

    static std::string CodexRefreshErrorCode(const std::string& body) {
        const json parsed = json::parse(body, nullptr, false);

        if (parsed.is_discarded() || !parsed.is_object()) {
            return {};
        }

        const auto error = parsed.find("error");

        if (error != parsed.end()) {
            if (error->is_string()) {
                return error->get<std::string>();
            }

            if (error->is_object()) {
                for (const char* key : { "code", "error" }) {
                    const auto nested = error->find(key);

                    if (nested != error->end() && nested->is_string()) {
                        return nested->get<std::string>();
                    }
                }
            }
        }

        const auto code = parsed.find("code");
        return code != parsed.end() && code->is_string() ? code->get<std::string>() : std::string{};
    }

    // Rotates the Codex access token in place. Returns false when the stored
    // refresh token cannot produce a new one; `detail` then explains why.
    static bool RefreshCodexTokens(
        const std::filesystem::path& authPath,
        json& auth,
        std::string& accessToken,
        std::string& detail
    ) {
        if (!auth.contains("tokens") || !auth.at("tokens").is_object()) {
            detail = "Codex auth.json does not contain tokens";
            return false;
        }

        const std::string refreshToken = ReadStringFlexible(
            auth.at("tokens"),
            { "refresh_token", "refreshToken" }
        );

        if (refreshToken.empty()) {
            detail = "Codex auth.json has no refresh_token; run `codex login` again";
            return false;
        }

        const std::string body =
            "grant_type=refresh_token"
            "&client_id=" + UrlFormEncode(kCodexOAuthClientId) +
            "&refresh_token=" + UrlFormEncode(refreshToken);

        const Network::HttpResponse response = Network::get_instance()->RequestUrl(
            kCodexTokenUrl,
            "POST",
            L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n",
            body
        );

        if (response.statusCode == 400 || response.statusCode == 401) {
            const std::string code = CodexRefreshErrorCode(response.body);

            if (code == "refresh_token_expired") {
                detail = "the Codex session expired; run `codex login` again";
            }
            else if (code == "refresh_token_reused") {
                detail = "the Codex refresh token was already rotated by another client";
            }
            else if (code == "refresh_token_invalidated") {
                detail = "the Codex refresh token was revoked; run `codex login` again";
            }
            else {
                detail = "Codex token refresh failed: HTTP " + std::to_string(response.statusCode);
            }

            return false;
        }

        if (response.statusCode < 200 || response.statusCode >= 300) {
            detail = "Codex token refresh failed: HTTP " + std::to_string(response.statusCode);
            return false;
        }

        const json decoded = json::parse(response.body, nullptr, false);

        if (decoded.is_discarded() || !decoded.is_object()) {
            detail = "Codex token refresh returned an unreadable response";
            return false;
        }

        const std::string refreshed = ReadStringFlexible(decoded, { "access_token" });

        if (refreshed.empty()) {
            detail = "Codex token refresh returned no access_token; run `codex login` again";
            return false;
        }

        accessToken = refreshed;
        auth["tokens"]["access_token"] = refreshed;

        for (const char* key : { "refresh_token", "id_token" }) {
            const std::string value = ReadStringFlexible(decoded, { key });

            if (!value.empty()) {
                auth["tokens"][key] = value;
            }
        }

        // Codex writes last_refresh as an ISO-8601 UTC instant.
        const std::time_t now = std::time(nullptr);
        std::tm utc{};

        if (gmtime_s(&utc, &now) == 0) {
            char stamp[32]{};

            if (std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc) > 0) {
                auth["last_refresh"] = std::string(stamp);
            }
        }

        // Persisting is best effort: a read-only or externally-locked auth.json
        // still leaves the in-memory token usable for this refresh cycle.
        try {
            std::ofstream file(authPath, std::ios::binary | std::ios::trunc);

            if (file) {
                file << auth.dump(2);
            }
        }
        catch (const std::exception&) {
        }

        detail.clear();
        return true;
    }

    static Snapshot FetchSnapshotFromAuthFile(
        const std::filesystem::path& authPath,
        bool enforceCredentialStoreSafety,
        const std::string& sourceLabel
    ) {
        json auth = ReadAuthJson(authPath, enforceCredentialStoreSafety);

        if (!auth.contains("tokens") || !auth.at("tokens").is_object()) {
            throw std::runtime_error("Codex auth.json does not contain tokens");
        }

        const json& tokens = auth.at("tokens");

        std::string accessToken = ReadStringFlexible(
            tokens,
            { "access_token", "accessToken" }
        );
        std::string accountId = ReadStringFlexible(
            tokens,
            { "account_id", "accountId", "chatgpt_account_id", "chatgptAccountId" }
        );

        if (accessToken.empty()) {
            throw std::runtime_error("Codex access_token missing");
        }

        if (accountId.empty()) {
            throw std::runtime_error("Codex account_id missing");
        }

        const std::string safeAccountId =
            Network::get_instance()->StripHeaderValue(accountId);

        auto buildHeaders = [&](const std::string& token) {
            return std::wstring(L"Authorization: Bearer ") +
                Network::get_instance()->Utf8ToWide(Network::get_instance()->StripHeaderValue(token)) +
                L"\r\n"
                L"ChatGPT-Account-ID: " +
                Network::get_instance()->Utf8ToWide(safeAccountId) +
                L"\r\n"
                L"Accept: application/json\r\n"
                L"Origin: https://chatgpt.com\r\n"
                L"Referer: https://chatgpt.com/codex\r\n"
                L"originator: Codex Desktop\r\n";
        };

        std::wstring headers = buildHeaders(accessToken);

        Network::HttpResponse usage = Network::get_instance()->RequestUrl(
            "https://chatgpt.com/backend-api/wham/usage",
            "GET",
            headers
        );

        // The stored access token expires in about an hour, which is why an
        // idle AQC reports token_expired. Recover the way Codex itself does -
        // but re-read auth.json first: if the CLI already rotated the token,
        // replaying our copy of the refresh token is rejected as
        // refresh_token_reused and would break the CLI's session too.
        if (usage.statusCode == 401) {
            try {
                const json current = ReadAuthJson(authPath, false);

                if (current.contains("tokens") && current.at("tokens").is_object()) {
                    const std::string currentToken = ReadStringFlexible(
                        current.at("tokens"),
                        { "access_token", "accessToken" }
                    );

                    if (!currentToken.empty() && currentToken != accessToken) {
                        auth = current;
                        accessToken = currentToken;
                        headers = buildHeaders(accessToken);
                        usage = Network::get_instance()->RequestUrl(
                            "https://chatgpt.com/backend-api/wham/usage",
                            "GET",
                            headers
                        );
                    }
                }
            }
            catch (const std::exception&) {
            }

            if (usage.statusCode == 401) {
                std::string refreshDetail;

                if (RefreshCodexTokens(authPath, auth, accessToken, refreshDetail)) {
                    headers = buildHeaders(accessToken);
                    usage = Network::get_instance()->RequestUrl(
                        "https://chatgpt.com/backend-api/wham/usage",
                        "GET",
                        headers
                    );
                }
                else if (!refreshDetail.empty()) {
                    throw std::runtime_error("Codex usage failed: " + refreshDetail);
                }
            }
        }

        if (usage.statusCode < 200 || usage.statusCode >= 300) {
            throw std::runtime_error("Codex usage failed: HTTP " + std::to_string(usage.statusCode));
        }

        Network::HttpResponse resets = Network::get_instance()->RequestUrl(
            "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits",
            "GET",
            headers
        );

        Network::HttpResponse monthlyUsage = Network::get_instance()->RequestUrl(
            "https://chatgpt.com/backend-api/accounts/" +
                EncodeUrlPathSegment(safeAccountId) +
                "/spend-controls/current-user/monthly-usage",
            "GET",
            headers
        );

        json resetJson = json::object();

        // Reset credits are supplemental. A missing/unsupported reset-credit
        // endpoint must not hide otherwise valid usage windows and balances.
        if (resets.statusCode >= 200 && resets.statusCode < 300 && !resets.body.empty()) {
            try {
                resetJson = JsonUtils::get_instance()->ParseRequired(resets.body);
            }
            catch (...) {
                resetJson = json::object();
            }
        }

        json monthlyUsageJson = json::object();

        // Codex Desktop requests current monthly usage separately from the
        // core wham/usage response. Treat it as supplemental: parse it when
        // available, but never discard valid rate-limit windows if it fails.
        if (monthlyUsage.statusCode >= 200 && monthlyUsage.statusCode < 300 &&
            !monthlyUsage.body.empty()) {
            try {
                monthlyUsageJson = JsonUtils::get_instance()->ParseRequired(monthlyUsage.body);
            }
            catch (...) {
                monthlyUsageJson = json::object();
            }
        }

        Snapshot snapshot = ParseUsageAndResets(
            JsonUtils::get_instance()->ParseRequired(usage.body),
            resetJson
        );

        if (!snapshot.extraUsage.valid) {
            ParseCodexMonthlyUsage(monthlyUsageJson, snapshot);
        }

        snapshot.statusText = "Source: " + sourceLabel + ": " + authPath.string();

        std::string store = ConfiguredCredentialStore();

        if (!enforceCredentialStoreSafety &&
            authPath == DefaultAuthPath() &&
            (store == "auto" || store == "keyring" || store == "ephemeral")) {
            snapshot.statusText += " (config store is " + store + "; this file may not be the active Codex account)";
        }

        return snapshot;
    }

    static std::string ReadTailSharedReadOnly(
        const std::filesystem::path& path,
        unsigned long long maximumBytes
    ) {
        const std::wstring widePath = path.wstring();
        HANDLE file = CreateFileW(
            widePath.c_str(),
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
            const DWORD chunkSize = static_cast<DWORD>(std::min<size_t>(
                text.size() - totalRead,
                1024 * 1024
            ));
            DWORD read = 0;

            if (!ReadFile(file, text.data() + totalRead, chunkSize, &read, nullptr) || read == 0) {
                break;
            }

            totalRead += read;
        }

        CloseHandle(file);
        text.resize(totalRead);

        // A tail read can start in the middle of one JSONL record. Drop only
        // that partial first record; the source file is never modified/copied.
        if (start > 0) {
            const size_t firstNewline = text.find('\n');

            if (firstNewline == std::string::npos) {
                return {};
            }

            text.erase(0, firstNewline + 1);
        }

        return text;
    }

    static std::filesystem::path LatestCodexSessionFile() {
        const std::filesystem::path sessions = CodexHomePath() / "sessions";
        std::error_code ec;

        if (!std::filesystem::exists(sessions, ec) || ec) {
            return {};
        }

        std::filesystem::path latest;
        std::filesystem::file_time_type latestTime{};
        bool haveLatest = false;

        std::filesystem::recursive_directory_iterator it(
            sessions,
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

            const std::filesystem::file_time_type writeTime = entry.last_write_time(ec);

            if (ec) {
                ec.clear();
                continue;
            }

            if (!haveLatest || writeTime > latestTime) {
                latest = entry.path();
                latestTime = writeTime;
                haveLatest = true;
            }
        }

        return latest;
    }

    struct CodexTokenUsageRecord {
        bool valid = false;
        long long totalTokens = 0;
        long long inputTokens = 0;
        long long cachedInputTokens = 0;
        long long outputTokens = 0;
        long long reasoningOutputTokens = 0;
        long long modelContextWindow = 0;
        long long modelAutoCompactTokenLimit = 0;
        bool cumulativeValid = false;
        long long cumulativeTotalTokens = 0;
    };

    struct CodexModelCompactionMetadata {
        bool valid = false;
        long long contextWindow = 0;
        long long autoCompactTokenLimit = 0;
        int effectiveContextWindowPercent = 0;
    };

    static const json* FindCodexModelMetadataRecursive(
        const json& value,
        const std::string& model,
        int depth = 0
    ) {
        if (depth > 8 || model.empty()) {
            return nullptr;
        }

        if (value.is_object()) {
            const std::string slug = ReadStringFlexible(
                value,
                { "slug", "model", "id" }
            );

            if (slug == model) {
                return &value;
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                if (const json* found = FindCodexModelMetadataRecursive(
                    it.value(),
                    model,
                    depth + 1
                )) {
                    return found;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (const json* found = FindCodexModelMetadataRecursive(
                    item,
                    model,
                    depth + 1
                )) {
                    return found;
                }
            }
        }

        return nullptr;
    }

    static CodexModelCompactionMetadata ReadCodexModelCompactionMetadata(
        const std::string& model
    ) {
        CodexModelCompactionMetadata metadata;

        if (model.empty()) {
            return metadata;
        }

        const std::filesystem::path cachePath = CodexHomePath() / "models_cache.json";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(cachePath, ec) || ec) {
            return metadata;
        }

        const std::string text = Network::get_instance()->ReadTextFile(cachePath);
        if (text.empty()) {
            return metadata;
        }

        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded()) {
            return metadata;
        }

        const json* modelObject = FindCodexModelMetadataRecursive(root, model);
        if (!modelObject) {
            return metadata;
        }

        auto readPositiveCount = [](const json& object, std::initializer_list<const char*> keys) {
            const std::optional<double> value = ReadNumberFlexible(object, keys);
            if (!value || !std::isfinite(*value) || *value <= 0.0) {
                return 0LL;
            }
            return static_cast<long long>(std::floor(*value));
        };

        metadata.contextWindow = readPositiveCount(
            *modelObject,
            { "context_window", "contextWindow", "max_context_window", "maxContextWindow" }
        );
        metadata.autoCompactTokenLimit = readPositiveCount(
            *modelObject,
            { "auto_compact_token_limit", "autoCompactTokenLimit" }
        );

        const long long effectivePercent = readPositiveCount(
            *modelObject,
            { "effective_context_window_percent", "effectiveContextWindowPercent" }
        );
        if (effectivePercent > 0 && effectivePercent <= 100) {
            metadata.effectiveContextWindowPercent = static_cast<int>(effectivePercent);
        }

        metadata.valid = metadata.contextWindow > 0 ||
            metadata.autoCompactTokenLimit > 0 ||
            metadata.effectiveContextWindowPercent > 0;
        return metadata;
    }

    static long long ResolveCodexContextWindowForModel(const std::string& model)
    {
        const long long configured = g_modelContextWindow.load();
        if (configured > 0) {
            std::string configuredModel;
            {
                std::lock_guard<std::mutex> lock(g_configModelMutex);
                configuredModel = g_configModel;
            }
            // config/read reflects the configured model, while a thread may
            // temporarily use a different per-thread selection. Never apply a
            // context override captured for one model to another model.
            if (configuredModel.empty() || model.empty() || configuredModel == model) {
                return configured;
            }
        }

        const CodexModelCompactionMetadata metadata =
            ReadCodexModelCompactionMetadata(model);
        if (!metadata.valid || metadata.contextWindow <= 0) {
            return 0;
        }

        if (metadata.effectiveContextWindowPercent > 0 &&
            metadata.effectiveContextWindowPercent <= 100) {
            return static_cast<long long>(std::llround(
                static_cast<double>(metadata.contextWindow) *
                static_cast<double>(metadata.effectiveContextWindowPercent) / 100.0
            ));
        }
        return metadata.contextWindow;
    }

    static long long ResolveCodexDefaultAutoCompactTokenLimit(
        const std::string& model,
        long long liveEffectiveContextWindow
    ) {
        const CodexModelCompactionMetadata metadata =
            ReadCodexModelCompactionMetadata(model);

        if (!metadata.valid) {
            return 0;
        }

        // Token telemetry reports Codex's effective modelContextWindow, while
        // the model catalog stores the raw context window plus the percentage
        // considered usable. Convert the LIVE window back to the corresponding
        // raw catalog window so stale cache sizes cannot override live session
        // accounting. Codex then derives the default auto-compact edge at 90%
        // of that raw window.
        long long rawContextWindow = 0;
        if (liveEffectiveContextWindow > 0 && metadata.effectiveContextWindowPercent > 0) {
            rawContextWindow = static_cast<long long>(std::llround(
                static_cast<double>(liveEffectiveContextWindow) * 100.0 /
                static_cast<double>(metadata.effectiveContextWindowPercent)
            ));
        }
        else if (metadata.contextWindow > 0 &&
            liveEffectiveContextWindow == metadata.contextWindow) {
            // Some catalogs use 100% as the effective window and may omit the
            // percentage in older cache formats.
            rawContextWindow = metadata.contextWindow;
        }

        if (rawContextWindow <= 0) {
            return 0;
        }

        const long long ninetyPercentOfContext =
            (rawContextWindow / 10LL) * 9LL +
            ((rawContextWindow % 10LL) * 9LL) / 10LL;

        if (ninetyPercentOfContext <= 0) {
            return 0;
        }

        // A model-specific catalog limit, when present, is itself clamped by
        // Codex to 90% of the resolved raw model context window. When absent,
        // 90% is Codex's default automatic-compaction limit.
        if (metadata.autoCompactTokenLimit > 0) {
            return (std::min)(metadata.autoCompactTokenLimit, ninetyPercentOfContext);
        }

        return ninetyPercentOfContext;
    }

    static long long CodexRecordUnixSeconds(const json& record) {
        if (!record.is_object() || !record.contains("timestamp")) {
            return 0;
        }

        const auto parsed = ParseTimeFlexible(record.at("timestamp"));
        if (parsed.time_since_epoch().count() == 0) {
            return 0;
        }

        return static_cast<long long>(std::chrono::system_clock::to_time_t(parsed));
    }

    static CodexTokenUsageRecord ParseCodexTokenUsageRecord(const json& record) {
        CodexTokenUsageRecord usage;
        const json* payload = FindObjectField(record, { "payload" });

        if (!payload) {
            return usage;
        }

        const std::string payloadType = UsageTelemetry::LowerCopy(
            ReadStringFlexible(*payload, { "type" })
        );
        if (payloadType != "token_count" && payloadType != "tokencount") {
            return usage;
        }

        const json* info = FindObjectField(*payload, { "info" });
        if (!info) {
            return usage;
        }

        const json* last = FindObjectField(
            *info,
            { "last_token_usage", "lastTokenUsage", "last" }
        );

        if (!last) {
            return usage;
        }

        auto readCount = [](const json& object, std::initializer_list<const char*> keys) {
            return static_cast<long long>(std::max(
                0.0,
                ReadNumberFlexible(object, keys).value_or(0.0)
            ));
        };

        usage.totalTokens = readCount(*last, { "total_tokens", "totalTokens" });
        usage.inputTokens = readCount(*last, { "input_tokens", "inputTokens" });
        usage.cachedInputTokens = readCount(
            *last,
            { "cached_input_tokens", "cachedInputTokens" }
        );
        usage.outputTokens = readCount(*last, { "output_tokens", "outputTokens" });
        usage.reasoningOutputTokens = readCount(
            *last,
            { "reasoning_output_tokens", "reasoningOutputTokens" }
        );
        usage.modelContextWindow = readCount(
            *info,
            { "model_context_window", "modelContextWindow" }
        );
        // Some/newer transcript shapes may carry the effective threshold next
        // to token usage. Prefer it whenever present; otherwise the app-server
        // config/read cache below supplies the value.
        usage.modelAutoCompactTokenLimit = readCount(
            *info,
            { "model_auto_compact_token_limit", "modelAutoCompactTokenLimit" }
        );
        if (usage.modelAutoCompactTokenLimit <= 0) {
            usage.modelAutoCompactTokenLimit = readCount(
                *payload,
                { "model_auto_compact_token_limit", "modelAutoCompactTokenLimit" }
            );
        }

        if (usage.totalTokens <= 0) {
            usage.totalTokens = std::max(0LL, usage.inputTokens + usage.outputTokens);
        }

        const json* cumulative = FindObjectField(
            *info,
            { "total_token_usage", "totalTokenUsage", "total" }
        );

        if (cumulative) {
            const std::optional<double> cumulativeTotal = ReadNumberFlexible(
                *cumulative,
                { "total_tokens", "totalTokens" }
            );

            if (cumulativeTotal) {
                usage.cumulativeValid = true;
                usage.cumulativeTotalTokens = static_cast<long long>(std::max(0.0, *cumulativeTotal));
            }
        }

        usage.valid = usage.totalTokens > 0 || usage.modelContextWindow > 0 ||
            usage.inputTokens > 0 || usage.outputTokens > 0;
        return usage;
    }

    static const json* FindCodexCompactionObjectRecursive(
        const json& value,
        int depth = 0
    ) {
        if (depth > 8) {
            return nullptr;
        }

        if (value.is_object()) {
            for (const char* key : { "type", "subtype", "event", "kind" }) {
                if (!value.contains(key) || !value.at(key).is_string()) {
                    continue;
                }

                if (UsageTelemetry::LowerCopy(value.at(key).get<std::string>()).find("compact") !=
                    std::string::npos) {
                    return &value;
                }
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!it.value().is_object() && !it.value().is_array()) {
                    continue;
                }

                if (const json* nested = FindCodexCompactionObjectRecursive(it.value(), depth + 1)) {
                    return nested;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (const json* nested = FindCodexCompactionObjectRecursive(item, depth + 1)) {
                    return nested;
                }
            }
        }

        return nullptr;
    }

    static bool CodexCompactionStateFromRecord(
        const json& record,
        bool& state
    ) {
        const json* compactObject = FindCodexCompactionObjectRecursive(record);

        if (!compactObject) {
            return false;
        }

        const json& source = *compactObject;
        const std::string type = UsageTelemetry::LowerCopy(
            ReadStringFlexible(source, { "type", "event", "kind", "subtype" })
        );
        const std::string status = UsageTelemetry::LowerCopy(
            ReadStringFlexible(source, { "status", "phase" })
        );
        const std::string combined = type + " " + status;

        if (combined.find("compact") == std::string::npos) {
            return false;
        }

        const std::optional<bool> completed = ReadBoolFlexible(
            source,
            { "completed", "isCompleted", "is_completed" }
        );

        const bool completionEvent =
            combined.find("context_compacted") != std::string::npos ||
            combined.find("thread/compacted") != std::string::npos ||
            combined.find("compaction_completed") != std::string::npos ||
            combined.find("compact_completed") != std::string::npos ||
            combined.find("compacted") != std::string::npos ||
            status == "completed" || status == "complete" || status == "done" ||
            (completed.has_value() && *completed);

        if (completionEvent) {
            state = false;
            return true;
        }

        const bool runningEvent =
            combined.find("compaction_started") != std::string::npos ||
            combined.find("compact_started") != std::string::npos ||
            combined.find("context_compaction") != std::string::npos ||
            combined.find("contextcompaction") != std::string::npos ||
            combined.find("compacting") != std::string::npos ||
            status == "running" || status == "started" || status == "in_progress" ||
            (completed.has_value() && !*completed);

        if (runningEvent) {
            state = true;
            return true;
        }

        return false;
    }

    static UsageTelemetry::RunUsage ReadCodexRunFromLines(
        const std::vector<std::string>& lines
    ) {
        UsageTelemetry::RunUsage run;
        long long lastCumulativeTotal = 0;
        bool haveCumulativeTotal = false;
        long long runBaselineTotal = 0;
        bool haveRunBaseline = false;

        auto startRun = [&](long long at) {
            run = {};
            run.valid = true;
            run.running = true;
            run.thinking = true;
            run.startedAtUnixSeconds = at;
            run.thinkingStartedAtUnixSeconds = at;
            haveRunBaseline = haveCumulativeTotal;
            runBaselineTotal = lastCumulativeTotal;
        };

        for (const std::string& candidate : lines) {
            const bool mayAffectRun =
                candidate.find("user_message") != std::string::npos ||
                candidate.find("userMessage") != std::string::npos ||
                candidate.find("task_started") != std::string::npos ||
                candidate.find("taskStarted") != std::string::npos ||
                candidate.find("turn_started") != std::string::npos ||
                candidate.find("turnStarted") != std::string::npos ||
                candidate.find("task_complete") != std::string::npos ||
                candidate.find("taskComplete") != std::string::npos ||
                candidate.find("turn_completed") != std::string::npos ||
                candidate.find("turnCompleted") != std::string::npos ||
                candidate.find("turn_aborted") != std::string::npos ||
                candidate.find("turnAborted") != std::string::npos ||
                candidate.find("agent_reasoning") != std::string::npos ||
                candidate.find("agentReasoning") != std::string::npos ||
                candidate.find("agent_message") != std::string::npos ||
                candidate.find("agentMessage") != std::string::npos ||
                candidate.find("tool_call") != std::string::npos ||
                candidate.find("toolCall") != std::string::npos ||
                candidate.find("exec_command") != std::string::npos ||
                candidate.find("execCommand") != std::string::npos ||
                candidate.find("token_count") != std::string::npos ||
                candidate.find("tokenCount") != std::string::npos;

            if (!mayAffectRun) {
                continue;
            }

            json record = json::parse(candidate, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const long long at = CodexRecordUnixSeconds(record);
            const json* payload = FindObjectField(record, { "payload" });
            const std::string type = payload
                ? UsageTelemetry::LowerCopy(ReadStringFlexible(*payload, { "type" }))
                : std::string{};

            const CodexTokenUsageRecord tokenUsage = ParseCodexTokenUsageRecord(record);
            if (tokenUsage.valid) {
                if (tokenUsage.cumulativeValid) {
                    lastCumulativeTotal = tokenUsage.cumulativeTotalTokens;
                    haveCumulativeTotal = true;
                }

                if (run.valid && run.running) {
                    run.tokenStatsValid = true;
                    run.rawInputTokens = tokenUsage.inputTokens;
                    run.cacheReadInputTokens = tokenUsage.cachedInputTokens;
                    run.inputTokens = std::max(
                        0LL,
                        tokenUsage.inputTokens - tokenUsage.cachedInputTokens
                    );
                    run.currentTokens = tokenUsage.outputTokens;
                    run.reasoningOutputTokens = tokenUsage.reasoningOutputTokens;

                    if (tokenUsage.cumulativeValid && haveRunBaseline) {
                        run.tokens = std::max(
                            0LL,
                            tokenUsage.cumulativeTotalTokens - runBaselineTotal
                        );
                    }
                    else {
                        run.tokens = tokenUsage.totalTokens;
                    }
                }
                continue;
            }

            const bool startsRun =
                type == "user_message" || type == "usermessage" ||
                type == "task_started" || type == "taskstarted" ||
                type == "turn_started" || type == "turnstarted";

            if (startsRun) {
                // task_started can immediately follow user_message for the same
                // turn. Do not reset the timer or cumulative baseline twice.
                if (!run.valid || !run.running ||
                    type == "user_message" || type == "usermessage") {
                    startRun(at);
                }
                continue;
            }

            if (!run.valid || !run.running) {
                continue;
            }

            if (type == "task_complete" || type == "taskcomplete" ||
                type == "turn_completed" || type == "turncompleted" ||
                type == "turn_aborted" || type == "turnaborted") {
                run.running = false;
                run.thinking = false;
                continue;
            }

            if (type.find("agent_reasoning") != std::string::npos ||
                type.find("agentreasoning") != std::string::npos ||
                type == "reasoning") {
                run.thinking = true;
                if (run.thinkingStartedAtUnixSeconds <= 0) {
                    run.thinkingStartedAtUnixSeconds = at;
                }
            }
            else if (type.find("agent_message") != std::string::npos ||
                type.find("agentmessage") != std::string::npos ||
                type.find("tool_call") != std::string::npos ||
                type.find("toolcall") != std::string::npos ||
                type.find("exec_command") != std::string::npos ||
                type.find("execcommand") != std::string::npos) {
                run.thinking = false;
            }
        }

        return run;
    }

    static LocalTelemetry ReadLatestCodexTelemetry() {
        LocalTelemetry local;
        UsageTelemetry::ContextUsage& context = local.context;
        const std::filesystem::path session = LatestCodexSessionFile();

        if (session.empty()) {
            return local;
        }

        // Read only the most recent portion. Token-count records are emitted
        // throughout a session, so the latest record should be in this tail.
        const std::string text = ReadTailSharedReadOnly(session, 8ULL * 1024ULL * 1024ULL);

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

        bool foundUsage = false;
        bool compacting = false;
        long long lastRetainedTokens = 0;
        long long latestTotalTokens = 0;
        long long latestRecordAutoCompactTokenLimit = 0;
        long long preCompactionTokens = 0;
        bool waitingForPostCompactionUsage = false;

        for (const std::string& candidate : lines) {
            const bool mayContainState =
                candidate.find("compact") != std::string::npos ||
                candidate.find("token_count") != std::string::npos;

            if (!mayContainState) {
                continue;
            }

            json record = json::parse(candidate, nullptr, false);

            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const long long at = CodexRecordUnixSeconds(record);
            const bool wasCompacting = compacting;
            const bool stateChanged = CodexCompactionStateFromRecord(record, compacting);

            if (stateChanged && !wasCompacting && compacting) {
                context.compactionStartedAtUnixSeconds = at;
                preCompactionTokens = lastRetainedTokens;
                context.compactionPreTokens = preCompactionTokens;
            }
            else if (stateChanged && wasCompacting && !compacting) {
                context.compactionCompletedAtUnixSeconds = at;
                waitingForPostCompactionUsage = preCompactionTokens > 0;
            }

            const CodexTokenUsageRecord tokenUsage = ParseCodexTokenUsageRecord(record);
            if (!tokenUsage.valid) {
                continue;
            }

            const long long retainedTokens = std::max(
                0LL,
                tokenUsage.totalTokens - tokenUsage.reasoningOutputTokens
            );

            if (compacting) {
                // Codex persists a fresh token-count record once the compacted
                // context becomes active. This is a reliable passive completion
                // signal even when no explicit completion event is persisted.
                compacting = false;
                context.compactionCompletedAtUnixSeconds = at;
                waitingForPostCompactionUsage = preCompactionTokens > 0;
            }

            if (waitingForPostCompactionUsage && retainedTokens > 0) {
                context.compactionSavedTokens = std::max(
                    0LL,
                    preCompactionTokens - retainedTokens
                );
                waitingForPostCompactionUsage = false;
            }

            lastRetainedTokens = retainedTokens;
        }

        context.compacting = compacting;
        local.run = ReadCodexRunFromLines(lines);

        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            // Do not parse transcript/message records. Only the two metadata
            // event types needed for the token card are decoded.
            const bool mayContainUsage =
                it->find("token_count") != std::string::npos ||
                it->find("turn_context") != std::string::npos;

            if (!mayContainUsage) {
                continue;
            }

            json record = json::parse(*it, nullptr, false);

            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const json* payload = FindObjectField(record, { "payload" });

            if (!payload) {
                continue;
            }

            const std::string type = UsageTelemetry::LowerCopy(
                ReadStringFlexible(*payload, { "type" })
            );

            if (context.model.empty() && type == "turn_context") {
                context.model = ReadStringFlexible(*payload, { "model" });
            }

            if (!foundUsage && (type == "token_count" || type == "tokencount")) {
                const CodexTokenUsageRecord usage = ParseCodexTokenUsageRecord(record);
                if (!usage.valid) {
                    continue;
                }

                context.inputTokens = usage.inputTokens;
                context.cachedInputTokens = usage.cachedInputTokens;
                context.outputTokens = usage.outputTokens;
                context.reasoningOutputTokens = usage.reasoningOutputTokens;
                latestTotalTokens = usage.totalTokens;
                latestRecordAutoCompactTokenLimit = usage.modelAutoCompactTokenLimit;

                // Codex's own context accounting excludes reasoning output from
                // the tokens retained in the active model context. Keep the raw
                // output/reasoning breakdown visible, but use the retained count
                // for the context-window progress bar.
                context.usedTokens = std::max(
                    0LL,
                    usage.totalTokens - context.reasoningOutputTokens
                );
                context.contextWindowTokens = usage.modelContextWindow;

                if (context.usedTokens <= 0 && usage.totalTokens <= 0) {
                    context.usedTokens = std::max(
                        0LL,
                        context.inputTokens + context.outputTokens - context.reasoningOutputTokens
                    );
                }

                context.valid = context.usedTokens > 0 && context.contextWindowTokens > 0;

                context.sourceLabel = "Newest local Codex thread · direct read-only metadata";
                foundUsage = context.valid;
            }

            if (foundUsage && !context.model.empty()) {
                break;
            }
        }

        // New/changed Codex models are resolved from live config or the local
        // models_cache catalog instead of a hard-coded model-name table. This
        // also fills the brief gap before token_count persists its own window.
        if (context.usedTokens > 0 && context.contextWindowTokens <= 0) {
            context.contextWindowTokens = ResolveCodexContextWindowForModel(context.model);
        }
        context.valid = context.usedTokens > 0 && context.contextWindowTokens > 0;

        if (context.valid && g_modelAutoCompactTokenLimitScope.load() == 0) {
            // Priority matches Codex itself: per-event/effective metadata first,
            // then an explicit config override, then the active model's catalog
            // default. config/read commonly returns null for the limit because
            // the normal default belongs to ModelInfo, not Config.
            long long autoCompactThreshold = latestRecordAutoCompactTokenLimit;
            if (autoCompactThreshold <= 0) {
                autoCompactThreshold = g_modelAutoCompactTokenLimit.load();
            }
            if (autoCompactThreshold <= 0) {
                autoCompactThreshold = ResolveCodexDefaultAutoCompactTokenLimit(
                    context.model,
                    context.contextWindowTokens
                );
            }

            if (autoCompactThreshold > 0) {
                // The live modelContextWindow is already the effective usable
                // context (for example 272k * 95% = 258.4k). Codex's normal
                // auto-compact threshold can therefore legitimately be below
                // this displayed window.
                context.autoCompactThresholdTokens = autoCompactThreshold;
                const long long autoCompactUsedTokens = std::clamp(
                    latestTotalTokens > 0 ? latestTotalTokens : context.usedTokens,
                    0LL,
                    context.autoCompactThresholdTokens
                );
                const double percentLeft =
                    (static_cast<double>(context.autoCompactThresholdTokens - autoCompactUsedTokens) /
                        static_cast<double>(context.autoCompactThresholdTokens)) * 100.0;
                context.autoCompactPercentValid = true;
                context.autoCompactPercentLeft = static_cast<int>(std::clamp(
                    std::llround(percentLeft),
                    0LL,
                    100LL
                ));
            }
        }

        return local;
    }

    LocalTelemetry ReadLocalTelemetry() {
        return ReadLatestCodexTelemetry();
    }

    UsageTelemetry::ContextUsage ReadLocalContextUsage() {
        return ReadLatestCodexTelemetry().context;
    }

    static std::string ReadConfiguredModel(const json& configResult) {
        if (!configResult.is_object()) {
            return {};
        }
        const json* config = &configResult;
        if (configResult.contains("config") && configResult.at("config").is_object()) {
            config = &configResult.at("config");
        }
        return ReadStringFlexible(*config, { "model" });
    }

    static long long ReadModelContextWindow(const json& configResult) {
        if (!configResult.is_object()) {
            return 0;
        }
        const json* config = &configResult;
        if (configResult.contains("config") && configResult.at("config").is_object()) {
            config = &configResult.at("config");
        }
        const std::optional<double> value = ReadNumberFlexible(
            *config, { "model_context_window", "modelContextWindow" });
        if (!value || !std::isfinite(*value) || *value <= 0.0) {
            return 0;
        }
        return static_cast<long long>(std::floor(*value));
    }

    static long long ReadModelAutoCompactTokenLimit(const json& configResult) {
        if (!configResult.is_object()) {
            return 0;
        }

        const json* config = &configResult;
        if (configResult.contains("config") && configResult.at("config").is_object()) {
            config = &configResult.at("config");
        }

        const std::optional<double> value = ReadNumberFlexible(
            *config,
            { "model_auto_compact_token_limit", "modelAutoCompactTokenLimit" }
        );

        if (!value || !std::isfinite(*value) || *value <= 0.0) {
            return 0;
        }

        return static_cast<long long>(std::floor(*value));
    }

    static int ReadModelAutoCompactTokenLimitScope(const json& configResult) {
        // Codex defaults this setting to Total. Only BodyAfterPrefix needs
        // special handling because the ordinary total-token telemetry cannot
        // be used as the numerator for that scope.
        if (!configResult.is_object()) {
            return 0;
        }

        const json* config = &configResult;
        if (configResult.contains("config") && configResult.at("config").is_object()) {
            config = &configResult.at("config");
        }

        std::string scope = UsageTelemetry::LowerCopy(ReadStringFlexible(
            *config,
            { "model_auto_compact_token_limit_scope", "modelAutoCompactTokenLimitScope" }
        ));

        scope.erase(std::remove_if(scope.begin(), scope.end(), [](unsigned char ch) {
            return ch == '_' || ch == '-';
        }), scope.end());

        if (scope == "bodyafterprefix") {
            return 1;
        }

        return 0;
    }

    static void FinalizeCodexAccess(Snapshot& snapshot) {
        UsageTelemetry::SetAvailable(snapshot.access);

        const bool monthlySpendReached =
            snapshot.usageNotice.find("Monthly usage limit reached") != std::string::npos ||
            (snapshot.extraUsage.valid && snapshot.extraUsage.hasUsedPercent &&
                UsageTelemetry::IsExhausted(snapshot.extraUsage.usedPercent));

        if (monthlySpendReached) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = "Monthly usage/credits exhausted";
        }
        else {
            std::vector<std::string> exhaustedRateLimits;

            for (const UsageBar& bar : snapshot.bars) {
                const std::string resetCreditText = UsageTelemetry::LowerCopy(
                    bar.label + " " + bar.sublabel
                );
                const bool resetCreditOnly =
                    resetCreditText.find("reset credit") != std::string::npos;

                // Reset credits are supplemental recovery credits. Having zero
                // banked credits (or an exhausted reset-credit-only bucket) does
                // not mean the normal Codex plan itself is out of usage.
                if (bar.valid && bar.blocksProvider && !resetCreditOnly &&
                    UsageTelemetry::IsExhausted(bar.usedPercent)) {
                    exhaustedRateLimits.push_back(bar.label.empty() ? "Usage" : bar.label);
                }
            }

            const bool explicitlyExhausted =
                snapshot.usageNotice.find("Usage exhausted") != std::string::npos;

            if (explicitlyExhausted || !exhaustedRateLimits.empty()) {
                std::string detail = explicitlyExhausted
                    ? snapshot.usageNotice
                    : "Usage exhausted";

                if (!exhaustedRateLimits.empty()) {
                    if (explicitlyExhausted) {
                        detail += " · ";
                    }
                    else {
                        detail += ": ";
                    }

                    for (size_t i = 0; i < exhaustedRateLimits.size(); ++i) {
                        if (i != 0) detail += ", ";
                        detail += exhaustedRateLimits[i];
                    }
                }

                snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
                snapshot.access.detail = std::move(detail);
            }
        }

        // Credit and reset-credit balances are supplemental to the core plan
        // quota. Zero supplemental credits must not force OUT OF USAGE while a
        // normal blocking Codex quota window is still available.
        LocalTelemetry local = ReadLatestCodexTelemetry();
        snapshot.context = std::move(local.context);
        snapshot.run = std::move(local.run);
    }

    Snapshot FetchSnapshot(AccountSource source, const std::string& customAuthPath) {
        Snapshot snapshot;

        // Never carry an old explicit auto-compact override/scope into a new
        // refresh. If no override is returned, local telemetry resolves the
        // active model's normal catalog-derived threshold instead.
        g_modelAutoCompactTokenLimit.store(0);
        g_modelContextWindow.store(0);
        {
            std::lock_guard<std::mutex> lock(g_configModelMutex);
            g_configModel.clear();
        }
        g_modelAutoCompactTokenLimitScope.store(0);

        if (source == AccountSource::AuthFile) {
            snapshot = FetchSnapshotFromAuthFile(
                DefaultAuthPath(),
                false,
                "Codex auth.json selected"
            );
            FinalizeCodexAccess(snapshot);
            return snapshot;
        }

        if (source == AccountSource::CustomAuthFile) {
            if (customAuthPath.empty()) {
                throw std::runtime_error("Custom Codex auth.json is selected, but no path was configured");
            }

            snapshot = FetchSnapshotFromAuthFile(
                ExpandCustomAuthPath(customAuthPath),
                false,
                "Custom Codex auth.json selected"
            );
            FinalizeCodexAccess(snapshot);
            return snapshot;
        }

        CodexAppServer::Result current = CodexAppServer::ReadCurrentAccountRateLimits();

        if (current.kind == CodexAppServer::ResultKind::Success) {
            g_modelAutoCompactTokenLimit.store(
                ReadModelAutoCompactTokenLimit(current.configResult)
            );
            {
                std::lock_guard<std::mutex> lock(g_configModelMutex);
                g_configModel = ReadConfiguredModel(current.configResult);
            }
            g_modelContextWindow.store(
                ReadModelContextWindow(current.configResult)
            );
            g_modelAutoCompactTokenLimitScope.store(
                ReadModelAutoCompactTokenLimitScope(current.configResult)
            );
            snapshot = ParseAppServerSnapshot(current.accountResult, current.rateLimitsResult);
            FinalizeCodexAccess(snapshot);
            return snapshot;
        }

        if (current.kind == CodexAppServer::ResultKind::Error) {
            throw std::runtime_error("Codex app-server error: " + current.detail);
        }

        if (source == AccountSource::ActiveAccount) {
            std::string detail = current.detail.empty()
                ? "Codex app-server account APIs are unavailable"
                : current.detail;
            throw std::runtime_error("Active Codex account only is selected: " + detail);
        }

        // Auto mode uses auth.json only when app-server is genuinely unavailable.
        // Ambiguous keyring/auto/ephemeral configurations are still refused here.
        snapshot = FetchSnapshotFromAuthFile(
            DefaultAuthPath(),
            true,
            "Codex auto fallback auth.json"
        );
        FinalizeCodexAccess(snapshot);
        return snapshot;
    }

}
