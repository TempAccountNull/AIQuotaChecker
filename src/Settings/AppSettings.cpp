#include "Global.hpp"

#include "AppSettings.hpp"
#include "Math.hpp"
#include "ResetTime.hpp"

#include <windows.h>

namespace AppSettings
{
    static bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback, const std::wstring& path)
    {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, path.c_str()) != 0;
    }

    static int ReadInt(const wchar_t* section, const wchar_t* key, int fallback, const std::wstring& path)
    {
        return GetPrivateProfileIntW(section, key, fallback, path.c_str());
    }

    static bool WriteBool(const wchar_t* section, const wchar_t* key, bool value, const std::wstring& path)
    {
        return WritePrivateProfileStringW(section, key, value ? L"1" : L"0", path.c_str()) != 0;
    }

    static bool WriteInt(const wchar_t* section, const wchar_t* key, int value, const std::wstring& path)
    {
        std::wstring text = std::to_wstring(value);
        return WritePrivateProfileStringW(section, key, text.c_str(), path.c_str()) != 0;
    }

    static void LoadProvider(const wchar_t* section, ProviderNotifications& p, const std::wstring& path, bool hasResetCredits)
    {
        p.enabled = ReadBool(section, L"Enabled", p.enabled, path);
        p.resetCredits = hasResetCredits ? ReadBool(section, L"ResetCredits", p.resetCredits, path) : false;
        p.prepareReset = ReadBool(section, L"PrepareReset", p.prepareReset, path);
        p.exactReset = ReadBool(section, L"ExactReset", p.exactReset, path);
        p.prepareMinutes = ClampPrepareMinutes(ReadInt(section, L"PrepareMinutes", p.prepareMinutes, path));
    }

    static bool SaveProvider(const wchar_t* section, const ProviderNotifications& p, const std::wstring& path, bool hasResetCredits)
    {
        bool ok = true;

        ok = WriteBool(section, L"Enabled", p.enabled, path) && ok;

        if (hasResetCredits) {
            ok = WriteBool(section, L"ResetCredits", p.resetCredits, path) && ok;
        }

        ok = WriteBool(section, L"PrepareReset", p.prepareReset, path) && ok;
        ok = WriteBool(section, L"ExactReset", p.exactReset, path) && ok;
        ok = WriteInt(section, L"PrepareMinutes", ClampPrepareMinutes(p.prepareMinutes), path) && ok;

        return ok;
    }

    static void LoadQuotaRule(const wchar_t* section, const wchar_t* enabledKey, const wchar_t* percentKey, QuotaWarningRule& rule, const std::wstring& path)
    {
        rule.enabled = ReadBool(section, enabledKey, rule.enabled, path);
        rule.percent = ClampPercent(ReadInt(section, percentKey, rule.percent, path));
    }

    static bool SaveQuotaRule(const wchar_t* section, const wchar_t* enabledKey, const wchar_t* percentKey, const QuotaWarningRule& rule, const std::wstring& path)
    {
        bool ok = true;
        ok = WriteBool(section, enabledKey, rule.enabled, path) && ok;
        ok = WriteInt(section, percentKey, ClampPercent(rule.percent), path) && ok;
        return ok;
    }

    static void LoadCodexQuotaWarnings(CodexQuotaWarnings& q, const std::wstring& path)
    {
        LoadQuotaRule(L"CodexQuotaWarnings", L"FiveHourEnabled", L"FiveHourPercent", q.fiveHour, path);
        LoadQuotaRule(L"CodexQuotaWarnings", L"WeeklyEnabled", L"WeeklyPercent", q.weekly, path);
    }

    static bool SaveCodexQuotaWarnings(const CodexQuotaWarnings& q, const std::wstring& path)
    {
        bool ok = true;
        ok = SaveQuotaRule(L"CodexQuotaWarnings", L"FiveHourEnabled", L"FiveHourPercent", q.fiveHour, path) && ok;
        ok = SaveQuotaRule(L"CodexQuotaWarnings", L"WeeklyEnabled", L"WeeklyPercent", q.weekly, path) && ok;
        return ok;
    }

    static void LoadClaudeQuotaWarnings(ClaudeQuotaWarnings& q, const std::wstring& path)
    {
        LoadQuotaRule(L"ClaudeQuotaWarnings", L"CurrentSessionEnabled", L"CurrentSessionPercent", q.currentSession, path);
        LoadQuotaRule(L"ClaudeQuotaWarnings", L"AllModelsEnabled", L"AllModelsPercent", q.allModels, path);
        LoadQuotaRule(L"ClaudeQuotaWarnings", L"SonnetEnabled", L"SonnetPercent", q.sonnet, path);
        LoadQuotaRule(L"ClaudeQuotaWarnings", L"FableEnabled", L"FablePercent", q.fable, path);
        LoadQuotaRule(L"ClaudeQuotaWarnings", L"CreditsEnabled", L"CreditsPercent", q.credits, path);
    }

    static bool SaveClaudeQuotaWarnings(const ClaudeQuotaWarnings& q, const std::wstring& path)
    {
        bool ok = true;
        ok = SaveQuotaRule(L"ClaudeQuotaWarnings", L"CurrentSessionEnabled", L"CurrentSessionPercent", q.currentSession, path) && ok;
        ok = SaveQuotaRule(L"ClaudeQuotaWarnings", L"AllModelsEnabled", L"AllModelsPercent", q.allModels, path) && ok;
        ok = SaveQuotaRule(L"ClaudeQuotaWarnings", L"SonnetEnabled", L"SonnetPercent", q.sonnet, path) && ok;
        ok = SaveQuotaRule(L"ClaudeQuotaWarnings", L"FableEnabled", L"FablePercent", q.fable, path) && ok;
        ok = SaveQuotaRule(L"ClaudeQuotaWarnings", L"CreditsEnabled", L"CreditsPercent", q.credits, path) && ok;
        return ok;
    }

    static void LoadZAiQuotaWarnings(ZAiQuotaWarnings& q, const std::wstring& path)
    {
        LoadQuotaRule(L"ZAiQuotaWarnings", L"GLM52Enabled", L"GLM52Percent", q.glm52, path);
        LoadQuotaRule(L"ZAiQuotaWarnings", L"TurboEnabled", L"TurboPercent", q.turbo, path);
    }

    static bool SaveZAiQuotaWarnings(const ZAiQuotaWarnings& q, const std::wstring& path)
    {
        bool ok = true;
        ok = SaveQuotaRule(L"ZAiQuotaWarnings", L"GLM52Enabled", L"GLM52Percent", q.glm52, path) && ok;
        ok = SaveQuotaRule(L"ZAiQuotaWarnings", L"TurboEnabled", L"TurboPercent", q.turbo, path) && ok;
        return ok;
    }

    std::wstring GetSettingsIniPath()
    {
        wchar_t exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        if (len == 0 || len >= MAX_PATH) {
            return L"settings.ini";
        }

        std::wstring path(exePath, len);
        size_t slash = path.find_last_of(L"\\/");

        if (slash != std::wstring::npos) {
            path.resize(slash + 1);
        }
        else {
            path.clear();
        }

        path += L"settings.ini";
        return path;
    }

    int ClampNotificationPositionIndex(int value)
    {
        if (value < 0) return 0;
        if (value > 4) return 4;
        return value;
    }

    int ClampPercent(int value)
    {
        return Math::get_instance()->ClampPercent(value);
    }

    int ClampPrepareMinutes(int value)
    {
        return Math::get_instance()->ClampPrepareMinutes(value);
    }

    int ClampAutoRefreshMinutes(int value)
    {
        return Math::get_instance()->ClampAutoRefreshMinutes(value);
    }

    int ClampResetDisplayMode(int value)
    {
        return ResetTime::get_instance()->ClampMode(value);
    }

    void Load(Settings& settings)
    {
        std::wstring path = GetSettingsIniPath();

        settings.showRemaining = ReadBool(L"UI", L"ShowRemaining", settings.showRemaining, path);
        settings.showResetDateDetails = ReadBool(L"UI", L"ShowResetDateDetails", settings.showResetDateDetails, path);
        settings.resetDisplayMode = ClampResetDisplayMode(ReadInt(L"UI", L"ResetDisplayMode", settings.resetDisplayMode, path));
        settings.notificationsInsideWindow = ReadBool(L"Notifications", L"InsideWindow", settings.notificationsInsideWindow, path);
        settings.notificationPositionIndex = ClampNotificationPositionIndex(ReadInt(L"Notifications", L"PositionIndex", settings.notificationPositionIndex, path));
        settings.autoRefreshEnabled = ReadBool(L"AutoRefresh", L"Enabled", settings.autoRefreshEnabled, path);
        settings.autoRefreshMinutes = ClampAutoRefreshMinutes(ReadInt(L"AutoRefresh", L"Minutes", settings.autoRefreshMinutes, path));

        LoadProvider(L"CodexNotifications", settings.codex, path, true);
        LoadProvider(L"ClaudeNotifications", settings.claude, path, false);
        LoadProvider(L"ZAiNotifications", settings.zai, path, false);
        LoadCodexQuotaWarnings(settings.codexQuotaWarnings, path);
        LoadClaudeQuotaWarnings(settings.claudeQuotaWarnings, path);
        LoadZAiQuotaWarnings(settings.zaiQuotaWarnings, path);
    }

    bool Save(const Settings& settings)
    {
        std::wstring path = GetSettingsIniPath();
        bool ok = true;

        ok = WriteBool(L"UI", L"ShowRemaining", settings.showRemaining, path) && ok;
        ok = WriteBool(L"UI", L"ShowResetDateDetails", settings.showResetDateDetails, path) && ok;
        ok = WriteInt(L"UI", L"ResetDisplayMode", ClampResetDisplayMode(settings.resetDisplayMode), path) && ok;
        ok = WriteBool(L"Notifications", L"InsideWindow", settings.notificationsInsideWindow, path) && ok;
        ok = WriteInt(L"Notifications", L"PositionIndex", ClampNotificationPositionIndex(settings.notificationPositionIndex), path) && ok;
        ok = WriteBool(L"AutoRefresh", L"Enabled", settings.autoRefreshEnabled, path) && ok;
        ok = WriteInt(L"AutoRefresh", L"Minutes", ClampAutoRefreshMinutes(settings.autoRefreshMinutes), path) && ok;

        ok = SaveProvider(L"CodexNotifications", settings.codex, path, true) && ok;
        ok = SaveProvider(L"ClaudeNotifications", settings.claude, path, false) && ok;
        ok = SaveProvider(L"ZAiNotifications", settings.zai, path, false) && ok;
        ok = SaveCodexQuotaWarnings(settings.codexQuotaWarnings, path) && ok;
        ok = SaveClaudeQuotaWarnings(settings.claudeQuotaWarnings, path) && ok;
        ok = SaveZAiQuotaWarnings(settings.zaiQuotaWarnings, path) && ok;

        return ok;
    }
}
