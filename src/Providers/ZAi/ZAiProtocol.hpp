#pragma once

// Wire formats verified against ZCode 3.14.0, build a1328db1 (supplied app.asar).
// Keep these parsers independent of Win32/HTTP so regressions can be tested offline.
#include "ZAi.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <sstream>

namespace ZAi::Protocol
{
    using Json = nlohmann::json;
    inline constexpr const char* AppVersion = "3.14.0";
    inline constexpr const char* BalanceUrl =
        "https://zcode.z.ai/api/v1/zcode-plan/billing/balance?app_version=3.14.0";
    inline constexpr const char* QuotaUrl = "https://api.z.ai/api/monitor/usage/quota/limit";
    inline constexpr const char* SubscriptionUrl = "https://api.z.ai/api/biz/subscription/list";
    inline constexpr const char* McpUrl = "https://zcode.z.ai/api/v1/mcp/usage";

    inline std::string Trim(std::string text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    inline std::string Lower(std::string text)
    {
        for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return text;
    }

    inline std::string String(const Json& object, const char* key)
    {
        if (!object.is_object()) return {};
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? Trim(it->get<std::string>()) : std::string{};
    }

    inline std::optional<double> Number(const Json& value)
    {
        double number = 0;
        if (value.is_number()) number = value.get<double>();
        else if (value.is_string()) {
            // Classic locale, full consumption, and finite-only: never turn "1junk",
            // booleans, null, NaN, or infinity into a quota or an integer timestamp.
            std::istringstream input(Trim(value.get<std::string>()));
            input.imbue(std::locale::classic());
            if (!(input >> number)) return {};
            input >> std::ws;
            if (!input.eof()) return {};
        }
        else return {};
        return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
    }

    inline std::optional<double> Number(const Json& object, const char* key)
    {
        if (!object.is_object()) return {};
        const auto it = object.find(key);
        return it == object.end() ? std::nullopt : Number(*it);
    }

    inline std::optional<double> NonNegative(const Json& object, const char* key)
    {
        auto n = Number(object, key);
        return n && *n >= 0 ? n : std::nullopt;
    }

    inline long long Epoch(const Json& object, const char* key)
    {
        auto n = Number(object, key);
        if (!n || *n <= 0) return 0;
        double seconds = *n > 100000000000.0 ? *n / 1000.0 : *n;
        // Bound to year 9999 before any floating-point -> integer conversion.
        return seconds <= 253402300799.0 ? static_cast<long long>(seconds) : 0;
    }

    inline const Json* Data(const Json& root, bool requireCodeZero = false)
    {
        if (!root.is_object()) return nullptr;
        auto success = root.find("success");
        if (success != root.end() && (!success->is_boolean() || !success->get<bool>())) return nullptr;
        auto code = root.find("code");
        if (code != root.end()) {
            const auto n = Number(*code);
            if (!n || (*n != 0 && (requireCodeZero || *n != 200))) return nullptr;
        }
        else if (requireCodeZero || success == root.end()) return nullptr;
        auto data = root.find("data");
        return data != root.end() && !data->is_null() ? &*data : nullptr;
    }

    inline std::string ModelLabel(const Json& item, const std::string& fallback)
    {
        for (const char* key : { "show_name", "displayName", "model", "model_name", "modelName" }) {
            auto label = String(item, key);
            if (!label.empty()) return label;
        }
        std::string models;
        auto caps = item.find("capabilities");
        if (caps != item.end() && caps->is_array()) {
            std::set<std::string> seen;
            for (const auto& cap : *caps) {
                if (!cap.is_string()) continue;
                auto text = Trim(cap.get<std::string>());
                if (Lower(text).rfind("model:", 0) != 0) continue;
                text = Trim(text.substr(6));
                if (text.empty() || !seen.insert(Lower(text)).second) continue;
                if (!models.empty()) models += ", ";
                models += text;
            }
        }
        if (!models.empty()) return models;
        for (const char* key : { "name", "entitlement_id", "meter" }) {
            auto label = String(item, key);
            if (!label.empty()) return label;
        }
        return fallback;
    }

    inline std::string Count(double value)
    {
        // Streaming does not overflow an integer for large but finite server values.
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(0) << value;
        std::string text = out.str();
        for (std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(text.size()) - 3; pos > 0; pos -= 3)
            text.insert(static_cast<size_t>(pos), ",");
        return text;
    }

    inline void SetStyle(UsageBar& bar)
    {
        bar.valid = true;
        bar.green = true;
        bar.red = bar.white = bar.thin = false;
    }

    inline bool ExpiredPlan(const Json& plan, long long now)
    {
        const auto status = Lower(String(plan, "status"));
        const auto end = Epoch(plan, "ends_at");
        return status == "expired" || (status == "active" && end > 0 && end <= now);
    }

    inline bool StartPlan(const Json& plan, long long now)
    {
        if (Lower(String(plan, "status")) != "active" || ExpiredPlan(plan, now)) return false;
        const auto id = Lower(String(plan, "plan_id"));
        const auto name = Lower(String(plan, "name"));
        return id.find("start-plan") != std::string::npos || id.find("start plan") != std::string::npos ||
            name.find("start plan") != std::string::npos || name.find("start-plan") != std::string::npos;
    }

    inline bool MatchesPlan(const Json& bucket, const Json& plan)
    {
        const auto user = String(bucket, "user_plan_id");
        const auto otherUser = String(plan, "user_plan_id");
        if (!user.empty() && !otherUser.empty()) return user == otherUser;
        const auto id = String(bucket, "plan_id");
        return !id.empty() && id == String(plan, "plan_id");
    }

    struct StartResult
    {
        bool activePlan = false;
        bool usable = false;
        std::string error;
    };

    inline StartResult ApplyStartPlan(Snapshot& snapshot, const Json& root,
        long long localNow, long long httpDate = 0)
    {
        const auto* data = Data(root, true);
        if (!data || !data->is_object() || !data->contains("plans") || !data->at("plans").is_array())
            return { false, false, "Start Plan returned an invalid billing envelope" };
        const auto serverNow = Epoch(*data, "server_time");
        const auto now = httpDate > 0 ? httpDate : (serverNow > 0 ? serverNow : localNow);
        const auto& plans = data->at("plans");
        const Json* active = nullptr;
        for (const auto& plan : plans) {
            if (StartPlan(plan, now)) { active = &plan; break; }
        }
        if (!active) return { false, false, "No active ZCode Start Plan was returned for the signed-in account" };
        const auto planName = String(*active, "name");
        snapshot.plan = "Z.Ai " + (planName.empty() ? "Start Plan" : planName);
        const auto balances = data->find("balances");
        if (balances == data->end() || !balances->is_array())
            return { true, false, "The active Start Plan returned no balance array" };
        std::set<std::string> seen;
        const size_t before = snapshot.bars.size();
        for (const auto& balance : *balances) {
            if (!balance.is_object()) continue;
            bool matched = false, nonExpired = false;
            std::string period;
            for (const auto& plan : plans) {
                if (!MatchesPlan(balance, plan)) continue;
                matched = true;
                if (!ExpiredPlan(plan, now)) nonExpired = true;
                const auto entries = plan.find("entitlements");
                if (entries == plan.end() || !entries->is_array()) continue;
                for (const auto& entry : *entries) {
                    if (String(entry, "entitlement_id") == String(balance, "entitlement_id"))
                        period = String(entry, "period");
                }
            }
            if (matched && !nonExpired) continue;
            const auto reset = Epoch(balance, "expires_at");
            const auto periodStart = Epoch(balance, "period_start");
            const auto periodEnd = Epoch(balance, "period_end");
            if ((reset > 0 && reset <= now) || (periodStart > 0 && periodStart > now) ||
                (periodEnd > 0 && periodEnd <= now)) continue;
            auto total = NonNegative(balance, "total_units");
            auto used = NonNegative(balance, "used_units");
            auto remaining = NonNegative(balance, "remaining_units");
            if (!total && used && remaining && std::isfinite(*used + *remaining)) total = *used + *remaining;
            if (!total || *total <= 0 || (!remaining && !used)) continue;
            if (!remaining) remaining = std::max(0.0, *total - *used);
            const double available = std::clamp(*remaining, 0.0, *total);
            UsageBar bar;
            bar.label = ModelLabel(balance, "Usage credits");
            // A model can have several buckets (e.g. purchased/off-peak/daily).
            // Do not deduplicate by display name or combine independent windows.
            bar.identity = "start:" + Json::array({ String(balance, "bucket_id"),
                String(balance, "user_plan_id"), String(balance, "plan_id"),
                String(balance, "entitlement_id"), String(balance, "meter"),
                String(balance, "unit_type"), Epoch(balance, "period_start"),
                Epoch(balance, "period_end"), reset, bar.label }).dump();
            if (!seen.insert(bar.identity).second) continue;
            bar.spendBalance = true;
            // Start balances report remaining/total. Coding quota percentage below
            // has the OPPOSITE meaning and scale; do not share the heuristic.
            bar.usedPercent = static_cast<float>(std::clamp(100.0 * (1.0 - available / *total), 0.0, 100.0));
            bar.sublabel = Count(available) + " / " + Count(*total) + " left";
            if (!period.empty()) bar.sublabel += " | " + period;
            bar.resetAtUnixSeconds = reset;
            SetStyle(bar);
            snapshot.bars.push_back(std::move(bar));
        }
        const bool usable = snapshot.bars.size() > before;
        return { true, usable, usable ? "" : "The active Start Plan returned no usable, unexpired balances" };
    }

    inline std::string PeriodLabel(const Json& limit)
    {
        auto unit = Number(limit, "unit"), number = Number(limit, "number");
        if (!unit || std::floor(*unit) != *unit) return {};
        int count = number && *number >= 1 && *number <= 10000 && std::floor(*number) == *number
            ? static_cast<int>(*number) : 1;
        if (*unit == 3) return std::to_string(count) + "-hour";
        if (*unit == 5) return count == 1 ? "Monthly" : std::to_string(count) + "-month";
        if (*unit == 6) return count == 1 ? "Weekly" : std::to_string(count) + "-week";
        return {};
    }

    inline bool ApplyCodingQuota(Snapshot& snapshot, const Json& root)
    {
        const auto* data = Data(root);
        if (!data || !data->is_object()) return false;
        const auto limits = data->find("limits");
        if (limits == data->end() || !limits->is_array()) return false;
        size_t before = snapshot.bars.size();
        std::set<std::string> seen;
        for (const auto& limit : *limits) {
            if (!limit.is_object()) continue;
            const auto type = String(limit, "type");
            if (type.empty()) continue;
            // unit/number describe PERIODS, never capacities. Prefer the wire's
            // explicit used percentage: 1 is one percent, NOT 100% or 99% used.
            auto percent = Number(limit, "percentage");
            const auto used = NonNegative(limit, "currentValue");
            const auto remaining = NonNegative(limit, "remaining");
            if (!percent && used && remaining && std::isfinite(*used + *remaining) && *used + *remaining > 0)
                percent = 100.0 * (*used / (*used + *remaining));
            if (!percent) continue;
            UsageBar bar;
            std::string kind = type == "TIME_LIMIT" ? "Tools" :
                (type == "TOKENS_LIMIT" || type == "CREDIT_LIMIT") ? "Coding credits" : type;
            const auto period = PeriodLabel(limit);
            bar.label = period.empty() ? kind : period + " " + kind;
            bar.sharedRateLimit = type == "TOKENS_LIMIT" || type == "CREDIT_LIMIT";
            bar.resetAtUnixSeconds = Epoch(limit, "nextResetTime");
            // Reset times move on every refresh; they are display state, not
            // the identity of the coding window used by widget drag/drop.
            bar.identity = "coding:" + Json::array({ type, Number(limit, "unit").value_or(0),
                Number(limit, "number").value_or(0) }).dump();
            if (!seen.insert(bar.identity).second) continue;
            bar.usedPercent = static_cast<float>(std::clamp(*percent, 0.0, 100.0));
            if (remaining) bar.sublabel = Count(*remaining) + " left";
            else if (used) bar.sublabel = Count(*used) + " used";
            SetStyle(bar);
            snapshot.bars.push_back(std::move(bar));
        }
        if (snapshot.bars.size() == before) return false;
        const auto level = String(*data, "level");
        snapshot.plan = "Z.Ai GLM Coding" + (level.empty() ? "" : " " + level);
        return true;
    }

    inline void ApplySubscription(Snapshot& snapshot, const Json& root)
    {
        const auto* data = Data(root);
        if (!data || !data->is_array()) return;
        for (const auto& item : *data) {
            const auto id = String(item, "productId");
            const auto name = String(item, "productName");
            const auto current = item.find("inCurrentPeriod");
            if (Lower(id + " " + name).find("coding") == std::string::npos ||
                String(item, "status") != "VALID" || current == item.end() ||
                !current->is_boolean() || !current->get<bool>()) continue;
            snapshot.details.push_back({ name.empty() ? id : name, "Subscription", "VALID", "Status" });
            return;
        }
    }

    inline bool ApplyMcpUsage(Snapshot& snapshot, const Json& root)
    {
        const auto* data = Data(root, true);
        if (!data || !data->is_object()) return false;
        const auto usage = data->find("total_usage");
        if (usage == data->end() || !usage->is_object()) return false;
        const auto used = NonNegative(*usage, "used"), limit = NonNegative(*usage, "limit"),
            remaining = NonNegative(*usage, "remaining");
        // Exact integer counts only, with a safe conversion bound.
        constexpr double maxCount = 9007199254740991.0;
        if (!used || !limit || !remaining || *limit <= 0 ||
            *used > maxCount || *limit > maxCount || *remaining > maxCount ||
            std::floor(*used) != *used || std::floor(*limit) != *limit || std::floor(*remaining) != *remaining) return false;
        McpUsage mcp;
        mcp.valid = true;
        mcp.used = static_cast<long long>(*used);
        mcp.limit = static_cast<long long>(*limit);
        mcp.remaining = static_cast<long long>(std::min(*remaining, *limit));
        mcp.level = String(*data, "level");
        mcp.nextRefreshAtUnixSeconds = Epoch(*data, "next_refresh_at");
        snapshot.mcp = std::move(mcp);
        return true;
    }

    inline void FinalizeAccess(Snapshot& snapshot)
    {
        size_t valid = 0, exhausted = 0;
        bool rateLimited = false;
        for (const auto& bar : snapshot.bars) {
            if (!bar.valid) continue;
            ++valid;
            if (UsageTelemetry::IsExhausted(bar.usedPercent)) {
                ++exhausted;
                rateLimited = rateLimited || bar.sharedRateLimit;
            }
        }
        if (!valid) {
            snapshot.access.state = UsageTelemetry::AccessState::Unavailable;
            snapshot.access.detail = "No usable Z.Ai quota was returned";
        }
        else {
            UsageTelemetry::SetAvailable(snapshot.access);
            // A depleted bucket/model must not hide an alternative live balance.
            if (rateLimited) {
                snapshot.access.state = UsageTelemetry::AccessState::RateLimited;
                snapshot.access.detail = "A Coding Plan request window is exhausted; wait for its reset";
            }
            else if (exhausted == valid) {
                snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
                snapshot.access.detail = "All returned usage allocations are exhausted";
            }
            else if (exhausted) snapshot.access.detail = "Some usage allocations are exhausted";
        }
    }
}
