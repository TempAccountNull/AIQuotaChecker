#pragma once

#include <string>

class ResetTime final
{
public:
    enum Mode
    {
        Static = 0,
        Flip = 1,
        Digital = 2,
        Count = 3
    };

    static ResetTime* get_instance();

    int ClampMode(int mode) const;
    const char* const* ModeNames() const;
    int ModeCount() const;
    const char* Description(int mode) const;

    std::string Format(long long resetAtUnixSeconds, int mode, bool showAbsoluteDate) const;

private:
    long long NowSeconds(int mode) const;
    std::string FormatStatic(long long secondsLeft) const;
    std::string FormatFlip(long long secondsLeft) const;
    std::string FormatDigital(long long secondsLeft) const;
};
