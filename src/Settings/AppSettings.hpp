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
        bool codexAutoRefreshEnabled = true;
        bool claudeAutoRefreshEnabled = true;
        bool zaiAutoRefreshEnabled = true;
        bool grokAutoRefreshEnabled = true;

        // 0 = Auto (Desktop, credentials file, then environment token)
        // 1 = Claude Desktop only
        // 2 = Claude Code .credentials.json only
        // 3 = CLAUDE_CODE_OAUTH_TOKEN only
        int claudeAccountSource = 0;

        // 0 = Auto (active Codex account, then default auth.json when app-server is unavailable)
        // 1 = Active Codex account only (app-server)
        // 2 = Default CODEX_HOME\auth.json only
        // 3 = Custom auth.json path only
        int codexAccountSource = 0;
        std::string codexCustomAuthPath;

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
    int ClampClaudeAccountSource(int value);
    int ClampCodexAccountSource(int value);

    void Load(Settings& settings);
    bool Save(const Settings& settings);
    bool SaveClaudeAccountSource(int value);
    bool SaveCodexAccountSource(int value, const std::string& customAuthPath);
}