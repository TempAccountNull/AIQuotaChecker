#pragma once

class Math final
{
public:
    static Math* get_instance();

    int ClampPercent(int value) const;
    int ClampPrepareMinutes(int value) const;
    int ClampAutoRefreshMinutes(int value) const;
    float ClampPercentFloat(float value) const;
    double ClampPercentDouble(double value) const;
    float PercentUsed(double used, double total) const;
    float PercentRemaining(float usedPercent) const;
};
