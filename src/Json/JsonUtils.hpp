#pragma once

#include <initializer_list>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace JsonUtils
{
    using Json = nlohmann::json;

    class Client final
    {
    public:
        Json ParseOrNull(const std::string& text) const;
        Json ParseRequired(const std::string& text) const;
        std::string String(const Json& value, const char* key) const;
        double Number(const Json& value, const char* key, double fallback = 0.0) const;
        std::optional<double> NumberOpt(const Json& value, const char* key) const;
        std::optional<double> NumberAny(const Json& value, std::initializer_list<const char*> keys) const;
        bool Bool(const Json& value, const char* key, bool fallback = false) const;
        const Json* UnwrapData(const Json& root) const;
        long long UnixSeconds(const Json& value) const;
        long long UnixSecondsField(const Json& object, const char* a, const char* b = nullptr, const char* c = nullptr) const;
    };

    Client* get_instance();

    // Compatibility wrappers for older call sites while refactors move to get_instance()->.
    Json ParseOrNull(const std::string& text);
    Json ParseRequired(const std::string& text);
    std::string String(const Json& value, const char* key);
    double Number(const Json& value, const char* key, double fallback = 0.0);
    std::optional<double> NumberOpt(const Json& value, const char* key);
    std::optional<double> NumberAny(const Json& value, std::initializer_list<const char*> keys);
    bool Bool(const Json& value, const char* key, bool fallback = false);
    const Json* UnwrapData(const Json& root);
    long long UnixSeconds(const Json& value);
    long long UnixSecondsField(const Json& object, const char* a, const char* b = nullptr, const char* c = nullptr);
}
