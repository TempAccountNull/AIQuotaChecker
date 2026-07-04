#pragma once

#include <chrono>
#include <string>

class Format final
{
public:
    static Format* get_instance();

    std::string Percent(double value) const;
    std::string QuotaLeft(float usedPercent, const std::string& noun = "quota") const;
    std::string Dollars(double value) const;
    std::string MoneyOrUnits(double value) const;
    std::string IntegerWithCommas(double value) const;
    std::string UnixResetTime(long long unixSeconds) const;
    std::string ResetShort(long long unixSeconds) const;
    std::string ResetDateTime(long long unixSeconds) const;
    std::string ExpiryTime(std::chrono::system_clock::time_point tp) const;
    std::string TimeRemaining(std::chrono::system_clock::time_point tp) const;
    std::string ResetRelative(std::chrono::system_clock::time_point tp) const;
    std::string ResetLong(std::chrono::system_clock::time_point tp) const;
};
