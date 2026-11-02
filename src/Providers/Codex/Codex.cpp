#include "Global.hpp"

#include "Codex.hpp"
#include "CodexAppServer.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Math.hpp"
#include "Format.hpp"
#include "Text.hpp"

#include <cctype>

using json = nlohmann::json;

namespace Codex {

    // The config/read values the 1-second telemetry thread needs. They are
    // replaced as one unit once a refresh finishes, so the model and its window
    // never disagree and a slow app-server read never blanks them meanwhile.
    struct CodexConfig {
        std::string model;
        // Effective context window: a model-agnostic fallback until a fresh
        // thread emits its first token_count model_context_window.
        long long contextWindow = 0;
        // Explicit override. Zero is normal: Codex usually derives the
        // compaction edge from the active model's catalog entry instead.
        long long autoCompactTokenLimit = 0;
        // body_after_prefix needs a different numerator than total active
        // context, so the countdown is shown only for the default total scope.
        bool bodyAfterPrefixScope = false;
    };

    static std::mutex g_configMutex;
    static CodexConfig g_config;

    static CodexConfig CurrentConfig() {
        std::lock_guard<std::mutex> lock(g_configMutex);
        return g_config;
    }

    static std::filesystem::path CodexHomePath() {
        const std::filesystem::path configured = Network::get_instance()->EnvPath("CODEX_HOME");
        return configured.empty() ? Network::get_instance()->UserProfilePath() / ".codex" : configured;
    }

    static std::filesystem::path DefaultAuthPath() {
        return CodexHomePath() / "auth.json";
    }

    static std::filesystem::path ExpandCustomAuthPath(const std::string& configuredPath) {
        if (configuredPath.empty()) {
            return {};
        }

        std::wstring input = Text::get_instance()->Utf8ToWide(configuredPath);

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

            if (equals == std::string::npos ||
                Text::get_instance()->TrimCopy(line.substr(0, equals)) != "cli_auth_credentials_store") {
                continue;
            }

            std::string value = Text::get_instance()->TrimCopy(line.substr(equals + 1));

            if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }

