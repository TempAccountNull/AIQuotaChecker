#include "Global.hpp"
#include "ResetTime.hpp"
#include "Format.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

ResetTime* ResetTime::get_instance()
{
    static ResetTime instance;
    return &instance;
}

int ResetTime::ClampMode(int mode) const
{
    if (mode < Static) {
        return Static;
    }

    if (mode >= Count) {
        return Static;
    }

    return mode;
}

const char* const* ResetTime::ModeNames() const
{
    static const char* names[] = {
        "Static",
        "Flip",
        "Digital"
    };

    return names;
}

int ResetTime::ModeCount() const
{
    return Count;
}

const char* ResetTime::Description(int mode) const
{
    switch (ClampMode(mode)) {
    case Flip:
        return "Per-digit flip-clock countdown; changing digits flip in their own boxes";
    case Digital:
        return "Digital countdown with seconds";
    case Static:
    default:
        return "Minute-based countdown";
    }
}

long long ResetTime::NowSeconds(int mode) const
{
    long long now = static_cast<long long>(std::time(nullptr));

    if (ClampMode(mode) == Static) {
        return (now / 60) * 60;
    }

    return now;
}

std::string ResetTime::FormatStatic(long long secondsLeft) const
{
    if (secondsLeft <= 0) {
        return "Reset available";
    }

    long long totalMinutes = secondsLeft / 60;

    if (totalMinutes <= 0) {
        return "Resets in under 1m";
    }

    long long days = totalMinutes / 1440;
    totalMinutes %= 1440;
    long long hours = totalMinutes / 60;
    long long minutes = totalMinutes % 60;

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
    else {
        out << minutes << "m";
    }

    return out.str();
}

std::string ResetTime::FormatFlip(long long secondsLeft) const
{
    if (secondsLeft <= 0) {
        return "Reset available";
    }

    long long days = secondsLeft / 86400;
    secondsLeft %= 86400;
    long long hours = secondsLeft / 3600;
    secondsLeft %= 3600;
    long long minutes = secondsLeft / 60;
    long long seconds = secondsLeft % 60;

    std::ostringstream out;
    out << "Resets in ";

    if (days > 0) {
        out << "[" << days << "d] ";
    }

    out << "[" << std::setw(2) << std::setfill('0') << hours << "]:"
        << "[" << std::setw(2) << std::setfill('0') << minutes << "]:"
        << "[" << std::setw(2) << std::setfill('0') << seconds << "]";

    return out.str();
}

std::string ResetTime::FormatDigital(long long secondsLeft) const
{
    if (secondsLeft <= 0) {
        return "Reset available";
    }

    long long days = secondsLeft / 86400;
    secondsLeft %= 86400;
    long long hours = secondsLeft / 3600;
    secondsLeft %= 3600;
    long long minutes = secondsLeft / 60;
    long long seconds = secondsLeft % 60;

    std::ostringstream out;
    out << "Resets in ";

    if (days > 0) {
        out << days << "d ";
    }

    if (hours > 0 || days > 0) {
        out << hours << "h ";
    }

    if (minutes > 0 || hours > 0 || days > 0) {
        out << minutes << "m ";
    }

    out << seconds << "s";
    return out.str();
}

std::string ResetTime::Format(long long resetAtUnixSeconds, int mode, bool showAbsoluteDate) const
{
    if (resetAtUnixSeconds <= 0) {
        return {};
    }

    mode = ClampMode(mode);
    long long secondsLeft = resetAtUnixSeconds - NowSeconds(mode);

    std::string text;

    switch (mode) {
    case Flip:
        text = FormatFlip(secondsLeft);
        break;
    case Digital:
        text = FormatDigital(secondsLeft);
        break;
    case Static:
    default:
        text = FormatStatic(secondsLeft);
        break;
    }

    if (showAbsoluteDate) {
        std::string when = Format::get_instance()->ResetDateTime(resetAtUnixSeconds);

        if (!when.empty()) {
            if (!text.empty()) {
                text += " · ";
            }

            text += when;
        }
    }

    return text;
}
