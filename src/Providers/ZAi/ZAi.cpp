#include "Global.hpp"

#include "ZAi.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"

#include <bcrypt.h>
#include <shlobj.h>
#include <array>
#include <cctype>
#include <locale>
#include <regex>
#include <set>

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

            inline std::string String(const Json& object, const char* key)
            {
                if (!object.is_object()) return {};
                const auto it = object.find(key);
                return it != object.end() && it->is_string() ? Text::get_instance()->TrimCopy(it->get<std::string>()) : std::string{};
            }

            inline std::optional<double> Number(const Json& value)
            {
                double number = 0;
                if (value.is_number()) number = value.get<double>();
                else if (value.is_string()) {
                    // Classic locale, full consumption, and finite-only: never turn "1junk",
                    // booleans, null, NaN, or infinity into a quota or an integer timestamp.
                    std::istringstream input(Text::get_instance()->TrimCopy(value.get<std::string>()));
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

            // ZCode 3.16.3's model catalog (client config): the builtinModels
            // GLM-5.3 and GLM-5.3-Flash, the default on every builtin provider,
            // and the bring-your-own-key GLM-5.2 and GLM-5-Turbo.
            struct CatalogModel
            {
                const char* name;
                long long contextWindow;
            };
            inline constexpr CatalogModel Catalog[] = {
                { "GLM-5.3", 1000000 }, { "GLM-5.3-Flash", 1000000 },
                { "GLM-5.2", 1048576 }, { "GLM-5-Turbo", 204800 },
            };

            inline const CatalogModel* FindCatalogModel(const std::string& model)
            {
                const auto lower = Text::get_instance()->ToLowerCopy(model);
                for (const auto& entry : Catalog) {
                    if (lower == Text::get_instance()->ToLowerCopy(entry.name)) return &entry;
                }
                return nullptr;
            }

            // The catalog's spelling of a model id ("glm-5.3-flash" -> "GLM-5.3-Flash").
            inline std::string ModelDisplayName(const std::string& model)
            {
                const auto* entry = FindCatalogModel(model);
                return entry ? entry->name : model;
            }

            // ZCode 3.16.3's startPlanPreview ("zcode-v3-start-plan"): daily
            // model_usage grants, used only when the API gives no units.
            inline std::optional<double> StartPlanPreviewGrant(const std::string& model)
            {
                const auto lower = Text::get_instance()->ToLowerCopy(model);
                if (lower == "glm-5.3") return 3000000.0;
                if (lower == "glm-5.3-flash") return 5000000.0;
                return std::nullopt;
            }

            inline std::string ModelLabel(const Json& item, const std::string& fallback)
            {
                for (const char* key : { "show_name", "displayName", "model", "model_name", "modelName" }) {
                    auto label = String(item, key);
                    if (!label.empty()) return ModelDisplayName(label);
                }
                std::string models;
                auto caps = item.find("capabilities");
                if (caps != item.end() && caps->is_array()) {
                    std::set<std::string> seen;
                    for (const auto& cap : *caps) {
                        if (!cap.is_string()) continue;
                        auto text = Text::get_instance()->TrimCopy(cap.get<std::string>());
                        if (Text::get_instance()->ToLowerCopy(text).rfind("model:", 0) != 0) continue;
                        text = ModelDisplayName(Text::get_instance()->TrimCopy(text.substr(6)));
                        if (text.empty() || !seen.insert(Text::get_instance()->ToLowerCopy(text)).second) continue;
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

            inline std::string DateText(long long unixSeconds)
            {
                return Format::get_instance()->ExpiryTime(std::chrono::system_clock::from_time_t(unixSeconds));
            }

            inline bool ExpiredPlan(const Json& plan, long long now)
            {
                const auto status = Text::get_instance()->ToLowerCopy(String(plan, "status"));
                const auto end = Epoch(plan, "ends_at");
                return status == "expired" || (status == "active" && end > 0 && end <= now);
            }

            inline bool StartPlan(const Json& plan, long long now)
            {
                if (Text::get_instance()->ToLowerCopy(String(plan, "status")) != "active" || ExpiredPlan(plan, now)) return false;
                const auto id = Text::get_instance()->ToLowerCopy(String(plan, "plan_id"));
                const auto name = Text::get_instance()->ToLowerCopy(String(plan, "name"));
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
                    std::optional<double> grant;
                    for (const auto& plan : plans) {
                        if (!MatchesPlan(balance, plan)) continue;
                        matched = true;
                        if (!ExpiredPlan(plan, now)) nonExpired = true;
                        const auto entries = plan.find("entitlements");
                        if (entries == plan.end() || !entries->is_array()) continue;
                        for (const auto& entry : *entries) {
                            if (String(entry, "entitlement_id") != String(balance, "entitlement_id")) continue;
                            period = String(entry, "period");
                            if (const auto units = NonNegative(entry, "grant_units"); units && *units > 0) grant = units;
                        }
                    }
                    if (matched && !nonExpired) continue;
                    const auto reset = Epoch(balance, "expires_at");
                    const auto periodStart = Epoch(balance, "period_start");
                    const auto periodEnd = Epoch(balance, "period_end");
                    if ((reset > 0 && reset <= now) || (periodStart > 0 && periodStart > now) ||
                        (periodEnd > 0 && periodEnd <= now)) continue;
                    UsageBar bar;
                    bar.label = ModelLabel(balance, "Usage credits");
                    auto total = NonNegative(balance, "total_units");
                    auto used = NonNegative(balance, "used_units");
                    auto remaining = NonNegative(balance, "remaining_units");
                    if (!total && used && remaining && std::isfinite(*used + *remaining)) total = *used + *remaining;
                    // The API's own units first; ZCode's published grant only when it gives none.
                    if (!total) total = grant;
                    if (!total) total = StartPlanPreviewGrant(bar.label);
                    if (!total || *total <= 0 || (!remaining && !used)) continue;
                    if (!remaining) remaining = std::max(0.0, *total - *used);
                    const double available = std::clamp(*remaining, 0.0, *total);
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
                    bar.sublabel = Format::get_instance()->IntegerWithCommas(available) + " / " + Format::get_instance()->IntegerWithCommas(*total) + " left";
                    if (!period.empty()) bar.sublabel += " | " + period;
                    bar.resetAtUnixSeconds = reset;
                    bar.valid = true;
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
                    if (remaining) bar.sublabel = Format::get_instance()->IntegerWithCommas(*remaining) + " left";
                    else if (used) bar.sublabel = Format::get_instance()->IntegerWithCommas(*used) + " used";
                    // ZCode lists the models drawing on each non-tool window.
                    std::string models;
                    std::set<std::string> listed;
                    const auto details = limit.find("usageDetails");
                    if (type != "TIME_LIMIT" && details != limit.end() && details->is_array()) {
                        for (const auto& detail : *details) {
                            auto model = String(detail, "displayName");
                            if (model.empty()) model = ModelDisplayName(String(detail, "modelCode"));
                            if (model.empty() || !listed.insert(model).second) continue;
                            models += (models.empty() ? "" : ", ") + model;
                        }
                    }
                    if (!models.empty()) bar.sublabel += (bar.sublabel.empty() ? "" : " | ") + models;
                    bar.valid = true;
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
                    if (Text::get_instance()->ToLowerCopy(id + " " + name).find("coding") == std::string::npos ||
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
            //                      available_week_resets: [{ expire_at }],
            //                      latest_five_hour_reset_history: { used_at } | null,
            //                      latest_week_reset_history: { used_at } | null } }
            inline void ApplyResetStatus(Snapshot& snapshot, const Json& root, long long now)
            {
                const auto* data = Data(root, true);
                if (!data || !data->is_object()) return;
                snapshot.resetsReported = true;
                for (const auto& [key, type] : { std::pair<const char*, const char*>{ "available_five_hour_resets", "5-hour" },
                                                 std::pair<const char*, const char*>{ "available_week_resets", "Weekly" } }) {
                    const auto list = data->find(key);
                    if (list == data->end() || !list->is_array()) continue;
                    // As ZCode does, only unexpired resets count (expire_at is in ms).
                    for (const auto& reset : *list) {
                        if (const auto expires = Epoch(reset, "expire_at"); expires > now) snapshot.resets.push_back({ type, expires });
                    }
                }
                std::stable_sort(snapshot.resets.begin(), snapshot.resets.end(), [](const QuotaReset& a, const QuotaReset& b) {
                    return a.expiresAtUnixSeconds < b.expiresAtUnixSeconds;
                });
                const auto usedAt = [&](const char* key) {
                    const auto history = data->find(key);
                    return history != data->end() && history->is_object() ? Epoch(*history, "used_at") : 0LL;
                };
                snapshot.lastFiveHourResetUnixSeconds = usedAt("latest_five_hour_reset_history");
                snapshot.lastWeeklyResetUnixSeconds = usedAt("latest_week_reset_history");
            }

            // data.<group>.<items> when it is an array.
            inline const Json* DataList(const Json& root, const char* group, const char* items)
            {
                const auto* data = Data(root);
                if (!data || !data->is_object()) return nullptr;
                const auto usage = data->find(group);
                if (usage == data->end() || !usage->is_object()) return nullptr;
                const auto list = usage->find(items);
                return list != usage->end() && list->is_array() ? &*list : nullptr;
            }

            // Rows in sortOrder (array order where absent): name -> value.
            template <typename Name>
            inline std::vector<std::pair<std::string, long long>> SortedRows(const Json* rows, const Name& name, const char* valueKey)
            {
                std::vector<std::tuple<double, std::string, long long>> found;
                if (!rows) return {};
                for (const auto& row : *rows) {
                    const auto label = name(row);
                    const auto value = NonNegative(row, valueKey);
                    if (label.empty() || !value || *value > 9007199254740991.0) continue;
                    found.emplace_back(Number(row, "sortOrder").value_or(static_cast<double>(found.size())), label,
                        static_cast<long long>(*value));
                }
                std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                std::vector<std::pair<std::string, long long>> result;
                for (auto& [order, label, value] : found) result.emplace_back(std::move(label), value);
                return result;
            }

            // Coding Plan usage history, from four read-only monitor calls:
            //   credit-usage/activity  data { summary { totalTokens, peakDailyTokens, peakDailyTokensDate,
            //                          totalUsageDurationMs, currentStreakDays, longestStreakDays },
            //                          series [{ date, totalTokens, modelCallCount, mcpCalls }] }
            //   usage-detail MODEL     data { modelUsage { modelDataList [{ modelName, totalTokens, sortOrder }] } }
            //   usage-detail MCP       data { mcpUsage { mcpDataList [{ mcpName | toolName | mcpCode, totalUsageCount }] } }
            //   model-performance-day  data { proMaxDecodeSpeed [], liteDecodeSpeed [] }
            // Valid with the activity summary; the lists and speeds are best effort.
            inline UsageHistory ParseUsageHistory(const Json& activity, const Json& models, const Json& tools,
                const Json& performance, const std::string& today)
            {
                UsageHistory history;
                const auto* data = Data(activity);
                const auto summary = data && data->is_object() ? data->find("summary") : Json::const_iterator{};
                if (!data || !data->is_object() || summary == data->end() || !summary->is_object()) return history;
                const auto count = [](const Json& object, const char* key) {
                    const auto value = NonNegative(object, key);
                    return value && *value <= 9007199254740991.0 ? static_cast<long long>(*value) : -1LL;
                };
                history.valid = true;
                history.totalTokens = count(*summary, "totalTokens");
                history.peakDailyTokens = count(*summary, "peakDailyTokens");
                history.peakDate = String(*summary, "peakDailyTokensDate");
                if (const auto ms = count(*summary, "totalUsageDurationMs"); ms >= 0) history.totalUsageSeconds = ms / 1000;
                history.currentStreakDays = static_cast<int>(count(*summary, "currentStreakDays"));
                history.longestStreakDays = static_cast<int>(count(*summary, "longestStreakDays"));
                if (const auto series = data->find("series"); series != data->end() && series->is_array()) {
                    // A reported series without today's row means nothing yet today.
                    history.todayTokens = history.todayModelCalls = history.todayToolCalls = 0;
                    for (const auto& day : *series) {
                        if (String(day, "date") != today) continue;
                        history.todayTokens = std::max(0LL, count(day, "totalTokens"));
                        history.todayModelCalls = std::max(0LL, count(day, "modelCallCount"));
                        history.todayToolCalls = std::max(0LL, count(day, "mcpCalls"));
                    }
                }

                // ZCode drops its cache / output breakdown buckets from the model list.
                static const std::set<std::string> breakdown = { "cached_input", "cached_output", "cachedInput",
                    "cache_input", "cacheInput", "uncached_input", "uncachedInput", "output", "output_tokens",
                    "outputTokens", "\xE7\xBC\x93\xE5\xAD\x98", "\xE6\x9C\xAA\xE7\xBC\x93\xE5\xAD\x98", "\xE8\xBE\x93\xE5\x87\xBA" };
                history.models = SortedRows(DataList(models, "modelUsage", "modelDataList"), [](const Json& row) {
                    const auto code = String(row, "modelCode"), name = String(row, "modelName");
                    if (breakdown.count(code) || breakdown.count(Text::get_instance()->ToLowerCopy(name))) return std::string{};
                    return ModelDisplayName(name.empty() ? code : name);
                }, "totalTokens");
                history.tools = SortedRows(DataList(tools, "mcpUsage", "mcpDataList"), [](const Json& row) {
                    return Text::get_instance()->FirstNonEmpty({ String(row, "mcpName"), String(row, "toolName"), String(row, "mcpCode") });
                }, "totalUsageCount");

                // The latest day's decode speed per tier, as ZCode's health check reads it.
                if (const auto* speeds = Data(performance); speeds && speeds->is_object()) {
                    const auto latest = [&](const char* key) {
                        double speed = -1.0;
                        const auto series = speeds->find(key);
                        if (series != speeds->end() && series->is_array()) {
                            for (const auto& value : *series) {
                                if (const auto n = Number(value); n && *n > 0) speed = *n;
                            }
                        }
                        return speed;
                    };
                    history.proDecodeTps = latest("proMaxDecodeSpeed");
                    history.liteDecodeTps = latest("liteDecodeSpeed");
                }
                return history;
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
                text = Text::get_instance()->TrimCopy(std::move(text));
                if (text.size() > 7 && Text::get_instance()->ToLowerCopy(text.substr(0, 6)) == "bearer" &&
                    (text[6] == ' ' || text[6] == '\t')) text = Text::get_instance()->TrimCopy(text.substr(7));
                if (text.size() < 16 || text.size() > 16384 || Text::get_instance()->ToLowerCopy(text).rfind("enc:", 0) == 0) return {};
                for (unsigned char ch : text) if (ch < 33 || ch > 126) return {};
                return text;
            }

            // encodeURIComponent also leaves these unescaped; ZCode builds its
            // credential keys and request URLs with it.
            inline constexpr std::string_view UriComponentMarks = "!*'()";

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
                    return Text::get_instance()->TrimCopy(std::move(*value));
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
                            "-individual-coding-plan:account:" + Text::get_instance()->UrlEncode(identity, UriComponentMarks) + ":api-key";
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
                // A path variable, trimmed; empty when unset.
                std::wstring EnvironmentPath(const char* name)
                {
                    return Text::get_instance()->TrimCopy(Network::get_instance()->EnvPath(name).wstring());
                }

                std::filesystem::path OsHome()
                {
                    // Match Node os.homedir() on Windows, not HOME or a migrated data path.
                    if (auto profile = Network::get_instance()->EnvPath("USERPROFILE"); !profile.empty()) return profile;
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
                    // No trim: ZCode hashes the exact value. Like Node, a lone
                    // surrogate encodes as U+FFFD.
                    auto overrideSecret = Network::get_instance()->EnvText("ZCODE_CREDENTIAL_SECRET");
                    if (!overrideSecret.empty()) return overrideSecret;
                    std::array<wchar_t, 257> buffer{};
                    DWORD size = static_cast<DWORD>(buffer.size());
                    const std::string username = GetUserNameW(buffer.data(), &size)
                        ? Text::get_instance()->WideToUtf8(std::wstring(buffer.data())) : "unknown";
                    return "zcode-credential-fallback:win32:" + Text::get_instance()->WideToUtf8(OsHome().wstring()) + ":" + username;
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
                auto home = EnvironmentPath("ZCODE_DESKTOP_HOME_DIR");
                if (home.empty()) home = EnvironmentPath("HOME");
                if (home.empty()) home = EnvironmentPath("USERPROFILE");
                if (home.empty()) home = OsHome().wstring();
                return std::filesystem::path(home) / L".zcode" / L"v2" / L"setting.json";
            }

            std::filesystem::path DataRoot(const Protocol::Json& setting)
            {
                const auto configured = Protocol::String(setting, "dataBaseDir");
                if (!configured.empty())
                    return std::filesystem::path(std::u8string(configured.begin(), configured.end())) / L".zcode";
                auto base = EnvironmentPath("ZCODE_DATA_BASE_DIR");
                if (base.empty()) base = EnvironmentPath("HOME");
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
            // Coding Plan usage history per account and plan scope, refreshed at
            // most every 10 minutes: fetched at, history.
            inline std::mutex g_historyMutex;
            inline std::unordered_map<std::string, std::pair<long long, UsageHistory>> g_history;
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
                if (url.size() < 9 || Text::get_instance()->ToLowerCopy(url.substr(0, 8)) != "https://") return false;
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
                // The last request's HTTP status, or its business 401/402/403/429.
                int lastStatus = 0;
                auto request = [&](const Request& query, const std::string& name, bool optional,
                    long long* serverTime = nullptr) -> Protocol::Json {
                    lastStatus = 0;
                    if (!Https(query.url)) {
                        if (!optional) fail(name + " URL must use HTTPS", 0, 2);
                        return nullptr;
                    }
                    try {
                        auto response = transport(query);
                        lastStatus = response.status;
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
                            lastStatus = code;
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
                    return origin + "/api/biz/v1/organization/" + Text::get_instance()->UrlEncode(organization, CredentialFormat::UriComponentMarks) +
                        "/projects/" + Text::get_instance()->UrlEncode(project, CredentialFormat::UriComponentMarks) + "/api_keys";
                };
                const auto text = [](const Protocol::Json& item, const char* key) {
                    const auto it = item.is_object() ? item.find(key) : item.end();
                    if (!item.is_object() || it == item.end() || it->is_null()) return std::string{};
                    return Text::get_instance()->TrimCopy(it->is_string() ? it->get<std::string>() : it->dump());
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
                        Text::get_instance()->UrlEncode(location.apiKeyId, CredentialFormat::UriComponentMarks) + "/access_tokens",
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
                    // Every OAuth rotation adds a scope and each entry holds a secret:
                    // drop the expired ones rather than keep them for the process's life.
                    std::erase_if(g_tokens, [&](const auto& entry) { return entry.second.refreshAt <= options.now; });
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
                    Protocol::ApplyResetStatus(result, request(query, "Quota resets", true), options.now);
                };
                // Usage history: read-only GETs on the quota host with the quota
                // call's own credentials and team headers, at most every 10 minutes.
                // A failure only leaves history invalid.
                auto history = [&](Snapshot& result, const std::string& token, const TeamPlan* team) {
                    const auto scope = Protocol::Json::array({ origin, credentials.accountId,
                        team ? team->organizationId : "", team ? team->projectId : "",
                        credentials.accountId.empty() ? std::to_string(std::hash<std::string>{}(token)) : "" }).dump();
                    {
                        std::lock_guard<std::mutex> lock(g_historyMutex);
                        const auto cached = g_history.find(scope);
                        if (cached != g_history.end() && options.now - cached->second.first < 600) {
                            result.history = cached->second.second;
                            return;
                        }
                    }
                    const auto day = [&](int offset, const char* time) {
                        return Text::get_instance()->UrlEncode(Format::get_instance()->LocalDateKey(options.now, offset) + " " + time, CredentialFormat::UriComponentMarks);
                    };
                    const auto week = "startTime=" + day(-6, "00:00:00") + "&endTime=" + day(0, "23:59:59");
                    const std::string type = team ? "type=3&" : "type=1&";
                    const auto get = [&](const std::string& path) {
                        Request read{ origin + path, token, false, {} };
                        if (team) read.extraHeaders = { { "bigmodel-organization", team->organizationId },
                            { "bigmodel-project", team->projectId } };
                        return request(read, "Usage history", true);
                    };
                    const auto activity = get("/api/monitor/credit-usage/activity?" + type +
                        "startTime=" + day(-365, "00:00:00") + "&endTime=" + day(0, "23:59:59"));
                    // Without the activity summary there is no history to show.
                    if (!Protocol::Data(activity)) return;
                    const auto models = get("/api/monitor/credit-usage/usage-detail?" + type + week + "&usageType=MODEL");
                    const auto tools = get("/api/monitor/credit-usage/usage-detail?" + type + week + "&usageType=MCP");
                    const auto performance = get("/api/monitor/usage/model-performance-day?" + week);
                    UsageHistory parsed = Protocol::ParseUsageHistory(activity, models, tools, performance,
                        Format::get_instance()->LocalDateKey(options.now, 0));
                    if (parsed.valid) {
                        std::lock_guard<std::mutex> lock(g_historyMutex);
                        g_history[scope] = { options.now, parsed };
                    }
                    result.history = std::move(parsed);
                };
                auto quota = [&](const std::string& token, const TeamPlan* team, const std::string& name) -> bool {
                    // Team quota is the same endpoint scoped by ?type=2 and project headers.
                    Request query{ origin + Protocol::QuotaPath + (team ? "?type=2" : ""), token, false, {} };
                    if (team) query.extraHeaders = { { "bigmodel-organization", team->organizationId },
                        { "bigmodel-project", team->projectId } };
                    const auto root = request(query, name + " quota", false);
                    if (!root.is_object()) {
                        // A rejected minted token is dead (its key was disabled or
                        // rotated): forget it so the next refresh mints a fresh one.
                        if (lastStatus == 401 || lastStatus == 403) {
                            std::lock_guard<std::mutex> lock(g_tokenMutex);
                            std::erase_if(g_tokens, [&](const auto& entry) { return entry.second.token == token; });
                        }
                        return false;
                    }
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
                    history(candidate, token, team);
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
    struct Sqlite3;
    struct Sqlite3Stmt;

    // The system SQLite (winsqlite3, Windows 10 and later), loaded on demand so
    // AQC needs no dependency. Only System32 is searched: a sqlite3.dll on PATH
    // or beside the app must never load into a process that holds Codex,
    // Claude and ZCode credentials.
    class DynamicSqlite
    {
    public:
        DynamicSqlite() = default;
        DynamicSqlite(const DynamicSqlite&) = delete;
        DynamicSqlite& operator=(const DynamicSqlite&) = delete;

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
            const std::string utf8 = Text::get_instance()->WideToUtf8(path.wstring());
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
        // winsqlite3 exports __stdcall (SQLITE_APICALL): WINAPI on x86, and
        // the one convention on x64.
        using OpenV2Fn = int(WINAPI*)(const char*, Sqlite3**, int, const char*);
        using CloseV2Fn = int(WINAPI*)(Sqlite3*);
        using PrepareV2Fn = int(WINAPI*)(Sqlite3*, const char*, int, Sqlite3Stmt**, const char**);
        using StepFn = int(WINAPI*)(Sqlite3Stmt*);
        using FinalizeFn = int(WINAPI*)(Sqlite3Stmt*);
        using ColumnCountFn = int(WINAPI*)(Sqlite3Stmt*);
        using ColumnTextFn = const unsigned char*(WINAPI*)(Sqlite3Stmt*, int);
        using BusyTimeoutFn = int(WINAPI*)(Sqlite3*, int);

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

            m_module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
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

    // The first value under one of `keys`, depth first, that `accept` takes
    // (&json::is_object, &json::is_array).
    static const json* FindByKeyRecursive(
        const json& value,
        std::initializer_list<const char*> keys,
        bool (json::*accept)() const noexcept,
        int depth = 0
    ) {
        if (depth > 10 || (!value.is_object() && !value.is_array())) {
            return nullptr;
        }
        if (value.is_object()) {
            for (const char* key : keys) {
                auto it = value.find(key);
                if (it != value.end() && ((*it).*accept)()) {
                    return &*it;
                }
            }
        }
        for (const json& item : value) {
            if (const json* found = FindByKeyRecursive(item, keys, accept, depth + 1)) {
                return found;
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

    static std::optional<bool> ZCodeCompactionStateFromJson(
        const json& value,
        int depth = 0
    ) {
        if (depth > 10) return std::nullopt;
        if (value.is_object()) {
            const std::string activeTurnKind = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(value, "activeTurnKind"));
            if (activeTurnKind == "compact") return true;
            if (activeTurnKind == "regular" || activeTurnKind == "rewind") return false;

            const std::string type = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(value, "type"));
            const std::string status = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(value, "status"));
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

        // usedTokens / maxTokens also name request fields (a maxTokens: 32768
        // output cap is no context window), so those count only inside a
        // context object; the context-prefixed names are unambiguous anywhere.
        std::optional<double> used = FindNumberByKeyRecursive(
            root,
            { "contextUsed", "context_used" }
        );
        std::optional<double> maximum = FindNumberByKeyRecursive(
            root,
            { "contextWindow", "context_window", "contextWindowTokens" }
        );
        const std::optional<double> compactThreshold = FindNumberByKeyRecursive(
            root,
            { "autoCompactThresholdTokens", "auto_compact_threshold_tokens" }
        );

        // The session's contextWindow:{usedTokens,maxTokens} and the host UI's
        // normalized contextUsage:{used,size}. Only accept those generic names
        // from an actual context object so an unrelated quota/usage
        // {used,size} cannot corrupt the context meter.
        if (const json* contextObject = FindByKeyRecursive(
            root, { "contextWindow", "contextUsage", "context_usage" }, &json::is_object)) {
            if (!used) {
                used = FindNumberByKeyRecursive(*contextObject, { "usedTokens", "used" });
            }
            if (!maximum) {
                maximum = FindNumberByKeyRecursive(
                    *contextObject, { "maxTokens", "contextWindowTokens", "size" });
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

        if (const json* cache = FindByKeyRecursive(root, { "cache", "cacheHit" }, &json::is_object)) {
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

        if (const json* breakdown = FindByKeyRecursive(root, { "contextUsageBreakdown", "breakdown" }, &json::is_array)) {
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

    // Pre-3.16 ZCode kept model limits in v2\config.json; 3.16 only reads it
    // once to import old settings. Kept as a fallback for older installs.
    static long long ZCodeLegacyContextWindow(const std::string& model)
    {
        const json root = json::parse(Network::get_instance()->ReadTextFile(
            Credentials::DataRoot() / "v2" / "config.json"), nullptr, false);
        const auto providersIt = root.is_object() ? root.find("provider") : root.end();
        if (!root.is_object() || providersIt == root.end() || !providersIt->is_object()) {
            return 0;
        }

        const std::string wanted = Text::get_instance()->CompactLower(model);
        long long best = 0;
        for (auto provider = providersIt->begin(); provider != providersIt->end(); ++provider) {
            if (!provider.value().is_object()) continue;
            auto models = provider.value().find("models");
            if (models == provider.value().end() || !models->is_object()) continue;
            for (auto item = models->begin(); item != models->end(); ++item) {
                if (!item.value().is_object()) continue;
                const std::string displayName = JsonUtils::get_instance()->String(item.value(), "name");
                if (Text::get_instance()->CompactLower(item.key()) != wanted &&
                    (displayName.empty() || Text::get_instance()->CompactLower(displayName) != wanted)) {
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

    // For when no ZCode config file can be read: the 3.16.3 catalog, then the
    // zcode-builtin.json revision 43 windows of the older GLM models.
    static long long KnownGlmContextWindow(const std::string& model)
    {
        if (const auto* entry = Protocol::FindCatalogModel(model)) {
            return entry->contextWindow;
        }

        static const std::pair<const char*, long long> kModels[] = {
            { "glm-5v-turbo", 200000 }, { "glm-5.1-highspeed", 200000 }, { "glm-5.1", 200000 }, { "glm-5", 200000 },
            { "glm-4.7-flashx", 200000 }, { "glm-4.7-flash", 200000 }, { "glm-4.7", 200000 },
            { "glm-4.6v-flashx", 131072 }, { "glm-4.6v-flash", 131072 }, { "glm-4.6v", 131072 },
            { "glm-4.6", 200000 }, { "glm-4.5-air", 131072 }, { "glm-4.5", 131072 },
            { "glm-4.1v-thinking-flashx", 65536 }, { "glm-4.1v-thinking-flash", 65536 },
            { "glm-4-flashx-250414", 131072 }, { "glm-4-flash-250414", 131072 }, { "glm-4v-flash", 16384 },
        };
        const std::string lower = Text::get_instance()->ToLowerCopy(model);
        for (const auto& [id, window] : kModels) {
            if (lower == id) return window;
        }
        return 0;
    }

    // Model limits as ZCode 3.16 resolves them:
    //   1. zcode-builtin.json (newest revision): modelRules in order,
    //      whole-id case-insensitive regex on modelMatch; then
    //      builtinProviderModelRules (exact).
    //   2. provider_config.json providerModelRules / manualProviderModelRules
    //      (exact provider + model), which win.
    // A later match overwrites only the fields it sets. 0 when none sets one.
    static long long ContextWindowFromRules(const json& builtin, const json& personal,
        const std::string& model, const std::string& providerId)
    {
        long long window = 0;
        const auto apply = [&window](const json& rule) {
            const auto config = rule.find("config");
            if (config == rule.end() || !config->is_object()) return;
            const auto properties = config->find("properties");
            if (properties == config->end() || !properties->is_object()) return;
            const auto value = JsonUtils::get_instance()->NumberOpt(*properties, "contextWindow");
            if (value && *value > 0.0) window = static_cast<long long>(*value);
        };
        const auto exact = [&](const json& rule) {
            return Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(rule, "modelId")) ==
                    Text::get_instance()->ToLowerCopy(model) &&
                (providerId.empty() || JsonUtils::get_instance()->String(rule, "providerId") == providerId);
        };
        const auto rules = [](const json& root, const char* list) -> const json* {
            if (!root.is_object()) return nullptr;
            const auto config = root.find("config");
            if (config == root.end() || !config->is_object()) return nullptr;
            const auto ruleSets = config->find("modelConfigRules");
            if (ruleSets == config->end() || !ruleSets->is_object()) return nullptr;
            const auto found = ruleSets->find(list);
            return found != ruleSets->end() && found->is_array() ? &*found : nullptr;
        };

        if (const json* modelRules = rules(builtin, "modelRules")) {
            for (const json& rule : *modelRules) {
                const std::string pattern = JsonUtils::get_instance()->String(rule, "modelMatch");
                if (pattern.empty()) continue;
                try {
                    if (std::regex_match(model, std::regex(pattern, std::regex::ECMAScript | std::regex::icase))) apply(rule);
                }
                catch (const std::regex_error&) {}
            }
        }
        if (const json* providerRules = rules(builtin, "builtinProviderModelRules")) {
            for (const json& rule : *providerRules) if (exact(rule)) apply(rule);
        }
        for (const char* list : { "providerModelRules", "manualProviderModelRules" }) {
            if (const json* personalRules = rules(personal, list)) {
                for (const json& rule : *personalRules) if (exact(rule)) apply(rule);
            }
        }
        return window;
    }

    // The rule files live under v2: runtime\provider\<platform>\<version>\
    // endpoint-<hash>\zcode-builtin.json (newest revision wins) and
    // provider_config.json. Then the pre-3.16 config, then the known models.
    static long long ZCodeConfiguredContextWindow(const std::string& model, const std::string& providerId)
    {
        if (model.empty()) {
            return 0;
        }

        const std::filesystem::path v2 = Credentials::DataRoot() / "v2";
        json builtin;
        double revision = -1.0;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(v2 / "runtime" / "provider",
                 std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().filename() != "zcode-builtin.json") continue;
            json candidate = json::parse(Network::get_instance()->ReadTextFile(it->path()), nullptr, false);
            const double candidateRevision = JsonUtils::get_instance()->Number(candidate, "revision", -1.0);
            if (candidate.is_object() && candidateRevision > revision) {
                builtin = std::move(candidate);
                revision = candidateRevision;
            }
        }

        long long window = ContextWindowFromRules(builtin,
            json::parse(Network::get_instance()->ReadTextFile(v2 / "provider_config.json"), nullptr, false),
            model, providerId);
        if (window <= 0) window = ZCodeLegacyContextWindow(model);
        if (window <= 0) window = KnownGlmContextWindow(model);
        return window;
    }

    // ZCode 3.16 lets ZCODE_SESSION_DB_PATH / ZCODE_SESSION_DB, then
    // ~/.zcode/cli/config.json storage.sessionDbPath, move the session database.
    static std::filesystem::path ZCodeSessionDbPath()
    {
        // "~/" is the home directory; anything else must be absolute.
        const auto expand = [](const std::wstring& configured) -> std::filesystem::path {
            const std::wstring text = Text::get_instance()->TrimCopy(configured);
            const std::filesystem::path path = text.rfind(L"~/", 0) == 0
                ? Credentials::OsHome() / text.substr(2)
                : std::filesystem::path(text);
            return path.is_absolute() ? path : std::filesystem::path{};
        };
        // Read wide: GetEnvText is the ANSI code page, which mangles a non-ASCII path.
        for (const char* name : { "ZCODE_SESSION_DB_PATH", "ZCODE_SESSION_DB" }) {
            const auto value = Credentials::EnvironmentPath(name);
            if (!value.empty()) return expand(value);
        }
        const auto config = json::parse(Network::get_instance()->ReadTextFile(
            Credentials::OsHome() / ".zcode" / "cli" / "config.json"), nullptr, false);
        if (config.is_object() && config.contains("storage") && config.at("storage").is_object()) {
            // JSON text is UTF-8.
            const auto configured = JsonUtils::get_instance()->String(config.at("storage"), "sessionDbPath");
            if (!Text::get_instance()->TrimCopy(configured).empty())
                return expand(std::filesystem::path(std::u8string(configured.begin(), configured.end())).wstring());
        }
        return Credentials::DataRoot() / "cli" / "db" / "db.sqlite";
    }

    static LocalTelemetry ReadZCodeLocalTelemetry()
    {
        LocalTelemetry local;
        const std::filesystem::path dbPath = ZCodeSessionDbPath();
        std::error_code ec;
        if (dbPath.empty() || !std::filesystem::is_regular_file(dbPath, ec) || ec) {
            return local;
        }

        DynamicSqlite db;
        if (!db.OpenReadOnly(dbPath)) {
            return local;
        }

        // Newer session databases soft-delete with time_deleted; older ones lack it.
        const bool softDeletes = !db.Query(
            "SELECT name FROM pragma_table_info('session') WHERE name='time_deleted'").empty();
        const auto sessions = db.Query(
            std::string("SELECT id, COALESCE(time_compacting,0), time_updated "
            "FROM session WHERE time_archived IS NULL AND (parent_id IS NULL OR parent_id='') ") +
            (softDeletes ? "AND time_deleted IS NULL " : "") +
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

        std::string providerId;
        if (!modelRows.empty() && modelRows.front().size() >= 14) {
            const auto& row = modelRows.front();
            providerId = row[12];
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
            local.context.model = Protocol::ModelDisplayName(row[0]);
            local.context.sourceLabel = "ZCode live session database \xc2\xb7 direct read-only";

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
        if (local.context.contextWindowTokens <= 0 && !local.context.model.empty()) {
            local.context.contextWindowTokens = ZCodeConfiguredContextWindow(local.context.model, providerId);
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
        try { return ReadZCodeLocalTelemetry(); }
        catch (...) { return {}; } // Local telemetry is optional.
    }

    Snapshot FetchSnapshot()
    {
        auto credentials = Credentials::Load();
        auto environmentToken = [&](std::initializer_list<const char*> names) {
            for (const auto* name : names) {
                auto token = CredentialFormat::Token(Network::get_instance()->GetEnvText(name));
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
        const auto customBalance = Text::get_instance()->TrimCopy(Network::get_instance()->GetEnvText("ZCODE_PLAN_BILLING_BALANCE_URL"));
        if (!customBalance.empty()) options.balanceUrl = customBalance;
        options.currentUrl = Text::get_instance()->TrimCopy(Network::get_instance()->GetEnvText("ZCODE_PLAN_BILLING_CURRENT_URL"));
        // Client::Fetch's request() turns any transport exception into a
        // failed (or, for optional reads, empty) result without leaking it.
        Snapshot snapshot = Client::Fetch(credentials, options, [&](const Client::Request& query) {
            auto headers = query.bearer
                ? Network::get_instance()->BearerJsonHeaders(query.token, "https://zcode.z.ai", "https://zcode.z.ai/")
                : Network::get_instance()->RawAuthorizationJsonHeaders(query.token, "https://zcode.z.ai", "https://zcode.z.ai/");
            for (const auto& extra : query.extraHeaders)
                headers += Text::get_instance()->Utf8ToWide(extra.first + ": " + extra.second + "\r\n");
            const auto response = Network::get_instance()->RequestUrl(query.url, query.method, headers, query.body, false);
            return Client::Response{ response.statusCode, response.body, response.serverUnixSeconds };
        });
        // A moved/locked telemetry database cannot invalidate live quota.
        LocalTelemetry local = ReadLocalTelemetry();
        snapshot.context = std::move(local.context);
        snapshot.run = std::move(local.run);
        for (auto& bar : snapshot.bars) bar.resetText = Format::get_instance()->ResetShort(bar.resetAtUnixSeconds);
        return snapshot;
    }
}
