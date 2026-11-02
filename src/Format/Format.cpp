#include "Global.hpp"
#include "Format.hpp"
#include "Math.hpp"

#include <cctype>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

Format* Format::get_instance()
{
    static Format instance;
    return &instance;
}

std::string Format::Percent(double value) const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << value << "%";
    return out.str();
}

std::string Format::QuotaLeft(float usedPercent, const std::string& noun) const
{
    float remaining = Math::get_instance()->PercentRemaining(usedPercent);

    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << remaining << "% " << noun << " left";
    return out.str();
}

std::string Format::Dollars(double value, int decimals) const
{
    std::ostringstream out;
    out << "$" << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}

// Inserts thousands separators into the integer digits of a plain number.
static std::string GroupThousands(std::string text)
{
    const size_t start = !text.empty() && text.front() == '-' ? 1 : 0;
    const size_t end = std::min(text.find('.'), text.size());

    for (size_t at = end; at > start + 3; at -= 3) {
        text.insert(at - 3, ",");
    }

    return text;
}

std::string Format::IntegerWithCommas(double value) const
{
    // Rounded as a double and printed whole: llround overflowed past 2^63.
    // Adding 0.0 turns a rounded -0 into 0.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(0) << std::round(value) + 0.0;
    return GroupThousands(out.str());
}

std::string Format::CreditAmount(double value) const
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2) << value;
    std::string text = out.str();

    while (!text.empty() && text.back() == '0' && text.find('.') != std::string::npos) text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text == "-0") text = "0";

    return GroupThousands(text);
}

std::string Format::MonthDay(long long unixSeconds) const
{
    return LocalTime(unixSeconds, "%b %d");
}

std::string Format::LocalDateKey(long long unixSeconds, int offsetDays) const
{
    const std::time_t at = static_cast<std::time_t>(unixSeconds);
    std::tm date{};

    if (localtime_s(&date, &at) != 0) {
        return {};
    }

    // Day arithmetic on the calendar, from noon, so a 23- or 25-hour DST day
    // cannot land on the wrong date.
    date.tm_mday += offsetDays;
    date.tm_hour = 12;
    date.tm_isdst = -1;

    if (std::mktime(&date) == -1) {
        return {};
    }

    char text[16]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d", &date);
    return text;
}

std::string Format::UtcDateKey(long long unixSeconds, int offsetDays) const
{
    const std::time_t at = static_cast<std::time_t>(unixSeconds + offsetDays * 86400LL);
    std::tm date{};
    char text[16]{};

    if (gmtime_s(&date, &at) == 0) {
        std::strftime(text, sizeof(text), "%Y-%m-%d", &date);
    }

    return text;
}

std::string Format::UnixResetTime(long long unixSeconds) const
{
    if (unixSeconds <= 0) {
        return "the next reset";
    }

    return LocalTime(unixSeconds, "%a %I:%M %p");
}

std::string Format::ResetShort(long long unixSeconds) const
{
    if (unixSeconds <= 0) {
        return {};
    }

    return ResetRelative(std::chrono::system_clock::from_time_t(static_cast<std::time_t>(unixSeconds)));
}

std::string Format::ResetDateTime(long long unixSeconds) const
{
    if (unixSeconds <= 0) {
        return {};
    }

    return LocalTime(unixSeconds, "%a %b %d %I:%M %p");
}

std::string Format::ExpiryTime(std::chrono::system_clock::time_point tp) const
{
    return LocalTime(std::chrono::system_clock::to_time_t(tp), "%b %d %I:%M %p");
}

std::string Format::ResetRelative(std::chrono::system_clock::time_point tp) const
{
    using namespace std::chrono;

    if (tp.time_since_epoch().count() == 0) {
        return {};
    }

    auto now = system_clock::now();
    auto totalSeconds = duration_cast<seconds>(tp - now).count();

    if (totalSeconds <= 0) {
        return "Reset available";
    }

    long long days = totalSeconds / 86400;
    totalSeconds %= 86400;
    long long hours = totalSeconds / 3600;
    totalSeconds %= 3600;
    long long minutes = totalSeconds / 60;

    std::ostringstream out;
    out << "Resets in ";

    if (days > 0) {
        out << days << "d";

        if (hours > 0) {
            out << " " << hours << "h";
        }
    }
    else if (hours > 0) {
        out << hours << "h";

        if (minutes > 0) {
            out << " " << minutes << "m";
        }
    }
    else if (minutes > 0) {
        out << minutes << "m";
    }
    else {
        out << "under 1m";
    }

    return out.str();
}

std::string Format::LocalTime(long long unixSeconds, std::string pattern) const
{
    // put_time's own %Z is Windows' full zone name; swap in the short one.
    for (size_t at = pattern.find("%Z"); at != std::string::npos; at = pattern.find("%Z", at)) {
        const std::string zone = TimeZoneAbbreviation(unixSeconds);
        pattern.replace(at, 2, zone);
        at += zone.size();
    }

    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime{};
    localtime_s(&localTime, &t);

    std::ostringstream out;
    out << std::put_time(&localTime, pattern.c_str());
    std::string text = out.str();

    // put_time pads the day and the hour to two digits; a padded number is
    // one that starts the text or follows a space.
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == '0' && (i == 0 || text[i - 1] == ' ') &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            text.erase(i, 1);
        }
    }

    return text;
}

std::string Format::TimeZoneAbbreviation(long long unixSeconds) const
{
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime{};
    localtime_s(&localTime, &t);

    // The registry key name is English on every Windows display language,
    // unlike StandardName, so it is matched first. It comes back empty when
    // dynamic DST is turned off; StandardName is the same text on English
    // Windows. These are the zones with a short English name; the rest get
    // the offset, as in Desktop.
    static const std::pair<const wchar_t*, const char*> kNamedZones[] = {
        { L"Eastern Standard Time", "E" },
        { L"US Eastern Standard Time", "E" },
        { L"Central Standard Time", "C" },
        { L"Mountain Standard Time", "M" },
        { L"US Mountain Standard Time", "M" },
        { L"Pacific Standard Time", "P" },
        { L"Alaskan Standard Time", "AK" },
        { L"Hawaiian Standard Time", "H" },
        { L"Atlantic Standard Time", "A" }
    };

    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    if (GetDynamicTimeZoneInformation(&zone) != TIME_ZONE_ID_INVALID) {
        const wchar_t* name = zone.TimeZoneKeyName[0] ? zone.TimeZoneKeyName : zone.StandardName;

        for (const auto& [key, prefix] : kNamedZones) {
            if (std::wcscmp(name, key) == 0) {
                return std::string(prefix) + (localTime.tm_isdst > 0 ? "DT" : "ST");
            }
        }
    }

    // Seconds east of UTC: the local wall clock read back as if it were UTC.
    const long long offset = static_cast<long long>(_mkgmtime(&localTime)) - unixSeconds;
    const long long minutes = (offset < 0 ? -offset : offset) / 60;

    std::ostringstream out;
    out << "GMT";
    if (minutes != 0) {
        out << (offset < 0 ? "-" : "+") << minutes / 60;
        if (minutes % 60 != 0) {
            out << ":" << std::setw(2) << std::setfill('0') << minutes % 60;
        }
    }

    return out.str();
}
