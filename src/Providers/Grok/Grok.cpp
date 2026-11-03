#include "Global.hpp"
#include "Grok.hpp"

#include "Format.hpp"
#include "JsonUtils.hpp"
#include "Math.hpp"
#include "Network.hpp"
#include "Text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Grok
{
    namespace
    {
        using Json = JsonUtils::Json;

        // auth.json scopes in the CLI's lookup order: its OAuth client, then
        // the pre-OIDC sign-in it inherits. A plain API key can't read billing.
        constexpr const char* kOidcScope = "https://auth.x.ai::b1a00492-073a-47ea-816f-4c329264a828";
        constexpr const char* kLegacyScope = "https://accounts.x.ai/sign-in";
        constexpr const char* kApiKeyScope = "xai::api_key";
        constexpr const char* kXaiIssuer = "https://auth.x.ai";
        constexpr const char* kProxyBase = "https://cli-chat-proxy.grok.com/v1";
        // The installed CLI's version comes from version.json; this stands in
        // when that file is missing.
        constexpr const char* kClientVersion = "1.0.70";
        // A credential without expires_at lives this long from create_time.
        constexpr long long kTokenTtlSeconds = 30LL * 24 * 3600;

        struct GrokAuth
        {
            std::string key;
            std::string userId;
            std::string plan;
        };

        // %GROK_HOME%, else %USERPROFILE%\.grok.
        static std::filesystem::path GrokHome()
        {
            const std::filesystem::path home = Network::get_instance()->EnvPath("GROK_HOME");
            return home.empty() ? Network::get_instance()->UserProfilePath() / ".grok" : home;
        }

        // A USD-cents field ({"val": 1234}). proto3 omits a zero val, so "{}" is $0.
        static std::optional<long long> Cents(const Json& object, const char* key)
        {
            const Json* cent = JsonUtils::get_instance()->FindObject(object, { key });

            if (!cent) {
                return std::nullopt;
            }

            return static_cast<long long>(JsonUtils::get_instance()->Number(*cent, "val", 0.0));
        }

        // Billing keeps credits as negative cents (accounting convention);
        // amounts are shown absolute, as Grok's own clients do.
        static std::string Money(long long cents)
        {
            return Format::get_instance()->Dollars(static_cast<double>(std::llabs(cents)) / 100.0);
        }

        // The JWT's numeric `tier` claim, named as Grok's settings display it.
        // The CLI maps the claim the same way.
        static std::string PlanFromToken(const std::string& token)
        {
            const std::optional<double> tier = JsonUtils::get_instance()->NumberOpt(
                JsonUtils::get_instance()->JwtClaims(token), "tier");

            if (!tier) {
                return {};
            }

            switch (static_cast<int>(*tier)) {
            case 0: return "Free";
            case 1: return "SuperGrok";
            case 2: return "X Basic";
            case 3: return "X Premium";
            case 4: return "X Premium+";
            case 5: return "SuperGrok Heavy";
            case 6: return "SuperGrok Lite";
            case 7: return "SuperGrok Plus";
            default: return {};
            }
        }

        // The usage period's proto enum (USAGE_PERIOD_TYPE_WEEKLY) picks the
        // title; anything else is the generic pool.
        static std::string PeriodTitle(const std::string& type)
        {
            const std::string lower = Text::get_instance()->ToLowerCopy(type);

            if (lower.find("weekly") != std::string::npos) return "Weekly Limit";
            if (lower.find("monthly") != std::string::npos) return "Monthly Limit";
            return "Usage";
        }

        // "PRODUCT_GROK_BUILD" -> "Grok Build".
        static std::string ProductLabel(std::string raw)
        {
            if (raw.rfind("PRODUCT_", 0) == 0) {
                raw.erase(0, 8);
            }

            bool wordStart = true;
            for (char& c : raw) {
                if (c == '_') {
                    c = ' ';
                    wordStart = true;
                    continue;
                }

                const unsigned char u = static_cast<unsigned char>(c);
                c = static_cast<char>(wordStart ? std::toupper(u) : std::tolower(u));
                wordStart = false;
            }

            return raw;
        }

        static float FlooredPercent(double percent)
        {
            return static_cast<float>(std::floor(Math::get_instance()->ClampPercentDouble(percent)));
        }

        static GrokAuth LoadAuthFromFile(const std::filesystem::path& authPath, long long nowUnixSeconds)
        {
            // A parse error message would quote the file, token included.
            const Json root = JsonUtils::get_instance()->ParseOrNull(
                Network::get_instance()->ReadRequiredTextFile(authPath)
            );

            if (!root.is_object()) {
                throw std::runtime_error("auth.json is not valid JSON");
            }

            const Json* entry = JsonUtils::get_instance()->FindObject(root, { kOidcScope, kLegacyScope });

            if (!entry) {
                throw std::runtime_error(root.contains(kApiKeyScope)
                    ? "usage needs a grok.com sign-in; run grok login"
                    : "credentials not found; run grok login");
            }

            // Billing answers only an auth.x.ai sign-in; the CLI refuses the
            // rest before asking, and ignores the old web-login tokens outright.
            const std::string mode = JsonUtils::get_instance()->String(*entry, "auth_mode");

            if (mode == "web_login" || mode == "grok") {
                throw std::runtime_error("sign-in is outdated; run grok login");
            }

            if ((mode != "oidc" && mode != "external") ||
                JsonUtils::get_instance()->String(*entry, "oidc_issuer") != kXaiIssuer) {
                throw std::runtime_error("usage needs a grok.com sign-in; run grok login");
            }

            long long expiresAt = JsonUtils::get_instance()->UnixSecondsField(*entry, "expires_at");

            if (expiresAt <= 0) {
                const long long created = JsonUtils::get_instance()->UnixSecondsField(*entry, "create_time");
                expiresAt = created > 0 ? created + kTokenTtlSeconds : 0;
            }

            // Renewing it here would rotate the CLI's refresh token; grok
            // renews its own when it runs.
            if (expiresAt > 0 && nowUnixSeconds >= expiresAt) {
                throw std::runtime_error("token expired; open grok to renew it");
            }

            GrokAuth auth;
            auth.key = JsonUtils::get_instance()->String(*entry, "key");
            auth.userId = JsonUtils::get_instance()->String(*entry, { "user_id", "principal_id" });

            if (auth.key.empty()) {
                throw std::runtime_error("auth token missing from auth.json");
            }

            auth.plan = PlanFromToken(auth.key);
            return auth;
        }

        static GrokAuth LoadAuth(long long nowUnixSeconds)
        {
            const std::filesystem::path authPath = GrokHome() / "auth.json";
            std::error_code ec;

            if (!std::filesystem::exists(authPath, ec)) {
                throw std::runtime_error("credentials not found; run grok login");
            }

            return LoadAuthFromFile(authPath, nowUnixSeconds);
        }

        static std::string ClientVersion()
        {
            const std::string version = JsonUtils::get_instance()->String(
                JsonUtils::get_instance()->ParseOrNull(
                    Network::get_instance()->ReadTextFile(GrokHome() / "version.json")),
                "version");

            return version.empty() ? kClientVersion : Network::get_instance()->StripHeaderValue(version);
        }

        // The headers the CLI's own billing request sends.
        static std::wstring BuildHeaders(const GrokAuth& auth, const std::string& version)
        {
            std::string headers;
            headers += "Authorization: Bearer " + Network::get_instance()->StripHeaderValue(auth.key) + "\r\n";
            headers += "X-XAI-Token-Auth: xai-grok-cli\r\n";

            if (!auth.userId.empty()) {
                headers += "x-userid: " + Network::get_instance()->StripHeaderValue(auth.userId) + "\r\n";
            }

            headers += "x-grok-client-version: " + version + "\r\n";
            headers += "x-grok-client-mode: interactive\r\n";
            headers += "Accept: */*\r\n";
            headers += "Accept-Encoding: gzip, deflate\r\n";
            headers += "User-Agent: grok-pager/" + version + " grok-shell/" + version + " (windows; x86_64)\r\n";

            return Network::get_instance()->Utf8ToWide(headers);
        }

        // Newest first: "Oct 26–Nov 2 · $1.20 on-demand" for the weekly credit
        // periods, "Mar 2025 · $18.00 used" for the legacy monthly cycles.
        static std::vector<std::string> HistoryLines(const Json& config)
        {
            constexpr size_t kShown = 6;
            static const char* const kMonths[] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };

            std::vector<std::string> lines;
            const Json* history = JsonUtils::get_instance()->FindArray(config, { "history" });

            if (!history) {
                return lines;
            }

            for (auto entry = history->rbegin(); entry != history->rend() && lines.size() < kShown; ++entry) {
                const long long onDemand = Cents(*entry, "onDemandUsed").value_or(0);

                if (const Json* period = JsonUtils::get_instance()->FindObject(*entry, { "period" })) {
                    const long long start = JsonUtils::get_instance()->UnixSecondsField(*period, "start");
                    const long long end = JsonUtils::get_instance()->UnixSecondsField(*period, "end");

                    if (start <= 0) {
                        continue;
                    }

                    std::string line = Format::get_instance()->MonthDay(start);
                    if (end > 0) {
                        line += "\xe2\x80\x93" + Format::get_instance()->MonthDay(end);
                    }
                    lines.push_back(line + " \xc2\xb7 " + Money(onDemand) + " on-demand");
                }
                else if (const Json* cycle = JsonUtils::get_instance()->FindObject(*entry, { "billingCycle" })) {
                    const int year = static_cast<int>(JsonUtils::get_instance()->Number(*cycle, "year"));
                    const int month = static_cast<int>(JsonUtils::get_instance()->Number(*cycle, "month"));

                    if (year <= 0 || month < 1 || month > 12) {
                        continue;
                    }

                    const long long total = Cents(*entry, "totalUsed")
                        .value_or(Cents(*entry, "includedUsed").value_or(0) + onDemand);
                    std::string line = std::string(kMonths[month - 1]) + " " + std::to_string(year) +
                        " \xc2\xb7 " + Money(total) + " used";
                    if (onDemand != 0) {
                        line += " (" + Money(onDemand) + " on-demand)";
                    }
                    lines.push_back(std::move(line));
                }
            }

            return lines;
        }

        // The rule's amounts are cents like the rest of billing. proto3 omits
        // false, so a disabled rule arrives without "enabled".
        static AutoTopUp ParseAutoTopUp(const Json& root)
        {
            AutoTopUp topUp;

            if (!root.is_object()) {
                return topUp;
            }

            topUp.valid = true;
            const Json* rule = JsonUtils::get_instance()->FindObject(root, { "rule" });
            topUp.enabled = rule && JsonUtils::get_instance()->Bool(*rule, "enabled");

            if (topUp.enabled) {
                if (const std::optional<long long> amount = Cents(*rule, "topupAmount")) {
                    topUp.amountText = Money(*amount);
                }
                if (const std::optional<long long> limit = Cents(*rule, "maxAmountPerMonth")) {
                    topUp.monthlyLimitText = Money(*limit);
                }
            }

            return topUp;
        }

        // Best-effort, like the CLI: a failed fetch leaves the rule unknown
        // rather than reading as "off".
        static AutoTopUp FetchAutoTopUp(const std::wstring& headers)
        {
            const Network::HttpResponse response = Network::get_instance()->TryRequestUrl(
                std::string(kProxyBase) + "/auto-topup-rule", "GET", headers);

            if (response.statusCode < 200 || response.statusCode >= 300) {
                return {};
            }

            return ParseAutoTopUp(JsonUtils::get_instance()->ParseOrNull(response.body));
        }

        static Snapshot ParseBillingResponse(const Json& root)
        {
            const Json* config = JsonUtils::get_instance()->FindObject(root, { "config" });

            if (!config) {
                throw std::runtime_error("no billing data available");
            }

            Snapshot snapshot;
            snapshot.statusText = "Updated";
            snapshot.lastUpdated = "just now";

            // As the CLI computes it: creditUsagePercent, else the legacy
            // used / monthlyLimit. proto3 omits a zero, so no percent means
            // 0%. Floored like the backend's limiter: 99.99% is 99%, never
            // 100% until the allowance is spent.
            const long long limit = Cents(*config, "monthlyLimit").value_or(0);
            const long long used = Cents(*config, "used").value_or(0);
            double percent = 0.0;

            if (const std::optional<double> credit = JsonUtils::get_instance()->NumberOpt(*config, "creditUsagePercent")) {
                percent = *credit;
            }
            else if (limit > 0) {
                percent = static_cast<double>(used) * 100.0 / static_cast<double>(limit);
            }

            const Json* period = JsonUtils::get_instance()->FindObject(*config, { "currentPeriod" });
            UsageWindow& window = snapshot.weeklyLimit;
            window.valid = true;
            window.title = PeriodTitle(period ? JsonUtils::get_instance()->String(*period, "type") : std::string());
            window.usedPercent = FlooredPercent(percent);
            window.resetAtUnixSeconds = period ? JsonUtils::get_instance()->UnixSecondsField(*period, "end") : 0;

            if (window.resetAtUnixSeconds <= 0) {
                window.resetAtUnixSeconds = JsonUtils::get_instance()->UnixSecondsField(*config, "billingPeriodEnd");
            }

            if (window.resetAtUnixSeconds > 0) {
                window.resetText = "Resets";
            }

            // Per-product usage (PRODUCT_GROK_BUILD); the CLI doesn't show it yet.
            if (const Json* products = JsonUtils::get_instance()->FindArray(*config, { "productUsage" })) {
                for (const Json& item : *products) {
                    const std::string product = JsonUtils::get_instance()->String(item, "product");

                    if (!product.empty()) {
                        snapshot.products.push_back({ ProductLabel(product),
                            FlooredPercent(JsonUtils::get_instance()->Number(item, "usagePercent")) });
                    }
                }
            }

            // Pay-as-you-go is on while the on-demand cap is above zero; its
            // spend falls back to the legacy overage past the monthly limit.
            const long long prepaid = std::llabs(Cents(*config, "prepaidBalance").value_or(0));
            const long long cap = std::llabs(Cents(*config, "onDemandCap").value_or(0));
            const long long onDemandUsed = std::llabs(
                Cents(*config, "onDemandUsed").value_or(std::max(used - limit, 0LL)));

            ExtraCredits& extra = snapshot.extraCredits;
            extra.valid = prepaid > 0 || cap > 0;

            if (prepaid > 0) {
                extra.balanceText = Money(prepaid);
            }

            if (cap > 0) {
                extra.usedText = Money(onDemandUsed);
                extra.capText = Money(cap);
            }

            snapshot.history = HistoryLines(*config);

            // A spent allowance draws on the credits, then on the cap.
            UsageTelemetry::SetAvailable(snapshot.access);

            if (UsageTelemetry::IsExhausted(window.usedPercent) && prepaid <= 0 && onDemandUsed >= cap) {
                snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
                snapshot.access.detail = window.title + " usage exhausted";
            }

            return snapshot;
        }

        // The server's "error" text when the body has one, else the body.
        static std::string ErrorDetail(const std::string& body)
        {
            const std::string error = JsonUtils::get_instance()->String(
                JsonUtils::get_instance()->ParseOrNull(body), "error");

            return error.empty() ? body.substr(0, 240) : error;
        }
    }

    Snapshot FetchSnapshot()
    {
        Snapshot snapshot;
        snapshot.lastUpdated = "now";

        try {
            const GrokAuth auth = LoadAuth(static_cast<long long>(std::time(nullptr)));
            const std::wstring headers = BuildHeaders(auth, ClientVersion());
            const Network::HttpResponse response = Network::get_instance()->RequestUrl(
                std::string(kProxyBase) + "/billing?format=credits", "GET", headers);

            if (response.statusCode < 200 || response.statusCode >= 300) {
                const std::string detail = ErrorDetail(response.body);
                snapshot.statusText = "Grok HTTP " + std::to_string(response.statusCode) +
                    (detail.empty() ? std::string() : ": " + detail);
                snapshot.access = UsageTelemetry::FromHttpFailure(response.statusCode, response.body, snapshot.statusText);
            }
            else {
                snapshot = ParseBillingResponse(JsonUtils::get_instance()->ParseRequired(response.body));

                if (!snapshot.extraCredits.balanceText.empty()) {
                    snapshot.extraCredits.autoTopUp = FetchAutoTopUp(headers);
                }
            }

            if (!auth.plan.empty()) {
                snapshot.plan = auth.plan;
            }
        }
        catch (const std::exception& e) {
            snapshot.statusText = std::string("Grok error: ") + e.what();
            snapshot.access = UsageTelemetry::FromText(snapshot.statusText);
        }

        return snapshot;
    }
}
