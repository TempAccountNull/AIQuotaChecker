#pragma once

#include "Claude.hpp"
#include "NotifyGUI.hpp"

namespace ClaudeNotifier
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

        QuotaWarningRule currentSession;
        QuotaWarningRule allModels;
        QuotaWarningRule sonnet;
        QuotaWarningRule fable;
        QuotaWarningRule credits;
    };

    void SetPosition(NotifyPosition position);
    NotifyPosition GetPosition();

    void SetConfig(const Config& config);
    Config GetConfig();

    void ResetState();

    void SetQuotaNotificationRepeatSeconds(int seconds);

    void Poll(const Claude::Snapshot& snapshot, void (*refreshClaudeAsync)());
}