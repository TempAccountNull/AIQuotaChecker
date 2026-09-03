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
        bool widgetMode = false;
        bool widgetPinned = false;
        // Interface font size in points for the custom panels.
        int uiFontSize = 14;
        // Which sections the widget renders. Bit set = shown.
        int widgetSections = 0x7F;
        // Host order in the widget, as a comma-separated key list.
        std::string widgetOrder = "codex,claude,zai,grok";
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

        // 100 = default animation speed. Range is 25%-250%.
        int claudeThinkingShimmerSpeedPercent = 100;

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
    int ClampUiFontSize(int value);

    // Widget section toggles.
    enum WidgetSection : int
    {
        WidgetSectionQuota     = 1 << 0,  // session / weekly / model limits
        WidgetSectionContext   = 1 << 1,  // context meter
        WidgetSectionExtra     = 1 << 2,  // extra usage / credits
        WidgetSectionCost      = 1 << 3,  // session spend
        WidgetSectionActivity  = 1 << 4,  // live token telemetry
        WidgetSectionModel     = 1 << 5,  // model + context limit
        WidgetSectionDetails   = 1 << 6   // provider-specific detail rows
    };
    int ClampClaudeAccountSource(int value);
    int ClampClaudeThinkingShimmerSpeedPercent(int value);
    int ClampCodexAccountSource(int value);

    void Load(Settings& settings);
    bool Save(const Settings& settings);
    bool SaveClaudeAccountSource(int value);
    bool SaveCodexAccountSource(int value, const std::string& customAuthPath);
}