            return Text::get_instance()->ToLowerCopy(value);
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
            throw std::runtime_error("Codex auth.json not found at " + Text::get_instance()->WideToUtf8(path.wstring()));
        }

        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Codex auth.json path is not a file: " + Text::get_instance()->WideToUtf8(path.wstring()));
        }

        // A parse error quotes what it last read, which could be part of a
        // token, so it is never passed on.
        json auth = JsonUtils::get_instance()->ParseOrNull(Network::get_instance()->ReadTextFile(path));

        if (!auth.is_object()) {
            throw std::runtime_error("Codex auth.json could not be read or is not valid JSON: " + Text::get_instance()->WideToUtf8(path.wstring()));
        }

        return auth;
    }

    // A positive count, floored; 0 when absent or not a positive finite number.
    static long long ReadPositiveCount(const json& object, std::initializer_list<const char*> keys) {
        const std::optional<double> value = JsonUtils::get_instance()->NumberAny(object, keys);
        return value && std::isfinite(*value) && *value > 0.0 ? static_cast<long long>(std::floor(*value)) : 0;
    }

    static const json* FindCreditsObject(const json& body) {
        if (!body.is_object()) {
            return nullptr;
        }

        if (const json* credits = JsonUtils::get_instance()->FindObject(body, { "credits" })) {
            return credits;
        }

        if (body.contains("balance") || body.contains("hasCredits") ||
            body.contains("has_credits") || body.contains("unlimited")) {
            return &body;
        }

        return nullptr;
    }

    // Spend controls count credits, or USD where the unit says so.
    static std::string FormatCodexAmount(double value, bool usd) {
        return usd ? Format::get_instance()->Dollars(value) : Format::get_instance()->CreditAmount(value);
    }

    static void ParseCodexCreditBalance(const json& body, Snapshot& snapshot) {
        const json* credits = FindCreditsObject(body);

        if (!credits) {
            return;
        }

        const std::optional<double> balance = JsonUtils::get_instance()->NumberOpt(*credits, "balance");
        const std::optional<bool> hasCredits = JsonUtils::get_instance()->BoolOpt(*credits, { "hasCredits", "has_credits" });
        const std::optional<bool> unlimited = JsonUtils::get_instance()->BoolOpt(*credits, { "unlimited" });

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
            parsed.balanceText = Format::get_instance()->CreditAmount(std::max(0.0, *balance)) + " credits";
        }
        else if (!parsed.hasCredits) {
            parsed.balanceText = "No credits";
        }
        else {
            parsed.balanceText = "Credits available";
        }

        // approx_*_messages: [low, high] estimates of what the balance buys,
        // null when the server has none.
        const auto range = [&](std::initializer_list<const char*> keys, const char* where) -> std::string {
            for (const char* key : keys) {
                const auto found = credits->find(key);

                if (found != credits->end() && found->is_array() && found->size() == 2 &&
                    (*found)[0].is_number() && (*found)[1].is_number()) {
                    const auto count = [](const json& value) {
                        return Format::get_instance()->IntegerWithCommas(value.get<double>());
                    };
                    return "~" + count((*found)[0]) + "\xe2\x80\x93" + count((*found)[1]) + " " + where;
                }
            }

            return {};
        };
        const std::string local = range({ "approx_local_messages", "approxLocalMessages" }, "local");
        const std::string cloud = range({ "approx_cloud_messages", "approxCloudMessages" }, "cloud");

        if (!local.empty() || !cloud.empty()) {
            parsed.approxMessagesText = (local.empty() || cloud.empty() ? local + cloud : local + " \xc2\xb7 " + cloud) + " messages";
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
            snapshot.usageNotice += " \xc2\xb7 " + notice;
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
                std::string type = JsonUtils::get_instance()->String(value, { "type", "kind", "reason" });

                if (!type.empty()) {
                    return type;
                }
            }
        }

        return {};
    }

    static const json* FindSpendControlObject(const json& body) {
        return JsonUtils::get_instance()->FindObject(body, { "spendControl", "spend_control" });
    }

    // Codex keeps working past an exhausted main limit by falling onto a second
    // pool published as an additional rate limit named "gpt-reserve". Without
    // this we report "usage exhausted" while the user is still being served.
    static bool CodexReserveActive(const json& body) {
        const json* additional = nullptr;

        for (const char* key : { "additional_rate_limits", "additionalRateLimits" }) {
            if (body.contains(key) && body.at(key).is_array()) {
                additional = &body.at(key);
                break;
            }
        }

        if (!additional) {
            return false;
        }

        for (const json& entry : *additional) {
            if (!entry.is_object()) continue;

            // Matched on the raw slug: the display transform would have already
            // turned this into "Gpt Reserve" by the time a label is built.
            const std::string name = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(entry, { "limitName", "limit_name" }));

            if (name != "gpt-reserve" && name != "gpt_reserve") continue;

            const json* limit = JsonUtils::get_instance()->FindObject(entry, { "rateLimit", "rate_limit" });
            const json& scope = limit ? *limit : entry;

            const std::optional<bool> allowed = JsonUtils::get_instance()->BoolOpt(scope, { "allowed" });
            const bool reached =
                JsonUtils::get_instance()->BoolOpt(scope, { "limitReached", "limit_reached" }).value_or(false);

            if (allowed.value_or(false) && !reached) {
                return true;
            }
        }

        return false;
    }

    // Adds the server's own exhaustion notices. Returns true when this limit
    // says it is reached (rate_limit_reached, limit_reached or allowed=false).
    static bool ParseCodexLimitStatus(const json& body, Snapshot& snapshot) {
        if (!body.is_object()) {
            return false;
        }

        const bool reserveActive = CodexReserveActive(body);
        const std::string reachedNotice = reserveActive
            ? "Main limit reached - running on Luna Reserve"
            : "Usage exhausted";

        const std::string reachedType = ReadStatusIdentifier(
            body,
            { "rateLimitReachedType", "rate_limit_reached_type" }
        );
        const std::string lowerType = Text::get_instance()->ToLowerCopy(reachedType);

        if (lowerType == "rate_limit_reached") {
            AppendUsageNotice(snapshot, reachedNotice);
        }
        else if (!lowerType.empty() && lowerType != "none" && lowerType != "null") {
            AppendUsageNotice(snapshot, "Usage exhausted: " + HumanizeIdentifier(reachedType));
        }

        const std::optional<bool> allowed = JsonUtils::get_instance()->BoolOpt(body, { "allowed" });
        const bool explicitRateLimitReached =
            JsonUtils::get_instance()->BoolOpt(body, { "limitReached", "limit_reached" }).value_or(false) ||
            (allowed.has_value() && !*allowed);

        if (explicitRateLimitReached && reachedType.empty()) {
            AppendUsageNotice(snapshot, reachedNotice);
        }

        if (const json* credits = FindCreditsObject(body);
            credits && JsonUtils::get_instance()->BoolOpt(*credits, { "overage_limit_reached", "overageLimitReached" }).value_or(false)) {
            AppendUsageNotice(snapshot, "Credit overage limit reached");
        }

        bool spendReached = JsonUtils::get_instance()->BoolOpt(
            body,
            { "spendControlReached", "spend_control_reached" }
        ).value_or(false);

        if (const json* spendControl = FindSpendControlObject(body)) {
            spendReached = spendReached || JsonUtils::get_instance()->BoolOpt(
                *spendControl,
                { "reached", "spendControlReached", "spend_control_reached" }
            ).value_or(false);
        }

        if (spendReached) {
            AppendUsageNotice(snapshot, "Monthly usage limit reached");
        }

        return lowerType == "rate_limit_reached" || explicitRateLimitReached;
    }

    // What wham/usage says Codex is serving from right now. A full plan window
    // is not "out of usage" while Luna Reserve or credits still cover requests:
    //   ambient_usage.default.source.kind = included | credits | reserve | unavailable
    // Older responses only carry rate_limit.allowed; mirror the app's fallback.
    static void ParseCodexUsageSource(const json& body, Snapshot& snapshot) {
        if (const json* ambient = JsonUtils::get_instance()->FindObject(body, { "ambient_usage", "ambientUsage" })) {
            if (const json* def = JsonUtils::get_instance()->FindObject(*ambient, { "default" })) {
                if (const json* source = JsonUtils::get_instance()->FindObject(*def, { "source" })) {
                    const std::string kind = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(*source, "kind"));

                    if (!kind.empty()) {
                        snapshot.usageSource = kind;
                        return;
                    }
                }
            }
        }

        const json* rateLimit = JsonUtils::get_instance()->FindObject(body, { "rate_limit", "rateLimit" });
        const std::optional<bool> allowed = rateLimit
            ? JsonUtils::get_instance()->BoolOpt(*rateLimit, { "allowed" })
            : std::nullopt;

        if (!allowed) {
            return;
        }

        const bool credits = snapshot.creditBalance.valid &&
            (snapshot.creditBalance.hasCredits || snapshot.creditBalance.unlimited);

        snapshot.usageSource = *allowed ? "included"
            : CodexReserveActive(body) ? "reserve"
            : credits ? "credits"
            : "unavailable";
    }

    // A reserve or credit source means the exhausted plan is not the end of the
    // line; the per-bucket status parser cannot see that, so restate it here.
    static void ApplyUsageSourceNotice(Snapshot& snapshot) {
        if (snapshot.usageSource != "reserve" && snapshot.usageSource != "credits") {
            return;
        }

        const std::string replacement = snapshot.usageSource == "reserve"
            ? "Main limit reached - running on Luna Reserve"
            : "Main limit reached - running on credits";
        const std::string separator = " \xc2\xb7 ";
        const std::string notices = std::move(snapshot.usageNotice);
        snapshot.usageNotice.clear();

        for (size_t start = 0; start <= notices.size();) {
            const size_t end = notices.find(separator, start);
            const std::string part = notices.substr(start, end == std::string::npos ? std::string::npos : end - start);
            AppendUsageNotice(snapshot, part.rfind("Usage exhausted", 0) == 0 ? replacement : part);

            if (end == std::string::npos) {
                break;
            }

            start = end + separator.size();
        }
    }

    // Unix seconds of the next reset; 0 when unknown.
    static long long ParseResetAtField(const json& object) {
        // A server-computed countdown beats an absolute timestamp: reset_at is
        // compared against the local clock, so a machine that is off by minutes
        // shows a reset that is wrong by the same amount. reset_after_seconds
        // carries no such assumption.
        const std::optional<double> after = JsonUtils::get_instance()->NumberAny(
            object,
            { "reset_after_seconds", "resetAfterSeconds", "resets_in_seconds", "resetsInSeconds" }
        );

        if (after && *after >= 0.0) {
            return static_cast<long long>(std::time(nullptr)) + static_cast<long long>(*after);
        }

        return JsonUtils::get_instance()->UnixSecondsField(object, { "reset_at", "resetAt", "resets_at", "resetsAt" });
    }

    static void ParseCodexIndividualLimit(const json& body, Snapshot& snapshot) {
        const json* spendControl = FindSpendControlObject(body);
        const json* limit = JsonUtils::get_instance()->FindObject(body, { "individualLimit", "individual_limit" });

        if (!limit && spendControl) {
            limit = JsonUtils::get_instance()->FindObject(*spendControl, { "individualLimit", "individual_limit" });
        }

        bool reached = JsonUtils::get_instance()->BoolOpt(
            body,
            { "spendControlReached", "spend_control_reached" }
        ).value_or(false);

        if (spendControl) {
            reached = reached || JsonUtils::get_instance()->BoolOpt(
                *spendControl,
                { "reached", "spendControlReached", "spend_control_reached" }
            ).value_or(false);
        }

        if (limit) {
            reached = reached || JsonUtils::get_instance()->BoolOpt(
                *limit,
                { "spendControlReached", "spend_control_reached", "reached" }
            ).value_or(false);
        }

        if (!limit && !reached) {
            return;
        }

        const json empty = json::object();
        const json& source = limit ? *limit : empty;
        const std::optional<double> maximum = JsonUtils::get_instance()->NumberOpt(source, "limit");
        const std::optional<double> used = JsonUtils::get_instance()->NumberOpt(source, "used");
        const std::optional<double> remainingPercent = JsonUtils::get_instance()->NumberAny(source, { "remainingPercent", "remaining_percent" });
        const std::optional<double> explicitUsedPercent = JsonUtils::get_instance()->NumberAny(source, { "usedPercent", "used_percent" });
        const std::optional<double> remainingAmount = JsonUtils::get_instance()->NumberOpt(source, "remaining");
        const bool usd = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(source, "unit")) == "usd";
        const std::string noun = usd ? "" : " credits";
        const auto amount = [&](double value) { return FormatCodexAmount(std::max(0.0, value), usd); };

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
            parsed.spentText = amount(*used) + " of " + amount(*maximum) + noun + " used";
        }
        else if (used) {
            parsed.spentText = amount(*used) + noun + " used";
        }
        else if (reached) {
            parsed.spentText = "Monthly usage limit reached";
        }

        if (maximum) {
            parsed.limitText = amount(*maximum) + (usd ? " limit" : " credit limit");
        }

        if (maximum && used) {
            parsed.remainingText = amount(*maximum - *used) + noun + " remaining";
        }
        else if (remainingAmount) {
            parsed.remainingText = amount(*remainingAmount) + noun + " remaining";
        }
        else if (remainingPercent) {
            parsed.remainingText = Format::get_instance()->Percent(
                Math::get_instance()->ClampPercentDouble(*remainingPercent)
            ) + " remaining";
        }
        else if (reached) {
            parsed.remainingText = "Limit reached";
        }

        parsed.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(
            source, { "resetsAt", "resets_at", "resetAt", "reset_at" });
        parsed.resetText = Format::get_instance()->ResetShort(parsed.resetAtUnixSeconds);

        snapshot.extraUsage = std::move(parsed);
    }

    static void ParseCodexMonthlyUsage(const json& body, Snapshot& snapshot) {
        if (!body.is_object() || snapshot.extraUsage.valid) {
            return;
        }

        const json* monthlyUsage = JsonUtils::get_instance()->FindObject(
            body,
            { "monthlyUsage", "monthly_usage" }
        );
        const json& source = monthlyUsage ? *monthlyUsage : body;
        const json* effectiveLimit = JsonUtils::get_instance()->FindObject(
            source,
            { "effectiveMonthlyLimit", "effective_monthly_limit" }
        );

        if (!effectiveLimit) {
            return;
        }

        // ?supports_usage_limit_modes=true adds limit_amount {unit, amount},
        // limit_mode and balance_unit (credit | usd). As in Desktop, the limit
        // counts in balance_unit: limit_amount when its unit matches, else the
        // bare limit, which is credits - so a usd workspace has none.
        std::string unit = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(source, { "balance_unit", "balanceUnit" }));

        if (unit.empty()) {
            unit = "credit";
        }

        const json* limitAmount = JsonUtils::get_instance()->FindObject(*effectiveLimit, { "limit_amount", "limitAmount" });
        const std::optional<double> maximum =
            limitAmount && Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(*limitAmount, "unit")) == unit
                ? JsonUtils::get_instance()->NumberOpt(*limitAmount, "amount")
                : unit == "usd" ? std::nullopt
                : JsonUtils::get_instance()->NumberOpt(*effectiveLimit, "limit");

        const std::optional<double> used = JsonUtils::get_instance()->NumberAny(
            source,
            { "currentMonthUsage", "current_month_usage" }
        );

        const bool usd = unit == "usd";
        const std::string noun = usd ? "" : " credits";
        const std::string mode = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(*effectiveLimit, { "limit_mode", "limitMode" }));

        // An unlimited mode has no cap to measure against: report spend only.
        if (mode == "unlimited_platform_max") {
            if (!used) {
                return;
            }

            ExtraUsage parsed;
            parsed.valid = true;
            parsed.spentText = FormatCodexAmount(std::max(0.0, *used), usd) + noun + " used";
            parsed.limitText = "No monthly limit";
            snapshot.extraUsage = std::move(parsed);
            return;
        }

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
            FormatCodexAmount(safeUsed, usd) + " of " + FormatCodexAmount(safeMaximum, usd) + noun + " used";
        parsed.limitText = FormatCodexAmount(safeMaximum, usd) + (usd ? " limit" : " credit limit");
        parsed.remainingText =
            FormatCodexAmount(std::max(0.0, safeMaximum - safeUsed), usd) + noun + " remaining";

        // Desktop blocks only on a HARD_CAP; any other mode just warns.
        const std::string enforcement = JsonUtils::get_instance()->String(*effectiveLimit, { "enforcement_mode", "enforcementMode" });
        parsed.enforced = enforcement.empty() || enforcement == "HARD_CAP";

        parsed.resetAtUnixSeconds = ParseResetAtField(source);

        if (parsed.resetAtUnixSeconds <= 0) {
            parsed.resetAtUnixSeconds = ParseResetAtField(*effectiveLimit);
        }

        parsed.resetText = Format::get_instance()->ResetShort(parsed.resetAtUnixSeconds);
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

        std::optional<double> used = JsonUtils::get_instance()->NumberAny(
            window,
            { "used_percent", "usedPercent" }
        );

        // The sidebar's monthly_limit may carry only used / limit.
        if (!used) {
            const std::optional<double> spent = JsonUtils::get_instance()->NumberOpt(window, "used");
            const std::optional<double> cap = JsonUtils::get_instance()->NumberOpt(window, "limit");

            if (spent && cap && *cap > 0.0) {
                used = *spent / *cap * 100.0;
            }
        }

        // A reset timestamp without utilization is not enough to draw a quota
        // bar. Missing utilization must remain hidden, not become 0% used.
        if (!used) {
            return bar;
        }

        bar.valid = true;
        bar.usedPercent = static_cast<float>(Math::get_instance()->ClampPercentDouble(*used));
        bar.resetAtUnixSeconds = ParseResetAtField(window);
        return bar;
    }

    static void AddUsageBarIfUnique(Snapshot& snapshot, UsageBar bar) {
        if (!bar.valid) {
            return;
        }

        // Different Codex response paths spell the same reserve name as
        // "GPT-reserve" and "GPT Reserve". Compare display text by its
        // alphanumeric form so punctuation/case differences cannot create a
        // second copy of the same weekly reserve.
        const auto comparable = [](const std::string& value) {
            return Text::get_instance()->CompactLower(value, false);
        };

        const std::string label = comparable(bar.label);
        const std::string sublabel = comparable(bar.sublabel);

        for (const UsageBar& existing : snapshot.bars) {
            if (comparable(existing.label) == label &&
                comparable(existing.sublabel) == sublabel &&
                existing.resetAtUnixSeconds == bar.resetAtUnixSeconds) {
                return;
            }
        }

        snapshot.bars.push_back(std::move(bar));
    }

    static const json* GetMainRateLimitObject(const json& usageJson) {
        return JsonUtils::get_instance()->FindObject(usageJson, { "rate_limit", "rateLimit" });
    }

    static double ReadWindowDurationMinutes(const json& window) {
        // window_minutes is the rollout's spelling.
        const std::optional<double> minutes = JsonUtils::get_instance()->NumberAny(
            window,
            { "windowDurationMins", "window_duration_mins", "window_minutes" }
        );

        if (minutes && *minutes > 0.0) {
            return *minutes;
        }

        const std::optional<double> seconds = JsonUtils::get_instance()->NumberAny(
            window,
            { "limitWindowSeconds", "limit_window_seconds" }
        );

        if (seconds && *seconds > 0.0) {
            return *seconds / 60.0;
        }

        return 0.0;
    }

    // The banked-reset count. applicable_* counts only the credits redeemable
    // against the limit being hit; available_count is the whole wallet and
    // over-reports what is usable right now.
    static int ParseResetCreditsAvailableCount(const json& source) {
        const json* rateLimit = GetMainRateLimitObject(source);

        for (const json* object : {
            &source,
            JsonUtils::get_instance()->FindObject(source, { "rate_limit_reset_credits", "rateLimitResetCredits" }),
            rateLimit ? JsonUtils::get_instance()->FindObject(*rateLimit, { "rate_limit_reset_credits", "rateLimitResetCredits" }) : nullptr
        }) {
            const std::optional<double> count = object
                ? JsonUtils::get_instance()->NumberAny(*object, {
                    "applicable_available_count", "applicableAvailableCount",
                    "available_count", "availableCount" })
                : std::nullopt;

            if (count && *count >= 0.0) {
                return static_cast<int>(*count);
            }
        }

        return -1;
    }

    // { credits: [...], available_count, total_earned_count, history_enabled }:
    // bare from /wham/rate-limit-reset-credits, nested in a usage response.
    static const json& ResetCreditList(const json& source) {
        const json* nested = JsonUtils::get_instance()->FindObject(source, { "rate_limit_reset_credits", "rateLimitResetCredits" });
        return nested ? *nested : source;
    }

    static ResetCredit ParseResetCreditLedgerEntry(const json& credit) {
        ResetCredit reset;
        // Codex Desktop's title for an untitled reset.
        reset.title = Text::get_instance()->TrimCopy(JsonUtils::get_instance()->String(credit, { "title", "name" }));

        if (reset.title.empty()) {
            reset.title = "Usage reset";
        }

        reset.status = JsonUtils::get_instance()->String(credit, "status");
        reset.resetType = JsonUtils::get_instance()->String(credit, { "reset_type", "resetType" });
        reset.grantedAt = std::chrono::system_clock::from_time_t(
            JsonUtils::get_instance()->UnixSecondsField(credit, { "granted_at", "grantedAt" }));
        reset.expiresAt = std::chrono::system_clock::from_time_t(
            JsonUtils::get_instance()->UnixSecondsField(credit, { "expires_at", "expiresAt" }));
        return reset;
    }

    static void ParseResetCredits(const json& source, Snapshot& snapshot) {
        if (const int count = ParseResetCreditsAvailableCount(source); count >= 0) {
            snapshot.resetCreditsAvailableCount = count;
        }

        const json& list = ResetCreditList(source);

        if (const std::optional<double> earned = JsonUtils::get_instance()->NumberAny(list, { "total_earned_count", "totalEarnedCount" })) {
            snapshot.resetCreditsEarnedCount = static_cast<int>(*earned);
        }

        snapshot.resetCredits.clear();
        snapshot.resetCreditLedger.clear();

        if (!list.is_object() || !list.contains("credits") || !list.at("credits").is_array()) {
            return;
        }

        for (const json& credit : list.at("credits")) {
            if (!credit.is_object()) {
                continue;
            }

            // Every ledger item is kept: an expiry can legitimately be missing
            // while the title, status and count still exist.
            const ResetCredit reset = ParseResetCreditLedgerEntry(credit);
            snapshot.resetCreditLedger.push_back(reset);

            if (Text::get_instance()->ToLowerCopy(reset.status) == "available") {
                snapshot.resetCredits.push_back(reset);
            }
        }

        // Soonest expiry first; unknown expiries last.
        const auto byExpiry = [](const ResetCredit& a, const ResetCredit& b) {
            const bool aKnown = a.expiresAt.time_since_epoch().count() != 0;
            const bool bKnown = b.expiresAt.time_since_epoch().count() != 0;
            return aKnown != bKnown ? aKnown : a.expiresAt < b.expiresAt;
        };

        std::sort(snapshot.resetCredits.begin(), snapshot.resetCredits.end(), byExpiry);
        std::sort(snapshot.resetCreditLedger.begin(), snapshot.resetCreditLedger.end(), byExpiry);
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

        const std::string lower = Text::get_instance()->ToLowerCopy(raw);
        const auto startsWith = [&](const char* prefix) { return lower.rfind(prefix, 0) == 0; };

        // Codex Desktop 26.1027 labels. The Pro numbers are fixed SKU names, and
        // "business" is Enterprise CBP - the self-serve Business plan is "team".
        if (lower == "prolite") return "Codex Pro 100";
        if (lower == "pro") return "Codex Pro 200";
        if (lower == "promax") return "Codex Pro 500";
        if (lower == "free_workspace") return "Codex Business Free";
        if (lower == "team" || startsWith("self_serve_business")) return "Codex Business";
        if (lower == "business" || lower == "enterprise" || lower == "deprecated_enterprise" || lower == "ent26" ||
            startsWith("enterprise_cbp") || startsWith("hc") || lower == "finserv" || lower == "law") {
            return "Codex Enterprise";
        }
        if (lower == "education" || lower == "deprecated_edu" || startsWith("edu")) return "Codex EDU";
        if (lower == "k12") return "Codex for Teachers";
        if (lower == "quorum") return "Codex for Clinicians";
        if (startsWith("sci") || lower == "rosalind_academic") return "Codex Academic Researchers";

        return "Codex " + HumanizeIdentifier(lower);
    }

    static std::string FormatCodexPlan(const json& usageJson) {
        for (const char* key : { "plan_type", "planType", "plan", "account_plan", "accountPlan" }) {
            const std::string raw = ExtractStringRecursive(usageJson, key);

            if (!raw.empty()) {
                return FormatCodexPlanToken(raw);
            }
        }

        return FormatCodexPlanToken({});
    }

    static std::string ExtractAppServerPlanType(
        const json& accountResult,
        const json& rateLimit
    ) {
        // The live rate-limit plan wins: Codex Desktop treats account/read's
        // planType as planAtLogin, which goes stale after an upgrade.
        const std::string livePlan = JsonUtils::get_instance()->String(rateLimit, { "planType", "plan_type", "plan" });
        const json* account = JsonUtils::get_instance()->FindObject(accountResult, { "account" });

        return !livePlan.empty() || !account
            ? livePlan
            : JsonUtils::get_instance()->String(*account, { "planType", "plan_type", "plan" });
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

            const std::string lower = Text::get_instance()->ToLowerCopy(output.substr(start, end - start));

            for (const char* acronym : kAcronyms) {
                if (lower != acronym) continue;

                for (size_t i = start; i < end; ++i) {
                    output[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(output[i])));
                }
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

        std::string out;

        if (Text::get_instance()->ToLowerCopy(parts[0]) == "gpt") {
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

    // Codex Desktop shows the "gpt-reserve" pool (limit id base_model_inference)
    // as "Luna Reserve".
    static bool IsCodexReserveName(const std::string& name) {
        const std::string compact = Text::get_instance()->CompactLower(name, false);
        return compact == "gptreserve" || compact == "basemodelinference" || compact == "lunareserve";
    }

    static std::string AppServerRateLimitName(const json& rateLimit, const std::string& fallbackKey) {
        // limit_name arrives already display-cased from the server; running it
        // through HumanizeIdentifier was destroying it (hyphens to spaces, then
        // "5.3" split into "5 3"). Only the id/key fallbacks are real slugs.
        const std::string display = JsonUtils::get_instance()->String(rateLimit, { "limitName", "limit_name" });
        std::string slug = JsonUtils::get_instance()->String(rateLimit, { "limitId", "limit_id" });

        if (IsCodexReserveName(display) || (display.empty() && IsCodexReserveName(slug.empty() ? fallbackKey : slug))) {
            return "Luna Reserve";
        }

        if (!display.empty()) {
            return FormatCodexLimitName(display);
        }

        if (slug.empty()) {
            slug = fallbackKey;
        }

        const std::string name = HumanizeIdentifier(slug);
        return name.empty() ? "Codex" : name;
    }

    static bool IsCodexMainLimit(const json& rateLimit, const std::string& key) {
        const std::string id = JsonUtils::get_instance()->String(rateLimit, { "limitId", "limit_id" });
        const std::string candidate = Text::get_instance()->ToLowerCopy(id.empty() ? key : id);
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

        const json* canonical = JsonUtils::get_instance()->FindObject(result, { "rateLimits", "rate_limits" });

        if (canonical) {
            canonicalId = JsonUtils::get_instance()->String(*canonical, { "limitId", "limit_id" });

            AppServerRateLimitEntry entry;
            entry.key = canonicalId.empty() ? "codex" : canonicalId;
            entry.value = canonical;
            entry.main = true;
            entries.push_back(std::move(entry));
        }

        const json* byLimitId = JsonUtils::get_instance()->FindObject(
            result,
            { "rateLimitsByLimitId", "rate_limits_by_limit_id" }
        );

        if (byLimitId) {
            for (auto it = byLimitId->begin(); it != byLimitId->end(); ++it) {
                if (!it.value().is_object()) {
                    continue;
                }

                const std::string valueId = JsonUtils::get_instance()->String(it.value(), { "limitId", "limit_id" });
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
            return std::to_string(rounded / (24 * 60)) + "-day window";
        }

        if (rounded % 60 == 0) {
            return std::to_string(rounded / 60) + "-hour window";
        }

        return std::to_string(rounded) + "-minute window";
    }

    static bool DurationApproximately(double minutes, double targetMinutes) {
        return minutes > 0.0 &&
            std::abs(minutes - targetMinutes) <= targetMinutes * 0.05;
    }

    static std::string WindowTypeLabel(double durationMinutes, int sourceOrder) {
        if (DurationApproximately(durationMinutes, 5.0 * 60.0)) return "Session";
        if (DurationApproximately(durationMinutes, 7.0 * 24.0 * 60.0)) return "Weekly";
        if (DurationApproximately(durationMinutes, 24.0 * 60.0)) return "Daily";
        if (DurationApproximately(durationMinutes, 30.0 * 24.0 * 60.0)) return "Monthly";
        if (durationMinutes > 0.0) return "Usage";

        return sourceOrder == 0 ? "Session" : "Weekly";
    }

    // A limit's primary and secondary windows, shortest first, each named by
    // its length. App-server spells them primary / secondary, wham
    // primary_window / secondary_window.
    static void ParseRateLimitWindows(
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

        const auto addWindow = [&](std::initializer_list<const char*> keys, int sourceOrder) {
            const json* window = JsonUtils::get_instance()->FindObject(rateLimit, keys);

            // Explicit null/missing utilization means the window does not
            // exist for this account. It must stay hidden.
            if (window && JsonUtils::get_instance()->NumberAny(*window, { "used_percent", "usedPercent" })) {
                windows.push_back({ window, ReadWindowDurationMinutes(*window), sourceOrder });
            }
        };

        addWindow({ "primary_window", "primary", "primaryWindow" }, 0);
        addWindow({ "secondary_window", "secondary", "secondaryWindow" }, 1);

        std::stable_sort(windows.begin(), windows.end(), [](const Candidate& a, const Candidate& b) {
            if (a.durationMinutes > 0.0 && b.durationMinutes > 0.0 &&
                a.durationMinutes != b.durationMinutes) {
                return a.durationMinutes < b.durationMinutes;
            }
            return a.sourceOrder < b.sourceOrder;
        });

        for (const Candidate& candidate : windows) {
            const std::string windowName = WindowTypeLabel(candidate.durationMinutes, candidate.sourceOrder);

            AddUsageBarIfUnique(snapshot, ParseCodexWindow(
                *candidate.window,
                isMain ? windowName : rateLimitName + " \xc2\xb7 " + windowName,
                FormatWindowDuration(candidate.durationMinutes),
                isMain
            ));
        }
    }

    // A server notice ("title: description") with Codex Desktop's "{time}"
    // placeholder filled from its reset_at.
    static std::string CodexWarningText(const json& warning) {
        std::string text = JsonUtils::get_instance()->String(warning, "title");
        const std::string description = JsonUtils::get_instance()->String(warning, "description");

        if (!description.empty()) {
            text += text.empty() ? description : ": " + description;
        }

        const long long resetAt = JsonUtils::get_instance()->UnixSecondsField(warning, { "reset_at" });

        for (size_t at; resetAt > 0 && (at = text.find("{time}")) != std::string::npos;) {
            text.replace(at, 6, Format::get_instance()->ExpiryTime(std::chrono::system_clock::from_time_t(resetAt)));
        }

        return text;
    }

    // A fully-formed monthly quota the server already computed - limit, used,
    // percent and countdown - rather than one we derive. Also carries the
    // server's own warning copy, per model.
    static void ParseCodexSidebarWarnings(const json& body, Snapshot& snapshot) {
        const json* warnings = JsonUtils::get_instance()->FindObject(
            body, { "sidebar_usage_warnings", "sidebarUsageWarnings" });

        if (!warnings) {
            return;
        }

        // default, plus by_model { <model slug>: same shape } for models with
        // their own monthly cap.
        const auto addMonthly = [&snapshot](const json& warning, const std::string& label) {
            if (const json* monthly = JsonUtils::get_instance()->FindObject(warning, { "monthly_limit", "monthlyLimit" })) {
                AddUsageBarIfUnique(snapshot, ParseCodexWindow(*monthly, label, std::string{}, false));
            }
        };

        if (const json* def = JsonUtils::get_instance()->FindObject(*warnings, { "default" })) {
            addMonthly(*def, "Monthly");
            AppendUsageNotice(snapshot, CodexWarningText(*def));
        }

        if (const json* byModel = JsonUtils::get_instance()->FindObject(*warnings, { "by_model", "byModel" })) {
            for (auto it = byModel->begin(); it != byModel->end(); ++it) {
                if (it.value().is_object()) {
                    addMonthly(it.value(), "Monthly \xc2\xb7 " + it.key());
                }
            }
        }
    }

    static Snapshot ParseAppServerSnapshot(
        const json& accountResult,
        const json& result
    ) {
        Snapshot snapshot;
        snapshot.statusText = "Source: Codex app-server active account";
        snapshot.lastUpdated = "just now";

        const json* mainRateLimit = nullptr;
        bool mainReached = false;

        for (const AppServerRateLimitEntry& entry : CollectAppServerRateLimits(result)) {
            if (!entry.value) {
                continue;
            }

            ParseRateLimitWindows(*entry.value, AppServerRateLimitName(*entry.value, entry.key), entry.main, snapshot);

            // Only the main limit blocks Codex. A spent model-specific or
            // reserve bucket is just that bar, never OUT OF USAGE.
            if (entry.main) {
                mainRateLimit = mainRateLimit ? mainRateLimit : entry.value;
                mainReached = ParseCodexLimitStatus(*entry.value, snapshot) || mainReached;
            }

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

        snapshot.plan = FormatCodexPlanToken(ExtractAppServerPlanType(
            accountResult, mainRateLimit ? *mainRateLimit : json::object()));
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

        ParseCodexSidebarWarnings(result, snapshot);

        // ordinaryUsageAllowed is optional, and Codex Desktop 26.1027 never
        // reads it. Without it, an exhausted or reached main limit is what says
        // the plan ran out; Luna Reserve or credits can still keep Codex going.
        const bool mainExhausted = mainReached ||
            std::any_of(snapshot.bars.begin(), snapshot.bars.end(), [](const UsageBar& bar) {
                return bar.valid && bar.quotaNotificationEligible && UsageTelemetry::IsExhausted(bar.usedPercent);
            });
        const bool reserveLeft = std::any_of(snapshot.bars.begin(), snapshot.bars.end(), [](const UsageBar& bar) {
            return bar.valid && bar.label.rfind("Luna Reserve", 0) == 0 &&
                !UsageTelemetry::IsExhausted(bar.usedPercent);
        });
        const bool credits = snapshot.creditBalance.valid &&
            (snapshot.creditBalance.hasCredits || snapshot.creditBalance.unlimited);
        const bool ordinary = JsonUtils::get_instance()->BoolOpt(
            result, { "ordinaryUsageAllowed", "ordinary_usage_allowed" }).value_or(!mainExhausted);

        snapshot.usageSource = ordinary ? "included"
            : reserveLeft ? "reserve"
            : credits ? "credits"
            : "unavailable";
        ApplyUsageSourceNotice(snapshot);

        ParseResetCredits(result, snapshot);
        return snapshot;
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

        const json* rateLimit = JsonUtils::get_instance()->FindObject(entry, { "rate_limit", "rateLimit" });
        const json* body = rateLimit ? rateLimit : &entry;

        // Named exactly as the app-server path names it, so a bar keeps its
        // label and notifier key when the account source changes.
        const std::string limitName = Text::get_instance()->TrimCopy(
            JsonUtils::get_instance()->String(entry, { "limit_name", "limitName", "rate_limit_name", "rateLimitName" }));
        std::string name = limitName.empty() ? AppServerRateLimitName(*body, fallbackName)
            : IsCodexReserveName(limitName) ? "Luna Reserve"
            : FormatCodexLimitName(limitName);

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

        // No limit status here: only the main limit's exhaustion blocks Codex.
        ParseRateLimitWindows(*body, name, isMain, snapshot);

        for (const json* source : { &entry, body }) {
            if (!snapshot.creditBalance.valid) {
                ParseCodexCreditBalance(*source, snapshot);
            }

            if (!snapshot.extraUsage.valid) {
                ParseCodexIndividualLimit(*source, snapshot);
            }
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
        if (const json* byLimitId = JsonUtils::get_instance()->FindObject(usageJson, { "rate_limits_by_limit_id", "rateLimitsByLimitId" })) {
            for (auto it = byLimitId->begin(); it != byLimitId->end(); ++it) {
                ParseLegacyAdditionalRateLimitEntry(it.value(), it.key(), usageJson, snapshot);
            }
        }
    }

    static Snapshot ParseUsageAndResets(const json& usageJson, const json& resetJson) {
        Snapshot snapshot;
        snapshot.plan = FormatCodexPlan(usageJson);
        snapshot.lastUpdated = "just now";
        snapshot.resetCreditsAvailableCount = ParseResetCreditsAvailableCount(usageJson);

        ParseCodexCreditBalance(usageJson, snapshot);
        ParseCodexIndividualLimit(usageJson, snapshot);
        ParseCodexMonthlyUsage(usageJson, snapshot);
        ParseCodexSidebarWarnings(usageJson, snapshot);
        ParseCodexLimitStatus(usageJson, snapshot);

        if (const json* promo = JsonUtils::get_instance()->FindObject(usageJson, { "promo" })) {
            snapshot.promoCampaign = JsonUtils::get_instance()->String(*promo, "campaign_id");
        }

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

            ParseRateLimitWindows(*rateLimit, "Codex", true, snapshot);
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

        // Code review has its own pool; it never blocks ordinary Codex work.
        if (const json* codeReview = JsonUtils::get_instance()->FindObject(usageJson, { "code_review_rate_limit", "codeReviewRateLimit" })) {
            ParseRateLimitWindows(*codeReview, "Code review", false, snapshot);
        }

        // The server's own copy: limit warnings, and the upsell that names when
        // usage comes back ("...try again at {time}").
        for (const char* key : { "rate_limit_warning", "rate_limit_upsell" }) {
            if (const json* notice = JsonUtils::get_instance()->FindObject(usageJson, { key })) {
                AppendUsageNotice(snapshot, CodexWarningText(*notice));
            }
        }

        ParseCodexUsageSource(usageJson, snapshot);
        ApplyUsageSourceNotice(snapshot);
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

    static std::string CodexRefreshErrorCode(const std::string& body) {
        // { error: "code" }, { error: { code } } or { code }.
        const json parsed = JsonUtils::get_instance()->ParseOrNull(body);
        const json* error = JsonUtils::get_instance()->FindObject(parsed, { "error" });
        const std::string nested = error ? JsonUtils::get_instance()->String(*error, { "code", "error" }) : std::string{};
        return nested.empty() ? JsonUtils::get_instance()->String(parsed, { "error", "code" }) : nested;
    }

    // Replaces auth.json without ever leaving it truncated or half-written:
    // the CLI reads it at any moment, and a broken file logs it out.
    static bool WriteAuthJsonAtomically(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::path temporary = path;
        temporary += ".tmp";

        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();

        if (file && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }

        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    static std::string RefreshTokenOf(const json& auth) {
        const json* tokens = JsonUtils::get_instance()->FindObject(auth, { "tokens" });
        return tokens ? JsonUtils::get_instance()->String(*tokens, { "refresh_token", "refreshToken" }) : std::string{};
    }

    // Re-read just before refreshing: if the CLI rotated the token since we
    // read the file, its copy is current, and replaying ours would spend a
    // token the CLI no longer holds. True when that brought a new access
    // token to try.
    static bool AdoptRotatedTokens(const std::filesystem::path& authPath, json& auth, std::string& accessToken) {
        try {
            json current = ReadAuthJson(authPath, false);
            const std::string currentRefresh = RefreshTokenOf(current);

            if (currentRefresh.empty() || currentRefresh == RefreshTokenOf(auth)) {
                return false;
            }

            auth = std::move(current);
            const std::string currentAccess = JsonUtils::get_instance()->String(auth.at("tokens"), { "access_token", "accessToken" });

            if (currentAccess.empty() || currentAccess == accessToken) {
                return false;
            }

            accessToken = currentAccess;
            return true;
        }
        catch (const std::exception&) {
            // Unreadable right now: refresh from the copy we already hold.
            return false;
        }
    }

    // Rotates the Codex access token in place. Returns false when no new one
    // can be had; `detail` then explains why.
    static bool RefreshCodexTokens(
        const std::filesystem::path& authPath,
        json& auth,
        std::string& accessToken,
        std::string& detail
    ) {
        if (AdoptRotatedTokens(authPath, auth, accessToken)) {
            detail.clear();
            return true;
        }

        const std::string refreshToken = RefreshTokenOf(auth);

        if (refreshToken.empty()) {
            detail = "Codex auth.json has no refresh_token; run `codex login` again";
            return false;
        }

        const std::string body =
            "grant_type=refresh_token"
            "&client_id=" + Text::get_instance()->UrlEncode(kCodexOAuthClientId) +
            "&refresh_token=" + Text::get_instance()->UrlEncode(refreshToken);

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

        const json decoded = JsonUtils::get_instance()->ParseOrNull(response.body);
        const std::string refreshed = JsonUtils::get_instance()->String(decoded, "access_token");

        if (refreshed.empty()) {
            detail = "Codex token refresh returned no access_token; run `codex login` again";
            return false;
        }

        accessToken = refreshed;
        auth["tokens"]["access_token"] = refreshed;

        for (const char* key : { "refresh_token", "id_token" }) {
            const std::string value = JsonUtils::get_instance()->String(decoded, key);

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

        // The old refresh token is spent now. If the new one cannot be saved,
        // the next refresh - ours or the CLI's - is rejected, so say so now.
        if (!WriteAuthJsonAtomically(authPath, auth.dump(2))) {
            detail = "the Codex token was refreshed but auth.json could not be saved; run `codex login` again";
            return false;
        }

        detail.clear();
        return true;
    }

    // Where backend-api calls go and what they carry, built the way Codex
    // Desktop builds them: CODEX_API_BASE_URL or the workspace's
    // data-residency origin (account/read workspaceRouting) with its routing
    // header, the account id from the token's own claims, and
    // X-OpenAI-Fedramp for FedRAMP accounts.
    struct CodexBackend {
        std::string base;
        std::string accountId;
        std::string planType; // the token's chatgpt_plan_type
        std::wstring headers;
    };

    static CodexBackend MakeCodexBackend(const std::string& token, std::string accountId, const json& accountResult) {
        CodexBackend backend;
        backend.base = Text::get_instance()->TrimCopy(Network::get_instance()->GetEnvText("CODEX_API_BASE_URL"));

        while (!backend.base.empty() && backend.base.back() == '/') {
            backend.base.pop_back();
        }

        // The bearer token goes wherever this points.
        if (!backend.base.empty() && Text::get_instance()->ToLowerCopy(backend.base.substr(0, 8)) != "https://") {
            throw std::runtime_error("CODEX_API_BASE_URL must be an https:// URL");
        }

        if (backend.base.empty()) {
            backend.base = "https://chatgpt.com/backend-api";
        }

        std::string routingOverride;

        if (const json* routing = JsonUtils::get_instance()->FindObject(accountResult, { "workspaceRouting", "workspace_routing" })) {
            // A bare https origin; the backend-api path is kept, as Desktop does.
            const std::string origin = JsonUtils::get_instance()->String(*routing, { "backendOrigin", "backend_origin" });

            if (origin.rfind("https://", 0) == 0 && origin.find('/', 8) == std::string::npos) {
                backend.base = origin + "/backend-api";
            }

            routingOverride = JsonUtils::get_instance()->String(*routing, { "accountRoutingOverride", "account_routing_override" });

            if (accountId.empty()) {
                accountId = JsonUtils::get_instance()->String(*routing, { "chatgptAccountId", "chatgpt_account_id" });
            }
        }

        const json claims = JsonUtils::get_instance()->JwtClaims(token);
        const json* auth = JsonUtils::get_instance()->FindObject(claims, { "https://api.openai.com/auth" });

        if (auth) {
            const std::string claimed = JsonUtils::get_instance()->String(*auth, { "chatgpt_account_id", "account_id" });

            if (!claimed.empty()) {
                accountId = claimed;
            }

            backend.planType = JsonUtils::get_instance()->String(*auth, "chatgpt_plan_type");
        }

        backend.accountId = Network::get_instance()->StripHeaderValue(accountId);

        std::string headers =
            "Authorization: Bearer " + Network::get_instance()->StripHeaderValue(token) + "\r\n" +
            (backend.accountId.empty() ? std::string{} : "ChatGPT-Account-ID: " + backend.accountId + "\r\n") +
            "Accept: application/json\r\n"
            "Origin: https://chatgpt.com\r\n"
            "Referer: https://chatgpt.com/codex\r\n"
            "originator: Codex Desktop\r\n"
            "User-Agent: Codex Desktop/26.1027.30824 (Windows NT 10.0; x64)\r\n"
            "OAI-App-Brand: chatgpt\r\n"
            "x-openai-codex-pricing-chooser: 1\r\n";

        if (!routingOverride.empty() && routingOverride != "NO_CONSTRAINT") {
            headers += "X-OpenAI-Account-Routing-Override: " +
                Network::get_instance()->StripHeaderValue(routingOverride) + "\r\n";
        }

        if (auth && JsonUtils::get_instance()->BoolOpt(*auth, { "chatgpt_account_is_fedramp" }).value_or(false)) {
            headers += "X-OpenAI-Fedramp: true\r\n";
        }

        backend.headers = Text::get_instance()->Utf8ToWide(headers);
        return backend;
    }

    // Optional backend-api reads. They never throw and never fail the
    // snapshot. `settled` turns false when an answer may change on a retry
    // (offline, 401, 429, 5xx), so callers cache only settled results; 403
    // and 404 settle too, meaning "not for this account". After one transport
    // failure the rest are skipped, so an outage costs one timeout, not ten.
    struct CodexBackendReads {
        const CodexBackend& backend;
        bool settled = true;
        bool offline = false;

        json Get(const std::string& path, const std::wstring& extraHeaders = {}) {
            if (offline) {
                settled = false;
                return json();
            }

            const Network::HttpResponse response = Network::get_instance()->TryRequestUrl(
                backend.base + path, "GET", backend.headers + extraHeaders);
            const bool ok = response.statusCode >= 200 && response.statusCode < 300;

            offline = response.statusCode == 0;
            settled = settled && (ok || response.statusCode == 403 || response.statusCode == 404);
            return ok ? JsonUtils::get_instance()->ParseOrNull(response.body) : json();
        }
    };

    // Codex Desktop's names for usage surfaces (settings.usage.surface.*).
    static std::string SurfaceLabel(const std::string& surface) {
        static const std::pair<const char*, const char*> kLabels[] = {
            { "desktop_app", "Desktop App" }, { "work_desktop", "Desktop (Work)" }, { "cli", "CLI" },
            { "vscode", "Extension" }, { "jetbrains", "JetBrains" }, { "web", "Cloud" },
            { "work_web", "Web" }, { "mobile", "Mobile" }, { "work_mobile", "Mobile" },
            { "slack", "Slack" }, { "linear", "Linear" }, { "sdk", "SDK" }, { "exec", "Exec" },
            { "github", "GitHub" }, { "github_code_review", "GitHub code review" },
            { "agent_identity", "Access tokens" }, { "codex_security_cloud", "Codex Security Cloud" },
            { "unknown", "Uncategorized" }, { "other", "Other" },
        };

        for (const auto& [key, label] : kLabels) {
            if (surface == key) {
                return label;
            }
        }

        return HumanizeIdentifier(surface);
    }

    // GET /wham/profiles/me: { profile: { display_name, username },
    // stats: { lifetime_tokens, peak_daily_tokens, longest_running_turn_sec,
    // current_streak_days, longest_streak_days, total_threads, ...,
    // daily_usage_buckets / weekly_usage_buckets: [{ start_date, tokens }],
    // top_invocations: [{ plugin_name | skill_name, usage_count }] },
    // metadata: { stats_as_of, stats_error } }.
    static ProfileStats ParseCodexProfileStats(const json& body) {
        ProfileStats profile;
        const json* stats = JsonUtils::get_instance()->FindObject(body, { "stats" });

        if (!stats) {
            return profile;
        }

        const auto count = [](const json& object, const char* key) {
            const std::optional<double> value = JsonUtils::get_instance()->NumberOpt(object, key);
            return value && *value >= 0.0 ? static_cast<long long>(std::llround(*value)) : -1LL;
        };
        const auto percent = [&](const char* key) {
            return JsonUtils::get_instance()->NumberOpt(*stats, key).value_or(-1.0);
        };

        if (const json* user = JsonUtils::get_instance()->FindObject(body, { "profile" })) {
            profile.displayName = Text::get_instance()->TrimCopy(JsonUtils::get_instance()->String(*user, "display_name"));
            profile.username = Text::get_instance()->TrimCopy(JsonUtils::get_instance()->String(*user, "username"));
        }

        profile.lifetimeTokens = count(*stats, "lifetime_tokens");
        profile.peakDailyTokens = count(*stats, "peak_daily_tokens");
        profile.longestTaskSeconds = count(*stats, "longest_running_turn_sec");
        profile.currentStreakDays = static_cast<int>(count(*stats, "current_streak_days"));
        profile.longestStreakDays = static_cast<int>(count(*stats, "longest_streak_days"));
        profile.totalThreads = count(*stats, "total_threads");
        profile.reasoningEffort = JsonUtils::get_instance()->String(*stats, "most_used_reasoning_effort");
        profile.reasoningEffortPercent = percent("most_used_reasoning_effort_percentage");
        profile.fastModePercent = percent("fast_mode_usage_percentage");
        profile.skillsUsed = count(*stats, "total_skills_used");
        profile.uniqueSkills = count(*stats, "unique_skills_used");
        profile.workspaceRank = static_cast<int>(count(*stats, "workspace_rank"));
        profile.workspaceUsers = static_cast<int>(count(*stats, "workspace_total_user_count"));

        if (const json* buckets = JsonUtils::get_instance()->FindArray(*stats, { "daily_usage_buckets" })) {
            const std::string today = Format::get_instance()->LocalDateKey(std::time(nullptr));

            for (const json& bucket : *buckets) {
                if (JsonUtils::get_instance()->String(bucket, "start_date") == today) {
                    profile.todayTokens = count(bucket, "tokens");
                }
            }
        }

        if (const json* buckets = JsonUtils::get_instance()->FindArray(*stats, { "weekly_usage_buckets" })) {
            for (const json& bucket : *buckets) {
                const std::string start = JsonUtils::get_instance()->String(bucket, "start_date");

                if (!start.empty()) {
                    profile.weeklyTokens.emplace_back(start, std::max(0LL, count(bucket, "tokens")));
                }
            }

            std::stable_sort(profile.weeklyTokens.begin(), profile.weeklyTokens.end());
        }

        if (const json* invocations = JsonUtils::get_instance()->FindArray(*stats, { "top_invocations" })) {
            for (const json& invocation : *invocations) {
                const std::string name = Text::get_instance()->FirstNonEmpty({
                    Text::get_instance()->TrimCopy(JsonUtils::get_instance()->String(invocation, "plugin_name")),
                    Text::get_instance()->TrimCopy(JsonUtils::get_instance()->String(invocation, "skill_name")),
                    JsonUtils::get_instance()->String(invocation, "plugin_id"),
                    JsonUtils::get_instance()->String(invocation, "skill_id")
                });

                if (!name.empty()) {
                    profile.topInvocations.emplace_back(name, std::max(0LL, count(invocation, "usage_count")));
                }
            }

            std::stable_sort(profile.topInvocations.begin(), profile.topInvocations.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
        }

        if (const json* metadata = JsonUtils::get_instance()->FindObject(body, { "metadata" })) {
            profile.statsAsOf = JsonUtils::get_instance()->String(*metadata, "stats_as_of");
            profile.statsError = !Text::get_instance()->TrimCopy(JsonUtils::get_instance()->String(*metadata, "stats_error")).empty();
        }

        profile.valid = profile.lifetimeTokens >= 0 || profile.peakDailyTokens >= 0 || profile.currentStreakDays >= 0;
        return profile;
    }

    // GET /subscriptions/auto_top_up/settings: { is_enabled, recharge_threshold,
    // recharge_target, recharge_monthly_limit } - credit amounts, often strings.
    static AutoTopUp ParseCodexAutoTopUp(const json& body) {
        AutoTopUp topUp;
        const std::optional<bool> enabled = JsonUtils::get_instance()->BoolOpt(body, { "is_enabled" });

        if (!enabled) {
            return topUp;
        }

        const auto credits = [&](const char* key) {
            const std::optional<double> value = JsonUtils::get_instance()->NumberOpt(body, key);
            return value ? Format::get_instance()->CreditAmount(*value) + " credits" : std::string{};
        };

        topUp.valid = true;
        topUp.enabled = *enabled;
        topUp.thresholdText = credits("recharge_threshold");
        topUp.targetText = credits("recharge_target");
        topUp.monthlyLimitText = credits("recharge_monthly_limit");
        return topUp;
    }

    // GET /wham/usage/plan_limit_history?days=7: { periods: [{ window_minutes,
    // starts_at, ends_at, used_basis_points }] }; basis points / 100 = percent.
    static std::vector<PlanLimitPeriod> ParseCodexPlanLimitHistory(const json& body) {
        std::vector<PlanLimitPeriod> periods;

        if (const json* items = JsonUtils::get_instance()->FindArray(body, { "periods" })) {
            for (const json& item : *items) {
                PlanLimitPeriod period;
                period.windowMinutes = static_cast<int>(JsonUtils::get_instance()->NumberOpt(item, "window_minutes").value_or(0.0));
                period.startsAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(item, "starts_at");
                period.endsAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(item, "ends_at");

                if (const std::optional<double> basisPoints = JsonUtils::get_instance()->NumberOpt(item, "used_basis_points")) {
                    period.usedPercent = static_cast<float>(*basisPoints / 100.0);
                }

                if (period.windowMinutes > 0 && period.endsAtUnixSeconds > 0) {
                    periods.push_back(period);
                }
            }
        }

        std::stable_sort(periods.begin(), periods.end(), [](const PlanLimitPeriod& a, const PlanLimitPeriod& b) {
            return a.startsAtUnixSeconds < b.startsAtUnixSeconds;
        });
        return periods;
    }

    // GET /wham/rate-limit-reset-credits/history: { events: [{ id, kind:
    // granted | used, occurred_at }], window_start, as_of, next_cursor } -
    // the newest page of the last 30 days.
    static std::vector<ResetCredit> ParseCodexResetHistory(const json& body) {
        std::vector<ResetCredit> history;

        if (const json* events = JsonUtils::get_instance()->FindArray(body, { "events" })) {
            for (const json& event : *events) {
                if (!event.is_object()) {
                    continue;
                }

                ResetCredit item;
                item.title = JsonUtils::get_instance()->String(event, "kind") == "granted" ? "Reset received" : "Reset used";
                item.grantedAt = std::chrono::system_clock::from_time_t(
                    JsonUtils::get_instance()->UnixSecondsField(event, { "occurred_at" }));
                history.push_back(std::move(item));
            }
        }

        return history;
    }

    // GET /wham/usage/credit-usage-events: { data: [{ date, product_surface,
    // credit_amount, usage_id }] }. The newest 10.
    static std::vector<CreditEvent> ParseCodexCreditEvents(const json& body) {
        std::vector<CreditEvent> events;

        if (const json* items = JsonUtils::get_instance()->FindArray(body, { "data" })) {
            for (const json& item : *items) {
                CreditEvent event;
                event.date = JsonUtils::get_instance()->String(item, "date").substr(0, 10);
                event.surface = SurfaceLabel(JsonUtils::get_instance()->String(item, "product_surface"));
                event.credits = JsonUtils::get_instance()->NumberOpt(item, "credit_amount").value_or(0.0);

                if (!event.date.empty()) {
                    events.push_back(std::move(event));
                }
            }
        }

        std::stable_sort(events.begin(), events.end(), [](const CreditEvent& a, const CreditEvent& b) {
            return a.date > b.date;
        });

        if (events.size() > 10) {
            events.resize(10);
        }

        return events;
    }

    // GET /wham/usage/daily-token-usage-breakdown: { data: [{ date,
    // product_surface_usage_values: { <surface>: value } }], units }. Each
    // surface's share of the window's total, largest first.
    static std::vector<SurfaceShare> ParseCodexSurfaceShare(const json& body) {
        std::vector<SurfaceShare> shares;
        double total = 0.0;

        if (const json* days = JsonUtils::get_instance()->FindArray(body, { "data" })) {
            for (const json& day : *days) {
                const json* values = JsonUtils::get_instance()->FindObject(day, { "product_surface_usage_values" });

                if (!values) {
                    continue;
                }

                for (auto it = values->begin(); it != values->end(); ++it) {
                    const double value = it.value().is_number() ? it.value().get<double>() : 0.0;

                    if (!(value > 0.0) || !std::isfinite(value)) {
                        continue;
                    }

                    const std::string label = SurfaceLabel(it.key());
                    const auto found = std::find_if(shares.begin(), shares.end(),
                        [&](const SurfaceShare& share) { return share.surface == label; });

                    if (found == shares.end()) {
                        shares.push_back({ label, value });
                    }
                    else {
                        found->percent += value;
                    }

                    total += value;
                }
            }
        }

        for (SurfaceShare& share : shares) {
            share.percent = share.percent * 100.0 / total;
        }

        std::stable_sort(shares.begin(), shares.end(), [](const SurfaceShare& a, const SurfaceShare& b) {
            return a.percent > b.percent;
        });
        return shares;
    }

    // GET /wham/analytics/daily-workspace-usage-counts: { data: [{ date,
    // totals: { credits, threads, turns, text_total_tokens, ... } }] }.
    static std::vector<DailyUsage> ParseCodexDailyUsage(const json& body) {
        std::vector<DailyUsage> days;

        if (const json* items = JsonUtils::get_instance()->FindArray(body, { "data" })) {
            for (const json& item : *items) {
                const json* totals = JsonUtils::get_instance()->FindObject(item, { "totals" });
                const auto count = [&](const char* key) {
                    return totals ? static_cast<long long>(JsonUtils::get_instance()->NumberOpt(*totals, key).value_or(0.0)) : 0LL;
                };

                DailyUsage day;
                day.date = JsonUtils::get_instance()->String(item, "date").substr(0, 10);
                day.credits = totals ? JsonUtils::get_instance()->NumberOpt(*totals, "credits").value_or(0.0) : 0.0;
                day.turns = count("turns");
                day.threads = count("threads");
                // As Desktop counts it when the total is missing.
                day.tokens = count("text_total_tokens");

                if (day.tokens <= 0) {
                    day.tokens = count("uncached_text_input_tokens") + count("cached_text_input_tokens") + count("text_output_tokens");
                }

                if (totals && !day.date.empty()) {
                    days.push_back(std::move(day));
                }
            }
        }

        std::stable_sort(days.begin(), days.end(), [](const DailyUsage& a, const DailyUsage& b) {
            return a.date < b.date;
        });
        return days;
    }

    // GET /subscriptions?account_id=: { will_renew, active_until,
    // billing_period, entitlement: { renews_at, cancels_at, expires_at } }.
    static Subscription ParseCodexSubscription(const json& body) {
        Subscription subscription;
        const json* entitlement = JsonUtils::get_instance()->FindObject(body, { "entitlement" });
        const json empty = json::object();
        const json& terms = entitlement ? *entitlement : empty;
        const long long renewsAt = JsonUtils::get_instance()->UnixSecondsField(terms, "renews_at");
        const long long cancelsAt = JsonUtils::get_instance()->UnixSecondsField(terms, "cancels_at");
        const long long activeUntil = JsonUtils::get_instance()->UnixSecondsField(body, "active_until");

        // will_renew decides; renews_at alone survives a cancellation.
        subscription.renews = JsonUtils::get_instance()->BoolOpt(body, { "will_renew" }).value_or(renewsAt > 0 && cancelsAt <= 0);
        subscription.atUnixSeconds = subscription.renews
            ? (renewsAt > 0 ? renewsAt : activeUntil)
            : (cancelsAt > 0 ? cancelsAt : activeUntil > 0 ? activeUntil : JsonUtils::get_instance()->UnixSecondsField(terms, "expires_at"));

        if (subscription.atUnixSeconds <= 0) {
            return {};
        }

        subscription.valid = true;
        subscription.text = (subscription.atUnixSeconds < static_cast<long long>(std::time(nullptr)) ? "Ended "
            : subscription.renews ? "Renews " : "Ends ") + Format::get_instance()->MonthDay(subscription.atUnixSeconds);
        subscription.billingPeriod = JsonUtils::get_instance()->String(body, "billing_period");
        return subscription;
    }

    // The slow-moving account details that FetchCodexBackendExtras caches.
    static void CopyAccountExtras(const Snapshot& from, Snapshot& to) {
        to.profile = from.profile;
        to.workspaceCredits = from.workspaceCredits;
        to.resetHistory = from.resetHistory;
        to.autoTopUp = from.autoTopUp;
        to.planLimitHistory = from.planLimitHistory;
        to.creditEvents = from.creditEvents;
        to.surfaceShare = from.surfaceShare;
        to.dailyUsage = from.dailyUsage;
        to.subscription = from.subscription;
    }

    // Backend-only extras shared by both account sources; each is optional.
    // The monthly cap is read every refresh. The rest moves slowly (Desktop
    // caches it for 1-10 minutes), so it is fetched at most every 10 minutes
    // per account - the subscription hourly - and cached only when settled.
    static void FetchCodexBackendExtras(Snapshot& snapshot, const CodexBackend& backend, std::optional<bool> resetHistoryEnabled) {
        CodexBackendReads reads{ backend };
        const std::string account = Text::get_instance()->UrlEncode(backend.accountId);

        if (!snapshot.extraUsage.valid && !backend.accountId.empty()) {
            // The usage-limit-modes flag adds limit_mode / limit_amount / balance_unit.
            ParseCodexMonthlyUsage(reads.Get(
                "/accounts/" + account + "/spend-controls/current-user/monthly-usage?supports_usage_limit_modes=true",
                L"Cache-Control: no-store\r\nPragma: no-cache\r\n"), snapshot);
        }

        if (snapshot.plan == "Codex" && !backend.planType.empty()) {
            snapshot.plan = FormatCodexPlanToken(backend.planType);
        }

        struct Cached {
            long long extrasAt = 0;
            long long subscriptionAt = 0;
            Snapshot extras;
        };

        static std::mutex cacheMutex;
        static std::unordered_map<std::string, Cached> cache;
        const long long now = static_cast<long long>(std::time(nullptr));
        Cached entry;

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const auto found = cache.find(backend.accountId);

            if (found != cache.end()) {
                entry = found->second;
            }
        }

        if (now - entry.extrasAt >= 600) {
            reads.settled = true;
            Snapshot& extras = entry.extras;
            // Desktop sends this on its usage-history reads, whose days are UTC.
            const std::wstring categories = L"x-openai-codex-usage-categories: 1\r\n";
            const auto utcDate = [now](int offsetDays) { return Format::get_instance()->UtcDateKey(now, offsetDays); };

            extras.profile = ParseCodexProfileStats(reads.Get("/wham/profiles/me"));
            extras.workspaceCredits.clear();
            extras.autoTopUp = {};

            if (!backend.accountId.empty()) {
                // Team workspace owners only; everyone else gets a 403.
                const std::optional<double> balance = JsonUtils::get_instance()->NumberOpt(
                    reads.Get("/accounts/" + account + "/remaining_balance"), "balance");

                if (balance) {
                    extras.workspaceCredits =
                        Format::get_instance()->IntegerWithCommas(std::max(0.0, std::ceil(*balance))) + " workspace credits";
                }

                extras.autoTopUp = ParseCodexAutoTopUp(reads.Get(
                    "/subscriptions/auto_top_up/settings?include_payment_method=false",
                    Text::get_instance()->Utf8ToWide("X-OpenAI-Expected-Account-Id: " + backend.accountId + "\r\n")));
            }

            // The reset list says whether this account keeps a history at all.
            extras.resetHistory = resetHistoryEnabled.value_or(true)
                ? ParseCodexResetHistory(reads.Get("/wham/rate-limit-reset-credits/history"))
                : std::vector<ResetCredit>{};
            extras.planLimitHistory = ParseCodexPlanLimitHistory(reads.Get("/wham/usage/plan_limit_history?days=7"));
            extras.creditEvents = ParseCodexCreditEvents(reads.Get("/wham/usage/credit-usage-events", categories));
            extras.surfaceShare = ParseCodexSurfaceShare(reads.Get(
                "/wham/usage/daily-token-usage-breakdown?start_date=" + utcDate(-6) + "&end_date=" + utcDate(0) +
                "&group_by=day", categories));
            extras.dailyUsage = ParseCodexDailyUsage(reads.Get(
                "/wham/analytics/daily-workspace-usage-counts?start_date=" + utcDate(-29) + "&end_date=" + utcDate(0) +
                "&group_by=day&workspace_user=true"));

            if (reads.settled) {
                entry.extrasAt = now;
            }
        }

        if (now - entry.subscriptionAt >= 3600 && !backend.accountId.empty()) {
            reads.settled = true;
            entry.extras.subscription = ParseCodexSubscription(reads.Get("/subscriptions?account_id=" + account));

            if (reads.settled) {
                entry.subscriptionAt = now;
            }
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache[backend.accountId] = entry;
        }

        CopyAccountExtras(entry.extras, snapshot);
    }

    static Snapshot FetchSnapshotFromAuthFile(
        const std::filesystem::path& authPath,
        bool enforceCredentialStoreSafety,
        const std::string& sourceLabel
    ) {
        json auth = ReadAuthJson(authPath, enforceCredentialStoreSafety);
        const json* tokens = JsonUtils::get_instance()->FindObject(auth, { "tokens" });

        if (!tokens) {
            throw std::runtime_error("Codex auth.json does not contain tokens");
        }

        std::string accessToken = JsonUtils::get_instance()->String(*tokens, { "access_token", "accessToken" });
        const std::string accountId = JsonUtils::get_instance()->String(
            *tokens,
            { "account_id", "accountId", "chatgpt_account_id", "chatgptAccountId" }
        );

        if (accessToken.empty()) {
            throw std::runtime_error("Codex access_token missing");
        }

        if (accountId.empty()) {
            throw std::runtime_error("Codex account_id missing");
        }

        CodexBackend backend = MakeCodexBackend(accessToken, accountId, json());
        Network::HttpResponse usage = Network::get_instance()->RequestUrl(
            backend.base + "/wham/usage", "GET", backend.headers);

        // The stored access token expires in about an hour, which is why an
        // idle AQC reports token_expired. Recover the way Codex itself does:
        // adopt a token the CLI rotated meanwhile, else refresh it.
        if (usage.statusCode == 401) {
            std::string refreshDetail;

            if (!RefreshCodexTokens(authPath, auth, accessToken, refreshDetail)) {
                throw std::runtime_error("Codex usage failed: " + refreshDetail);
            }

            backend = MakeCodexBackend(accessToken, accountId, json());
            usage = Network::get_instance()->RequestUrl(backend.base + "/wham/usage", "GET", backend.headers);
        }

        if (usage.statusCode < 200 || usage.statusCode >= 300) {
            throw std::runtime_error("Codex usage failed: HTTP " + std::to_string(usage.statusCode));
        }

        const json usageJson = JsonUtils::get_instance()->ParseOrNull(usage.body);

        if (!usageJson.is_object()) {
            throw std::runtime_error("Codex usage returned an unreadable response");
        }

        // Reset credits are supplemental. A missing/unsupported reset-credit
        // endpoint must not hide otherwise valid usage windows and balances.
        const json resetJson = CodexBackendReads{ backend }.Get("/wham/rate-limit-reset-credits");

        Snapshot snapshot = ParseUsageAndResets(usageJson, resetJson.is_object() ? resetJson : json::object());
        FetchCodexBackendExtras(snapshot, backend, JsonUtils::get_instance()->BoolOpt(resetJson, { "history_enabled", "historyEnabled" }));
        snapshot.statusText = "Source: " + sourceLabel + ": " + Text::get_instance()->WideToUtf8(authPath.wstring());

        const std::string store = ConfiguredCredentialStore();

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

    // The newest rollout's records, from its last 8 MB: token-count records are
    // emitted throughout a session, so the latest one is in this tail.
    static std::vector<std::string> ReadLatestRolloutLines() {
        std::vector<std::string> lines;
        const std::filesystem::path session = LatestCodexSessionFile();

        if (session.empty()) {
            return lines;
        }

        std::istringstream stream(ReadTailSharedReadOnly(session, 8ULL * 1024ULL * 1024ULL));

        for (std::string line; std::getline(stream, line);) {
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
        }

        return lines;
    }

    struct CodexTokenUsageRecord {
        bool valid = false;
        long long totalTokens = 0;
        long long inputTokens = 0;
        long long cachedInputTokens = 0;
        // Present from Codex 26.1027 (-1 = not reported).
        long long cacheWriteInputTokens = -1;
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

    // Speed/workspace variants (-priority, -wm, -ultrafast) are never listed in
    // models_cache.json; they share their base model's limits.
    static std::string CodexBaseModelSlug(std::string model) {
        for (const std::string suffix : { "-ultrafast", "-priority", "-wm" }) {
            if (model.size() > suffix.size() &&
                model.compare(model.size() - suffix.size(), suffix.size(), suffix) == 0) {
                model.resize(model.size() - suffix.size());
                break;
            }
        }
        return model;
    }

    // models_cache.json is { models: [{ slug, context_window, ... }] }. Only
    // top-level models[].slug counts: a nested object (a future non-null
    // "upgrade") must not be mistaken for the model itself.
    static const json* FindCodexModelMetadata(const json& root, const std::string& model) {
        const json* models = model.empty() ? nullptr : JsonUtils::get_instance()->FindArray(root, { "models" });

        if (!models) {
            return nullptr;
        }

        for (const std::string& slug : { model, CodexBaseModelSlug(model) }) {
            for (const json& item : *models) {
                if (JsonUtils::get_instance()->String(item, "slug") == slug) {
                    return &item;
                }
            }
        }

        return nullptr;
    }

    // The catalog bundled with Codex 26.1027 (codex 0.166) for accounts whose
    // models_cache.json omits a model: 272K context at the usual 95% effective
    // share, except the two models with their own window.
    static long long KnownCodexContextWindow(const std::string& model) {
        const std::string slug = CodexBaseModelSlug(model);

        if (slug.rfind("gpt-", 0) != 0 && slug.rfind("codex-", 0) != 0) {
            return 0;
        }

        const long long window = slug == "gpt-6.1-terra" ? 500000
            : slug == "gpt-daybreak-red-latest" ? 372000
            : 272000;
        return window * 95 / 100;
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

        const json root = JsonUtils::get_instance()->ParseOrNull(Network::get_instance()->ReadTextFile(cachePath));
        const json* modelObject = FindCodexModelMetadata(root, model);

        if (!modelObject) {
            return metadata;
        }

        metadata.contextWindow = ReadPositiveCount(
            *modelObject,
            { "context_window", "contextWindow", "max_context_window", "maxContextWindow" }
        );
        metadata.autoCompactTokenLimit = ReadPositiveCount(
            *modelObject,
            { "auto_compact_token_limit", "autoCompactTokenLimit" }
        );

        const long long effectivePercent = ReadPositiveCount(
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

    static long long ResolveCodexContextWindowForModel(const std::string& model, const CodexConfig& config)
    {
        // config/read reflects the configured model, while a thread may
        // temporarily use a different per-thread selection. Never apply a
        // context override captured for one model to another model.
        if (config.contextWindow > 0 &&
            (config.model.empty() || model.empty() || config.model == model)) {
            return config.contextWindow;
        }

        const CodexModelCompactionMetadata metadata =
            ReadCodexModelCompactionMetadata(model);
        if (!metadata.valid || metadata.contextWindow <= 0) {
            return KnownCodexContextWindow(model);
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
            return std::min(metadata.autoCompactTokenLimit, ninetyPercentOfContext);
        }

        return ninetyPercentOfContext;
    }

    static long long CodexRecordUnixSeconds(const json& record) {
        return JsonUtils::get_instance()->UnixSecondsField(record, { "timestamp" });
    }

    static CodexTokenUsageRecord ParseCodexTokenUsageRecord(const json& record) {
        CodexTokenUsageRecord usage;
        const json* payload = JsonUtils::get_instance()->FindObject(record, { "payload" });

        if (!payload) {
            return usage;
        }

        const std::string payloadType = Text::get_instance()->ToLowerCopy(
            JsonUtils::get_instance()->String(*payload, "type")
        );
        if (payloadType != "token_count" && payloadType != "tokencount") {
            return usage;
        }

        const json* info = JsonUtils::get_instance()->FindObject(*payload, { "info" });
        if (!info) {
            return usage;
        }

        const json* last = JsonUtils::get_instance()->FindObject(
            *info,
            { "last_token_usage", "lastTokenUsage", "last" }
        );

        if (!last) {
            return usage;
        }

        auto readCount = [](const json& object, std::initializer_list<const char*> keys) {
            return static_cast<long long>(std::max(
                0.0,
                JsonUtils::get_instance()->NumberAny(object, keys).value_or(0.0)
            ));
        };

        usage.totalTokens = readCount(*last, { "total_tokens", "totalTokens" });
        usage.inputTokens = readCount(*last, { "input_tokens", "inputTokens" });
        usage.cachedInputTokens = readCount(
            *last,
            { "cached_input_tokens", "cachedInputTokens" }
        );
        if (JsonUtils::get_instance()->NumberAny(*last, { "cache_write_input_tokens", "cacheWriteInputTokens" })) {
            usage.cacheWriteInputTokens = readCount(*last, { "cache_write_input_tokens", "cacheWriteInputTokens" });
        }
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

        const json* cumulative = JsonUtils::get_instance()->FindObject(
            *info,
            { "total_token_usage", "totalTokenUsage", "total" }
        );

        if (cumulative) {
            const std::optional<double> cumulativeTotal = JsonUtils::get_instance()->NumberAny(
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

                if (Text::get_instance()->ToLowerCopy(value.at(key).get<std::string>()).find("compact") !=
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
        const std::string type = Text::get_instance()->ToLowerCopy(
            JsonUtils::get_instance()->String(source, { "type", "event", "kind", "subtype" })
        );
        const std::string status = Text::get_instance()->ToLowerCopy(
            JsonUtils::get_instance()->String(source, { "status", "phase" })
        );
        const std::string combined = type + " " + status;

        if (combined.find("compact") == std::string::npos) {
            return false;
        }

        const std::optional<bool> completed = JsonUtils::get_instance()->BoolOpt(
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
                candidate.find("tokenCount") != std::string::npos ||
                candidate.find("item_completed") != std::string::npos;

            if (!mayAffectRun) {
                continue;
            }

            json record = json::parse(candidate, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                continue;
            }

            const long long at = CodexRecordUnixSeconds(record);
            const json* payload = JsonUtils::get_instance()->FindObject(record, { "payload" });
            std::string type = payload
                ? Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(*payload, "type"))
                : std::string{};

            // Since Codex 26.1027 activity arrives as event_msg item_completed
            // { item: { type: UserMessage | Reasoning | AgentMessage |
            // CommandExecution | Extension } }; map it onto the old event names.
            if (type == "item_completed" || type == "itemcompleted") {
                const json* item = JsonUtils::get_instance()->FindObject(*payload, { "item" });
                const std::string itemType = item
                    ? Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(*item, "type"))
                    : std::string{};
                // A UserMessage item lands after task_started; it must not restart
                // a running turn, so it maps to the start-if-idle event.
                type = itemType == "usermessage" ? "turn_started"
                    : itemType == "reasoning" ? "agent_reasoning"
                    : itemType == "agentmessage" ? "agent_message"
                    : itemType == "commandexecution" || itemType == "extension" ? "exec_command"
                    : std::string{};
            }

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

    // The context card, run state and compaction from a rollout's records.
    // Codex Desktop's context meter is last_token_usage.total_tokens against
    // model_context_window, reasoning included; every count here uses that.
    static LocalTelemetry ParseCodexTelemetry(const std::vector<std::string>& lines, const CodexConfig& config) {
        LocalTelemetry local;
        UsageTelemetry::ContextUsage& context = local.context;
        bool foundUsage = false;
        bool compacting = false;
        long long lastTotalTokens = 0;
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
            const json* payload = JsonUtils::get_instance()->FindObject(record, { "payload" });

            // item_started / item_completed { item: ContextCompaction } only
            // bracket the compaction recorded below, after its post-compaction
            // token count: it must neither latch COMPACTING nor restart the
            // saved-token count.
            if (payload && JsonUtils::get_instance()->FindObject(*payload, { "item" })) {
                continue;
            }

            // A finished compaction, with no start event: Codex 26.1027 writes a
            // "compacted" record, older builds a response_item "compaction".
            if (Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(record, "type")) == "compacted" ||
                (payload && Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(*payload, "type")) == "compaction")) {
                if (!compacting) {
                    context.compactionStartedAtUnixSeconds = at;
                    preCompactionTokens = lastTotalTokens;
                    context.compactionPreTokens = preCompactionTokens;
                }

                compacting = false;
                context.compactionCompletedAtUnixSeconds = at;
                waitingForPostCompactionUsage = preCompactionTokens > 0;
                continue;
            }

            const bool wasCompacting = compacting;
            const bool stateChanged = CodexCompactionStateFromRecord(record, compacting);

            if (stateChanged && !wasCompacting && compacting) {
                context.compactionStartedAtUnixSeconds = at;
                preCompactionTokens = lastTotalTokens;
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

            if (compacting) {
                // Codex persists a fresh token-count record once the compacted
                // context becomes active. This is a reliable passive completion
                // signal even when no explicit completion event is persisted.
                compacting = false;
                context.compactionCompletedAtUnixSeconds = at;
                waitingForPostCompactionUsage = preCompactionTokens > 0;
            }

            if (waitingForPostCompactionUsage && tokenUsage.totalTokens > 0) {
                context.compactionSavedTokens = std::max(
                    0LL,
                    preCompactionTokens - tokenUsage.totalTokens
                );
                waitingForPostCompactionUsage = false;
            }

            lastTotalTokens = tokenUsage.totalTokens;
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

            const json* payload = JsonUtils::get_instance()->FindObject(record, { "payload" });

            if (!payload) {
                continue;
            }

            const std::string type = Text::get_instance()->ToLowerCopy(
                JsonUtils::get_instance()->String(*payload, "type")
            );

            if (context.model.empty() && type == "turn_context") {
                context.model = JsonUtils::get_instance()->String(*payload, "model");
            }

            if (!foundUsage && (type == "token_count" || type == "tokencount")) {
                const CodexTokenUsageRecord usage = ParseCodexTokenUsageRecord(record);
                if (!usage.valid) {
                    continue;
                }

                context.inputTokens = usage.inputTokens;
                context.cachedInputTokens = usage.cachedInputTokens;
                if (usage.cacheWriteInputTokens >= 0) {
                    // Newer rollouts report cache writes too, enough for the cache card.
                    context.cacheStatsValid = true;
                    context.cacheInputTokens = usage.inputTokens;
                    context.cacheReadTokens = usage.cachedInputTokens;
                    context.cacheWriteTokens = usage.cacheWriteInputTokens;
                    context.latestCacheHitPercentValid = usage.inputTokens > 0;
                    context.latestCacheHitPercent = usage.inputTokens > 0
                        ? 100.0 * static_cast<double>(usage.cachedInputTokens) / static_cast<double>(usage.inputTokens)
                        : 0.0;
                }
                context.outputTokens = usage.outputTokens;
                context.reasoningOutputTokens = usage.reasoningOutputTokens;
                latestRecordAutoCompactTokenLimit = usage.modelAutoCompactTokenLimit;
                context.usedTokens = usage.totalTokens;
                context.contextWindowTokens = usage.modelContextWindow;
                context.sourceLabel = "Newest local Codex thread \xc2\xb7 direct read-only metadata";
                foundUsage = context.usedTokens > 0 && context.contextWindowTokens > 0;
            }

            if (foundUsage && !context.model.empty()) {
                break;
            }
        }

        // New/changed Codex models are resolved from live config or the local
        // models_cache catalog instead of a hard-coded model-name table. This
        // also fills the brief gap before token_count persists its own window.
        if (context.usedTokens > 0 && context.contextWindowTokens <= 0) {
            context.contextWindowTokens = ResolveCodexContextWindowForModel(context.model, config);
        }
        context.valid = context.usedTokens > 0 && context.contextWindowTokens > 0;

        if (context.valid && !config.bodyAfterPrefixScope) {
            // Priority matches Codex itself: per-event/effective metadata first,
            // then an explicit config override, then the active model's catalog
            // default. config/read commonly returns null for the limit because
            // the normal default belongs to ModelInfo, not Config.
            long long autoCompactThreshold = latestRecordAutoCompactTokenLimit;
            if (autoCompactThreshold <= 0) {
                autoCompactThreshold = config.autoCompactTokenLimit;
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
                    context.usedTokens,
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
        return ParseCodexTelemetry(ReadLatestRolloutLines(), CurrentConfig());
    }

    static CodexConfig ReadCodexConfig(const json& configResult) {
        CodexConfig parsed;
        const json* nested = JsonUtils::get_instance()->FindObject(configResult, { "config" });
        const json& config = nested ? *nested : configResult;

        if (!config.is_object()) {
            return parsed;
        }

        parsed.model = JsonUtils::get_instance()->String(config, "model");
        parsed.contextWindow = ReadPositiveCount(config, { "model_context_window", "modelContextWindow" });
        parsed.autoCompactTokenLimit = ReadPositiveCount(
            config, { "model_auto_compact_token_limit", "modelAutoCompactTokenLimit" });
        // Codex defaults the scope to total; "body_after_prefix" is the other.
        parsed.bodyAfterPrefixScope = Text::get_instance()->CompactLower(JsonUtils::get_instance()->String(
            config, { "model_auto_compact_token_limit_scope", "modelAutoCompactTokenLimitScope" }), false) == "bodyafterprefix";
        return parsed;
    }

    static void FinalizeCodexAccess(Snapshot& snapshot) {
        UsageTelemetry::SetAvailable(snapshot.access);

        const bool monthlySpendReached =
            snapshot.usageNotice.find("Monthly usage limit reached") != std::string::npos ||
            (snapshot.extraUsage.valid && snapshot.extraUsage.enforced && snapshot.extraUsage.hasUsedPercent &&
                UsageTelemetry::IsExhausted(snapshot.extraUsage.usedPercent));

        if (monthlySpendReached) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = "Monthly usage/credits exhausted";
        }
        else if (snapshot.usageSource == "reserve" || snapshot.usageSource == "credits") {
            // Codex keeps serving from these after the plan windows run out.
            UsageTelemetry::SetAvailable(snapshot.access, snapshot.usageSource == "reserve"
                ? "Plan usage is used up; Codex is running on Luna Reserve"
                : "Plan usage is used up; Codex is running on credits");
        }
        else if (snapshot.usageSource == "unavailable") {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = snapshot.usageNotice.empty() ? "Usage exhausted" : snapshot.usageNotice;
        }
        else if (snapshot.usageSource != "included") {
            // No (known) source reported: an exhausted main window, or the
            // server's own "Usage exhausted", decides.
            std::string exhausted;

            for (const UsageBar& bar : snapshot.bars) {
                if (bar.valid && bar.quotaNotificationEligible && UsageTelemetry::IsExhausted(bar.usedPercent)) {
                    exhausted += (exhausted.empty() ? "" : ", ") + (bar.label.empty() ? std::string("Usage") : bar.label);
                }
            }

            const bool explicitlyExhausted =
                snapshot.usageNotice.find("Usage exhausted") != std::string::npos;

            if (explicitlyExhausted || !exhausted.empty()) {
                snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
                snapshot.access.detail = (explicitlyExhausted ? snapshot.usageNotice : std::string("Usage exhausted")) +
                    (exhausted.empty() ? "" : (explicitlyExhausted ? " \xc2\xb7 " : ": ") + exhausted);
            }
        }

        LocalTelemetry local = ReadLocalTelemetry();
        snapshot.context = std::move(local.context);
        snapshot.run = std::move(local.run);
    }

    // Every token_count event in a rollout carries the limits Codex last saw:
    // payload.rate_limits { limit_id, limit_name, primary / secondary
    // { used_percent, window_minutes, resets_at (s) }, credits, plan_type }.
    // The newest per limit_id, keyed like account/rateLimits/read, lets an
    // offline or signed-out machine still show its last known quota; the
    // snapshot parsers read these snake_case fields as they are.
    static json ParseRolloutRateLimits(const std::vector<std::string>& lines, long long& observedAt) {
        observedAt = 0;
        json byId = json::object();

        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->find("\"rate_limits\"") == std::string::npos) {
                continue;
            }

            const json record = json::parse(*it, nullptr, false);
            const json* payload = JsonUtils::get_instance()->FindObject(record, { "payload" });
            const json* limits = payload ? JsonUtils::get_instance()->FindObject(*payload, { "rate_limits" }) : nullptr;

            if (!limits) {
                continue;
            }

            const std::string id = JsonUtils::get_instance()->String(*limits, "limit_id");
            const std::string key = id.empty() ? "codex" : id;

            if (byId.contains(key)) {
                continue;
            }

            byId[key] = *limits;

            if (observedAt == 0) {
                observedAt = CodexRecordUnixSeconds(record);
            }
        }

        if (byId.empty()) {
            return json();
        }

        json result = { { "rateLimitsByLimitId", byId } };
        if (byId.contains("codex")) result["rateLimits"] = byId.at("codex");
        return result;
    }

    Snapshot FetchSnapshot(AccountSource source, const std::string& customAuthPath) {
        Snapshot snapshot;
        // Published only once this refresh is done: until then the 1-second
        // telemetry thread keeps the previous config instead of a blank one.
        CodexConfig config;

        if (source == AccountSource::AuthFile) {
            snapshot = FetchSnapshotFromAuthFile(DefaultAuthPath(), false, "Codex auth.json selected");
        }
        else if (source == AccountSource::CustomAuthFile) {
            if (customAuthPath.empty()) {
                throw std::runtime_error("Custom Codex auth.json is selected, but no path was configured");
            }

            snapshot = FetchSnapshotFromAuthFile(
                ExpandCustomAuthPath(customAuthPath),
                false,
                "Custom Codex auth.json selected"
            );
        }
        else {
            const CodexAppServer::Result current = CodexAppServer::ReadCurrentAccountRateLimits();

            if (current.kind == CodexAppServer::ResultKind::Success) {
                config = ReadCodexConfig(current.configResult);
                snapshot = ParseAppServerSnapshot(current.accountResult, current.rateLimitsResult);
                snapshot.signedInAccounts = current.sessionCount;

                if (!current.authToken.empty()) {
                    // A bad CODEX_API_BASE_URL costs only the extras here.
                    try {
                        FetchCodexBackendExtras(
                            snapshot,
                            MakeCodexBackend(current.authToken, std::string{}, current.accountResult),
                            JsonUtils::get_instance()->BoolOpt(ResetCreditList(current.rateLimitsResult), { "history_enabled", "historyEnabled" })
                        );
                    }
                    catch (const std::exception& error) {
                        snapshot.statusText += std::string(" (") + error.what() + ")";
                    }
                }
            }
            else if (current.kind == CodexAppServer::ResultKind::Error) {
                throw std::runtime_error("Codex app-server error: " + current.detail);
            }
            else if (source == AccountSource::ActiveAccount) {
                throw std::runtime_error("Active Codex account only is selected: " + (current.detail.empty()
                    ? std::string("Codex app-server account APIs are unavailable")
                    : current.detail));
            }
            else {
                // Auto mode uses auth.json only when app-server is genuinely
                // unavailable. Ambiguous keyring/auto/ephemeral configurations
                // are still refused here.
                try {
                    snapshot = FetchSnapshotFromAuthFile(DefaultAuthPath(), true, "Codex auto fallback auth.json");
                }
                catch (const std::exception& error) {
                    // Offline or signed out: the newest local session still knows the
                    // limits as of its last turn. Say so rather than show nothing.
                    long long observedAt = 0;
                    const json offline = ParseRolloutRateLimits(ReadLatestRolloutLines(), observedAt);

                    if (offline.is_null()) {
                        throw;
                    }

                    snapshot = ParseAppServerSnapshot(json::object(), offline);
                    snapshot.statusText = std::string("Source: last local Codex session (live read failed: ") + error.what() + ")";
                    snapshot.lastUpdated = observedAt > 0
                        ? Format::get_instance()->ResetDateTime(observedAt) + " (offline)"
                        : "from local session (offline)";
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_config = std::move(config);
        }

        FinalizeCodexAccess(snapshot);
        return snapshot;
    }

}
