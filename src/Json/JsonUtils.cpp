#include "Global.hpp"

#include "JsonUtils.hpp"
#include "Text.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace
{
    // The UTC offset of an ISO 8601 time ("...T11:00:00-06:00" -> -21600);
    // 0 for "Z" or no zone, which reads as UTC.
    long long OffsetSeconds(const std::string& text)
    {
        const size_t time = text.find_first_of("T ", 8);
        const size_t zone = time == std::string::npos ? time : text.find_first_of("Zz+-", time + 1);

        if (zone == std::string::npos || text[zone] == 'Z' || text[zone] == 'z') {
            return 0;
        }

        int hours = 0;
        int minutes = 0;
        const char* digits = text.c_str() + zone + 1;
        const bool colon = text.size() > zone + 3 && text[zone + 3] == ':';

        if ((colon ? sscanf_s(digits, "%2d:%2d", &hours, &minutes) : sscanf_s(digits, "%2d%2d", &hours, &minutes)) < 1) {
            return 0;
        }

        const long long offset = hours * 3600LL + minutes * 60LL;
        return text[zone] == '-' ? -offset : offset;
    }
}

namespace JsonUtils
{
    Json Client::ParseOrNull(const std::string& text) const
    {
        try {
            return Json::parse(text);
        }
        catch (...) {
            return Json();
        }
    }

    Json Client::ParseRequired(const std::string& text) const
    {
        return Json::parse(text);
    }

    Json Client::JwtClaims(const std::string& token) const
    {
        const size_t first = token.find('.');
        const size_t second = first == std::string::npos ? first : token.find('.', first + 1);

        if (second == std::string::npos) {
            return Json();
        }

        // base64url, unpadded.
        std::string payload;
        unsigned int value = 0;
        int bits = 0;

        for (const char c : token.substr(first + 1, second - first - 1)) {
            const int digit = c >= 'A' && c <= 'Z' ? c - 'A'
                : c >= 'a' && c <= 'z' ? c - 'a' + 26
                : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '-' || c == '+' ? 62
                : c == '_' || c == '/' ? 63
                : -1;

            if (digit < 0) {
                break;
            }

            value = (value << 6) | static_cast<unsigned int>(digit);
            bits += 6;

            if (bits >= 8) {
                bits -= 8;
                payload.push_back(static_cast<char>((value >> bits) & 0xFF));
            }
        }

        const Json claims = ParseOrNull(payload);
        return claims.is_object() ? claims : Json();
    }

    std::string Client::String(const Json& value, const char* key) const
    {
        if (!value.is_object() || !value.contains(key)) {
            return {};
        }

        const Json& item = value.at(key);

        if (!item.is_string()) {
            return {};
        }

        return item.get<std::string>();
    }

    std::string Client::String(const Json& value, std::initializer_list<const char*> keys) const
    {
        for (const char* key : keys) {
            std::string text = String(value, key);

            if (!text.empty()) {
                return text;
            }
        }

        return {};
    }

    double Client::Number(const Json& value, const char* key, double fallback) const
    {
        if (!value.is_object() || !value.contains(key)) {
            return fallback;
        }

        const Json& item = value.at(key);

        if (item.is_number()) {
            return item.get<double>();
        }

        if (item.is_string()) {
            try {
                return std::stod(item.get<std::string>());
            }
            catch (...) {
                return fallback;
            }
        }

        return fallback;
    }

