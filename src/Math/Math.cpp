#include "Global.hpp"
#include "Math.hpp"

#include <algorithm>

Math* Math::get_instance()
{
    static Math instance;
    return &instance;
}

int Math::ClampPercent(int value) const
{
    if (value < 1) return 1;
    if (value > 100) return 100;
    return value;
}

int Math::ClampPrepareMinutes(int value) const
{
    if (value < 1) return 1;
    if (value > 120) return 120;
    return value;
}

int Math::ClampAutoRefreshMinutes(int value) const
{
    if (value < 1) return 1;
    if (value > 120) return 120;
    return value;
}

float Math::ClampPercentFloat(float value) const
{
    return std::clamp(value, 0.0f, 100.0f);
}

double Math::ClampPercentDouble(double value) const
{
    return std::clamp(value, 0.0, 100.0);
}

float Math::PercentUsed(double used, double total) const
{
    if (total <= 0.0) {
        return 0.0f;
    }

    return static_cast<float>(ClampPercentDouble((used / total) * 100.0));
}

float Math::PercentRemaining(float usedPercent) const
{
    return ClampPercentFloat(100.0f - usedPercent);
}
