#include "Global.hpp"

#include "ZAi.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

using json = nlohmann::json;

namespace ZAi
{
    // Credential storage, billing parsing, and requests stay private to ZAi.cpp.
    namespace
    {
        namespace Protocol
        {
            using Json = nlohmann::json;
            // Wire formats verified against ZCode 3.16.3, build a82dd43e.
            inline constexpr const char* AppVersion = "3.16.3";
            inline constexpr const char* BalanceUrl =
                "https://zcode.z.ai/api/v1/zcode-plan/billing/balance?app_version=3.16.3";
            inline constexpr const char* McpUrl = "https://zcode.z.ai/api/v1/mcp/usage";
            // Read-only. Its /use and /opportunity siblings spend or roll resets.
            inline constexpr const char* ResetStatusUrl = "https://zcode.z.ai/api/v1/coding-plan/reset/status";
            // Business API host per account family; quota, subscription and
            // project-token paths hang off it.
            inline constexpr const char* ZaiOrigin = "https://api.z.ai";
            inline constexpr const char* BigModelOrigin = "https://bigmodel.cn";
            inline constexpr const char* QuotaPath = "/api/monitor/usage/quota/limit";
            inline constexpr const char* SubscriptionPath = "/api/biz/subscription/list";
            inline constexpr const char* TeamSubscriptionPath = "/api/biz/team/subscribe/product/querySubscribeDetail";

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
                // ZCode accepts a quota envelope with neither code nor success.
                else if (requireCodeZero) return nullptr;
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

            inline std::string DateText(long long unixSeconds)
            {
                return Format::get_instance()->ExpiryTime(std::chrono::system_clock::from_time_t(unixSeconds));
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
                if (!active) {
                    const bool expired = std::any_of(plans.begin(), plans.end(),
                        [&](const Json& plan) { return ExpiredPlan(plan, now); });
                    return { false, false, expired ? "The ZCode Start Plan has expired"
                        : "No active ZCode Start Plan was returned for the signed-in account" };
                }
                const auto planName = String(*active, "name");
                snapshot.plan = "Z.Ai " + (planName.empty() ? "Start Plan" : planName);
                if (const auto ends = Epoch(*active, "ends_at"); ends > 0)
                    snapshot.details.push_back({ planName.empty() ? "Start Plan" : planName, "Plan", DateText(ends), "Expires" });
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
                if (!usable) {
                    // ZCode shows a plan as pending, not empty, until the earliest
                    // entitlement (or the plan itself) takes effect.
                    long long pending = 0;
                    bool future = true;
                    const auto entries = active->find("entitlements");
                    const bool listed = entries != active->end() && entries->is_array() && !entries->empty();
                    for (const auto& entry : listed ? *entries : Json::array({ *active })) {
                        auto at = Epoch(entry, "effective_at");
                        if (at <= 0) at = Epoch(*active, "starts_at");
                        future = future && at > now;
                        if (at > 0 && (!pending || at < pending)) pending = at;
                    }
                    if (future && pending > now)
                        return { true, false, "The Start Plan is pending until " + DateText(pending) };
                }
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

            inline bool ApplyCodingQuota(Snapshot& snapshot, const Json& root, const std::string& planName)
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
                    // Reset times move on every refresh; they are display
                    // state, not the identity of the coding window used by
                    // widget drag/drop.
                    bar.identity = "coding:" + Json::array({ type, Number(limit, "unit").value_or(0),
                        Number(limit, "number").value_or(0) }).dump();
                    if (!seen.insert(bar.identity).second) continue;
                    bar.usedPercent = static_cast<float>(std::clamp(*percent, 0.0, 100.0));
                    if (remaining) bar.sublabel = Count(*remaining) + " left";
                    else if (used) bar.sublabel = Count(*used) + " used";
                    // ZCode lists the models drawing on each non-tool window.
                    std::string models;
                    std::set<std::string> listed;
                    const auto details = limit.find("usageDetails");
                    if (type != "TIME_LIMIT" && details != limit.end() && details->is_array()) {
                        for (const auto& detail : *details) {
                            auto model = String(detail, "displayName");
                            if (model.empty()) model = String(detail, "modelCode");
                            if (model.empty() || !listed.insert(model).second) continue;
                            models += (models.empty() ? "" : ", ") + model;
                        }
                    }
                    if (!models.empty()) bar.sublabel += (bar.sublabel.empty() ? "" : " | ") + models;
                    SetStyle(bar);
                    snapshot.bars.push_back(std::move(bar));
                }
                if (snapshot.bars.size() == before) return false;
                const auto level = String(*data, "level");
                snapshot.plan = planName + (level.empty() ? "" : " (" + level + ")");
                return true;
            }

            // Last date in a subscription "valid" range, e.g.
            // "2026-10-01 00:00:00 - 2026-11-01 00:00:00" -> "2026-11-01".
            inline std::string ValidPeriodEnd(const std::string& valid)
            {
                static const std::regex date(R"(\d{4}-\d{2}-\d{2})");
                std::string last;
                for (std::sregex_iterator it(valid.begin(), valid.end(), date), end; it != end; ++it) last = it->str();
                return last;
            }

