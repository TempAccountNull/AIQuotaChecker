#pragma once

#include "Claude.hpp"

#include <string>

namespace UsageWindowModel
{
    class Builder
    {
    public:
        Builder& Title(const std::string& value);
        Builder& Subtitle(const std::string& value);
        Builder& UsedPercent(float value);
        Builder& Reset(long long unixSeconds, const std::string& text);
        Builder& Valid(bool value = true);

        Claude::UsageWindow Build() const;

    private:
        Claude::UsageWindow window_;
    };

    Claude::UsageWindow Placeholder(
        const std::string& title,
        const std::string& subtitle,
        bool valid = false
    );

    Claude::UsageWindow FablePlaceholder();
}
