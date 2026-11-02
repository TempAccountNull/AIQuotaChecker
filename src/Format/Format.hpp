#pragma once

#include <chrono>
#include <string>

class Format final
{
public:
    static Format* get_instance();

    std::string Percent(double value) const;
    std::string QuotaLeft(float usedPercent, const std::string& noun = "quota") const;
    // decimals = 0 gives whole dollars ("$250").
    std::string Dollars(double value, int decimals = 2) const;
    std::string IntegerWithCommas(double value) const;
    // A credit amount: comma-grouped, at most two decimals ("38,125.34", "1,234").
    std::string CreditAmount(double value) const;
    // "Oct 5", local time.
    std::string MonthDay(long long unixSeconds) const;
    // "2026-11-02": the local (UTC) calendar day `offsetDays` after the one
    // holding unixSeconds. The local one stays right across DST changes.
    std::string LocalDateKey(long long unixSeconds, int offsetDays = 0) const;
    std::string UtcDateKey(long long unixSeconds, int offsetDays = 0) const;
    std::string UnixResetTime(long long unixSeconds) const;
    std::string ResetShort(long long unixSeconds) const;
    std::string ResetDateTime(long long unixSeconds) const;
    std::string ExpiryTime(std::chrono::system_clock::time_point tp) const;
    std::string ResetRelative(std::chrono::system_clock::time_point tp) const;
    // Local time through a put_time pattern, without the leading zero put_time
    // gives the day and the hour ("Nov 5, 9:05 AM"). %Z is the short zone name.
    std::string LocalTime(long long unixSeconds, std::string pattern) const;
    // "CST" / "CDT" in the US zones, "GMT+1" elsewhere: the short name Claude
    // Desktop prints, never Windows' "Central Standard Time".
    std::string TimeZoneAbbreviation(long long unixSeconds) const;
};