            inline void ApplySubscription(Snapshot& snapshot, const Json& root, const std::string& brand)
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
                    const auto title = name.empty() ? id : name;
                    // ZCode titles the plan after the product, e.g. "GLM Coding Pro".
                    snapshot.plan = brand + " " + title;
                    const auto renew = item.find("autoRenew");
                    const bool autoRenew = renew != item.end() && ((renew->is_boolean() && renew->get<bool>()) ||
                        (renew->is_number() && renew->get<double>() == 1.0));
                    auto when = String(item, "nextRenewTime").substr(0, 10);
                    const bool renews = autoRenew && !when.empty();
                    if (when.empty()) when = ValidPeriodEnd(String(item, "valid"));
                    const auto cycle = String(item, "billingCycle");
                    snapshot.details.push_back({ title, cycle.empty() ? "Subscription" : "Subscription (" + cycle + ")",
                        when.empty() ? "VALID" : when, when.empty() ? "Status" : (renews ? "Renews" : "Expires") });
                    return;
                }
            }

            // /api/biz/team/subscribe/product/querySubscribeDetail data:
            //   { hasSubscription, status: EFFECTIVE|EXPIRED, memberGrantStatus: VALID|UNASSIGNED,
            //     productId, productName, subscribeEndTime, subscribePeriod }
            // Returns the reason the team plan is unusable, or empty when it is usable/unknown.
            inline std::string ApplyTeamSubscription(Snapshot& snapshot, const Json& root, const std::string& brand)
            {
                const auto* data = Data(root);
                if (!data || !data->is_object()) return {};
                const auto has = data->find("hasSubscription");
                if (has == data->end() || !has->is_boolean()) return {};
                if (!has->get<bool>()) return "The team has no Coding Plan subscription";
                const auto status = String(*data, "status");
                if (status == "EXPIRED") return "Team plan expired. Contact your team administrator.";
                if (status == "EFFECTIVE" && String(*data, "memberGrantStatus") == "UNASSIGNED")
                    return "Team plan not assigned. Contact your team admin.";
                const auto name = String(*data, "productName");
                const auto title = name.empty() ? String(*data, "productId") : name;
                if (!title.empty()) snapshot.plan = brand + " " + title;
                const auto ends = String(*data, "subscribeEndTime").substr(0, 10);
                const auto period = String(*data, "subscribePeriod");
                snapshot.details.push_back({ title.empty() ? "Team Coding Plan" : title,
                    period.empty() ? "Team subscription" : "Team subscription (" + period + ")",
                    ends.empty() ? status : ends, ends.empty() ? "Status" : "Ends" });
                return {};
            }

            inline bool ApplyMcpUsage(Snapshot& snapshot, const Json& root)
            {
                const auto* data = Data(root, true);
                if (!data || !data->is_object()) return false;
                const auto usage = data->find("total_usage");
                if (usage == data->end() || !usage->is_object()) return false;
                auto used = NonNegative(*usage, "used");
                const auto limit = NonNegative(*usage, "limit"), remaining = NonNegative(*usage, "remaining");
                // ZCode needs only limit and remaining; counts are rounded, with a safe conversion bound.
                constexpr double maxCount = 9007199254740991.0;
                if (!limit || !remaining || *limit <= 0 || *limit > maxCount) return false;
                const double left = std::min(*remaining, *limit);
                if (!used || *used > maxCount) used = *limit - left;
                McpUsage mcp;
                mcp.valid = true;
                mcp.used = std::llround(*used);
                mcp.limit = std::llround(*limit);
                mcp.remaining = std::llround(left);
                mcp.level = String(*data, "level");
                mcp.nextRefreshAtUnixSeconds = Epoch(*data, "next_refresh_at");
                snapshot.mcp = std::move(mcp);
                return true;
            }

            // Banked quota resets from /coding-plan/reset/status:
            //   { code: 0, data: { available_five_hour_resets: [{ expire_at }],
            //                      available_week_resets: [{ expire_at }], ... } }
            inline void ApplyResetStatus(Snapshot& snapshot, const Json& root)
            {
                const auto* data = Data(root, true);
                if (!data || !data->is_object()) return;
                for (const auto& [key, label] : { std::pair<const char*, const char*>{ "available_five_hour_resets", "5-hour quota resets" },
                                                  std::pair<const char*, const char*>{ "available_week_resets", "Weekly quota resets" } }) {
                    const auto list = data->find(key);
                    if (list == data->end() || !list->is_array() || list->empty()) continue;
                    long long soonest = 0;
                    for (const auto& reset : *list) {
                        const auto at = Epoch(reset, "expire_at");
                        if (at > 0 && (!soonest || at < soonest)) soonest = at;
                    }
                    snapshot.details.push_back({ std::to_string(list->size()) + " available", label,
                        soonest ? DateText(soonest) : "", soonest ? "Next expires" : "" });
                }
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

        namespace CredentialFormat
        {
            using Json = nlohmann::json;
            using Decoder = std::function<std::optional<std::string>(const std::string&)>;

            struct Envelope
            {
                std::vector<unsigned char> iv, tag, ciphertext;
            };

            inline std::optional<std::vector<unsigned char>> Base64Url(const std::string& text)
            {
                if (text.empty() || text.size() > 65536) return {};
                size_t end = text.size();
                while (end && text[end - 1] == '=') --end;
                if (text.size() - end > 2 || end % 4 == 1) return {};
                if (end != text.size() && text.size() % 4 != 0) return {};
                unsigned int bits = 0, value = 0;
                std::vector<unsigned char> result;
                result.reserve(end * 3 / 4);
                for (size_t pos = 0; pos < end; ++pos) {
                    const unsigned char c = static_cast<unsigned char>(text[pos]);
                    const int digit = c >= 'A' && c <= 'Z' ? c - 'A' :
                        c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                        c >= '0' && c <= '9' ? c - '0' + 52 :
                        c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : -1;
                    if (digit < 0) return {};
                    value = (value << 6) | static_cast<unsigned int>(digit);
                    bits += 6;
                    if (bits >= 8) { bits -= 8; result.push_back(static_cast<unsigned char>((value >> bits) & 255)); }
                }
                if (bits && (value & ((1u << bits) - 1u)) != 0) return {};
                return result;
            }

            inline std::optional<Envelope> ParseEnvelope(const std::string& text)
            {
                if (text.rfind("enc:v1:", 0) != 0 || text.size() > 65536) return {};
                const size_t first = text.find('.', 7);
                const size_t second = first == std::string::npos ? first : text.find('.', first + 1);
                if (first == std::string::npos || second == std::string::npos || text.find('.', second + 1) != std::string::npos) return {};
                auto iv = Base64Url(text.substr(7, first - 7));
                auto tag = Base64Url(text.substr(first + 1, second - first - 1));
                // ZCode 3.16 writes an encrypted empty string as "enc:v1:<iv>.<tag>.".
                const auto body = text.substr(second + 1);
                auto ciphertext = body.empty() ? std::optional<std::vector<unsigned char>>(std::vector<unsigned char>{}) : Base64Url(body);
                if (!iv || iv->size() != 12 || !tag || tag->size() != 16 || !ciphertext) return {};
                return Envelope{ std::move(*iv), std::move(*tag), std::move(*ciphertext) };
            }

            inline std::string Token(std::string text)
            {
                text = Protocol::Trim(std::move(text));
                if (text.size() > 7 && Protocol::Lower(text.substr(0, 6)) == "bearer" &&
                    (text[6] == ' ' || text[6] == '\t')) text = Protocol::Trim(text.substr(7));
                if (text.size() < 16 || text.size() > 16384 || Protocol::Lower(text).rfind("enc:", 0) == 0) return {};
                for (unsigned char ch : text) if (ch < 33 || ch > 126) return {};
                return text;
            }

            inline std::string EncodeComponent(const std::string& text)
            {
                static constexpr char hex[] = "0123456789ABCDEF";
                std::string result;
                for (unsigned char ch : text) {
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
                        ch == '!' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') result += static_cast<char>(ch);
                    else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
                }
                return result;
            }

            inline std::string DecodeComponent(const std::string& text)
            {
                std::string result;
                for (size_t i = 0; i < text.size(); ++i) {
                    if (text[i] == '%' && i + 2 < text.size() &&
                        std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                        result += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
                        i += 2;
                    }
                    else result += text[i];
                }
                return result;
            }

            enum class PlanPreference { Automatic, Start, Individual, Team };

            // Where ZCode 3.16 keeps a Coding Plan API key; it mints a short-lived
            // project access token from this instead of storing the key itself.
            struct ProjectLocation
            {
                std::string organizationId, projectId, apiKeyId;
            };

            struct TeamPlan
            {
                std::string organizationId, projectId;
                std::optional<ProjectLocation> location;
            };

            struct Selection
            {
                std::string startPlanJwt;
                std::string oauthAccessToken;
                std::vector<std::string> codingApiKeys;
                std::string diagnostic;
                bool currentZaiAccount = false; // Signed in to Z.ai or BigModel.
                PlanPreference preferredPlan = PlanPreference::Automatic;
                std::string family = "zai";     // oauth:active_provider: "zai" or "bigmodel".
                std::string accountId;          // Signed-in profile id (Z.ai user_id).
                std::optional<ProjectLocation> projectLocation;
                std::optional<TeamPlan> team;
            };

            inline std::string BusinessOrigin(const std::string& family)
            {
                return family == "bigmodel" ? Protocol::BigModelOrigin : Protocol::ZaiOrigin;
            }

            inline std::string Brand(const std::string& family)
            {
                return family == "bigmodel" ? "BigModel" : "Z.Ai";
            }

            inline std::string AccountIdentity(const Json& profile)
            {
                if (!profile.is_object()) return {};
                // OAuthCredentialRepo persists raw Z.Ai data.user (user_id), but
                // older stores contain the normalized id/username/displayName profile.
                const bool normalized = profile.contains("id") && profile["id"].is_string() &&
                    profile.contains("username") && profile["username"].is_string() &&
                    profile.contains("displayName") && profile["displayName"].is_string();
                const char* key = normalized || !profile.contains("user_id") ? "id" : "user_id";
                auto identity = Protocol::String(profile, key);
                const auto value = profile.find(key);
                if (identity.empty() && value != profile.end() && value->is_number_integer()) identity = value->dump();
                return identity == "unknown" ? std::string{} : identity;
            }

            inline PlanPreference ReadPlanPreference(const Json& setting, const std::string& family,
                std::optional<TeamPlan>& team)
            {
                if (!setting.is_object()) return PlanPreference::Automatic;
                const auto selections = setting.find("providerFamilyConnectionSelections");
                if (selections != setting.end()) {
                    // The structured selection is authoritative. Do not resurrect a
                    // stale legacy selection when this field is present but empty.
                    if (!selections->is_object()) return PlanPreference::Automatic;
                    const auto selected = selections->find(family);
                    const auto kind = selected == selections->end() ? std::string{} : Protocol::String(*selected, "kind");
                    if (kind == "individual-coding-plan") return PlanPreference::Individual;
                    if (kind == "start-plan") return PlanPreference::Start;
                    if (kind == "team-coding-plan") {
                        // { kind, productId, organizationId, projectId }
                        TeamPlan plan{ Protocol::String(*selected, "organizationId"), Protocol::String(*selected, "projectId") };
                        if (!plan.organizationId.empty() && !plan.projectId.empty()) {
                            team = std::move(plan);
                            return PlanPreference::Team;
                        }
                    }
                    return PlanPreference::Automatic;
                }
                const auto modes = setting.find("modelProviderFamilyModes");
                if (modes != setting.end() && Protocol::String(*modes, family.c_str()) == "apiKey")
                    return PlanPreference::Automatic;
                const auto keys = setting.find("modelProviderFamilySelectedKeys");
                const auto key = keys == setting.end() ? std::string{} : Protocol::String(*keys, family.c_str());
                if (key == "coding-plan:builtin:" + family + "-coding-plan") return PlanPreference::Individual;
                if (key == "coding-plan:builtin:" + family + "-start-plan") return PlanPreference::Start;
                return PlanPreference::Automatic;
            }

            inline bool Enabled(const Json& provider)
            {
                const auto it = provider.find("enabled");
                return it != provider.end() && it->is_boolean() && it->get<bool>();
            }

            inline Selection Select(const Json& store, const Json& config, const Decoder& decode,
                bool allowLegacyConfig = true, const Json& setting = Json::object())
            {
                Selection result;
                bool decryptFailed = false;
                auto read = [&](const std::string& key) -> std::string {
                    if (!store.is_object()) return {};
                    const auto it = store.find(key);
                    if (it == store.end() || !it->is_string()) return {};
                    auto value = decode(it->get<std::string>());
                    if (!value) { decryptFailed = true; return {}; }
                    return Protocol::Trim(std::move(*value));
                };
                const bool marker = store.is_object() && store.contains("oauth:active_provider");
                bool modernStore = marker;
                if (store.is_object()) {
                    for (auto it = store.begin(); it != store.end(); ++it)
                        modernStore = modernStore || it.key().rfind("oauth:", 0) == 0 || it.key().rfind("account-provider:", 0) == 0;
                }
                if (marker) {
                    const auto active = read("oauth:active_provider");
                    result.currentZaiAccount = active == "zai" || active == "bigmodel";
                    if (!result.currentZaiAccount) {
                        result.diagnostic = decryptFailed ? "Could not decrypt ZCode credentials. Run under the same Windows account and ZCODE_CREDENTIAL_SECRET as ZCode."
                            : "The active ZCode account is not Z.Ai or BigModel. Sign in to Z.Ai in ZCode.";
                        return result;
                    }
                    result.family = active;
                }
                else if (modernStore) {
                    result.diagnostic = "ZCode has no active signed-in account. Sign in to Z.Ai in ZCode.";
                    return result; // Never resurrect a signed-out account from cached API keys/config.
                }
                std::optional<TeamPlan> team;
                const auto settingPreference = ReadPlanPreference(setting, result.family, team);
                result.startPlanJwt = Token(read("zcodejwttoken"));
                if (!modernStore && result.startPlanJwt.empty()) result.startPlanJwt = Token(read("zcodeJwtToken"));
                if (result.currentZaiAccount) {
                    const auto prefix = "oauth:" + result.family + ":";
                    result.oauthAccessToken = Token(read(prefix + "access_token"));
                    const auto profileText = read(prefix + "user_info");
                    const auto profile = Json::parse(profileText, nullptr, false);
                    const auto identity = AccountIdentity(profile);
                    result.accountId = identity;
                    if (identity.empty())
                        result.diagnostic = "The signed-in " + Brand(result.family) +
                            " profile has no usable user_id/id. Reopen ZCode and refresh the Coding Plan.";
                    if (!identity.empty()) {
                        // ZCode 3.14 stored the key itself. Do not enumerate keys from
                        // other accounts, team projects, or providers.
                        const auto key = "account-provider:coding-plan:account:" + result.family +
                            "-individual-coding-plan:account:" + EncodeComponent(identity) + ":api-key";
                        const auto token = Token(read(key));
                        if (!token.empty()) result.codingApiKeys.push_back(token);
                        // ZCode 3.16 stores only where the key lives:
                        //   project-key-location:v1:<encodeURIComponent(JSON
                        //     [origin, family, accountId, team|null, 1, "non-team"|null])>
                        //   -> encrypted { organizationId, projectId, apiKeyId }
                        const std::string locationPrefix = "project-key-location:v1:";
                        const auto origin = BusinessOrigin(result.family);
                        for (auto it = store.begin(); it != store.end(); ++it) {
                            if (it.key().rfind(locationPrefix, 0) != 0) continue;
                            const auto scope = Json::parse(DecodeComponent(it.key().substr(locationPrefix.size())), nullptr, false);
                            if (!scope.is_array() || scope.size() < 4 || scope[0] != origin ||
                                scope[1] != result.family || scope[2] != identity) continue;
                            const auto value = Json::parse(read(it.key()), nullptr, false);
                            ProjectLocation location{ Protocol::String(value, "organizationId"),
                                Protocol::String(value, "projectId"), Protocol::String(value, "apiKeyId") };
                            if (location.organizationId.empty() || location.projectId.empty() || location.apiKeyId.empty()) continue;
                            if (scope[3].is_null()) {
                                if (!result.projectLocation) result.projectLocation = std::move(location);
                            }
                            else if (team && location.organizationId == team->organizationId &&
                                location.projectId == team->projectId) team->location = std::move(location);
                        }
                    }
                }
                const auto providers = config.find("provider");
                if (config.is_object() && providers != config.end() && providers->is_object()) {
                    bool personal = false, startSelected = false;
                    for (auto it = providers->begin(); it != providers->end(); ++it) {
                        if (!it->is_object() || !Enabled(*it)) continue;
                        const bool coding = it.key() == "account:zai-individual-coding-plan" || it.key() == "builtin:zai-coding-plan";
                        const bool start = it.key() == "account:zai-start-plan" || it.key() == "builtin:zai-start-plan";
                        personal = personal || coding;
                        startSelected = startSelected || start;
                        // Legacy, explicitly enabled Z.Ai providers only. Never scan arbitrary
                        // JSON tokens, refresh tokens, transcripts, or other vendors' API keys.
                        if (modernStore || !allowLegacyConfig || (!coding && !start)) continue;
                        const auto options = it->find("options");
                        const Json& values = options != it->end() && options->is_object() ? *options : *it;
                        std::string raw = Protocol::String(values, "apiKey");
                        if (raw.empty()) raw = Protocol::String(values, "api_key");
                        const auto decoded = raw.empty() ? std::optional<std::string>{} : decode(raw);
                        const auto token = decoded ? Token(*decoded) : std::string{};
                        if (token.empty()) continue;
                        if (start && result.startPlanJwt.empty()) result.startPlanJwt = token;
                        if (coding && std::find(result.codingApiKeys.begin(), result.codingApiKeys.end(), token) == result.codingApiKeys.end())
                            result.codingApiKeys.push_back(token);
                    }
                    // Legacy enabled flags are only an ordering hint, never proof
                    // of a subscription and never a gate on the other plan's key.
                    if (personal != startSelected)
                        result.preferredPlan = personal ? PlanPreference::Individual : PlanPreference::Start;
                }
                if (setting.contains("providerFamilyConnectionSelections") ||
                    setting.contains("modelProviderFamilySelectedKeys") || setting.contains("modelProviderFamilyModes"))
                    result.preferredPlan = settingPreference;
                result.team = std::move(team);
                if (decryptFailed) result.diagnostic = "One or more ZCode credentials could not be decrypted. Check the Windows account and ZCODE_CREDENTIAL_SECRET used by ZCode.";
                return result;
            }
        }

        namespace Credentials
        {
            namespace
            {
                std::wstring Environment(const wchar_t* name)
                {
                    DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
                    if (!count || count > 65536) return {};
                    std::wstring value(count, L'\0');
                    DWORD actual = GetEnvironmentVariableW(name, value.data(), count);
                    if (!actual || actual >= count) return {};
                    value.resize(actual);
                    return value;
                }

                std::string Utf8(const std::wstring& text)
                {
                    if (text.empty()) return {};
                    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
                    if (!count) return {};
                    std::string result(static_cast<size_t>(count), '\0');
                    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr)) return {};
                    return result;
                }

                std::wstring TrimPath(std::wstring text)
                {
                    const auto first = text.find_first_not_of(L" \t\r\n");
                    return first == std::wstring::npos ? std::wstring{} :
                        text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
                }

                std::filesystem::path OsHome()
                {
                    // Match Node os.homedir() on Windows, not HOME or a migrated data path.
                    const auto profile = Environment(L"USERPROFILE");
                    if (!profile.empty()) return std::filesystem::path(profile);
                    PWSTR folder = nullptr;
                    if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &folder)))
                        throw std::runtime_error("Could not resolve the Windows user profile for ZCode");
                    std::filesystem::path result(folder);
                    CoTaskMemFree(folder);
                    return result;
                }

                Protocol::Json ReadObject(const std::filesystem::path& path)
                {
                    std::error_code ec;
                    const bool exists = std::filesystem::exists(path, ec);
                    if (ec) throw std::runtime_error("Could not access a ZCode settings or credential file");
                    if (!exists) return Protocol::Json::object();
                    const auto size = std::filesystem::file_size(path, ec);
                    if (ec || size > 8 * 1024 * 1024) throw std::runtime_error("ZCode JSON file is unreadable or too large");
                    std::ifstream input(path, std::ios::binary);
                    if (!input) throw std::runtime_error("Could not read a ZCode settings or credential file");
                    std::string text(static_cast<size_t>(size), '\0');
                    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size())))
                        throw std::runtime_error("ZCode JSON file changed while reading; refresh again");
                    auto root = Protocol::Json::parse(text, nullptr, false);
                    if (!text.empty()) SecureZeroMemory(text.data(), text.size());
                    if (!root.is_object()) throw std::runtime_error("ZCode settings or credentials contain invalid JSON");
                    return root;
                }

                std::string DefaultSecret()
                {
                    auto overrideSecret = Environment(L"ZCODE_CREDENTIAL_SECRET");
                    if (!overrideSecret.empty()) return Utf8(overrideSecret); // No trim: ZCode hashes the exact value.
                    std::array<wchar_t, 257> buffer{};
                    DWORD size = static_cast<DWORD>(buffer.size());
                    const std::string username = GetUserNameW(buffer.data(), &size)
                        ? Utf8(std::wstring(buffer.data())) : "unknown";
                    return "zcode-credential-fallback:win32:" + Utf8(OsHome().wstring()) + ":" + username;
                }

                struct Algorithm
                {
                    BCRYPT_ALG_HANDLE handle = nullptr;
                    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
                };
                struct Hash
                {
                    BCRYPT_HASH_HANDLE handle = nullptr;
                    ~Hash() { if (handle) BCryptDestroyHash(handle); }
                };
                struct Key
                {
                    BCRYPT_KEY_HANDLE handle = nullptr;
                    ~Key() { if (handle) BCryptDestroyKey(handle); }
                };
                struct SecretBytes
                {
                    std::vector<unsigned char> value;
                    explicit SecretBytes(size_t size) : value(size) {}
                    ~SecretBytes() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
                };
            }

            std::optional<std::string> Decrypt(const std::string& value, const std::string& secret)
            {
                if (value.rfind("enc:", 0) != 0) return value; // Legacy plaintext store.
                const auto parsed = CredentialFormat::ParseEnvelope(value);
                if (!parsed || secret.empty() || secret.size() > 65536) return {};
                // An empty value cannot carry a credential; skip BCrypt's null-buffer path.
                if (parsed->ciphertext.empty()) return std::string{};
                // enc:v1:base64url(iv).base64url(tag).base64url(ciphertext), SHA256(secret).
                Algorithm sha, aes;
                if (BCryptOpenAlgorithmProvider(&sha.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
                    BCryptOpenAlgorithmProvider(&aes.handle, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return {};
                DWORD hashObjectSize = 0, keyObjectSize = 0, received = 0;
                if (BCryptGetProperty(sha.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjectSize),
                    sizeof(hashObjectSize), &received, 0) < 0) return {};
                SecretBytes hashObject(hashObjectSize), digest(32);
                Hash hash;
                if (BCryptCreateHash(sha.handle, &hash.handle, hashObject.value.data(), hashObjectSize, nullptr, 0, 0) < 0 ||
                    BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(const_cast<char*>(secret.data())),
                        static_cast<ULONG>(secret.size()), 0) < 0 ||
                    BCryptFinishHash(hash.handle, digest.value.data(), 32, 0) < 0) return {};
                if (BCryptSetProperty(aes.handle, BCRYPT_CHAINING_MODE,
                    reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0 ||
                    BCryptGetProperty(aes.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&keyObjectSize),
                        sizeof(keyObjectSize), &received, 0) < 0) return {};
                SecretBytes keyObject(keyObjectSize);
                Key key;
                if (BCryptGenerateSymmetricKey(aes.handle, &key.handle, keyObject.value.data(), keyObjectSize,
                    digest.value.data(), static_cast<ULONG>(digest.value.size()), 0) < 0) return {};
                BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
                BCRYPT_INIT_AUTH_MODE_INFO(info);
                info.pbNonce = const_cast<PUCHAR>(parsed->iv.data());
                info.cbNonce = static_cast<ULONG>(parsed->iv.size());
                info.pbTag = const_cast<PUCHAR>(parsed->tag.data());
                info.cbTag = static_cast<ULONG>(parsed->tag.size());
                SecretBytes plaintext(parsed->ciphertext.size());
                ULONG length = 0;
                const auto status = BCryptDecrypt(key.handle, const_cast<PUCHAR>(parsed->ciphertext.data()),
                    static_cast<ULONG>(parsed->ciphertext.size()), &info, nullptr, 0, plaintext.value.data(),
                    static_cast<ULONG>(plaintext.value.size()), &length, 0);
                // Authentication failure must never return plaintext or fall back to ciphertext.
                if (status < 0 || length > plaintext.value.size()) return {};
                return std::string(reinterpret_cast<const char*>(plaintext.value.data()), length);
            }

            std::filesystem::path SettingsPath()
            {
                auto home = TrimPath(Environment(L"ZCODE_DESKTOP_HOME_DIR"));
                if (home.empty()) home = TrimPath(Environment(L"HOME"));
                if (home.empty()) home = TrimPath(Environment(L"USERPROFILE"));
                if (home.empty()) home = OsHome().wstring();
                return std::filesystem::path(home) / L".zcode" / L"v2" / L"setting.json";
            }

            std::filesystem::path DataRoot(const Protocol::Json& setting)
            {
                const auto configured = Protocol::String(setting, "dataBaseDir");
                if (!configured.empty())
                    return std::filesystem::path(std::u8string(configured.begin(), configured.end())) / L".zcode";
                auto base = TrimPath(Environment(L"ZCODE_DATA_BASE_DIR"));
                if (base.empty()) base = TrimPath(Environment(L"HOME"));
                if (base.empty()) base = OsHome().wstring();
                return std::filesystem::path(base) / L".zcode";
            }

            std::filesystem::path DataRoot()
            {
                return DataRoot(ReadObject(SettingsPath()));
            }

            CredentialFormat::Selection Load()
            {
                CredentialFormat::Selection result;
                try {
                    // Read plan selection from the same home setting.json that
                    // resolves dataBaseDir, not from the migrated credential directory.
                    const auto setting = ReadObject(SettingsPath());
                    const auto root = DataRoot(setting) / L"v2";
                    const auto store = ReadObject(root / L"credentials.json");
                    auto config = Protocol::Json::object();
                    std::string configWarning;
                    try { config = ReadObject(root / L"config.json"); }
                    catch (...) { configWarning = "ZCode config.json could not be read; using only the active credential store."; }
                    auto secret = DefaultSecret();
                    // An existing empty store represents a signed-out desktop, not a
                    // reason to recover stale provider keys from config.json.
                    const bool allowLegacyConfig = !std::filesystem::exists(root / L"credentials.json");
                    result = CredentialFormat::Select(store, config,
                        [&](const std::string& value) { return Decrypt(value, secret); }, allowLegacyConfig, setting);
                    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size());
                    if (result.diagnostic.empty()) result.diagnostic = std::move(configWarning);
                }
                catch (const std::exception& error) { result.diagnostic = error.what(); }
                return result;
            }
        }

        namespace Client
        {
            struct Request
            {
                std::string url;
                std::string token;
                bool bearer = true;
                std::vector<std::pair<std::string, std::string>> extraHeaders;
                std::string method = "GET";
                std::string body;
            };
            struct Response
            {
                int status = 0;
                std::string body;
                long long serverUnixSeconds = 0;
            };

            // A minted project token lives about an hour. Reuse it until two
            // minutes before expiry, as ZCode does, instead of minting per refresh.
            struct CachedToken
            {
                std::string token;
                long long refreshAt = 0;
            };
            inline std::mutex g_tokenMutex;
            inline std::unordered_map<std::string, CachedToken> g_tokens;
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
                std::vector<std::string> planErrors;
                int errorRank = -1;
                UsageTelemetry::AccessStatus failure;
                auto fail = [&](const std::string& text, int status = 0, int rank = 1) {
                    if (std::find(planErrors.begin(), planErrors.end(), text) == planErrors.end())
                        planErrors.push_back(text);
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
                using CredentialFormat::ProjectLocation;
                using CredentialFormat::TeamPlan;
                const auto origin = CredentialFormat::BusinessOrigin(credentials.family);
                const auto brand = CredentialFormat::Brand(credentials.family);
                const bool login = credentials.currentZaiAccount && !credentials.oauthAccessToken.empty() &&
                    !credentials.accountId.empty();
                const auto keysUrl = [&](const std::string& organization, const std::string& project) {
                    return origin + "/api/biz/v1/organization/" + CredentialFormat::EncodeComponent(organization) +
                        "/projects/" + CredentialFormat::EncodeComponent(project) + "/api_keys";
                };
                const auto text = [](const Protocol::Json& item, const char* key) {
                    const auto it = item.is_object() ? item.find(key) : item.end();
                    if (!item.is_object() || it == item.end() || it->is_null()) return std::string{};
                    return Protocol::Trim(it->is_string() ? it->get<std::string>() : it->dump());
                };
                // Read-only discovery of the key ZCode mints from, for a location it
                // has not stored yet. Never POSTs a new key the way ZCode would.
                auto findLocation = [&](const TeamPlan* team, const std::string& name) -> std::optional<ProjectLocation> {
                    const auto customer = request({ origin + "/api/biz/customer/getCustomerInfo",
                        credentials.oauthAccessToken, false, {} }, name + " organizations", true);
                    const auto* info = Protocol::Data(customer);
                    const auto orgs = info && info->is_object() ? info->find("organizations") : Protocol::Json::const_iterator{};
                    if (!info || !info->is_object() || orgs == info->end() || !orgs->is_array()) return {};
                    std::string organization, project;
                    bool namedOrganization = false;
                    for (const auto& org : *orgs) {
                        const auto id = text(org, "organizationId");
                        const auto projects = org.is_object() ? org.find("projects") : org.end();
                        if (id.empty() || !org.is_object() || projects == org.end() || !projects->is_array()) continue;
                        if (team) {
                            for (const auto& item : *projects)
                                if (id == team->organizationId && text(item, "projectId") == team->projectId)
                                    organization = id, project = team->projectId;
                            continue;
                        }
                        // ZCode's personal project: skip team projects (type 2), prefer
                        // the default organization and project by name, else the first.
                        std::string first, named;
                        for (const auto& item : *projects) {
                            const auto projectId = text(item, "projectId");
                            if (projectId.empty() || text(item, "projectType") == "2") continue;
                            if (first.empty()) first = projectId;
                            if (named.empty() && text(item, "projectName").find("\xE9\xBB\x98\xE8\xAE\xA4\xE9\xA1\xB9\xE7\x9B\xAE") != std::string::npos)
                                named = projectId; // "默认项目"
                        }
                        const bool isNamed = text(org, "organizationName").find("\xE9\xBB\x98\xE8\xAE\xA4\xE6\x9C\xBA\xE6\x9E\x84") != std::string::npos; // "默认机构"
                        if (first.empty() || namedOrganization || (!organization.empty() && !isNamed)) continue;
                        organization = id;
                        project = named.empty() ? first : named;
                        namedOrganization = isNamed;
                    }
                    if (organization.empty()) return {};
                    Request list{ keysUrl(organization, project), credentials.oauthAccessToken, !team && credentials.family == "zai", {} };
                    if (team) list.extraHeaders = { { "bigmodel-organization", organization }, { "bigmodel-project", project } };
                    const auto keyList = request(list, name + " API keys", true);
                    const auto* keys = Protocol::Data(keyList);
                    if (!keys || !keys->is_array()) return {};
                    for (const auto& key : *keys) {
                        if (text(key, "name") != (team ? "zcode-team-api-key" : "zcode-api-key") ||
                            (team && text(key, "keyType") != "2")) continue;
                        if (const auto id = text(key, "apiKey"); !id.empty()) return ProjectLocation{ organization, project, id };
                    }
                    return {};
                };
                // ZCode 3.16 sends a short-lived project access token, not a stored key:
                //   POST .../api_keys/{apiKeyId}/access_tokens {"clientType":"zcode","clientVersion"}
                //   -> { code: 0|200, data: { enable, accessToken, tokenType, expiresIn, expiresAt } }
                auto projectToken = [&](const ProjectLocation& location, const std::string& name) -> std::string {
                    const auto scope = Protocol::Json::array({ origin, credentials.accountId, location.organizationId,
                        location.projectId, location.apiKeyId, credentials.oauthAccessToken }).dump();
                    {
                        std::lock_guard<std::mutex> lock(g_tokenMutex);
                        const auto cached = g_tokens.find(scope);
                        if (cached != g_tokens.end() && options.now < cached->second.refreshAt) return cached->second.token;
                    }
                    Request mint{ keysUrl(location.organizationId, location.projectId) + "/" +
                        CredentialFormat::EncodeComponent(location.apiKeyId) + "/access_tokens",
                        credentials.oauthAccessToken, true, {}, "POST",
                        std::string(R"({"clientType":"zcode","clientVersion":")") + Protocol::AppVersion + "\"}" };
                    const auto root = request(mint, name + " project token", false);
                    const auto* data = Protocol::Data(root);
                    if (!data || !data->is_object()) {
                        if (root.is_object()) fail(name + " project token request was unsuccessful", 0, 2);
                        return {};
                    }
                    const auto enable = data->find("enable");
                    if (enable != data->end() && enable->is_boolean() && !enable->get<bool>()) {
                        fail(name + " API key is disabled", 0, 2);
                        return {};
                    }
                    const auto token = CredentialFormat::Token(Protocol::String(*data, "accessToken"));
                    long long expiry = Protocol::Epoch(*data, "expiresAt");
                    if (const auto in = Protocol::NonNegative(*data, "expiresIn"); in && *in < 1e9 &&
                        (!expiry || options.now + static_cast<long long>(*in) < expiry))
                        expiry = options.now + static_cast<long long>(*in);
                    if (token.empty() || expiry <= options.now) {
                        fail(name + " project token response was invalid", 0, 2);
                        return {};
                    }
                    std::lock_guard<std::mutex> lock(g_tokenMutex);
                    g_tokens[scope] = { token, expiry - 120 };
                    return token;
                };
                auto officialMcp = [&](Snapshot& result, const TeamPlan* team) {
                    // Called only after this account's Coding Plan quota succeeds.
                    // Bind MCP to that scope, including a fallback from Start;
                    // legacy provider enabled flags must not hide its quota. Never combine
                    // cached OAuth credentials with another account's explicit key/JWT.
                    if (!credentials.currentZaiAccount || credentials.startPlanJwt.empty() ||
                        credentials.oauthAccessToken.empty()) return;
                    Request query{ Protocol::McpUrl, credentials.startPlanJwt, true, {
                        { "X-Bigmodel-Authorization", "Bearer " + credentials.oauthAccessToken }
                    } };
                    // Z.ai team plans send no target scope; BigModel teams name theirs.
                    if (!team) query.extraHeaders.push_back({ "Bigmodel-Target-Type", "PERSONAL" });
                    else if (credentials.family == "bigmodel") query.extraHeaders.insert(query.extraHeaders.end(), {
                        { "Bigmodel-Target-Type", "TEAM" }, { "Bigmodel-Organization", team->organizationId },
                        { "Bigmodel-Project", team->projectId } });
                    Protocol::ApplyMcpUsage(result, request(query, "MCP usage", true));
                };
                auto resets = [&](Snapshot& result, const TeamPlan* team) {
                    if (!credentials.currentZaiAccount || credentials.startPlanJwt.empty() ||
                        credentials.oauthAccessToken.empty()) return;
                    // Unlike MCP, this endpoint takes the OAuth token without "Bearer".
                    Request query{ Protocol::ResetStatusUrl, credentials.startPlanJwt, true, {
                        { "X-Bigmodel-Authorization", credentials.oauthAccessToken },
                        { "Bigmodel-Target-Type", team ? "TEAM" : "PERSONAL" }
                    } };
                    if (team) query.extraHeaders.insert(query.extraHeaders.end(), {
                        { "Bigmodel-Organization", team->organizationId }, { "Bigmodel-Project", team->projectId } });
                    Protocol::ApplyResetStatus(result, request(query, "Quota resets", true));
                };
                auto quota = [&](const std::string& token, const TeamPlan* team, const std::string& name) -> bool {
                    // Team quota is the same endpoint scoped by ?type=2 and project headers.
                    Request query{ origin + Protocol::QuotaPath + (team ? "?type=2" : ""), token, false, {} };
                    if (team) query.extraHeaders = { { "bigmodel-organization", team->organizationId },
                        { "bigmodel-project", team->projectId } };
                    const auto root = request(query, name + " quota", false);
                    if (!root.is_object()) return false;
                    Snapshot candidate;
                    candidate.lastUpdated = "now";
                    if (!Protocol::ApplyCodingQuota(candidate, root, brand + " " + name)) {
                        fail(Protocol::Data(root) ? name + " returned no usable quota windows"
                            : name + " returned an unsuccessful quota envelope", 0, Protocol::Data(root) ? 1 : 2);
                        return false;
                    }
                    if (team) {
                        Request detail{ origin + Protocol::TeamSubscriptionPath, credentials.oauthAccessToken,
                            credentials.family == "zai", { { "bigmodel-organization", team->organizationId },
                            { "bigmodel-project", team->projectId } } };
                        const auto reason = Protocol::ApplyTeamSubscription(candidate,
                            request(detail, name + " subscription", true), brand);
                        if (!reason.empty()) { fail(reason, 0, 2); return false; }
                    }
                    else Protocol::ApplySubscription(candidate, request({ origin + Protocol::SubscriptionPath, token, false, {} },
                        name + " subscription", true), brand);
                    candidate.statusText = "Source: " + brand + " " + name + " quota";
                    officialMcp(candidate, team);
                    resets(candidate, team);
                    Protocol::FinalizeAccess(candidate);
                    snapshot = std::move(candidate);
                    return true;
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
                    // The minted project token first (ZCode 3.16+), then any stored
                    // 3.14 key or explicit ZAI_CODING_API_KEY.
                    std::vector<std::string> keys;
                    if (login) {
                        auto location = credentials.projectLocation;
                        if (!location) location = findLocation(nullptr, "Individual Plan");
                        if (location) {
                            if (auto token = projectToken(*location, "Individual Plan"); !token.empty()) keys.push_back(std::move(token));
                        }
                    }
                    for (const auto& key : credentials.codingApiKeys)
                        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
                    if (keys.empty()) {
                        if (credentials.currentZaiAccount || !credentials.startPlanJwt.empty())
                            fail("Individual Plan API key was not found for the signed-in account. Open Individual Plan in ZCode and refresh.");
                        return false;
                    }
                    for (const auto& key : keys)
                        if (quota(key, nullptr, "Individual Plan")) return true;
                    return false;
                };
                auto teamPlan = [&]() -> bool {
                    if (!credentials.team) return false;
                    if (!login) { fail("The Team Plan needs the signed-in ZCode account. Sign in again in ZCode."); return false; }
                    auto location = credentials.team->location;
                    if (!location) location = findLocation(&*credentials.team, "Team Plan");
                    if (!location) { fail("Team Plan API key was not found. Open the Team Plan in ZCode and refresh."); return false; }
                    const auto token = projectToken(*location, "Team Plan");
                    return !token.empty() && quota(token, &*credentials.team, "Team Plan");
                };
                // A selected plan is an ordering hint, not an exclusive entitlement.
                // Probe the other supported plan after HTTP/API errors, an inactive plan,
                // missing credentials, or an empty/unusable quota response. Prefer the
                // Individual Plan when no explicit Start or Team Plan selection exists.
                if (credentials.preferredPlan == CredentialFormat::PlanPreference::Team) {
                    if (teamPlan()) return snapshot;
                    if (coding()) return snapshot;
                    if (start()) return snapshot;
                }
                else if (credentials.preferredPlan == CredentialFormat::PlanPreference::Start) {
                    if (start()) return snapshot;
                    if (coding()) return snapshot;
                }
                else {
                    if (coding()) return snapshot;
                    if (start()) return snapshot;
                }
                if (error.empty()) {
                    error = credentials.diagnostic.empty()
                        ? "Z.Ai credentials not found. Sign in and open the plan in ZCode, or set ZCODE_JWT_TOKEN / ZAI_CODING_API_KEY."
                        : credentials.diagnostic;
                    failure.state = UsageTelemetry::AccessState::Unavailable;
                    failure.detail = error;
                }
                else {
                    // Report both failed paths instead of letting the first Start Plan
                    // HTTP 400 mask why the Individual Plan could not be loaded.
                    error.clear();
                    for (const auto& detail : planErrors) {
                        if (!error.empty()) error += "; ";
                        error += detail;
                    }
                    if (!credentials.diagnostic.empty()) error += " " + credentials.diagnostic;
                }
                failure.detail = error;
                snapshot.statusText = "Z.Ai usage unavailable: " + error;
                snapshot.access = std::move(failure);
                return snapshot;
            }
        }
    }
}

