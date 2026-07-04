#include "Global.hpp"

#include "ZAi.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Text.hpp"
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
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ZAi
{
    static bool LooksLikeToken(const std::string& text)
    {
        if (text.size() < 32 || text.size() > 8192) {
            return false;
        }

        if (text.find(' ') != std::string::npos || text.find('\n') != std::string::npos || text.find('\r') != std::string::npos) {
            return false;
        }

        if (text.rfind("Bearer ", 0) == 0) {
            return text.size() > 40;
        }

        size_t dot1 = text.find('.');
        size_t dot2 = dot1 == std::string::npos ? std::string::npos : text.find('.', dot1 + 1);

        if (dot1 != std::string::npos && dot2 != std::string::npos) {
            return true;
        }

        return text.size() >= 48;
    }


    static void CollectJsonTokens(const json& value, std::vector<std::string>& tokens)
    {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                std::string key = Text::get_instance()->ToLowerCopy(it.key());

                if (it.value().is_string()) {
                    std::string text = it.value().get<std::string>();

                    if ((key.find("zcodejwttoken") != std::string::npos ||
                        key.find("zcode_jwt") != std::string::npos ||
                        key.find("jwt") != std::string::npos ||
                        key.find("access_token") != std::string::npos ||
                        key.find("accesstoken") != std::string::npos ||
                        key.find("api_key") != std::string::npos ||
                        key.find("apikey") != std::string::npos ||
                        key.find("token") != std::string::npos) && LooksLikeToken(text)) {
                        tokens.push_back(text);
                    }
                }

                CollectJsonTokens(it.value(), tokens);
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                CollectJsonTokens(item, tokens);
            }
        }
    }

    static void CollectRegexTokens(const std::string& text, std::vector<std::string>& tokens)
    {
        static const std::regex tokenRegex(
            R"rx((?:zcodejwttoken|zcodeJwtToken|zcode_jwt_token|access_token|accessToken|apiKey|api_key|token)"?\s*[:=]\s*"([^"\r\n]{32,8192})")rx",
            std::regex_constants::icase
        );

        for (auto it = std::sregex_iterator(text.begin(), text.end(), tokenRegex); it != std::sregex_iterator(); ++it) {
            std::string token = (*it)[1].str();

            if (LooksLikeToken(token)) {
                tokens.push_back(token);
            }
        }
    }


    static int ZCodeProviderPriority(const std::string& providerId, bool enabled)
    {
        std::string id = Text::get_instance()->ToLowerCopy(providerId);

        if (enabled && id == "builtin:zai-start-plan") {
            return 0;
        }

        if (enabled && id == "builtin:zai-coding-plan") {
            return 1;
        }

        if (enabled && id.find("zai") != std::string::npos && id.find("plan") != std::string::npos) {
            return 2;
        }

        if (enabled && id.find("zai") != std::string::npos) {
            return 3;
        }

        if (id == "builtin:zai-start-plan") {
            return 10;
        }

        if (id == "builtin:zai-coding-plan") {
            return 11;
        }

        if (id.find("zai") != std::string::npos) {
            return 12;
        }

        return 50;
    }

    static std::string FirstConfigToken(const json& object)
    {
        const char* keys[] = { "apiKey", "api_key", "accessToken", "access_token", "token", "zcodejwttoken" };

        for (const char* key : keys) {
            std::string value = JsonUtils::get_instance()->String(object, key);

            if (LooksLikeToken(value)) {
                return value;
            }
        }

        return {};
    }

    static void CollectZCodeConfigTokens(std::vector<std::string>& tokens)
    {
        std::filesystem::path path = Network::get_instance()->UserProfilePath() / ".zcode" / "v2" / "config.json";
        std::string text = Network::get_instance()->ReadTextFile(path);

        if (text.empty()) {
            return;
        }

        json root;

        try {
            root = JsonUtils::get_instance()->ParseRequired(text);
        }
        catch (...) {
            return;
        }

        if (!root.is_object() || !root.contains("provider") || !root.at("provider").is_object()) {
            return;
        }

        struct Candidate
        {
            int priority = 100;
            std::string token;
        };

        std::vector<Candidate> candidates;
        const json& providers = root.at("provider");

        for (auto it = providers.begin(); it != providers.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }

            const json& provider = it.value();
            bool enabled = JsonUtils::get_instance()->Bool(provider, "enabled", false);
            int priority = ZCodeProviderPriority(it.key(), enabled);
            std::string token;

            if (provider.contains("options") && provider.at("options").is_object()) {
                token = FirstConfigToken(provider.at("options"));
            }

            if (!LooksLikeToken(token)) {
                token = FirstConfigToken(provider);
            }

            if (LooksLikeToken(token)) {
                candidates.push_back({ priority, token });
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.priority < b.priority;
        });

        for (const Candidate& candidate : candidates) {
            tokens.push_back(candidate.token);
        }
    }

    static std::vector<std::filesystem::path> CandidateCredentialFiles()
    {
        std::vector<std::filesystem::path> paths;
        std::filesystem::path home = Network::get_instance()->UserProfilePath();

        paths.push_back(home / ".zcode" / "v2" / "config.json");
        paths.push_back(home / ".zcode" / "cli" / "config.json");
        paths.push_back(home / ".zcode" / "v2" / "credential.json");
        paths.push_back(home / ".zcode" / "v2" / "credentials.json");
        paths.push_back(home / ".zcode" / "v2" / "setting.json");

        std::string appData = Network::get_instance()->GetEnvText("APPDATA");

        if (!appData.empty()) {
            std::filesystem::path roaming(appData);
            paths.push_back(roaming / "ZCode" / "User" / "globalStorage" / "storage.json");
            paths.push_back(roaming / "ZCode" / "User" / "settings.json");
        }

        std::filesystem::path zcodeRoot = home / ".zcode";

        if (std::filesystem::exists(zcodeRoot)) {
            std::error_code ec;
            size_t count = 0;

            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                zcodeRoot,
                std::filesystem::directory_options::skip_permission_denied,
                ec
            )) {
                if (ec || count++ > 200) {
                    break;
                }

                if (!entry.is_regular_file(ec) || ec) {
                    continue;
                }

                std::filesystem::path p = entry.path();
                std::string ext = Text::get_instance()->ToLowerCopy(p.extension().string());

                if (ext != ".json" && ext != ".yaml" && ext != ".yml" && ext != ".txt") {
                    continue;
                }

                uintmax_t size = entry.file_size(ec);

                if (ec || size > 512 * 1024) {
                    continue;
                }

                paths.push_back(p);
            }
        }

        return paths;
    }

    static std::vector<std::string> LoadCandidateTokens()
    {
        std::vector<std::string> tokens;

        const char* envNames[] = {
            "ZCODE_JWT_TOKEN",
            "ZCODEJWTTOKEN",
            "ZAI_ZCODE_JWT_TOKEN",
            "ZAI_ACCESS_TOKEN",
            "ZCODE_ACCESS_TOKEN",
            "ZAI_TOKEN"
        };

        for (const char* name : envNames) {
            std::string value = Network::get_instance()->GetEnvText(name);

            if (LooksLikeToken(value)) {
                tokens.push_back(value);
            }
        }

        CollectZCodeConfigTokens(tokens);

        for (const std::filesystem::path& path : CandidateCredentialFiles()) {
            std::error_code ec;

            if (!std::filesystem::exists(path, ec) || ec) {
                continue;
            }

            if (std::filesystem::is_regular_file(path, ec)) {
                uintmax_t size = std::filesystem::file_size(path, ec);

                if (ec || size > 1024 * 1024) {
                    continue;
                }
            }

            std::string text = Network::get_instance()->ReadTextFile(path);

            if (text.empty()) {
                continue;
            }

            try {
                CollectJsonTokens(JsonUtils::get_instance()->ParseRequired(text), tokens);
            }
            catch (...) {
            }

            CollectRegexTokens(text, tokens);
        }

        std::vector<std::string> unique;
        std::set<std::string> seen;

        for (std::string token : tokens) {
            token = Network::get_instance()->StripHeaderValue(token);

            if (!LooksLikeToken(token)) {
                continue;
            }

            if (token.rfind("Bearer ", 0) == 0) {
                token = token.substr(7);
            }

            if (seen.insert(token).second) {
                unique.push_back(token);
            }
        }

        return unique;
    }





    static std::string CompactLower(std::string text)
    {
        return Text::get_instance()->CompactLower(text);
    }

    static std::string NormalizeModelName(std::string label)
    {
        std::string compact = CompactLower(label);

        if (compact.find("turbo") != std::string::npos) {
            return "GLM-5-Turbo";
        }

        if (compact.find("glm52") != std::string::npos || compact.find("glm5.2") != std::string::npos || compact.find("52") != std::string::npos) {
            return "GLM-5.2";
        }

        return label;
    }

    static bool IsZaiModelLabel(const std::string& label)
    {
        std::string normalized = NormalizeModelName(label);
        return normalized == "GLM-5.2" || normalized == "GLM-5-Turbo";
    }

    static int ModelSortRank(const std::string& label)
    {
        std::string normalized = NormalizeModelName(label);

        if (normalized == "GLM-5.2") return 0;
        if (normalized == "GLM-5-Turbo") return 1;
        return 10;
    }

    static std::string PickUsageLabel(const json& object, const std::string& fallback)
    {
        std::string label = Text::get_instance()->FirstNonEmpty({
            JsonUtils::get_instance()->String(object, "model"),
            JsonUtils::get_instance()->String(object, "model_name"),
            JsonUtils::get_instance()->String(object, "modelName"),
            JsonUtils::get_instance()->String(object, "show_name"),
            JsonUtils::get_instance()->String(object, "showName"),
            JsonUtils::get_instance()->String(object, "display_name"),
            JsonUtils::get_instance()->String(object, "displayName"),
            JsonUtils::get_instance()->String(object, "name"),
            JsonUtils::get_instance()->String(object, "type"),
            JsonUtils::get_instance()->String(object, "meter"),
            JsonUtils::get_instance()->String(object, "entitlement_id"),
            fallback
        });

        std::replace(label.begin(), label.end(), '_', ' ');
        return NormalizeModelName(label);
    }

    static void ApplyZaiBarStyle(UsageBar& bar)
    {
        bar.valid = true;
        bar.red = false;
        bar.white = false;
        bar.green = true;
        bar.thin = false;
    }

    static bool HasModelBar(const Snapshot& snapshot, const std::string& modelName)
    {
        for (const UsageBar& bar : snapshot.bars) {
            if (NormalizeModelName(bar.label) == modelName) {
                return true;
            }
        }

        return false;
    }

    static void AddFallbackModelBar(Snapshot& snapshot, const std::string& modelName, double total)
    {
        if (HasModelBar(snapshot, modelName)) {
            return;
        }

        UsageBar bar;
        bar.label = modelName;
        bar.sublabel = Format::get_instance()->IntegerWithCommas(total) + " / " + Format::get_instance()->IntegerWithCommas(total);
        bar.resetText = "";
        bar.usedPercent = 0.0f;
        ApplyZaiBarStyle(bar);
        snapshot.bars.push_back(bar);
    }

    static void FinalizeZaiBars(Snapshot& snapshot)
    {
        std::vector<UsageBar> unique;
        std::set<std::string> seenModels;

        for (UsageBar bar : snapshot.bars) {
            bar.label = NormalizeModelName(bar.label);
            ApplyZaiBarStyle(bar);

            std::string modelKey = IsZaiModelLabel(bar.label) ? bar.label : "";

            if (!modelKey.empty()) {
                if (!seenModels.insert(modelKey).second) {
                    continue;
                }
            }

            unique.push_back(bar);
        }

        snapshot.bars = unique;

        // ZCode Start Plan currently exposes these two model balances in the UI.
        // Keep both bars visible even if one endpoint omits a model row.
        AddFallbackModelBar(snapshot, "GLM-5.2", 3000000.0);
        AddFallbackModelBar(snapshot, "GLM-5-Turbo", 2000000.0);

        std::stable_sort(snapshot.bars.begin(), snapshot.bars.end(), [](const UsageBar& a, const UsageBar& b) {
            return ModelSortRank(a.label) < ModelSortRank(b.label);
        });
    }

    static void AddDetail(Snapshot& snapshot, const std::string& leftValue, const std::string& leftLabel, const std::string& rightValue = {}, const std::string& rightLabel = {})
    {
        if (leftValue.empty() && rightValue.empty()) {
            return;
        }

        DetailRow row;
        row.leftValue = leftValue;
        row.leftLabel = leftLabel;
        row.rightValue = rightValue;
        row.rightLabel = rightLabel;
        snapshot.details.push_back(row);
    }

    static void AddBalanceBar(Snapshot& snapshot, const json& balance)
    {
        if (!balance.is_object()) {
            return;
        }

        std::optional<double> total = JsonUtils::get_instance()->NumberAny(balance, {
            "total_units", "total", "total_tokens", "quota", "limit", "max", "number", "unit"
        });

        std::optional<double> used = JsonUtils::get_instance()->NumberAny(balance, {
            "used_units", "used", "usage", "used_tokens", "currentValue", "current", "consumed"
        });

        std::optional<double> remaining = JsonUtils::get_instance()->NumberAny(balance, {
            "remaining_units", "available_units", "remaining", "remain", "left", "available", "available_tokens", "balance"
        });

        if (!total && used && remaining) {
            total = *used + *remaining;
        }

        if (!used && total && remaining) {
            used = std::max(0.0, *total - *remaining);
        }

        if (!total && !used && !remaining) {
            return;
        }

        UsageBar bar;
        bar.label = PickUsageLabel(balance, "Usage credits");

        if (total && *total > 0.0 && used) {
            bar.usedPercent = Math::get_instance()->PercentUsed(*used, *total);
        }
        else {
            bar.usedPercent = 0.0f;
        }

        bar.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(balance, "period_end", "expires_at", "reset_at");

        if (bar.resetAtUnixSeconds == 0) {
            bar.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(balance, "expire_at", "expiresAt", "resetAt");
        }

        if (bar.resetAtUnixSeconds == 0) {
            bar.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(balance, "expire_time", "expiration", "endTime");
        }

        bar.resetText = Format::get_instance()->ResetShort(bar.resetAtUnixSeconds);
        ApplyZaiBarStyle(bar);

        if (remaining && total) {
            bar.sublabel = Format::get_instance()->IntegerWithCommas(*remaining) + " / " + Format::get_instance()->IntegerWithCommas(*total);
        }
        else if (used && total) {
            bar.sublabel = Format::get_instance()->IntegerWithCommas(*used) + " / " + Format::get_instance()->IntegerWithCommas(*total) + " used";
        }
        else if (remaining) {
            bar.sublabel = Format::get_instance()->IntegerWithCommas(*remaining) + " left";
        }

        snapshot.bars.push_back(bar);
    }

    static void AddQuotaLimitBar(Snapshot& snapshot, const json& limit)
    {
        if (!limit.is_object()) {
            return;
        }

        std::optional<double> total = JsonUtils::get_instance()->NumberAny(limit, {
            "number", "unit", "total", "total_tokens", "quota", "limit", "max", "total_units"
        });

        std::optional<double> used = JsonUtils::get_instance()->NumberAny(limit, {
            "usage", "currentValue", "used", "used_tokens", "consumed", "used_units", "current"
        });

        std::optional<double> remaining = JsonUtils::get_instance()->NumberAny(limit, {
            "remaining", "remain", "left", "available", "available_tokens", "remaining_units", "available_units", "balance"
        });

        if (!total && used && remaining) {
            total = *used + *remaining;
        }

        if (!used && total && remaining) {
            used = std::max(0.0, *total - *remaining);
        }

        bool hasPercentage = limit.contains("percentage") && limit.at("percentage").is_number();

        if (!total && !used && !remaining && !hasPercentage) {
            return;
        }

        UsageBar bar;
        bar.label = PickUsageLabel(limit, "Quota");

        if (total && *total > 0.0 && used) {
            bar.usedPercent = Math::get_instance()->PercentUsed(*used, *total);
        }
        else if (hasPercentage) {
            double pct = limit.at("percentage").get<double>();

            if (pct <= 1.0) {
                pct *= 100.0;
            }

            // ZCode percentage values generally represent remaining balance.
            // Convert to used percent so the global Show remaining toggle still works.
            bar.usedPercent = Math::get_instance()->ClampPercentFloat(static_cast<float>(100.0 - pct));
        }
        else {
            bar.usedPercent = 0.0f;
        }

        bar.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(limit, "nextResetTime", "reset_at", "expires_at");

        if (bar.resetAtUnixSeconds == 0) {
            bar.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(limit, "expire_at", "expiresAt", "resetAt");
        }

        if (bar.resetAtUnixSeconds == 0) {
            bar.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(limit, "expire_time", "expiration", "endTime");
        }

        bar.resetText = Format::get_instance()->ResetShort(bar.resetAtUnixSeconds);
        ApplyZaiBarStyle(bar);

        if (remaining && total) {
            bar.sublabel = Format::get_instance()->IntegerWithCommas(*remaining) + " / " + Format::get_instance()->IntegerWithCommas(*total);
        }
        else if (used && total) {
            bar.sublabel = Format::get_instance()->IntegerWithCommas(*used) + " / " + Format::get_instance()->IntegerWithCommas(*total) + " used";
        }
        else if (remaining) {
            bar.sublabel = Format::get_instance()->IntegerWithCommas(*remaining) + " left";
        }

        snapshot.bars.push_back(bar);
    }




    static bool HasAnyField(const json& object, std::initializer_list<const char*> keys)
    {
        if (!object.is_object()) {
            return false;
        }

        for (const char* key : keys) {
            if (object.contains(key)) {
                return true;
            }
        }

        return false;
    }

    static bool LooksLikeBalanceObject(const json& object)
    {
        return HasAnyField(object, { "total_units", "used_units", "remaining_units", "available_units", "reserved_units" });
    }

    static bool LooksLikeQuotaLimitObject(const json& object)
    {
        return HasAnyField(object, { "number", "usage", "currentValue", "remaining", "nextResetTime", "percentage" });
    }

    static bool AddUsageObject(Snapshot& snapshot, const json& object)
    {
        size_t before = snapshot.bars.size();

        if (LooksLikeBalanceObject(object)) {
            AddBalanceBar(snapshot, object);
        }
        else if (LooksLikeQuotaLimitObject(object)) {
            AddQuotaLimitBar(snapshot, object);
        }

        return snapshot.bars.size() > before;
    }

    static void ScanUsageJson(Snapshot& snapshot, const json& value, size_t maxAdded, size_t& added)
    {
        if (added >= maxAdded) {
            return;
        }

        if (value.is_object()) {
            if (AddUsageObject(snapshot, value)) {
                ++added;

                if (added >= maxAdded) {
                    return;
                }
            }

            const char* arrayKeys[] = { "balances", "limits", "quotas", "items", "data" };

            for (const char* key : arrayKeys) {
                if (!value.contains(key)) {
                    continue;
                }

                const json& child = value.at(key);

                if (child.is_array()) {
                    for (const json& item : child) {
                        ScanUsageJson(snapshot, item, maxAdded, added);

                        if (added >= maxAdded) {
                            return;
                        }
                    }
                }
                else if (child.is_object()) {
                    ScanUsageJson(snapshot, child, maxAdded, added);
                }
            }

            for (auto it = value.begin(); it != value.end(); ++it) {
                if (std::string(it.key()) == "balances" || std::string(it.key()) == "limits" || std::string(it.key()) == "quotas" || std::string(it.key()) == "items" || std::string(it.key()) == "data") {
                    continue;
                }

                if (it.value().is_object() || it.value().is_array()) {
                    ScanUsageJson(snapshot, it.value(), maxAdded, added);

                    if (added >= maxAdded) {
                        return;
                    }
                }
            }
        }
        else if (value.is_array()) {
            for (const json& item : value) {
                ScanUsageJson(snapshot, item, maxAdded, added);

                if (added >= maxAdded) {
                    return;
                }
            }
        }
    }

    static void ApplyCurrentResponse(Snapshot& snapshot, const json& root)
    {
        const json* data = JsonUtils::get_instance()->UnwrapData(root);

        if (!data || !data->is_object() || !data->contains("plans") || !data->at("plans").is_array()) {
            return;
        }

        const json& plans = data->at("plans");
        const json* selected = nullptr;

        for (const json& plan : plans) {
            std::string status = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(plan, "status"));
            std::string planId = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(plan, "plan_id"));
            std::string name = Text::get_instance()->ToLowerCopy(JsonUtils::get_instance()->String(plan, "name"));

            if (status == "active" && (planId.find("start-plan") != std::string::npos || name.find("start plan") != std::string::npos)) {
                selected = &plan;
                break;
            }
        }

        if (!selected && !plans.empty()) {
            selected = &plans.front();
        }

        if (!selected) {
            return;
        }

        std::string name = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(*selected, "name"), JsonUtils::get_instance()->String(*selected, "plan_id") });

        if (!name.empty()) {
            snapshot.plan = "Z.Ai " + name;
        }

        AddDetail(snapshot, Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(*selected, "status"), "unknown" }), "Plan status", Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(*selected, "plan_id"), JsonUtils::get_instance()->String(*selected, "user_plan_id") }), "Plan ID");
    }

    static void ApplyBalanceResponse(Snapshot& snapshot, const json& root)
    {
        const json* data = JsonUtils::get_instance()->UnwrapData(root);
        size_t before = snapshot.bars.size();

        if (data && data->is_object() && data->contains("balances") && data->at("balances").is_array()) {
            for (const json& balance : data->at("balances")) {
                AddBalanceBar(snapshot, balance);

                if (snapshot.bars.size() - before >= 6) {
                    break;
                }
            }
        }

        if (snapshot.bars.size() == before) {
            size_t added = 0;
            ScanUsageJson(snapshot, data ? *data : root, 6, added);
        }
    }

    static void ApplyQuotaResponse(Snapshot& snapshot, const json& root)
    {
        const json* data = JsonUtils::get_instance()->UnwrapData(root);

        if (!data || !data->is_object()) {
            return;
        }

        std::string level = JsonUtils::get_instance()->String(*data, "level");

        if (!level.empty() && snapshot.plan == "Z.Ai") {
            snapshot.plan = "Z.Ai GLM Coding " + level;
        }

        size_t before = snapshot.bars.size();

        if (data->contains("limits") && data->at("limits").is_array()) {
            for (const json& limit : data->at("limits")) {
                AddQuotaLimitBar(snapshot, limit);

                if (snapshot.bars.size() - before >= 6) {
                    break;
                }
            }
        }

        if (snapshot.bars.size() == before) {
            size_t added = 0;
            ScanUsageJson(snapshot, *data, 6, added);
        }
    }

    static void ApplySubscriptionList(Snapshot& snapshot, const json& root)
    {
        const json* data = JsonUtils::get_instance()->UnwrapData(root);

        if (!data || !data->is_array()) {
            return;
        }

        for (const json& item : *data) {
            std::string status = JsonUtils::get_instance()->String(item, "status");
            std::string product = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(item, "productName"), JsonUtils::get_instance()->String(item, "productId") });

            if (!product.empty()) {
                AddDetail(snapshot, product, "Subscription", status, "Status");
                return;
            }
        }
    }

    static bool ResponseOk(const Network::HttpResponse& response)
    {
        return response.statusCode >= 200 && response.statusCode < 300 && !response.body.empty();
    }

    Snapshot FetchSnapshot()
    {
        Snapshot snapshot;
        snapshot.lastUpdated = "now";

        std::vector<std::string> tokens = LoadCandidateTokens();

        if (tokens.empty()) {
            snapshot.statusText = "Z.Ai credentials not found. Sign in to ZCode or set ZCODE_JWT_TOKEN.";
            return snapshot;
        }

        std::vector<std::pair<std::string, const char*>> urls;
        std::set<std::string> seenUrls;

        auto addUrl = [&](const std::string& url, const char* kind) {
            if (!url.empty() && seenUrls.insert(url).second) {
                urls.push_back({ url, kind });
            }
        };

        std::string currentUrl = Network::get_instance()->GetEnvText("ZCODE_PLAN_BILLING_CURRENT_URL");
        std::string balanceUrl = Network::get_instance()->GetEnvText("ZCODE_PLAN_BILLING_BALANCE_URL");

        if (!currentUrl.empty()) {
            addUrl(currentUrl, "current");
        }

        if (!balanceUrl.empty()) {
            addUrl(balanceUrl, "balance");
        }

        addUrl("https://zcode.z.ai/api/v1/billing/current?app_version=1.0.0", "current");
        addUrl("https://zcode.z.ai/api/v1/billing/balance?app_version=1.0.0", "balance");
        addUrl("https://zcode.z.ai/api/v1/zcode-plan/billing/current?app_version=1.0.0", "current");
        addUrl("https://zcode.z.ai/api/v1/zcode-plan/billing/balance?app_version=1.0.0", "balance");
        addUrl("https://api.z.ai/api/monitor/usage/quota/limit", "quota");
        addUrl("https://api.z.ai/api/biz/subscription/list", "subscription");

        std::string lastError;

        for (const std::string& token : tokens) {
            bool anyOk = false;

            for (const auto& item : urls) {
                try {
                    std::wstring headers = (std::string(item.second) == "quota" || std::string(item.second) == "subscription")
                        ? Network::get_instance()->RawAuthorizationJsonHeaders(token, "https://zcode.z.ai", "https://zcode.z.ai/")
                        : Network::get_instance()->BearerJsonHeaders(token, "https://zcode.z.ai", "https://zcode.z.ai/");

                    Network::HttpResponse response = Network::get_instance()->RequestUrl(item.first, "GET", headers);

                    if (!ResponseOk(response)) {
                        std::ostringstream ss;
                        ss << item.second << " HTTP " << response.statusCode;
                        lastError = ss.str();
                        continue;
                    }

                    json root = JsonUtils::get_instance()->ParseOrNull(response.body);

                    if (root.is_discarded() || root.is_null()) {
                        lastError = std::string(item.second) + " returned invalid JSON";
                        continue;
                    }

                    anyOk = true;

                    std::string kind = item.second;

                    if (kind == "current") {
                        ApplyCurrentResponse(snapshot, root);
                    }
                    else if (kind == "balance") {
                        ApplyBalanceResponse(snapshot, root);
                    }
                    else if (kind == "quota") {
                        ApplyQuotaResponse(snapshot, root);
                    }
                    else if (kind == "subscription") {
                        ApplySubscriptionList(snapshot, root);
                    }
                }
                catch (const std::exception& e) {
                    lastError = e.what();
                }
            }

            if (anyOk) {
                FinalizeZaiBars(snapshot);
                snapshot.statusText = snapshot.bars.empty()
                    ? "Z.Ai connected, but no usage bars were returned by the ZCode billing endpoints."
                    : "";
                return snapshot;
            }
        }

        snapshot.statusText = "Z.Ai usage unavailable";

        if (!lastError.empty()) {
            snapshot.statusText += ": " + lastError;
        }

        return snapshot;
    }
}
