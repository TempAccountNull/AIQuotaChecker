#include "Global.hpp"
#include "Window.hpp"
#include "Math.hpp"

#include <algorithm>

namespace UsageWindowModel
{
    Builder& Builder::Title(const std::string& value)
    {
        window_.title = value;
        return *this;
    }

    Builder& Builder::Subtitle(const std::string& value)
    {
        window_.subtitle = value;
        return *this;
    }

    Builder& Builder::UsedPercent(float value)
    {
        window_.usedPercent = Math::get_instance()->ClampPercentFloat(value);
        return *this;
    }

    Builder& Builder::Reset(long long unixSeconds, const std::string& text)
    {
        window_.resetAtUnixSeconds = unixSeconds;
        window_.resetText = text;
        return *this;
    }

    Builder& Builder::Valid(bool value)
    {
        window_.valid = value;
        return *this;
    }

    Claude::UsageWindow Builder::Build() const
    {
        return window_;
    }

    Claude::UsageWindow Placeholder(const std::string& title, const std::string& subtitle, bool valid)
    {
        return Builder()
            .Title(title)
            .Subtitle(subtitle)
            .UsedPercent(0.0f)
            .Reset(0, "")
            .Valid(valid)
            .Build();
    }

    Claude::UsageWindow FablePlaceholder()
    {
        return Placeholder("Fable", "You haven't used Fable yet", true);
    }
}
