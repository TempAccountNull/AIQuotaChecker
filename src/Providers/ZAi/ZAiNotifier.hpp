#pragma once

#include "NotifyGUI.hpp"
#include "ZAi.hpp"

namespace ZAiNotifier
{
    struct QuotaWarningRule
    {
        bool enabled = true;
        int percent = 90;
    };

    struct Config
    {
        bool enabled = true;
        bool prepareReset = true;
        bool exactReset = true;
        int prepareMinutes = 5;
        QuotaWarningRule glm52;
        QuotaWarningRule turbo;
    };

    void SetPosition(NotifyPosition position);
    NotifyPosition GetPosition();

    void SetConfig(const Config& config);
    Config GetConfig();

    void ResetState();
    void SetQuotaNotificationRepeatSeconds(int seconds);

    void Poll(const ZAi::Snapshot& snapshot);
}
