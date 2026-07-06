#pragma once

#include <string>

namespace AppSettings
{
    struct QuotaWarningRule
    {
        bool enabled = true;
        int percent = 90;
    };

    struct CodexQuotaWarnings
    {
        QuotaWarningRule fiveHour;
        QuotaWarningRule weekly;
    };

    struct ClaudeQuotaWarnings
    {
        QuotaWarningRule currentSession;
        QuotaWarningRule allModels;
        QuotaWarningRule sonnet;
        QuotaWarningRule fable;
        QuotaWarningRule credits;
    };

    struct ZAiQuotaWarnings
    {
        QuotaWarningRule glm52;
        QuotaWarningRule turbo;
    };

    struct GrokQuotaWarnings
    {
        QuotaWarningRule weekly;
    };

    struct ProviderNotifications
    {
        bool enabled = true;
        bool resetCredits = true;
        bool prepareReset = true;
        bool exactReset = true;
        int prepareMinutes = 5;
    };

    struct Settings
    {
        bool showRemaining = false;
        bool showResetDateDetails = false;
        int resetDisplayMode = 0;
        bool notificationsInsideWindow = false;
        int notificationPositionIndex = 3;
        bool autoRefreshEnabled = true;
        int autoRefreshMinutes = 1;

        ProviderNotifications codex;
        ProviderNotifications claude;
        ProviderNotifications zai;
        ProviderNotifications grok;

        CodexQuotaWarnings codexQuotaWarnings;
        ClaudeQuotaWarnings claudeQuotaWarnings;
        ZAiQuotaWarnings zaiQuotaWarnings;
        GrokQuotaWarnings grokQuotaWarnings;
    };

    std::wstring GetSettingsIniPath();

    int ClampNotificationPositionIndex(int value);
    int ClampPercent(int value);
    int ClampPrepareMinutes(int value);
    int ClampAutoRefreshMinutes(int value);
    int ClampResetDisplayMode(int value);

    void Load(Settings& settings);
    bool Save(const Settings& settings);
}