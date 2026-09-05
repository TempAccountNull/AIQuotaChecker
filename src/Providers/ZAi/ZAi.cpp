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
#include <initializer_list>
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

        // Prefer the enabled provider in the active ZCode config, then other
        // credential files. Environment tokens are fallback-only because an old
        // process environment survives account changes.
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
            Network::get_instance()->UserProfilePath() / ".zcode" / "v2" / "config.json";
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

    static ZCodeLocalTelemetry ReadZCodeLocalTelemetry()
    {
        ZCodeLocalTelemetry local;
        const std::filesystem::path dbPath =
            Network::get_instance()->UserProfilePath() / ".zcode" / "cli" / "db" / "db.sqlite";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dbPath, ec) || ec) {
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
        const ZCodeLocalTelemetry local = ReadZCodeLocalTelemetry();
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


    static std::string NormalizeModelName(std::string label)
    {
        const std::string compact = CompactLower(label);

        if (compact.find("turbo") != std::string::npos &&
            compact.find("glm") != std::string::npos) {
            return "GLM-5-Turbo";
        }
        if (compact.find("glm53") != std::string::npos ||
            compact.find("glm5.3") != std::string::npos) {
            return "GLM-5.3";
        }
        if (compact.find("glm52") != std::string::npos ||
            compact.find("glm5.2") != std::string::npos) {
            return "GLM-5.2";
        }

        return label;
    }

    static bool IsZaiModelLabel(const std::string& label)
    {
        const std::string normalized = NormalizeModelName(label);
        const std::string compact = CompactLower(normalized);
        // Keep future/new GLM models instead of dropping them merely because
        // AQC did not know their name at compile time.
        return compact.rfind("glm", 0) == 0;
    }

    static int ModelSortRank(const std::string& label)
    {
        const std::string normalized = NormalizeModelName(label);
        if (normalized == "GLM-5.3") return 0;
        if (normalized == "GLM-5.2") return 1;
        if (normalized == "GLM-5-Turbo") return 2;
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
            "total_units", "total", "total_tokens", "quota", "limit", "max"
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
        bar.spendBalance = true;
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

    // ZCode describes a window's PERIOD with `unit` (an enum) and `number` (a
    // multiplier) - `unit 3 / number 5` is the five-hour pool, `unit 6` the
    // weekly one, `unit 5 / number 1` the monthly tool allowance. They are not
    // quota amounts: reading them as the total made a five-hour bar compute
    // used/5*100 and peg at 100%.
    static std::string ZAiPeriodLabel(const json& limit)
    {
        const std::optional<double> unit =
            JsonUtils::get_instance()->NumberAny(limit, { "unit" });

        if (!unit) {
            return {};
        }

        const std::optional<double> number =
            JsonUtils::get_instance()->NumberAny(limit, { "number" });
        const int count = number ? static_cast<int>(*number) : 0;

        switch (static_cast<int>(*unit)) {
        case 3:
            return count > 0 ? std::to_string(count) + "-hour" : "Hourly";
        case 5:
            return count > 1 ? std::to_string(count) + "-month" : "Monthly";
        case 6:
            return count > 1 ? std::to_string(count) + "-week" : "Weekly";
        default:
            return {};
        }
    }

    static void AddQuotaLimitBar(Snapshot& snapshot, const json& limit)
    {
        if (!limit.is_object()) {
            return;
        }

        std::optional<double> total = JsonUtils::get_instance()->NumberAny(limit, {
            "total", "total_tokens", "quota", "limit", "max", "total_units"
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

        // Prefer the period the server describes over whatever generic name
        // PickUsageLabel settled on - it is what tells two bars apart.
        const std::string period = ZAiPeriodLabel(limit);

        if (!period.empty()) {
            const std::string lower = Text::get_instance()->ToLowerCopy(bar.label);

            if (bar.label.empty() || lower == "quota" || lower == "usage") {
                bar.label = period;
            }
            else if (lower.find(Text::get_instance()->ToLowerCopy(period)) == std::string::npos) {
                bar.sublabel = period;
            }
        }

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

    static bool EnvelopeSucceeded(const json& root)
    {
        if (!root.is_object()) {
            return false;
        }

        if (root.contains("success") && root.at("success").is_boolean() &&
            !root.at("success").get<bool>()) {
            return false;
        }

        if (root.contains("code") && root.at("code").is_number()) {
            const int code = root.at("code").get<int>();
            return code == 0 || code == 200;
        }

        return true;
    }

    static bool ApplyCurrentResponse(Snapshot& snapshot, const json& root)
    {
        const json* data = JsonUtils::get_instance()->UnwrapData(root);

        if (!EnvelopeSucceeded(root) || !data || !data->is_object() ||
            !data->contains("plans") || !data->at("plans").is_array()) {
            return false;
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

        // Match ZCode itself: only an active Start Plan is authoritative.
        // Never select the first arbitrary/inactive plan as a fallback.
        if (!selected) {
            return false;
        }

        std::string name = Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(*selected, "name"), JsonUtils::get_instance()->String(*selected, "plan_id") });

        if (!name.empty()) {
            snapshot.plan = "Z.Ai " + name;
        }

        AddDetail(snapshot, Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(*selected, "status"), "unknown" }), "Plan status", Text::get_instance()->FirstNonEmpty({ JsonUtils::get_instance()->String(*selected, "plan_id"), JsonUtils::get_instance()->String(*selected, "user_plan_id") }), "Plan ID");
        return true;
    }

    static bool ApplyBalanceResponse(Snapshot& snapshot, const json& root)
    {
        if (!EnvelopeSucceeded(root)) {
            return false;
        }

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

        return snapshot.bars.size() > before;
    }

    // /api/v1/mcp/usage -> data.total_usage {used,limit,remaining} plus
    // next_refresh_at and level. Absent or malformed simply leaves it hidden.
    static bool ApplyMcpUsageResponse(Snapshot& snapshot, const json& root)
    {
        if (!root.is_object() || !root.contains("data") || !root.at("data").is_object()) {
            return false;
        }

        const json& data = root.at("data");
        const json* usage = nullptr;

        if (data.contains("total_usage") && data.at("total_usage").is_object()) {
            usage = &data.at("total_usage");
        }
        else if (data.contains("used") && data.contains("limit")) {
            usage = &data;
        }

        if (!usage) {
            return false;
        }

        auto number = [](const json& object, const char* key) -> long long {
            const auto it = object.find(key);
            return (it != object.end() && it->is_number())
                ? static_cast<long long>(it->get<double>())
                : 0;
        };

        McpUsage mcp;
        mcp.used = number(*usage, "used");
        mcp.limit = number(*usage, "limit");
        mcp.remaining = number(*usage, "remaining");

        if (mcp.limit <= 0 && mcp.used <= 0) {
            return false;
        }

        const auto level = data.find("level");
        if (level != data.end() && level->is_string()) {
            mcp.level = level->get<std::string>();
        }

        const long long nextRefresh = number(data, "next_refresh_at");
        if (nextRefresh > 0) {
            // The service reports seconds; tolerate milliseconds.
            mcp.nextRefreshAtUnixSeconds = nextRefresh > 100000000000LL
                ? nextRefresh / 1000
                : nextRefresh;
        }

        mcp.valid = true;
        snapshot.mcp = std::move(mcp);
        return true;
    }

    static bool ApplyQuotaResponse(Snapshot& snapshot, const json& root)
    {
        if (!EnvelopeSucceeded(root)) {
            return false;
        }

        const json* data = JsonUtils::get_instance()->UnwrapData(root);

        if (!data || !data->is_object()) {
            return false;
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

        return snapshot.bars.size() > before;
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

    static void FinalizeZAiAccess(Snapshot& snapshot)
    {
        if (snapshot.bars.empty()) {
            snapshot.access.state = UsageTelemetry::AccessState::Unavailable;
            snapshot.access.detail = "No usable Z.Ai usage data was returned";
            return;
        }

        UsageTelemetry::SetAvailable(snapshot.access);

        std::vector<std::string> exhaustedBalances;
        std::vector<std::string> exhaustedRateLimits;
        size_t rateLimitCount = 0;

        for (const UsageBar& bar : snapshot.bars) {
            if (!bar.valid) {
                continue;
            }

            if (!bar.spendBalance) {
                ++rateLimitCount;
            }

            if (!UsageTelemetry::IsExhausted(bar.usedPercent)) {
                continue;
            }

            std::vector<std::string>& target = bar.spendBalance
                ? exhaustedBalances
                : exhaustedRateLimits;
            target.push_back(bar.label.empty() ? "Usage" : bar.label);
        }

        auto join = [](const std::vector<std::string>& values) {
            std::string text;

            for (size_t i = 0; i < values.size(); ++i) {
                if (i != 0) text += ", ";
                text += values[i];
            }

            return text;
        };

        if (!exhaustedBalances.empty()) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = "Usage balance exhausted: " + join(exhaustedBalances);
        }
        else if (rateLimitCount > 0 && exhaustedRateLimits.size() == rateLimitCount) {
            snapshot.access.state = UsageTelemetry::AccessState::OutOfUsage;
            snapshot.access.detail = "All returned usage allocations are exhausted";
        }
        else if (!exhaustedRateLimits.empty()) {
            snapshot.access.detail = "Some usage allocations are exhausted: " + join(exhaustedRateLimits);
        }
    }

    Snapshot FetchSnapshot()
    {
        Snapshot snapshot;
        snapshot.lastUpdated = "now";
        const ZCodeLocalTelemetry local = ReadZCodeLocalTelemetry();
        ApplyLocalTelemetryToSnapshot(snapshot, local);

        std::vector<std::string> tokens = LoadCandidateTokens();

        if (tokens.empty()) {
            snapshot.statusText = "Z.Ai credentials not found. Sign in to ZCode or set ZCODE_JWT_TOKEN.";
            snapshot.access = UsageTelemetry::FromText(snapshot.statusText);
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
        addUrl("https://api.z.ai/api/v1/mcp/usage", "mcp");

        std::string lastError;
        std::string bestUnavailableReason;
        UsageTelemetry::AccessStatus strongestFailure;

        for (const std::string& token : tokens) {
            Snapshot candidate;
            candidate.lastUpdated = "now";
            ApplyLocalTelemetryToSnapshot(candidate, local);
            bool activeStartPlan = false;
            bool usableUsageData = false;
            bool receivedSuccessfulResponse = false;

            for (const auto& item : urls) {
                try {
                    std::wstring headers = (std::string(item.second) == "quota" ||
                        std::string(item.second) == "subscription" ||
                        std::string(item.second) == "mcp")
                        ? Network::get_instance()->RawAuthorizationJsonHeaders(token, "https://zcode.z.ai", "https://zcode.z.ai/")
                        : Network::get_instance()->BearerJsonHeaders(token, "https://zcode.z.ai", "https://zcode.z.ai/");

                    Network::HttpResponse response = Network::get_instance()->RequestUrl(item.first, "GET", headers);

                    if (!ResponseOk(response)) {
                        std::ostringstream ss;
                        ss << item.second << " HTTP " << response.statusCode;
                        lastError = ss.str();

                        UsageTelemetry::AccessStatus failure = UsageTelemetry::FromHttpFailure(
                            response.statusCode,
                            response.body,
                            lastError
                        );

                        auto rank = [](UsageTelemetry::AccessState state) {
                            switch (state) {
                            case UsageTelemetry::AccessState::OutOfUsage: return 4;
                            case UsageTelemetry::AccessState::RateLimited: return 3;
                            case UsageTelemetry::AccessState::Unavailable: return 2;
                            default: return 1;
                            }
                        };

                        if (rank(failure.state) > rank(strongestFailure.state)) {
                            strongestFailure = std::move(failure);
                        }
                        continue;
                    }

                    json root = JsonUtils::get_instance()->ParseOrNull(response.body);

                    if (root.is_discarded() || root.is_null()) {
                        lastError = std::string(item.second) + " returned invalid JSON";
                        continue;
                    }

                    if (!EnvelopeSucceeded(root)) {
                        lastError = std::string(item.second) + " returned an unsuccessful envelope";
                        continue;
                    }

                    receivedSuccessfulResponse = true;
                    std::string kind = item.second;

                    if (kind == "current") {
                        activeStartPlan = ApplyCurrentResponse(candidate, root) || activeStartPlan;
                    }
                    else if (kind == "balance") {
                        // The current ZCode balance envelope can include both
                        // plans and balances, so inspect both sections.
                        activeStartPlan = ApplyCurrentResponse(candidate, root) || activeStartPlan;
                        usableUsageData = ApplyBalanceResponse(candidate, root) || usableUsageData;
                    }
                    else if (kind == "quota") {
                        usableUsageData = ApplyQuotaResponse(candidate, root) || usableUsageData;
                    }
                    else if (kind == "subscription") {
                        ApplySubscriptionList(candidate, root);
                    }
                    else if (kind == "mcp") {
                        ApplyMcpUsageResponse(candidate, root);
                    }
                }
                catch (const std::exception& e) {
                    lastError = e.what();
                    UsageTelemetry::AccessStatus failure = UsageTelemetry::FromText(lastError);

                    if (strongestFailure.state == UsageTelemetry::AccessState::Unknown) {
                        strongestFailure = std::move(failure);
                    }
                }
            }

            if (receivedSuccessfulResponse) {
                FinalizeZaiBars(candidate);
                usableUsageData = usableUsageData || !candidate.bars.empty();

                if (!usableUsageData || candidate.bars.empty()) {
                    bestUnavailableReason = activeStartPlan
                        ? "Z.Ai usage unavailable: the active plan returned no usage balances"
                        : "Z.Ai usage unavailable: no active ZCode Start Plan or usable quota was returned";
                    continue;
                }

                candidate.statusText = activeStartPlan
                    ? "Source: active ZCode Start Plan"
                    : "Source: Z.Ai quota endpoint";
                FinalizeZAiAccess(candidate);
                return candidate;
            }
        }

        snapshot.statusText = bestUnavailableReason.empty()
            ? "Z.Ai usage unavailable"
            : bestUnavailableReason;

        if (bestUnavailableReason.empty() && !lastError.empty()) {
            snapshot.statusText += ": " + lastError;
        }

        if (!bestUnavailableReason.empty()) {
            snapshot.access.state = UsageTelemetry::AccessState::Unavailable;
            snapshot.access.detail = snapshot.statusText;
        }
        else {
            snapshot.access = strongestFailure.state == UsageTelemetry::AccessState::Unknown
                ? UsageTelemetry::FromText(snapshot.statusText)
                : strongestFailure;
        }
        return snapshot;
    }
}
