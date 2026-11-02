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
        // The first of `keys` holding a non-empty string.
        std::string String(const Json& value, std::initializer_list<const char*> keys) const;
        double Number(const Json& value, const char* key, double fallback = 0.0) const;
        std::optional<double> NumberOpt(const Json& value, const char* key) const;
        std::optional<double> NumberAny(const Json& value, std::initializer_list<const char*> keys) const;
        bool Bool(const Json& value, const char* key, bool fallback = false) const;
        // The first of `keys` holding a boolean, a number (non-zero is true) or
        // "true" / "yes" / "1" / "enabled" (or the opposite); nullopt when none
        // does, so a missing field stays distinguishable from false.
        std::optional<bool> BoolOpt(const Json& value, std::initializer_list<const char*> keys) const;
        // The first of `keys` holding an object / an array; null when none does.
        const Json* FindObject(const Json& value, std::initializer_list<const char*> keys) const;
        const Json* FindArray(const Json& value, std::initializer_list<const char*> keys) const;
        // Epoch seconds or milliseconds, or an ISO 8601 time (its UTC offset
        // honoured); 0 when absent or unreadable.
        long long UnixSeconds(const Json& value) const;
        long long UnixSecondsField(const Json& object, const char* a, const char* b = nullptr, const char* c = nullptr) const;
        // The first of `keys` holding a readable time; 0 when none does.
        long long UnixSecondsField(const Json& object, std::initializer_list<const char*> keys) const;
        // The (unverified) payload of a JWT; null when it is not one.
        Json JwtClaims(const std::string& token) const;
    };

    Client* get_instance();
}
