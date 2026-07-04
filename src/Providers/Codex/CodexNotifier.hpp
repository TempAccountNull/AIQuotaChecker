#pragma once

#include "Codex.hpp"
#include "NotifyGUI.hpp"

namespace CodexNotifier
{
    struct QuotaWarningRule
    {
        bool enabled = true;
        int percent = 90;
    };

    struct Config
    {
        bool enabled = true;
        bool resetCredits = true;
        bool prepareReset = true;
        bool exactReset = true;
        int prepareMinutes = 5;

        QuotaWarningRule fiveHour;
        QuotaWarningRule weekly;
    };

    void SetPosition(NotifyPosition position);
    NotifyPosition GetPosition();

    void SetConfig(const Config& config);
    Config GetConfig();

    void ResetState();

    void SetQuotaNotificationRepeatSeconds(int seconds);

    void Poll(const Codex::Snapshot& snapshot, void (*refreshCodexAsync)());
}