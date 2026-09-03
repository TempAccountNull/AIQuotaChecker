#include "Global.hpp"
#include "Grok.hpp"

#include <cctype>

#include "JsonUtils.hpp"
#include "Math.hpp"
#include "Network.hpp"
#include "Text.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Grok
{
    namespace
    {
        using Json = JsonUtils::Json;

        constexpr const char* kOidcScope = "https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828";
        constexpr const char* kLegacyScope = "https://accounts.x.ai/sign-in";
        constexpr const char* kBillingUrl = "https://cli-chat-proxy.grok.com/v1/billing?format=credits";
        constexpr const char* kClientVersion = "0.2.87";

        struct GrokAuth
        {
            std::string key;
            std::string userId;
            // xAI already stores a human-readable tier here ("SuperGrok",
            // "SuperGrok Heavy"); authMode is the fallback when it is absent.
            std::string subscriptionTier;
            std::string authMode;
        };

        // Raw tiers arrive in several shapes (supergrok_heavy, SuperGrok Heavy,
        // heavy). Normalise on letters only, the way the reference tools do.
        static std::string FormatGrokPlan(const std::string& tier, const std::string& authMode)
        {
            std::string letters;
            for (unsigned char c : tier) {
                if (std::isalpha(c)) letters.push_back(static_cast<char>(std::tolower(c)));
            }

            if (letters == "supergrokheavy" || letters == "heavy") return "SuperGrok Heavy";
            if (letters == "supergroklite") return "SuperGrok Lite";
            if (letters == "supergrok") return "SuperGrok";

            if (!letters.empty()) return tier;

            // No billing tier: an OIDC login is a paid SuperGrok account, a
            // session cookie tells us nothing more than that it is signed in.
            if (authMode == "oidc") return "SuperGrok";
            return {};
        }

        static std::string FormatMoneyFromCents(double cents)
        {
            double dollars = cents / 100.0;

            std::ostringstream out;
            out << '$' << std::fixed << std::setprecision(2) << dollars;
            return out.str();
        }

        static bool IsWeeklyPeriod(const std::string& type)
        {
            std::string lower = Text::get_instance()->ToLowerCopy(type);
            return lower.find("weekly") != std::string::npos;
        }

        static bool IsMonthlyPeriod(const std::string& type)
        {
            std::string lower = Text::get_instance()->ToLowerCopy(type);
            return lower.find("monthly") != std::string::npos;
        }

        static std::string PeriodTitle(const std::string& type)
        {
            if (IsWeeklyPeriod(type)) {
                return "Weekly Limit";
            }

            if (IsMonthlyPeriod(type)) {
                return "Monthly Limit";
            }

            return "Usage";
        }

        static std::string PeriodSubtitle(const std::string& type)
        {
            if (IsWeeklyPeriod(type)) {
                return "Shared Grok usage pool";
            }

            if (IsMonthlyPeriod(type)) {
                return "Monthly Grok usage pool";
            }

            return "Grok usage pool";
        }

        static const Json* FindAuthEntry(const Json& root, const std::string& scope)
        {
            if (!root.is_object() || !root.contains(scope)) {
                return nullptr;
            }

            const Json& entry = root.at(scope);
            return entry.is_object() ? &entry : nullptr;
        }

        static GrokAuth LoadAuthFromFile()
        {
            std::filesystem::path authPath = Network::get_instance()->UserProfilePath() / ".grok" / "auth.json";
            std::string text = Network::get_instance()->ReadRequiredTextFile(authPath);
            Json root = JsonUtils::get_instance()->ParseRequired(text);

            const Json* entry = FindAuthEntry(root, kOidcScope);

            if (!entry) {
                entry = FindAuthEntry(root, kLegacyScope);
            }

            if (!entry) {
                throw std::runtime_error("Grok auth scope not found in %USERPROFILE%\\.grok\\auth.json");
            }

            GrokAuth auth;
            auth.key = JsonUtils::get_instance()->String(*entry, "key");
            auth.userId = Text::get_instance()->FirstNonEmpty({
                JsonUtils::get_instance()->String(*entry, "user_id"),
                JsonUtils::get_instance()->String(*entry, "principal_id")
            });

            auth.subscriptionTier = Text::get_instance()->FirstNonEmpty({
                JsonUtils::get_instance()->String(*entry, "subscriptionTier"),
                JsonUtils::get_instance()->String(*entry, "subscription_tier"),
                JsonUtils::get_instance()->String(*entry, "subscription_tier_display")
            });
            auth.authMode = JsonUtils::get_instance()->String(*entry, "authMode");

            if (auth.key.empty()) {
                throw std::runtime_error("Grok auth token missing from %USERPROFILE%\\.grok\\auth.json");
            }

            return auth;
        }

        static GrokAuth LoadAuth()
        {
            // The signed-in CLI account is represented by auth.json. Prefer it over
            // inherited environment variables that can still reference an old account.
            std::filesystem::path authPath = Network::get_instance()->UserProfilePath() / ".grok" / "auth.json";

            if (std::filesystem::exists(authPath)) {
                return LoadAuthFromFile();
            }

            GrokAuth auth;
            auth.key = Network::get_instance()->GetEnvText("GROK_DEPLOYMENT_KEY");
            auth.userId = Network::get_instance()->GetEnvText("GROK_USER_ID");

            if (!auth.key.empty()) {
                return auth;
            }

            throw std::runtime_error("Grok credentials not found");
        }

        static std::wstring BuildHeaders(const GrokAuth& auth)
        {
            std::string headers;
            headers += "Authorization: Bearer " + Network::get_instance()->StripHeaderValue(auth.key) + "\r\n";
            headers += "X-Xai-Token-Auth: xai-grok-cli\r\n";

            if (!auth.userId.empty()) {
                headers += "X-Userid: " + Network::get_instance()->StripHeaderValue(auth.userId) + "\r\n";
            }

            headers += "X-Grok-Client-Version: ";
            headers += kClientVersion;
            headers += "\r\n";
            headers += "Accept: */*\r\n";
            headers += "Accept-Encoding: gzip, deflate\r\n";
            headers += "User-Agent: grok-pager/0.2.87 grok-shell/0.2.87 (windows; x86_64)\r\n";

            return Network::get_instance()->Utf8ToWide(headers);
        }

        static bool HasProductUsagePercent(const Json& config)
        {
            if (!config.is_object() || !config.contains("productUsage") ||
                !config.at("productUsage").is_array()) {
                return false;
            }

            for (const Json& item : config.at("productUsage")) {
                if (item.is_object() && JsonUtils::get_instance()->NumberOpt(item, "usagePercent")) {
                    return true;
                }
            }

            return false;
        }

        static float ProductUsageFallback(const Json& config)
        {
            if (!config.is_object() || !config.contains("productUsage") || !config.at("productUsage").is_array()) {
                return 0.0f;
            }

            double highest = 0.0;

            for (const Json& item : config.at("productUsage")) {
                if (!item.is_object()) {
                    continue;
                }

                std::optional<double> value = JsonUtils::get_instance()->NumberOpt(item, "usagePercent");

                if (value) {
                    highest = std::max(highest, *value);
                }
            }

            return static_cast<float>(Math::get_instance()->ClampPercentDouble(highest));
        }

        static void AddProductUsage(Snapshot& snapshot, const Json& config)
        {
            if (!config.is_object() || !config.contains("productUsage") || !config.at("productUsage").is_array()) {
                return;
            }

            for (const Json& item : config.at("productUsage")) {
                if (!item.is_object()) {
                    continue;
                }

                ProductUsage usage;
                usage.product = JsonUtils::get_instance()->String(item, "product");
                usage.usagePercent = static_cast<float>(Math::get_instance()->ClampPercentDouble(
                    JsonUtils::get_instance()->Number(item, "usagePercent", 0.0)
                ));

                if (!usage.product.empty()) {
                    snapshot.products.push_back(usage);
                }
            }
        }

        static Snapshot ParseBillingResponse(const Json& root)
        {
            Snapshot snapshot;
            snapshot.plan = "Grok";
            snapshot.statusText = "Updated";
            snapshot.lastUpdated = "just now";

            if (!root.is_object() || !root.contains("config") || !root.at("config").is_object()) {
                throw std::runtime_error("Grok billing response missing config object");
            }

            const Json& config = root.at("config");
            const Json* currentPeriod = nullptr;

            if (config.contains("currentPeriod") && config.at("currentPeriod").is_object()) {
                currentPeriod = &config.at("currentPeriod");
            }

            std::string periodType = currentPeriod ? JsonUtils::get_instance()->String(*currentPeriod, "type") : std::string();

            bool hasUsagePercent =
                JsonUtils::get_instance()->NumberOpt(config, "creditUsagePercent").has_value() ||
                HasProductUsagePercent(config);
            bool hasPeriod = currentPeriod != nullptr || config.contains("billingPeriodEnd");

            snapshot.weeklyLimit.valid = hasUsagePercent || hasPeriod;
            snapshot.weeklyLimit.title = PeriodTitle(periodType);
            snapshot.weeklyLimit.subtitle = PeriodSubtitle(periodType);
            snapshot.weeklyLimit.usedPercent = static_cast<float>(Math::get_instance()->ClampPercentDouble(
                JsonUtils::get_instance()->Number(config, "creditUsagePercent", ProductUsageFallback(config))
            ));

            if (currentPeriod) {
                snapshot.weeklyLimit.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(*currentPeriod, "end");
            }

            if (snapshot.weeklyLimit.resetAtUnixSeconds <= 0) {
                snapshot.weeklyLimit.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(config, "billingPeriodEnd");
            }

            if (snapshot.weeklyLimit.resetAtUnixSeconds > 0) {
                snapshot.weeklyLimit.resetText = "Resets";
            }

            double prepaidBalance = 0.0;

            if (config.contains("prepaidBalance") && config.at("prepaidBalance").is_object()) {
                prepaidBalance = JsonUtils::get_instance()->Number(config.at("prepaidBalance"), "val", 0.0);
            }

            double onDemandUsed = 0.0;

            if (config.contains("onDemandUsed") && config.at("onDemandUsed").is_object()) {
                onDemandUsed = JsonUtils::get_instance()->Number(config.at("onDemandUsed"), "val", 0.0);
            }

            snapshot.extraCredits.valid =
                (config.contains("prepaidBalance") && config.at("prepaidBalance").is_object()) ||
                (config.contains("onDemandUsed") && config.at("onDemandUsed").is_object());
            snapshot.extraCredits.balanceText = FormatMoneyFromCents(prepaidBalance);
            snapshot.extraCredits.usedText = FormatMoneyFromCents(onDemandUsed);
            snapshot.extraCredits.usedPercent = 0.0f;

            AddProductUsage(snapshot, config);

            if (!snapshot.weeklyLimit.valid && !snapshot.extraCredits.valid &&
                snapshot.products.empty()) {
                throw std::runtime_error("Grok billing response contained no usable usage data");
            }

            UsageTelemetry::SetAvailable(snapshot.access);

            if (snapshot.weeklyLimit.valid &&
                UsageTelemetry::IsExhausted(snapshot.weeklyLimit.usedPercent)) {
                snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
                snapshot.access.detail = snapshot.weeklyLimit.title + " usage exhausted";
            }

            return snapshot;
        }
    }

    Snapshot FetchSnapshot()
    {
        Snapshot snapshot;
        snapshot.lastUpdated = "now";

        try {
            GrokAuth auth = LoadAuth();
            Network::HttpResponse response = Network::get_instance()->RequestUrl(
                kBillingUrl,
                "GET",
                BuildHeaders(auth)
            );

            if (response.statusCode < 200 || response.statusCode >= 300) {
                std::string body = response.body;

                if (body.size() > 240) {
                    body.resize(240);
                }

                throw std::runtime_error("Grok HTTP " + std::to_string(response.statusCode) + (body.empty() ? std::string() : (": " + body)));
            }

            Json root = JsonUtils::get_instance()->ParseRequired(response.body);
            Snapshot parsed = ParseBillingResponse(root);

            const std::string plan = FormatGrokPlan(auth.subscriptionTier, auth.authMode);
            if (!plan.empty()) {
                parsed.plan = plan;
            }

            return parsed;
        }
        catch (const std::exception& e) {
            snapshot.statusText = std::string("Grok error: ") + e.what();
            snapshot.access = UsageTelemetry::FromText(snapshot.statusText);
            snapshot.weeklyLimit.valid = false;
            snapshot.extraCredits.valid = false;
            return snapshot;
        }
    }
}
