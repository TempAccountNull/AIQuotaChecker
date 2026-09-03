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
        int widgetSections = 0x7FFF;
        // Where the widget parks: an AppSettings::WidgetAnchor value.
        int widgetAnchor = 2;
        // Which monitor it parks on, as an index into the app's enumerated
        // list. Clamped against the live monitor count on use.
        int widgetMonitor = 0;
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
        WidgetSectionQuota      = 1 << 0,   // session / weekly / model limits
        WidgetSectionContext    = 1 << 1,   // context meter
        WidgetSectionExtra      = 1 << 2,   // extra usage / credits
        WidgetSectionCost       = 1 << 3,   // session spend
        WidgetSectionActivity   = 1 << 4,   // live token telemetry
        WidgetSectionModel      = 1 << 5,   // model + context limit
        WidgetSectionDetails    = 1 << 6,   // provider-specific detail rows
        WidgetSectionStatus     = 1 << 7,   // status / error banner
        WidgetSectionSession    = 1 << 8,   // statusLine session facts
        WidgetSectionCache      = 1 << 9,   // cache hit rate, read / write
        WidgetSectionBreakdown  = 1 << 10,  // context composition
        WidgetSectionCompaction = 1 << 11,  // last compaction result
        WidgetSectionMcp        = 1 << 12,  // ZCode MCP call quota
        WidgetSectionTiming     = 1 << 13,  // session / API wall time
        WidgetSectionPlan       = 1 << 14   // subscription / plan in use
    };

    // Every bit above; the default for a fresh install and the mask a stored
    // value is validated against.
    inline constexpr int kWidgetSectionAll = 0x7FFF;

    // Parking spots: the six top and bottom thirds of the work area. The widget
    // snaps to the nearest one when you drop it and rolls up into that edge.
    // Left and right spots are deliberately absent - a panel collapsed against
    // a side edge is a tall thin sliver with nowhere to put the readout.
    enum WidgetAnchor : int
    {
        WidgetAnchorTopLeft = 0,
        WidgetAnchorTopCenter,
        WidgetAnchorTopRight,
        WidgetAnchorBottomLeft,
        WidgetAnchorBottomCenter,
        WidgetAnchorBottomRight,
        WidgetAnchorCount
    };

    // Row 0 top, 1 bottom. Column 0 left, 1 centre, 2 right.
    inline int WidgetAnchorRow(int anchor)
    {
        return anchor <= WidgetAnchorTopRight ? 0 : 1;
    }

    inline int WidgetAnchorCol(int anchor)
    {
        return anchor <= WidgetAnchorTopRight ? anchor : anchor - WidgetAnchorBottomLeft;
    }

    inline int WidgetAnchorFrom(int row, int col)
    {
        return row == 0 ? col : WidgetAnchorBottomLeft + col;
    }

    inline int ClampWidgetAnchor(int anchor)
    {
        return (anchor < 0 || anchor >= WidgetAnchorCount) ? WidgetAnchorTopRight : anchor;
    }
    int ClampClaudeAccountSource(int value);
    int ClampClaudeThinkingShimmerSpeedPercent(int value);
    int ClampCodexAccountSource(int value);

    void Load(Settings& settings);
    bool Save(const Settings& settings);
    bool SaveClaudeAccountSource(int value);
    bool SaveCodexAccountSource(int value, const std::string& customAuthPath);
}