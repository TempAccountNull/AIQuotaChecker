#include "Global.hpp"
#include "Grok.hpp"

#include "Format.hpp"
#include "JsonUtils.hpp"
#include "Math.hpp"
#include "Network.hpp"
#include "Text.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

        // Cost ticks are 1e10 per USD (status_blocks.rs format_cost).
        constexpr double kTicksPerUsd = 1e10;
        constexpr unsigned long long kUpdatesTailBytes = 256 * 1024;
        // Grok cuts a compaction after 300 s by default; a start this old with
        // no end was abandoned (grok exited mid-compaction).
        constexpr long long kCompactionStaleSeconds = 600;

        static Json ReadJson(const std::filesystem::path& path)
        {
            return JsonUtils::get_instance()->ParseOrNull(Network::get_instance()->ReadTextFile(path));
        }

        // The newest top-level session folder. Grok renames a temp file over
        // signals.json, usage.json and summary.json, so a folder's own write
        // time moves with its session; subagents live inside their parent's.
        static std::filesystem::path NewestSessionDir(const std::filesystem::path& sessions)
        {
            std::filesystem::path newest;
            std::filesystem::file_time_type newestTime = std::filesystem::file_time_type::min();
            std::error_code ec;

            for (const std::filesystem::directory_entry& cwd : std::filesystem::directory_iterator(sessions, ec)) {
                std::error_code entryError;

                if (!cwd.is_directory(entryError)) {
                    continue;
                }

                for (const std::filesystem::directory_entry& session :
                    std::filesystem::directory_iterator(cwd.path(), entryError)) {
                    std::error_code timeError;
                    const std::filesystem::file_time_type written = session.last_write_time(timeError);

                    if (!timeError && written > newestTime && session.is_directory(timeError)) {
                        newest = session.path();
                        newestTime = written;
                    }
                }
            }

            return newest;
        }

        // models_cache.json's entry for a model: name, context_window and
        // auto_compact_threshold_percent, as /v1/models last returned them.
        static const Json* CachedModel(const Json& cache, const std::string& id)
        {
            const Json* models = JsonUtils::get_instance()->FindObject(cache, { "models" });
            const Json* model = models && !id.empty()
                ? JsonUtils::get_instance()->FindObject(*models, { id.c_str() })
                : nullptr;

            return model ? JsonUtils::get_instance()->FindObject(*model, { "info" }) : nullptr;
        }

        // resolve_auto_compact_threshold_percent's tiers: the environment, the
        // user's TOML, the server's per-model then global value, else 85. That
        // TOML isn't parsed here, so a threshold set in it leaves the trigger
        // unknown rather than wrong.
        static std::optional<int> AutoCompactPercent(const Json* model)
        {
            const std::string env = Network::get_instance()->EnvText("GROK_AUTO_COMPACT_THRESHOLD_PERCENT");
            int percent = -1;
            const auto [parsedTo, error] = std::from_chars(env.data(), env.data() + env.size(), percent);

            if (!env.empty() && error == std::errc() && parsedTo == env.data() + env.size() &&
                percent >= 0 && percent <= 100) {
                return percent;
            }

            for (const char* file : { "config.toml", "managed_config.toml" }) {
                const std::string toml = Network::get_instance()->ReadTextFile(GrokHome() / file);

                if (toml.find("auto_compact_threshold_percent") != std::string::npos) {
                    return std::nullopt;
                }
            }

            if (model) {
                if (const std::optional<double> perModel = JsonUtils::get_instance()->NumberOpt(*model, "auto_compact_threshold_percent")) {
                    return static_cast<int>(*perModel);
                }
            }

            const Json payload = JsonUtils::get_instance()->ParseOrNull(JsonUtils::get_instance()->String(
                ReadJson(GrokHome() / "settings_cache.json"), "payload"));

            if (const Json* settings = JsonUtils::get_instance()->FindObject(payload, { "settings" })) {
                if (const std::optional<double> global = JsonUtils::get_instance()->NumberOpt(*settings, "auto_compact_threshold_percent")) {
                    return static_cast<int>(*global);
                }
            }

            return 85;
        }

        // Newest line first, the end of updates.jsonl gives the live context
        // size (each streamed chunk's _meta.totalTokens, which Grok saves as
        // contextTokensUsed when the turn ends, or a compaction's result) and
        // the latest auto-compaction.
        static void ApplyUpdatesTail(const std::string& tail, long long nowUnixSeconds,
            UsageTelemetry::ContextUsage& context)
        {
            bool haveUsed = false;
            bool haveCompaction = false;

            for (size_t end = tail.size(); end > 0 && !(haveUsed && haveCompaction);) {
                const size_t newline = tail.rfind('\n', end - 1);
                const size_t start = newline == std::string::npos ? 0 : newline + 1;
                const std::string_view line(tail.data() + start, end - start);
                end = newline == std::string::npos ? 0 : newline;

                // auto_compact_* and auto_continue_completed.
                const bool compactionLine = line.find("\"auto_co") != std::string_view::npos;

                if (!compactionLine && (haveUsed || line.find("\"totalTokens\"") == std::string_view::npos)) {
                    continue;
                }

                const Json root = JsonUtils::get_instance()->ParseOrNull(std::string(line));
                const Json* params = JsonUtils::get_instance()->FindObject(root, { "params" });

                if (!params) {
                    continue;
                }

                const Json* update = JsonUtils::get_instance()->FindObject(*params, { "update" });
                const Json* meta = JsonUtils::get_instance()->FindObject(*params, { "_meta" });
                const std::string kind = update ? JsonUtils::get_instance()->String(*update, "sessionUpdate") : std::string();

                if (!haveUsed) {
                    std::optional<double> used = meta ? JsonUtils::get_instance()->NumberOpt(*meta, "totalTokens") : std::nullopt;

                    if (!used && update) {
                        used = JsonUtils::get_instance()->NumberOpt(*update,
                            kind == "auto_compact_completed" ? "tokens_after"
                            : kind == "auto_compact_started" ? "tokens_used"
                            : "total_tokens"); // auto_continue_completed
                    }

                    if (used) {
                        context.usedTokens = static_cast<long long>(*used);
                        haveUsed = true;
                    }
                }

                if (haveCompaction || kind.rfind("auto_compact_", 0) != 0) {
                    continue;
                }

                // started, completed, failed or cancelled: only the newest counts.
                haveCompaction = true;
                const long long at = JsonUtils::get_instance()->UnixSecondsField(root, "timestamp");

                if (kind == "auto_compact_started") {
                    context.compacting = at <= 0 || nowUnixSeconds - at < kCompactionStaleSeconds;
                    context.compactionStartedAtUnixSeconds = at;
                }
                else if (kind == "auto_compact_completed") {
                    const std::optional<double> before = JsonUtils::get_instance()->NumberOpt(*update, "tokens_before");
                    const std::optional<double> after = JsonUtils::get_instance()->NumberOpt(*update, "tokens_after");

                    if (before && after && *before > *after) {
                        context.compactionPreTokens = static_cast<long long>(*before);
                        context.compactionSavedTokens = static_cast<long long>(*before - *after);
                        context.compactionCompletedAtUnixSeconds = at;
                    }
                }
            }
        }

        // Grok's own /usage session block, condensed
        // (status_blocks.rs session_usage_block_text).
        static std::vector<std::string> SessionUsageLines(const Json& session)
        {
            const JsonUtils::Client* json = JsonUtils::get_instance();
            const Format* format = Format::get_instance();
            const double calls = json->Number(session, "modelCalls");

            if (calls <= 0) {
                return {};
            }

            std::vector<std::string> lines = {
                "Input: " + format->IntegerWithCommas(json->Number(session, "inputTokens")) +
                    " (" + format->IntegerWithCommas(json->Number(session, "cachedReadTokens")) + " cached)",
                "Output: " + format->IntegerWithCommas(json->Number(session, "outputTokens")) +
                    " (" + format->IntegerWithCommas(json->Number(session, "reasoningTokens")) + " reasoning)",
                "Total: " + format->IntegerWithCommas(json->Number(session, "totalTokens")) + " \xc2\xb7 " +
                    format->IntegerWithCommas(calls) + (calls == 1 ? " model call" : " model calls"),
            };

            const std::optional<double> ticks = json->NumberOpt(session, "costUsdTicks");
            lines.push_back(ticks ? "Cost: " + format->Dollars(*ticks / kTicksPerUsd, 4)
                : json->Bool(session, "costIsPartial") ? "Cost: not reported for some calls"
                : "Cost: not reported");

            if (json->Bool(session, "usageIsIncomplete")) {
                lines.push_back("Incomplete; may under-count");
            }

            return lines;
        }

        // Session totals and the latest turn give the average and latest hit.
        // Grok's input count includes the cached part.
        static void ApplyCacheStats(const Json& usage, UsageTelemetry::ContextUsage& context)
        {
            const JsonUtils::Client* json = JsonUtils::get_instance();
            const Json* session = json->FindObject(usage, { "session" });
            const double input = session ? json->Number(*session, "inputTokens") : 0.0;

            if (input <= 0.0) {
                return;
            }

            context.cacheStatsValid = true;
            context.averageCacheHitPercentValid = true;
            context.averageCacheHitPercent = json->Number(*session, "cachedReadTokens") * 100.0 / input;
            context.totalCacheInputTokens = static_cast<long long>(input);
            context.totalCacheReadTokens = static_cast<long long>(json->Number(*session, "cachedReadTokens"));
            context.totalCacheWriteTokens = static_cast<long long>(json->Number(*session, "cacheCreationTokens"));

            const Json* turns = json->FindArray(usage, { "turns" });
            const double turnInput = turns && !turns->empty() ? json->Number(turns->back(), "inputTokens") : 0.0;

            if (turnInput > 0.0) {
                context.latestCacheHitPercentValid = true;
                context.latestCacheHitPercent = json->Number(turns->back(), "cachedReadTokens") * 100.0 / turnInput;
                context.cacheInputTokens = static_cast<long long>(turnInput);
                context.cacheReadTokens = static_cast<long long>(json->Number(turns->back(), "cachedReadTokens"));
                context.cacheWriteTokens = static_cast<long long>(json->Number(turns->back(), "cacheCreationTokens"));
            }
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

    LocalSession ReadLocalSession()
    {
        LocalSession local;
        const std::filesystem::path dir = NewestSessionDir(GrokHome() / "sessions");

        if (dir.empty()) {
            return local;
        }

        // signals.json and usage.json are written when a turn ends;
        // updates.jsonl grows while it runs.
        const Json signals = ReadJson(dir / "signals.json");
        const Json usage = ReadJson(dir / "usage.json");
        const Json models = ReadJson(GrokHome() / "models_cache.json");
        const std::string modelId = JsonUtils::get_instance()->String(ReadJson(dir / "summary.json"), "current_model_id");
        const Json* model = CachedModel(models, modelId.empty()
            ? JsonUtils::get_instance()->String(signals, "primaryModelId")
            : modelId);

        UsageTelemetry::ContextUsage& context = local.context;
        context.model = model ? JsonUtils::get_instance()->String(*model, "name") : std::string();
        if (context.model.empty()) {
            context.model = modelId;
        }

        // The session's window (256K or 500K, the user's pick), else the model's default.
        context.contextWindowTokens = static_cast<long long>(JsonUtils::get_instance()->Number(signals, "contextWindowTokens"));
        if (context.contextWindowTokens <= 0 && model) {
            context.contextWindowTokens = static_cast<long long>(JsonUtils::get_instance()->Number(*model, "context_window"));
        }

        context.usedTokens = static_cast<long long>(JsonUtils::get_instance()->Number(signals, "contextTokensUsed", -1.0));
        ApplyUpdatesTail(Network::get_instance()->ReadFileTail(dir / "updates.jsonl", kUpdatesTailBytes),
            static_cast<long long>(std::time(nullptr)), context);
        context.valid = context.usedTokens >= 0 && context.contextWindowTokens > 0;
        context.sourceLabel = "Newest Grok session \xc2\xb7 direct read-only";

        if (context.valid) {
            if (const std::optional<int> percent = AutoCompactPercent(model)) {
                const long long threshold = context.contextWindowTokens * *percent / 100;

                if (threshold > 0) {
                    context.autoCompactPercentValid = true;
                    context.autoCompactThresholdTokens = threshold;
                    context.autoCompactPercentLeft = static_cast<int>(std::clamp(
                        std::llround(static_cast<double>(threshold - context.usedTokens) * 100.0 / static_cast<double>(threshold)),
                        0LL, 100LL));
                }
            }
        }

        ApplyCacheStats(usage, context);

        if (const Json* session = JsonUtils::get_instance()->FindObject(usage, { "session" })) {
            local.usageLines = SessionUsageLines(*session);
        }

        return local;
    }
}
