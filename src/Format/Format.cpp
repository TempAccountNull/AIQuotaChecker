#include "Global.hpp"
#include "Format.hpp"
#include "Math.hpp"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

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

std::string Format::Dollars(double value) const
{
    std::ostringstream out;
    out << "$" << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string Format::MoneyOrUnits(double value) const
{
    std::ostringstream out;

    if (std::fabs(value - std::round(value)) < 0.0001) {
        out << static_cast<long long>(std::llround(value));
    }
    else {
        out << std::fixed << std::setprecision(2) << value;
    }

    return out.str();
}

std::string Format::IntegerWithCommas(double value) const
{
    long long rounded = static_cast<long long>(std::llround(value));
    std::string text = std::to_string(rounded);

    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) {
        text.insert(static_cast<size_t>(i), ",");
    }

    return text;
}

std::string Format::UnixResetTime(long long unixSeconds) const
{
    if (unixSeconds <= 0) {
        return "the next reset";
    }

    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime{};
    localtime_s(&localTime, &t);

    std::ostringstream out;
    out << std::put_time(&localTime, "%a %I:%M %p");

    std::string text = out.str();
    size_t zeroPos = text.find(" 0");

    if (zeroPos != std::string::npos) {
        text.erase(zeroPos + 1, 1);
    }

    return text;
}

std::string Format::ResetShort(long long unixSeconds) const
{
    if (unixSeconds <= 0) {
        return {};
    }

    long long now = static_cast<long long>(std::time(nullptr));
    long long seconds = unixSeconds - now;

    if (seconds <= 0) {
        return "Reset available";
    }

    long long days = seconds / 86400;
    seconds %= 86400;
    long long hours = seconds / 3600;
    seconds %= 3600;
    long long minutes = seconds / 60;

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

std::string Format::ResetDateTime(long long unixSeconds) const
{
    if (unixSeconds <= 0) {
        return {};
    }

    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm localTime{};
    localtime_s(&localTime, &t);

    std::ostringstream out;
    out << std::put_time(&localTime, "%a %b %d %I:%M %p");

    std::string text = out.str();

    size_t dayZero = text.find(" 0");
    if (dayZero != std::string::npos) {
        text.erase(dayZero + 1, 1);
    }

    size_t hourZero = text.rfind(" 0");
    if (hourZero != std::string::npos) {
        text.erase(hourZero + 1, 1);
    }

    return text;
}

std::string Format::ExpiryTime(std::chrono::system_clock::time_point tp) const
{
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm localTime{};
    localtime_s(&localTime, &t);

    std::ostringstream out;
    out << std::put_time(&localTime, "%b %d %I:%M %p");

    std::string text = out.str();
    size_t zeroPos = text.find(" 0");

    if (zeroPos != std::string::npos) {
        text.erase(zeroPos + 1, 1);
    }

    return text;
}

std::string Format::TimeRemaining(std::chrono::system_clock::time_point tp) const
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto totalSeconds = duration_cast<seconds>(tp - now).count();

    if (totalSeconds <= 0) {
        return "expired";
    }

    long long days = totalSeconds / 86400;
    totalSeconds %= 86400;
    long long hours = totalSeconds / 3600;
    totalSeconds %= 3600;
    long long minutes = totalSeconds / 60;

    std::ostringstream out;

    if (days > 0) {
        out << days << "d";

        if (hours > 0) {
            out << " " << hours << "h";
        }

        out << " left";
        return out.str();
    }

    if (hours > 0) {
        out << hours << "h";

        if (minutes > 0) {
            out << " " << minutes << "m";
        }

        out << " left";
        return out.str();
    }

    if (minutes > 0) {
        out << minutes << "m left";
        return out.str();
    }

    return "under 1m left";
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

std::string Format::ResetLong(std::chrono::system_clock::time_point tp) const
{
    return ResetRelative(tp);
}