    std::optional<double> Client::NumberOpt(const Json& value, const char* key) const
    {
        if (!value.is_object() || !value.contains(key)) {
            return std::nullopt;
        }

        const Json& item = value.at(key);

        if (item.is_null()) {
            return std::nullopt;
        }

        if (item.is_number()) {
            return item.get<double>();
        }

        if (item.is_string()) {
            try {
                return std::stod(item.get<std::string>());
            }
            catch (...) {
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    std::optional<double> Client::NumberAny(const Json& value, std::initializer_list<const char*> keys) const
    {
        for (const char* key : keys) {
            std::optional<double> number = NumberOpt(value, key);

            if (number) {
                return number;
            }
        }

        return std::nullopt;
    }

    bool Client::Bool(const Json& value, const char* key, bool fallback) const
    {
        if (!value.is_object() || !value.contains(key)) {
            return fallback;
        }

        const Json& item = value.at(key);

        if (item.is_boolean()) {
            return item.get<bool>();
        }

        if (item.is_number_integer()) {
            return item.get<int>() != 0;
        }

        if (item.is_string()) {
            std::string text = Text::get_instance()->ToLowerCopy(item.get<std::string>());
            return text == "true" || text == "1" || text == "yes" || text == "enabled";
        }

        return fallback;
    }

    std::optional<bool> Client::BoolOpt(const Json& value, std::initializer_list<const char*> keys) const
    {
        if (!value.is_object()) {
            return std::nullopt;
        }

        for (const char* key : keys) {
            const auto item = value.find(key);

            if (item == value.end() || item->is_null()) {
                continue;
            }

            if (item->is_boolean()) {
                return item->get<bool>();
            }

            if (item->is_number()) {
                return item->get<double>() != 0.0;
            }

            if (item->is_string()) {
                const std::string text = Text::get_instance()->ToLowerCopy(item->get<std::string>());

                if (text == "true" || text == "1" || text == "yes" || text == "enabled") return true;
                if (text == "false" || text == "0" || text == "no" || text == "disabled") return false;
            }
        }

        return std::nullopt;
    }

    const Json* Client::FindObject(const Json& value, std::initializer_list<const char*> keys) const
    {
        if (!value.is_object()) {
            return nullptr;
        }

        for (const char* key : keys) {
            const auto item = value.find(key);

            if (item != value.end() && item->is_object()) {
                return &*item;
            }
        }

        return nullptr;
    }

    const Json* Client::FindArray(const Json& value, std::initializer_list<const char*> keys) const
    {
        if (!value.is_object()) {
            return nullptr;
        }

        for (const char* key : keys) {
            const auto item = value.find(key);

            if (item != value.end() && item->is_array()) {
                return &*item;
            }
        }

        return nullptr;
    }

    long long Client::UnixSeconds(const Json& value) const
    {
        if (value.is_null()) {
            return 0;
        }

        double raw = 0.0;

        if (value.is_number()) {
            raw = value.get<double>();
        }
        else if (value.is_string()) {
            std::string text = value.get<std::string>();

            try {
                size_t pos = 0;
                double parsed = std::stod(text, &pos);

                if (pos == text.size()) {
                    raw = parsed;
                }
                else {
                    std::tm tm{};
                    int year = 0;
                    int mon = 0;
                    int day = 0;
                    int hour = 0;
                    int min = 0;
                    int sec = 0;

                    if (sscanf_s(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) == 6 ||
                        sscanf_s(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &mon, &day, &hour, &min, &sec) == 6) {
                        tm.tm_year = year - 1900;
                        tm.tm_mon = mon - 1;
                        tm.tm_mday = day;
                        tm.tm_hour = hour;
                        tm.tm_min = min;
                        tm.tm_sec = sec;
                        return static_cast<long long>(_mkgmtime(&tm)) - OffsetSeconds(text);
                    }
                }
            }
            catch (...) {
                return 0;
            }
        }
        else {
            return 0;
        }

        if (raw <= 0.0) {
            return 0;
        }

        if (raw > 100000000000.0) {
            raw /= 1000.0;
        }

        return static_cast<long long>(raw);
    }

    long long Client::UnixSecondsField(const Json& object, const char* a, const char* b, const char* c) const
    {
        return UnixSecondsField(object, { a, b, c });
    }

    long long Client::UnixSecondsField(const Json& object, std::initializer_list<const char*> keys) const
    {
        if (!object.is_object()) {
            return 0;
        }

        for (const char* key : keys) {
            if (key && object.contains(key)) {
                long long value = UnixSeconds(object.at(key));

                if (value > 0) {
                    return value;
                }
            }
        }

        return 0;
    }

    Client* get_instance()
    {
        static Client client;
        return &client;
    }
}
