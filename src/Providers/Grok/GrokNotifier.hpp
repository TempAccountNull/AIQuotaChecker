#pragma once

#include "Grok.hpp"
#include "NotifyGUI.hpp"

namespace GrokNotifier
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

        QuotaWarningRule weekly;
    };

    void SetPosition(NotifyPosition position);
    NotifyPosition GetPosition();

    void SetConfig(const Config& config);
    Config GetConfig();

    void ResetState();
    void SetQuotaNotificationRepeatSeconds(int seconds);

    void Poll(const Grok::Snapshot& snapshot, void (*refreshGrokAsync)());
}