namespace ZAi
{
    static std::string CompactLower(std::string text)
    {
        return Text::get_instance()->CompactLower(text);
    }

    struct Sqlite3;
    struct Sqlite3Stmt;

    class DynamicSqlite
    {
    public:
        using OpenV2Fn = int(__cdecl*)(const char*, Sqlite3**, int, const char*);
        using CloseV2Fn = int(__cdecl*)(Sqlite3*);
        using PrepareV2Fn = int(__cdecl*)(Sqlite3*, const char*, int, Sqlite3Stmt**, const char**);
        using StepFn = int(__cdecl*)(Sqlite3Stmt*);
        using FinalizeFn = int(__cdecl*)(Sqlite3Stmt*);
        using ColumnCountFn = int(__cdecl*)(Sqlite3Stmt*);
        using ColumnTextFn = const unsigned char*(__cdecl*)(Sqlite3Stmt*, int);
        using BusyTimeoutFn = int(__cdecl*)(Sqlite3*, int);

        ~DynamicSqlite()
        {
            Close();
            if (m_module) {
                FreeLibrary(m_module);
            }
        }

        bool OpenReadOnly(const std::filesystem::path& path)
        {
            if (!LoadApi()) {
                return false;
            }

            Close();
            const std::string utf8 = WidePathToUtf8(path.wstring());
            if (utf8.empty()) {
                return false;
            }

            constexpr int kSqliteOk = 0;
            constexpr int kOpenReadOnly = 0x00000001;
            constexpr int kOpenNoMutex = 0x00008000;
            const int result = m_openV2(
                utf8.c_str(),
                &m_db,
                kOpenReadOnly | kOpenNoMutex,
                nullptr
            );
            if (result != kSqliteOk || !m_db) {
                Close();
                return false;
            }

            if (m_busyTimeout) {
                m_busyTimeout(m_db, 50);
            }
            return true;
        }

