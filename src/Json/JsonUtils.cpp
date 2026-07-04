#include "Global.hpp"

#include "JsonUtils.hpp"
#include "Text.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

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

    static std::string ToLowerCopy(std::string text)
    {
        return Text::get_instance()->ToLowerCopy(text);
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
            std::string text = ToLowerCopy(item.get<std::string>());
            return text == "true" || text == "1" || text == "yes" || text == "enabled";
        }

        return fallback;
    }

    const Json* Client::UnwrapData(const Json& root) const
    {
        if (root.is_object() && root.contains("data")) {
            return &root.at("data");
        }

        return &root;
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
                        return static_cast<long long>(_mkgmtime(&tm));
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
        if (!object.is_object()) {
            return 0;
        }

        const char* keys[] = { a, b, c };

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

    Json ParseOrNull(const std::string& text) { return get_instance()->ParseOrNull(text); }
    Json ParseRequired(const std::string& text) { return get_instance()->ParseRequired(text); }
    std::string String(const Json& value, const char* key) { return get_instance()->String(value, key); }
    double Number(const Json& value, const char* key, double fallback) { return get_instance()->Number(value, key, fallback); }
    std::optional<double> NumberOpt(const Json& value, const char* key) { return get_instance()->NumberOpt(value, key); }
    std::optional<double> NumberAny(const Json& value, std::initializer_list<const char*> keys) { return get_instance()->NumberAny(value, keys); }
    bool Bool(const Json& value, const char* key, bool fallback) { return get_instance()->Bool(value, key, fallback); }
    const Json* UnwrapData(const Json& root) { return get_instance()->UnwrapData(root); }
    long long UnixSeconds(const Json& value) { return get_instance()->UnixSeconds(value); }
    long long UnixSecondsField(const Json& object, const char* a, const char* b, const char* c) { return get_instance()->UnixSecondsField(object, a, b, c); }
}