        std::vector<std::vector<std::string>> Query(const std::string& sql) const
        {
            std::vector<std::vector<std::string>> rows;
            if (!m_db || !m_prepareV2 || !m_step || !m_finalize) {
                return rows;
            }

            Sqlite3Stmt* statement = nullptr;
            constexpr int kSqliteOk = 0;
            constexpr int kSqliteRow = 100;
            if (m_prepareV2(m_db, sql.c_str(), -1, &statement, nullptr) != kSqliteOk || !statement) {
                return rows;
            }

            const int columnCount = m_columnCount(statement);
            while (m_step(statement) == kSqliteRow) {
                std::vector<std::string> row;
                row.reserve(static_cast<size_t>(std::max(0, columnCount)));
                for (int column = 0; column < columnCount; ++column) {
                    const unsigned char* value = m_columnText(statement, column);
                    row.emplace_back(value ? reinterpret_cast<const char*>(value) : "");
                }
                rows.push_back(std::move(row));
            }

            m_finalize(statement);
            return rows;
        }

    private:
        static std::string WidePathToUtf8(const std::wstring& value)
        {
            if (value.empty()) {
                return {};
            }

            const int required = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.c_str(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr
            );
            if (required <= 0) {
                return {};
            }

            std::string result(static_cast<size_t>(required), '\0');
            if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.c_str(),
                static_cast<int>(value.size()),
                result.data(),
                required,
                nullptr,
                nullptr
            ) != required) {
                return {};
            }
            return result;
        }

        template <typename T>
        bool LoadProc(T& target, const char* name)
        {
            target = reinterpret_cast<T>(GetProcAddress(m_module, name));
            return target != nullptr;
        }

        bool LoadApi()
        {
            if (m_module) {
                return true;
            }

            // Windows 10/11 ship the system SQLite component as winsqlite3.
            // Fall back to sqlite3.dll for environments that provide their own
            // compatible runtime. Loading dynamically keeps AQC dependency-free.
            m_module = LoadLibraryW(L"winsqlite3.dll");
            if (!m_module) {
                m_module = LoadLibraryW(L"sqlite3.dll");
            }
            if (!m_module) {
                return false;
            }

            const bool required =
                LoadProc(m_openV2, "sqlite3_open_v2") &&
                LoadProc(m_closeV2, "sqlite3_close_v2") &&
                LoadProc(m_prepareV2, "sqlite3_prepare_v2") &&
                LoadProc(m_step, "sqlite3_step") &&
                LoadProc(m_finalize, "sqlite3_finalize") &&
                LoadProc(m_columnCount, "sqlite3_column_count") &&
                LoadProc(m_columnText, "sqlite3_column_text");
            LoadProc(m_busyTimeout, "sqlite3_busy_timeout");

            if (!required) {
                FreeLibrary(m_module);
                m_module = nullptr;
                return false;
            }
            return true;
        }

        void Close()
        {
            if (m_db && m_closeV2) {
                m_closeV2(m_db);
            }
            m_db = nullptr;
        }

        HMODULE m_module = nullptr;
        Sqlite3* m_db = nullptr;
        OpenV2Fn m_openV2 = nullptr;
        CloseV2Fn m_closeV2 = nullptr;
        PrepareV2Fn m_prepareV2 = nullptr;
        StepFn m_step = nullptr;
        FinalizeFn m_finalize = nullptr;
        ColumnCountFn m_columnCount = nullptr;
        ColumnTextFn m_columnText = nullptr;
        BusyTimeoutFn m_busyTimeout = nullptr;
    };

    static long long ParsePositiveLongLong(const std::string& text)
    {
        if (text.empty()) {
            return 0;
        }
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        return end != text.c_str() && value > 0 ? value : 0;
    }

    static long long MillisecondsOrSecondsToUnixSeconds(long long value)
    {
        if (value <= 0) {
            return 0;
        }
        return value > 100000000000LL ? value / 1000LL : value;
    }

    static std::string SqlQuote(std::string value)
    {
        size_t at = 0;
        while ((at = value.find('\'', at)) != std::string::npos) {
            value.insert(at, 1, '\'');
            at += 2;
        }
        return "'" + value + "'";
    }

    static std::optional<double> FindNumberByKeyRecursive(
        const json& value,
        std::initializer_list<const char*> keys,
        int depth = 0
    ) {
        if (depth > 10) {
            return std::nullopt;
        }

        if (value.is_object()) {
            for (const char* key : keys) {
                auto it = value.find(key);
                if (it == value.end()) {
                    continue;
                }
                if (it->is_number()) {
                    return it->get<double>();
                }
                if (it->is_string()) {
                    char* end = nullptr;
                    const std::string text = it->get<std::string>();
                    const double parsed = std::strtod(text.c_str(), &end);
                    if (end != text.c_str() && std::isfinite(parsed)) {
                        return parsed;
                    }
                }
            }
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (auto found = FindNumberByKeyRecursive(it.value(), keys, depth + 1)) {
                    return found;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (auto found = FindNumberByKeyRecursive(item, keys, depth + 1)) {
                    return found;
                }
            }
        }
        return std::nullopt;
    }

    static const json* FindObjectByKeyRecursive(
        const json& value,
        std::initializer_list<const char*> keys,
        int depth = 0
    ) {
        if (depth > 10) {
            return nullptr;
        }
        if (value.is_object()) {
            for (const char* key : keys) {
                auto it = value.find(key);
                if (it != value.end() && it->is_object()) {
                    return &*it;
                }
            }
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (const json* found = FindObjectByKeyRecursive(it.value(), keys, depth + 1)) {
                    return found;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (const json* found = FindObjectByKeyRecursive(item, keys, depth + 1)) {
                    return found;
                }
            }
        }
        return nullptr;
    }

    static const json* FindArrayByKeyRecursive(
        const json& value,
        std::initializer_list<const char*> keys,
        int depth = 0
    ) {
        if (depth > 10) {
            return nullptr;
        }
        if (value.is_object()) {
            for (const char* key : keys) {
                auto it = value.find(key);
                if (it != value.end() && it->is_array()) {
                    return &*it;
                }
            }
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (const json* found = FindArrayByKeyRecursive(it.value(), keys, depth + 1)) {
                    return found;
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                if (const json* found = FindArrayByKeyRecursive(item, keys, depth + 1)) {
                    return found;
                }
            }
        }
        return nullptr;
    }

    static std::string FriendlyZCodeContextSource(std::string source)
    {
        if (source == "system_prompt") return "System prompt";
        if (source == "meta_user_context") return "Meta context";
        if (source == "skills") return "Skills";
        if (source == "tool_prompt") return "Tool prompt";
        if (source == "system_tool_schemas") return "System tools";
        if (source == "mcp_tool_schemas") return "MCP tools";
        if (source == "messages") return "Messages";
        std::replace(source.begin(), source.end(), '_', ' ');
        if (!source.empty()) source.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(source.front())));
        return source;
    }

    static std::string JsonStringLower(const json& value, const char* key) {
        if (!value.is_object()) return {};
        auto it = value.find(key);
        if (it == value.end() || !it->is_string()) return {};
        return Text::get_instance()->ToLowerCopy(it->get<std::string>());
    }

    static std::optional<bool> ZCodeCompactionStateFromJson(
        const json& value,
        int depth = 0
    ) {
        if (depth > 10) return std::nullopt;
        if (value.is_object()) {
            const std::string activeTurnKind = JsonStringLower(value, "activeTurnKind");
            if (activeTurnKind == "compact") return true;
            if (activeTurnKind == "regular" || activeTurnKind == "rewind") return false;

            const std::string type = JsonStringLower(value, "type");
            const std::string status = JsonStringLower(value, "status");
            if (type == "compact") {
                if (status == "running") return true;
                if (status == "success" || status == "failed" || status == "noop" ||
                    status == "cancelled" || status == "completed" || status == "complete") {
                    return false;
                }
            }

            // Runtime state is sometimes nested one level under payload/state.
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!it.value().is_object()) continue;
                if (auto state = ZCodeCompactionStateFromJson(it.value(), depth + 1)) {
                    return state;
                }
            }
        }
        return std::nullopt;
    }

    static void ApplyZCodeRuntimeJson(
        const json& root,
        UsageTelemetry::ContextUsage& context
    ) {
        if (root.is_discarded() || root.is_null()) {
            return;
        }

        std::optional<double> used = FindNumberByKeyRecursive(
            root,
            { "usedTokens", "contextUsed", "context_used" }
        );
        std::optional<double> maximum = FindNumberByKeyRecursive(
            root,
            { "maxTokens", "contextWindow", "context_window", "contextWindowTokens" }
        );
        std::optional<double> compactThreshold = FindNumberByKeyRecursive(
            root,
            { "autoCompactThresholdTokens", "auto_compact_threshold_tokens" }
        );

        // The host UI's normalized runtime form is contextUsage:{used,size}.
        // Only accept those generic names from an actual context object so an
        // unrelated quota/usage {used,size} cannot corrupt the context meter.
        if (const json* contextObject = FindObjectByKeyRecursive(
            root, { "contextWindow", "contextUsage", "context_usage" })) {
            if (!used) {
                used = FindNumberByKeyRecursive(*contextObject, { "usedTokens", "used" });
            }
            if (!maximum) {
                maximum = FindNumberByKeyRecursive(
                    *contextObject, { "maxTokens", "contextWindowTokens", "size" });
            }
            if (!compactThreshold) {
                compactThreshold = FindNumberByKeyRecursive(
                    *contextObject, { "autoCompactThresholdTokens", "auto_compact_threshold_tokens" });
            }
        }

        if (used && *used > 0.0) {
            context.usedTokens = static_cast<long long>(std::floor(*used));
        }
        if (maximum && *maximum > 0.0) {
            context.contextWindowTokens = static_cast<long long>(std::floor(*maximum));
        }
        if (compactThreshold && *compactThreshold > 0.0) {
            context.autoCompactThresholdTokens = static_cast<long long>(std::floor(*compactThreshold));
        }

        if (const json* cache = FindObjectByKeyRecursive(root, { "cache", "cacheHit" })) {
            auto number = [&](std::initializer_list<const char*> keys) -> long long {
                const auto value = FindNumberByKeyRecursive(*cache, keys);
                return value && *value >= 0.0 ? static_cast<long long>(std::floor(*value)) : 0LL;
            };
            context.cacheInputTokens = number({ "inputTokens", "input_tokens" });
            context.cacheReadTokens = number({ "cacheReadTokens", "cachedReadTokens", "cache_read_tokens" });
            context.cacheWriteTokens = number({ "cacheWriteTokens", "cachedWriteTokens", "cache_write_tokens" });
            context.totalCacheInputTokens = number({ "totalInputTokens", "total_input_tokens" });
            context.totalCacheReadTokens = number({ "totalCacheReadTokens", "total_cache_read_tokens" });
            context.totalCacheWriteTokens = number({ "totalCacheWriteTokens", "total_cache_write_tokens" });

            const auto latest = FindNumberByKeyRecursive(*cache, { "latestHitRate", "latest_hit_rate" });
            const auto average = FindNumberByKeyRecursive(*cache, { "hitRate", "cacheHitRate", "hit_rate" });
            if (latest && std::isfinite(*latest)) {
                context.latestCacheHitPercentValid = true;
                context.latestCacheHitPercent = std::clamp(*latest <= 1.0 ? *latest * 100.0 : *latest, 0.0, 100.0);
            }
            if (average && std::isfinite(*average)) {
                context.averageCacheHitPercentValid = true;
                context.averageCacheHitPercent = std::clamp(*average <= 1.0 ? *average * 100.0 : *average, 0.0, 100.0);
            }
            context.cacheStatsValid = context.cacheInputTokens > 0 ||
                context.cacheReadTokens > 0 || context.cacheWriteTokens > 0 ||
                context.latestCacheHitPercentValid || context.averageCacheHitPercentValid;
        }

        if (const json* breakdown = FindArrayByKeyRecursive(root, { "contextUsageBreakdown", "breakdown" })) {
            std::vector<UsageTelemetry::ContextBreakdownEntry> entries;
            long long total = 0;
            for (const json& item : *breakdown) {
                if (!item.is_object()) continue;
                const std::string source = JsonUtils::get_instance()->String(item, "source");
                const auto chars = FindNumberByKeyRecursive(item, { "chars", "value", "tokens" });
                if (source.empty() || !chars || *chars < 0.0) continue;
                UsageTelemetry::ContextBreakdownEntry entry;
                entry.label = FriendlyZCodeContextSource(source);
                entry.value = static_cast<long long>(std::floor(*chars));
                total += entry.value;
                entries.push_back(std::move(entry));
            }
            if (total > 0) {
                for (auto& entry : entries) {
                    entry.percent = (static_cast<double>(entry.value) / static_cast<double>(total)) * 100.0;
                }
                context.breakdown = std::move(entries);
            }
        }
    }

    static long long ZCodeConfiguredContextWindow(const std::string& model)
    {
        if (model.empty()) {
            return 0;
        }
        const std::filesystem::path path =
            Credentials::DataRoot() / "v2" / "config.json";
        const std::string text = Network::get_instance()->ReadTextFile(path);
        if (text.empty()) {
            return 0;
        }

        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            return 0;
        }
        const auto providersIt = root.find("provider");
        if (providersIt == root.end() || !providersIt->is_object()) {
            return 0;
        }

        const std::string wanted = CompactLower(model);
        long long best = 0;
        for (auto provider = providersIt->begin(); provider != providersIt->end(); ++provider) {
            if (!provider.value().is_object()) continue;
            auto models = provider.value().find("models");
            if (models == provider.value().end() || !models->is_object()) continue;
            for (auto item = models->begin(); item != models->end(); ++item) {
                if (!item.value().is_object()) continue;
                const std::string displayName = JsonUtils::get_instance()->String(item.value(), "name");
                if (CompactLower(item.key()) != wanted &&
                    (displayName.empty() || CompactLower(displayName) != wanted)) {
                    continue;
                }
                const auto limit = item.value().find("limit");
                if (limit == item.value().end() || !limit->is_object()) continue;
                const auto contextValue = FindNumberByKeyRecursive(*limit, { "context" });
                if (contextValue && *contextValue > 0.0) {
                    best = std::max(best, static_cast<long long>(std::floor(*contextValue)));
                }
            }
        }
        return best;
    }

    struct ZCodeLocalTelemetry
    {
        UsageTelemetry::ContextUsage context;
        UsageTelemetry::RunUsage run;
        std::string model;
    };

    // ZCode 3.16 lets ZCODE_SESSION_DB_PATH / ZCODE_SESSION_DB, then
    // ~/.zcode/cli/config.json storage.sessionDbPath, move the session database.
    static std::filesystem::path ZCodeSessionDbPath()
    {
        const auto expand = [](std::string text) -> std::filesystem::path {
            text = Protocol::Trim(std::move(text));
            std::filesystem::path path = text.rfind("~/", 0) == 0
                ? Credentials::OsHome() / std::u8string(text.begin() + 2, text.end())
                : std::filesystem::path(std::u8string(text.begin(), text.end()));
            return path.is_absolute() ? path : std::filesystem::path{};
        };
        for (const char* name : { "ZCODE_SESSION_DB_PATH", "ZCODE_SESSION_DB" }) {
            const auto value = Network::get_instance()->GetEnvText(name);
            if (!Protocol::Trim(value).empty()) return expand(value);
        }
        const auto config = json::parse(Network::get_instance()->ReadTextFile(
            Credentials::OsHome() / ".zcode" / "cli" / "config.json"), nullptr, false);
        if (config.is_object() && config.contains("storage") && config.at("storage").is_object()) {
            const auto configured = JsonUtils::get_instance()->String(config.at("storage"), "sessionDbPath");
            if (!Protocol::Trim(configured).empty()) return expand(configured);
        }
        return Credentials::DataRoot() / "cli" / "db" / "db.sqlite";
    }

    static ZCodeLocalTelemetry ReadZCodeLocalTelemetry()
    {
        ZCodeLocalTelemetry local;
        const std::filesystem::path dbPath = ZCodeSessionDbPath();
        std::error_code ec;
        if (dbPath.empty() || !std::filesystem::is_regular_file(dbPath, ec) || ec) {
            return local;
        }

        DynamicSqlite db;
        if (!db.OpenReadOnly(dbPath)) {
            return local;
        }

        const auto sessions = db.Query(
            "SELECT id, COALESCE(time_compacting,0), time_updated "
            "FROM session WHERE time_archived IS NULL AND (parent_id IS NULL OR parent_id='') "
            "ORDER BY time_updated DESC LIMIT 1"
        );
        if (sessions.empty() || sessions.front().empty() || sessions.front()[0].empty()) {
            return local;
        }

        const std::string sessionId = sessions.front()[0];
        const std::string sessionSql = SqlQuote(sessionId);

        auto modelRows = db.Query(
            "SELECT model_id,status,started_at,COALESCE(first_token_at,0),input_tokens,output_tokens,"
            "reasoning_tokens,cache_creation_input_tokens,cache_read_input_tokens,computed_total_tokens,"
            "COALESCE(raw_usage_json,''),COALESCE(provider_metadata_json,''),provider_id,query_source "
            "FROM model_usage WHERE session_id=" + sessionSql +
            " AND (query_source='main_turn' OR query_source='main') "
            "ORDER BY started_at DESC, attempt_index DESC LIMIT 1"
        );
        if (modelRows.empty()) {
            modelRows = db.Query(
                "SELECT model_id,status,started_at,COALESCE(first_token_at,0),input_tokens,output_tokens,"
                "reasoning_tokens,cache_creation_input_tokens,cache_read_input_tokens,computed_total_tokens,"
                "COALESCE(raw_usage_json,''),COALESCE(provider_metadata_json,''),provider_id,query_source "
                "FROM model_usage WHERE session_id=" + sessionSql +
                " ORDER BY started_at DESC, attempt_index DESC LIMIT 1"
            );
        }

        if (!modelRows.empty() && modelRows.front().size() >= 14) {
            const auto& row = modelRows.front();
            local.model = row[0];
            const std::string status = Text::get_instance()->ToLowerCopy(row[1]);
            const long long startedAt = MillisecondsOrSecondsToUnixSeconds(ParsePositiveLongLong(row[2]));
            const long long input = ParsePositiveLongLong(row[4]);
            const long long output = ParsePositiveLongLong(row[5]);
            const long long reasoning = ParsePositiveLongLong(row[6]);
            const long long cacheWrite = ParsePositiveLongLong(row[7]);
            const long long cacheRead = ParsePositiveLongLong(row[8]);
            const long long computed = ParsePositiveLongLong(row[9]);

            local.context.inputTokens = input;
            local.context.outputTokens = output;
            local.context.reasoningOutputTokens = reasoning;
            local.context.cachedInputTokens = cacheRead;
            local.context.usedTokens = input + output;
            local.context.model = local.model;
            local.context.sourceLabel = "ZCode live session database · direct read-only";

            local.context.cacheStatsValid = input > 0 || cacheRead > 0 || cacheWrite > 0;
            local.context.cacheInputTokens = input;
            local.context.cacheReadTokens = cacheRead;
            local.context.cacheWriteTokens = cacheWrite;
            if (input > 0) {
                local.context.latestCacheHitPercentValid = true;
                local.context.latestCacheHitPercent = std::clamp(
                    static_cast<double>(cacheRead) * 100.0 / static_cast<double>(input),
                    0.0,
                    100.0
                );
            }

            local.run.valid = true;
            local.run.running = status == "running";
            local.run.thinking = local.run.running;
            local.run.startedAtUnixSeconds = startedAt;
            local.run.tokenStatsValid = input > 0 || output > 0 || reasoning > 0 || cacheRead > 0 || cacheWrite > 0;
            local.run.currentTokens = output;
            local.run.inputTokens = input;
            local.run.rawInputTokens = input;
            local.run.cacheCreationInputTokens = cacheWrite;
            local.run.cacheReadInputTokens = cacheRead;
            local.run.reasoningOutputTokens = reasoning;
            local.run.tokens = computed > 0 ? computed : input + output;

            for (const std::string& blob : { row[10], row[11] }) {
                if (blob.empty()) continue;
                const json metadata = json::parse(blob, nullptr, false);
                if (!metadata.is_discarded()) {
                    ApplyZCodeRuntimeJson(metadata, local.context);
                }
            }
        }

        // turn_usage has cumulative counters for the whole active user turn.
        // Use them only to enrich a currently-running model request; do not
        // call tool execution between model requests "THINKING".
        const auto turnRows = db.Query(
            "SELECT status,started_at,input_tokens,output_tokens,reasoning_tokens,"
            "cache_creation_input_tokens,cache_read_input_tokens,computed_total_tokens "
            "FROM turn_usage WHERE session_id=" + sessionSql +
            " ORDER BY started_at DESC LIMIT 1"
        );
        if (local.run.running && !turnRows.empty() && turnRows.front().size() >= 8) {
            const auto& row = turnRows.front();
            const std::string turnStatus = Text::get_instance()->ToLowerCopy(row[0]);
            if (turnStatus == "running") {
                const long long started =
                    MillisecondsOrSecondsToUnixSeconds(ParsePositiveLongLong(row[1]));
                if (started > 0) local.run.startedAtUnixSeconds = started;

                const long long input = ParsePositiveLongLong(row[2]);
                const long long output = ParsePositiveLongLong(row[3]);
                const long long reasoning = ParsePositiveLongLong(row[4]);
                const long long cacheWrite = ParsePositiveLongLong(row[5]);
                const long long cacheRead = ParsePositiveLongLong(row[6]);
                const long long computed = ParsePositiveLongLong(row[7]);
                if (input > 0 || output > 0 || reasoning > 0 ||
                    cacheWrite > 0 || cacheRead > 0) {
                    local.run.tokenStatsValid = true;
                    local.run.inputTokens = input;
                    local.run.rawInputTokens = input;
                    local.run.cacheCreationInputTokens = cacheWrite;
                    local.run.cacheReadInputTokens = cacheRead;
                    local.run.reasoningOutputTokens = reasoning;
                    local.run.tokens = computed > 0 ? computed : input + output;
                }
            }
        }

        // ZCode's runtime context surface is model-agnostic. Resolve the max
        // window from the active model's local registry when the request row
        // did not persist it. This supports every model ZCode adds to config,
        // rather than hard-coding only GLM-5.x names in AQC.
        if (local.context.contextWindowTokens <= 0 && !local.model.empty()) {
            local.context.contextWindowTokens = ZCodeConfiguredContextWindow(local.model);
        }

        const auto aggregate = db.Query(
            "SELECT COALESCE(SUM(input_tokens),0),COALESCE(SUM(cache_read_input_tokens),0),"
            "COALESCE(SUM(cache_creation_input_tokens),0) FROM model_usage WHERE session_id=" +
            sessionSql + " AND (query_source='main_turn' OR query_source='main') "
            "AND status IN ('running','completed')"
        );
        if (!aggregate.empty() && aggregate.front().size() >= 3) {
            const long long totalInput = ParsePositiveLongLong(aggregate.front()[0]);
            const long long totalRead = ParsePositiveLongLong(aggregate.front()[1]);
            const long long totalWrite = ParsePositiveLongLong(aggregate.front()[2]);
            local.context.totalCacheInputTokens = totalInput;
            local.context.totalCacheReadTokens = totalRead;
            local.context.totalCacheWriteTokens = totalWrite;
            if (totalInput > 0) {
                local.context.averageCacheHitPercentValid = true;
                local.context.averageCacheHitPercent = std::clamp(
                    static_cast<double>(totalRead) * 100.0 / static_cast<double>(totalInput),
                    0.0,
                    100.0
                );
                local.context.cacheStatsValid = true;
            }
        }

        // Exact persisted compact markers win. Never infer compaction merely
        // because the context is near a threshold: ZCode exposes an explicit
        // compact turn/status, so false COMPACTING states are unnecessary.
        const auto entries = db.Query(
            "SELECT data FROM session_entry WHERE session_id=" + sessionSql +
            " ORDER BY time_updated DESC LIMIT 32"
        );

        // Apply oldest -> newest so the newest runtime context wins.
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            if (it->empty() || (*it)[0].empty()) continue;
            const json entry = json::parse((*it)[0], nullptr, false);
            if (!entry.is_discarded()) {
                ApplyZCodeRuntimeJson(entry, local.context);
            }
        }

        // Resolve compact state newest -> oldest and stop at the first explicit
        // runtime marker. A historic "compact: running" event must never keep
        // a later completed/regular turn stuck in COMPACTING.
        for (const auto& row : entries) {
            if (row.empty() || row[0].empty()) continue;
            const json entry = json::parse(row[0], nullptr, false);
            if (entry.is_discarded()) continue;
            const auto compactState = ZCodeCompactionStateFromJson(entry);
            if (!compactState) continue;
            local.context.compacting = *compactState;
            if (*compactState && sessions.front().size() > 1) {
                local.context.compactionStartedAtUnixSeconds =
                    MillisecondsOrSecondsToUnixSeconds(
                        ParsePositiveLongLong(sessions.front()[1]));
            }
            break;
        }

        if (local.context.autoCompactThresholdTokens > 0 && local.context.usedTokens >= 0) {
            const long long threshold = local.context.autoCompactThresholdTokens;
            const long long used = std::clamp(local.context.usedTokens, 0LL, threshold);
            local.context.autoCompactPercentValid = true;
            local.context.autoCompactPercentLeft = static_cast<int>(std::clamp(
                std::llround(
                    static_cast<double>(threshold - used) * 100.0 /
                    static_cast<double>(threshold)
                ),
                0LL,
                100LL
            ));
        }

        local.context.valid = local.context.usedTokens > 0 &&
            local.context.contextWindowTokens > 0;
        return local;
    }


    LocalTelemetry ReadLocalTelemetry()
    {
        ZCodeLocalTelemetry local;
        try { local = ReadZCodeLocalTelemetry(); }
        catch (...) { return {}; } // Local telemetry is optional.
        LocalTelemetry result;
        result.context = local.context;
        result.run = local.run;
        return result;
    }

    static void ApplyLocalTelemetryToSnapshot(
        Snapshot& snapshot,
        const ZCodeLocalTelemetry& local
    ) {
        snapshot.context = local.context;
        snapshot.run = local.run;
    }


    Snapshot FetchSnapshot()
    {
        auto credentials = Credentials::Load();
        auto* network = Network::get_instance();
        auto environmentToken = [&](std::initializer_list<const char*> names) {
            for (const auto* name : names) {
                auto token = CredentialFormat::Token(network->GetEnvText(name));
                if (!token.empty()) return token;
            }
            return std::string{};
        };
        const auto jwt = environmentToken({ "ZCODE_JWT_TOKEN", "ZCODEJWTTOKEN", "ZAI_ZCODE_JWT_TOKEN", "ZCODE_ACCESS_TOKEN" });
        const auto key = environmentToken({ "ZAI_CODING_API_KEY", "ZAI_API_KEY" });
        // Explicit overrides win, but never combine another identity's token with
        // the locally cached OAuth token when constructing MCP authorization.
        if (!jwt.empty()) {
            if (jwt != credentials.startPlanJwt) credentials.currentZaiAccount = false;
            credentials.startPlanJwt = jwt;
            credentials.preferredPlan = CredentialFormat::PlanPreference::Start;
        }
        if (!key.empty()) {
            if (std::find(credentials.codingApiKeys.begin(), credentials.codingApiKeys.end(), key) == credentials.codingApiKeys.end())
                credentials.currentZaiAccount = false;
            credentials.codingApiKeys = { key };
            if (jwt.empty()) credentials.preferredPlan = CredentialFormat::PlanPreference::Individual;
        }
        Client::Options options;
        options.now = static_cast<long long>(std::time(nullptr));
        const auto customBalance = Protocol::Trim(network->GetEnvText("ZCODE_PLAN_BILLING_BALANCE_URL"));
        if (!customBalance.empty()) options.balanceUrl = customBalance;
        options.currentUrl = Protocol::Trim(network->GetEnvText("ZCODE_PLAN_BILLING_CURRENT_URL"));
        Snapshot snapshot = Client::Fetch(credentials, options, [&](const Client::Request& query) {
            auto headers = query.bearer
                ? network->BearerJsonHeaders(query.token, "https://zcode.z.ai", "https://zcode.z.ai/")
                : network->RawAuthorizationJsonHeaders(query.token, "https://zcode.z.ai", "https://zcode.z.ai/");
            for (const auto& extra : query.extraHeaders)
                headers += network->Utf8ToWide(extra.first + ": " + extra.second + "\r\n");
            const auto response = network->RequestUrl(query.url, query.method, headers, query.body, false);
            return Client::Response{ response.statusCode, response.body, response.serverUnixSeconds };
        });
        try { ApplyLocalTelemetryToSnapshot(snapshot, ReadZCodeLocalTelemetry()); }
        catch (...) {} // A moved/locked telemetry database cannot invalidate live quota.
        for (auto& bar : snapshot.bars) bar.resetText = Format::get_instance()->ResetShort(bar.resetAtUnixSeconds);
        return snapshot;
    }
}
