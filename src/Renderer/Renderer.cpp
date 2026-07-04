#include "Global.hpp"

#include "Renderer.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"
#include "ResetTime.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include "NotifyGUI.hpp"
#include "CodexNotifier.hpp"
#include "ClaudeNotifier.hpp"
#include "ZAiNotifier.hpp"
#include "AppSettings.hpp"

namespace Renderer
{
    static State* g_state = nullptr;

    static State& R()
    {
        return *g_state;
    }

#define g_shouldClose (*R().shouldClose)
#define g_codexMutex (*R().codexMutex)
#define g_claudeMutex (*R().claudeMutex)
#define g_zaiMutex (*R().zaiMutex)
#define g_codexState (*R().codexState)
#define g_claudeState (*R().claudeState)
#define g_zaiState (*R().zaiState)
#define g_codexLoading (*R().codexLoading)
#define g_claudeLoading (*R().claudeLoading)
#define g_zaiLoading (*R().zaiLoading)
#define g_showRemaining (*R().showRemaining)
#define g_showResetDateDetails (*R().showResetDateDetails)
#define g_resetDisplayMode (*R().resetDisplayMode)
#define g_showNotificationsInsideWindow (*R().showNotificationsInsideWindow)
#define g_autoRefreshEnabled (*R().autoRefreshEnabled)
#define g_autoRefreshMinutes (*R().autoRefreshMinutes)
#define g_notifyPositionIndex (*R().notifyPositionIndex)
#define g_notifyPosition (*R().notifyPosition)
#define g_notifyPositionNames (R().notifyPositionNames)
#define g_notifyPositions (R().notifyPositions)
#define g_codexNotifySettings (*R().codexNotifySettings)
#define g_claudeNotifySettings (*R().claudeNotifySettings)
#define g_zaiNotifySettings (*R().zaiNotifySettings)
#define g_codexQuotaWarnings (*R().codexQuotaWarnings)
#define g_claudeQuotaWarnings (*R().claudeQuotaWarnings)
#define g_zaiQuotaWarnings (*R().zaiQuotaWarnings)
#define RefreshCodexAsync (R().refreshCodexAsync)
#define RefreshClaudeAsync (R().refreshClaudeAsync)
#define RefreshZAiAsync (R().refreshZAiAsync)
#define SaveAppSettings (R().saveAppSettings)
#define ApplySettingsToRuntime (R().applySettingsToRuntime)

    static void ClearAutoRefreshWarning()
    {
        std::lock_guard<std::mutex> lock(*R().autoRefreshWarningMutex);
        R().autoRefreshWarning->clear();
    }

    static std::string GetAutoRefreshWarning()
    {
        std::lock_guard<std::mutex> lock(*R().autoRefreshWarningMutex);
        return *R().autoRefreshWarning;
    }

struct UiBar {
    std::string label;
    std::string sublabel;
    std::string rightText;
    long long resetAtUnixSeconds = 0;
    std::string detailValue1;
    std::string detailLabel1;
    std::string detailValue2;
    std::string detailLabel2;
    float usedPercent = 0.0f;
    bool valid = true;
    bool red = false;
    bool white = false;
    bool green = false;
    bool thin = false;
};



static ImU32 Color(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

static std::string FormatPercent(float value) {
    return Format::get_instance()->Percent(value);
}

static float DisplayPercentValue(float usedPercent) {
    usedPercent = Math::get_instance()->ClampPercentFloat(usedPercent);

    if (g_showRemaining) {
        return 100.0f - usedPercent;
    }

    return usedPercent;
}

static std::string FormatDisplayPercent(float usedPercent) {
    std::string text = FormatPercent(DisplayPercentValue(usedPercent));

    if (g_showRemaining) {
        text += " remaining";
    }
    else {
        text += " used";
    }

    return text;
}

static std::string PrefixBeforeTrailingPercent(const std::string& text) {
    size_t pct = text.rfind('%');

    if (pct == std::string::npos) {
        return "";
    }

    size_t doubleSpace = text.rfind("  ", pct);

    if (doubleSpace != std::string::npos) {
        return text.substr(0, doubleSpace + 2);
    }

    size_t singleSpace = text.rfind(' ', pct);

    if (singleSpace != std::string::npos) {
        return text.substr(0, singleSpace + 1);
    }

    return "";
}

static std::string FormatResetDateTime(long long unixSeconds);

static std::string TrimRightCopy(std::string text)
{
    return Text::get_instance()->TrimRightCopy(text);
}

static std::string BuildResetDisplayText(const std::string& fallback, long long resetAtUnixSeconds)
{
    if (resetAtUnixSeconds > 0) {
        std::string text = ResetTime::get_instance()->Format(
            resetAtUnixSeconds,
            g_resetDisplayMode,
            g_showResetDateDetails
        );

        if (!text.empty()) {
            return text;
        }
    }

    return fallback;
}

static std::string BuildCodexRightText(const Codex::UsageBar& bar) {
    std::string left;

    if (bar.resetAtUnixSeconds > 0) {
        left = BuildResetDisplayText("", bar.resetAtUnixSeconds);
    }
    else {
        left = TrimRightCopy(PrefixBeforeTrailingPercent(bar.rightText));
    }

    if (left.empty()) {
        return FormatDisplayPercent(bar.usedPercent);
    }

    return left + "  " + FormatDisplayPercent(bar.usedPercent);
}

static std::string BuildResetRightText(const std::string& resetText, float usedPercent, long long resetAtUnixSeconds = 0) {
    std::string left = BuildResetDisplayText(resetText, resetAtUnixSeconds);

    if (left.empty()) {
        return FormatDisplayPercent(usedPercent);
    }

    return left + "  " + FormatDisplayPercent(usedPercent);
}


static bool ShouldDrawFlipResetClock(const UiBar& bar)
{
    return g_resetDisplayMode == ResetTime::Flip && bar.resetAtUnixSeconds > 0;
}

static std::string TwoDigit(long long value)
{
    value = std::max<long long>(0, value);

    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << value;
    return out.str();
}

static std::string DisplayPercentFromUiBar(const UiBar& bar)
{
    std::string text = FormatPercent(bar.usedPercent);

    if (g_showRemaining) {
        text += " remaining";
    }
    else {
        text += " used";
    }

    return text;
}

struct FlipClockSegment
{
    std::string value;
    bool animate = false;
};

static std::vector<FlipClockSegment> BuildFlipClockSegments(long long resetAtUnixSeconds)
{
    long long secondsLeft = resetAtUnixSeconds - static_cast<long long>(std::time(nullptr));

    if (secondsLeft < 0) {
        secondsLeft = 0;
    }

    long long days = secondsLeft / 86400;
    secondsLeft %= 86400;
    long long hours = secondsLeft / 3600;
    secondsLeft %= 3600;
    long long minutes = secondsLeft / 60;
    long long seconds = secondsLeft % 60;

    std::vector<FlipClockSegment> segments;

    if (days > 0) {
        segments.push_back({ std::to_string(days) + "d", false });
    }

    segments.push_back({ TwoDigit(hours), false });
    segments.push_back({ TwoDigit(minutes), false });
    segments.push_back({ TwoDigit(seconds), true });
    return segments;
}

static float FlipSegmentWidth(const std::string& value)
{
    return std::max(30.0f, ImGui::CalcTextSize(value.c_str()).x + 16.0f);
}

static float FlipClockWidth(const std::vector<FlipClockSegment>& segments, const std::string& percentText)
{
    const float gap = 5.0f;
    const float colonGap = 5.0f;
    float width = ImGui::CalcTextSize("Resets in").x + 8.0f;

    for (size_t i = 0; i < segments.size(); ++i) {
        width += FlipSegmentWidth(segments[i].value);

        if (i + 1 < segments.size()) {
            width += ImGui::CalcTextSize(":").x + colonGap * 2.0f;
        }
    }

    if (!percentText.empty()) {
        width += gap + ImGui::CalcTextSize(percentText.c_str()).x;
    }

    return width;
}

static void DrawFlipSegment(ImDrawList* draw, ImVec2 pos, float width, float height, const std::string& value, bool animate)
{
    const float rounding = 4.0f;
    ImVec2 max(pos.x + width, pos.y + height);
    ImVec2 midLeft(pos.x, pos.y + height * 0.5f);
    ImVec2 midRight(pos.x + width, pos.y + height * 0.5f);

    draw->AddRectFilled(pos, ImVec2(max.x, pos.y + height * 0.5f), Color(43, 43, 43), rounding, ImDrawFlags_RoundCornersTop);
    draw->AddRectFilled(ImVec2(pos.x, pos.y + height * 0.5f), max, Color(25, 25, 25), rounding, ImDrawFlags_RoundCornersBottom);
    draw->AddRect(pos, max, Color(86, 86, 86), rounding);
    draw->AddLine(midLeft, midRight, Color(10, 10, 10));
    draw->AddLine(ImVec2(pos.x + 1.0f, pos.y + height * 0.5f + 1.0f), ImVec2(pos.x + width - 1.0f, pos.y + height * 0.5f + 1.0f), Color(62, 62, 62));

    if (animate) {
        float phase = static_cast<float>(std::fmod(ImGui::GetTime(), 1.0));
        int alpha = static_cast<int>((1.0f - phase) * 72.0f);
        draw->AddRectFilled(pos, ImVec2(max.x, pos.y + height * 0.5f), IM_COL32(255, 255, 255, alpha), rounding, ImDrawFlags_RoundCornersTop);

        float foldY = pos.y + 2.0f + (height - 4.0f) * phase;
        draw->AddLine(ImVec2(pos.x + 2.0f, foldY), ImVec2(max.x - 2.0f, foldY), IM_COL32(255, 255, 255, static_cast<int>((1.0f - phase) * 120.0f)));
    }

    ImVec2 textSize = ImGui::CalcTextSize(value.c_str());
    draw->AddText(
        ImVec2(pos.x + (width - textSize.x) * 0.5f, pos.y + (height - textSize.y) * 0.5f),
        Color(238, 238, 238),
        value.c_str()
    );
}

static void DrawFlipResetClock(ImDrawList* draw, const ImVec2& rowStart, float barWidth, const UiBar& bar)
{
    std::vector<FlipClockSegment> segments = BuildFlipClockSegments(bar.resetAtUnixSeconds);
    std::string percentText = DisplayPercentFromUiBar(bar);

    const float boxHeight = 24.0f;
    const float colonGap = 5.0f;
    float totalWidth = FlipClockWidth(segments, percentText);
    float x = rowStart.x + std::max(0.0f, barWidth - totalWidth);
    float y = rowStart.y - 3.0f;

    const char* prefix = "Resets in";
    ImVec2 prefixSize = ImGui::CalcTextSize(prefix);
    draw->AddText(ImVec2(x, rowStart.y), Color(190, 190, 190), prefix);
    x += prefixSize.x + 8.0f;

    for (size_t i = 0; i < segments.size(); ++i) {
        float boxWidth = FlipSegmentWidth(segments[i].value);
        DrawFlipSegment(draw, ImVec2(x, y), boxWidth, boxHeight, segments[i].value, segments[i].animate);
        x += boxWidth;

        if (i + 1 < segments.size()) {
            const char* colon = ":";
            ImVec2 colonSize = ImGui::CalcTextSize(colon);
            x += colonGap;
            draw->AddText(ImVec2(x, rowStart.y), Color(190, 190, 190), colon);
            x += colonSize.x + colonGap;
        }
    }

    if (!percentText.empty()) {
        x += 5.0f;
        draw->AddText(ImVec2(x, rowStart.y), Color(190, 190, 190), percentText.c_str());
    }
}

static void DrawUsageModeCheckbox() {
    ImGui::SameLine();
    ImGui::Checkbox("Show remaining", &g_showRemaining);
}

static std::string FormatExpiryTime(std::chrono::system_clock::time_point tp) {
    return Format::get_instance()->ExpiryTime(tp);
}

static std::string FormatTimeRemaining(std::chrono::system_clock::time_point tp) {
    return Format::get_instance()->TimeRemaining(tp);
}

static std::string FormatResetDateTime(long long unixSeconds)
{
    return Format::get_instance()->ResetDateTime(unixSeconds);
}


void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.TabRounding = 7.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.ItemSpacing = ImVec2(9.0f, 8.0f);

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.075f, 0.075f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.130f, 0.130f, 0.130f, 1.0f);
    colors[ImGuiCol_Border] = ImVec4(0.245f, 0.245f, 0.245f, 1.0f);

    colors[ImGuiCol_Text] = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);

    colors[ImGuiCol_Tab] = ImVec4(0.135f, 0.135f, 0.135f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.215f, 0.215f, 0.215f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.150f, 0.310f, 0.470f, 1.0f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.170f, 0.170f, 0.170f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.230f, 0.230f, 0.230f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.270f, 0.270f, 0.270f, 1.0f);

    colors[ImGuiCol_Button] = ImVec4(0.160f, 0.160f, 0.160f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.210f, 0.270f, 0.330f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.180f, 0.360f, 0.540f, 1.0f);

    colors[ImGuiCol_CheckMark] = ImVec4(0.230f, 0.560f, 0.920f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.230f, 0.560f, 0.920f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.360f, 0.680f, 1.000f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.150f, 0.230f, 0.310f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.180f, 0.300f, 0.410f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.200f, 0.360f, 0.520f, 1.0f);
}

static void DrawGenericQuotaIcon(ImVec2 center, float size) {
    constexpr float kPi = 3.14159265358979323846f;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    float radius = size * 0.5f;

    draw->AddCircle(center, radius, Color(80, 80, 80), 32, 2.0f);

    draw->PathClear();
    draw->PathArcTo(center, radius, -kPi * 0.85f, kPi * 0.25f, 32);
    draw->PathStroke(Color(0, 145, 255), false, 2.6f);

    float angle = -0.35f;

    ImVec2 needleEnd{
        center.x + std::cos(angle) * radius * 0.65f,
        center.y + std::sin(angle) * radius * 0.65f
    };

    draw->AddLine(center, needleEnd, Color(170, 170, 170), 2.0f);
    draw->AddCircleFilled(center, 2.5f, Color(170, 170, 170));
}

static void DrawThinBar(float displayPercent, float width, ImU32 fillColor, float height, ImU32 trackColor = Color(65, 65, 65)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();

    ImVec2 pos = ImGui::GetCursorScreenPos();

    float pct = Math::get_instance()->ClampPercentFloat(displayPercent);
    float fillW = width * (pct / 100.0f);

    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), trackColor, height * 0.5f);

    if (fillW > 0.0f) {
        draw->AddRectFilled(pos, ImVec2(pos.x + fillW, pos.y + height), fillColor, height * 0.5f);
    }

    ImGui::Dummy(ImVec2(width, height));
}

static void DrawCustomTitleBar() {
    ImGui::BeginChild("##title_bar", ImVec2(0.0f, 38.0f), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 titleMin = ImGui::GetWindowPos();
    ImVec2 titleSize = ImGui::GetWindowSize();

    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(titleMin, ImVec2(titleMin.x + titleSize.x, titleMin.y + titleSize.y), Color(22, 22, 22), 10.0f, ImDrawFlags_RoundCornersTop);

    ImGui::SetCursorPos(ImVec2(12.0f, 9.0f));

    ImVec2 iconPos = ImGui::GetCursorScreenPos();

    DrawGenericQuotaIcon(ImVec2(iconPos.x + 10.0f, iconPos.y + 10.0f), 18.0f);

    ImGui::SetCursorPosX(38.0f);
    ImGui::TextUnformatted("AI Quota Checker");

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.50f, 1.0f));
    // ImGui::TextUnformatted("Made with Love");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(titleSize.x - 42.0f, 6.0f));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.16f, 0.16f, 1.0f));

    if (ImGui::Button("X", ImVec2(30.0f, 24.0f))) {
        g_shouldClose = true;
    }

    ImGui::PopStyleColor(3);
    ImGui::EndChild();
}

static void DrawUsageDetailCell(const std::string& value, const std::string& label)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
    ImGui::TextUnformatted(value.empty() ? "--" : value.c_str());
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
}

static void DrawUnifiedUsageCard(const char* title, const std::vector<UiBar>& bars, float cardWidth) {
    float cardHeight = 50.0f;

    for (const UiBar& bar : bars) {
        if (!bar.valid) {
            continue;
        }

        if (bar.green) {
            cardHeight += bar.sublabel.empty() ? 66.0f : 88.0f;
        }
        else {
            cardHeight += bar.sublabel.empty() ? 54.0f : 70.0f;
        }

        if (ShouldDrawFlipResetClock(bar)) {
            cardHeight += 8.0f;
        }

        if (!bar.detailValue1.empty() || !bar.detailLabel1.empty() || !bar.detailValue2.empty() || !bar.detailLabel2.empty()) {
            cardHeight += 48.0f;
        }
    }

    ImGui::BeginChild(title, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetWindowPos();
    ImVec2 max{ min.x + cardWidth, min.y + cardHeight };

    draw->AddRectFilled(min, max, Color(35, 35, 35), 8.0f);
    draw->AddRect(min, max, Color(58, 58, 58), 8.0f);

    ImGui::SetCursorPos(ImVec2(14.0f, 12.0f));

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.82f, 0.82f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(14.0f, 42.0f));

    float barWidth = cardWidth - 28.0f;

    for (const UiBar& bar : bars) {
        if (!bar.valid) {
            continue;
        }

        ImGui::PushID(bar.label.c_str());

        ImDrawList* rowDraw = ImGui::GetWindowDrawList();
        ImVec2 rowStart = ImGui::GetCursorScreenPos();

        rowDraw->AddText(ImVec2(rowStart.x, rowStart.y), Color(235, 235, 235), bar.label.c_str());

        bool drawFlipClock = ShouldDrawFlipResetClock(bar);

        if (drawFlipClock) {
            DrawFlipResetClock(rowDraw, rowStart, barWidth, bar);
        }
        else {
            ImVec2 rightSize = ImGui::CalcTextSize(bar.rightText.c_str());
            rowDraw->AddText(ImVec2(rowStart.x + barWidth - rightSize.x, rowStart.y), Color(190, 190, 190), bar.rightText.c_str());
        }

        ImGui::Dummy(ImVec2(barWidth, drawFlipClock ? 28.0f : 20.0f));

        if (drawFlipClock && ImGui::IsItemHovered() && !bar.rightText.empty()) {
            ImGui::SetTooltip("%s", bar.rightText.c_str());
        }

        if (!bar.sublabel.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
            ImGui::TextUnformatted(bar.sublabel.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(barWidth, 2.0f));
        }

        ImU32 fill = Color(38, 132, 255);

        if (bar.red) {
            fill = Color(238, 65, 65);
        }
        else if (bar.green) {
            fill = Color(68, 215, 128);
        }
        else if (bar.white) {
            fill = Color(235, 235, 235);
        }

        float barHeight = bar.green ? 6.0f : (bar.thin ? 3.0f : 7.0f);
        DrawThinBar(bar.usedPercent, barWidth, fill, barHeight);

        if (!bar.detailValue1.empty() || !bar.detailLabel1.empty() || !bar.detailValue2.empty() || !bar.detailLabel2.empty()) {
            ImGui::Dummy(ImVec2(barWidth, 13.0f));

            if (ImGui::BeginTable("##usage_details", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                DrawUsageDetailCell(bar.detailValue1, bar.detailLabel1);

                ImGui::TableSetColumnIndex(1);
                DrawUsageDetailCell(bar.detailValue2, bar.detailLabel2);

                ImGui::EndTable();
            }
        }

        ImGui::Dummy(ImVec2(barWidth, 16.0f));
        ImGui::PopID();
    }

    ImGui::EndChild();
}

static bool IsUsableResetCreditStatus(const std::string& status)
{
    std::string lower = status;

    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return lower.empty()
        || lower == "available"
        || lower == "pending"
        || lower == "active"
        || lower == "granted";
}

static void DrawCodexResetCreditsCard(const Codex::Snapshot& snapshot, float cardWidth) {
    int bankedCount = snapshot.resetCreditsAvailableCount >= 0
        ? snapshot.resetCreditsAvailableCount
        : static_cast<int>(snapshot.resetCredits.size());

    int ledgerUsableCount = 0;

    for (const Codex::ResetCredit& credit : snapshot.resetCreditLedger) {
        if (IsUsableResetCreditStatus(credit.status)) {
            ++ledgerUsableCount;
        }
    }

    int notReflectedYet = std::max(ledgerUsableCount - bankedCount, 0);
    size_t ledgerCount = snapshot.resetCreditLedger.size();
    bool showPendingLine = notReflectedYet > 0;

    float rowHeight = 28.0f;
    float rows = static_cast<float>(std::max<size_t>(1, ledgerCount));
    float cardHeight = (showPendingLine ? 98.0f : 78.0f) + rows * rowHeight;

    ImGui::BeginChild("##codex_resets_card", ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::Text("Reset credits: %d banked", bankedCount);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
    ImGui::Text("Reset-credit ledger: %d usable %s", ledgerUsableCount, ledgerUsableCount == 1 ? "entry" : "entries");
    ImGui::PopStyleColor();

    if (showPendingLine) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.78f, 0.25f, 1.0f));
        ImGui::Text("Pending/not reflected yet: %d", notReflectedYet);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (snapshot.resetCreditLedger.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextUnformatted("No reset-credit ledger entries returned");
        ImGui::PopStyleColor();
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("##codex_reset_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Credit", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableSetupColumn("Expires", ImGuiTableColumnFlags_WidthStretch, 0.42f);

        for (size_t i = 0; i < snapshot.resetCreditLedger.size(); ++i) {
            const Codex::ResetCredit& credit = snapshot.resetCreditLedger[i];

            std::string left = "Ledger #" + std::to_string(i + 1);

            if (!credit.status.empty()) {
                left += " · ";
                left += credit.status;
            }

            if (!credit.title.empty()) {
                left += " · ";
                left += credit.title;
            }

            std::string expires = credit.expiresAt.time_since_epoch().count() != 0
                ? FormatExpiryTime(credit.expiresAt)
                : "--";

            std::string remaining = credit.expiresAt.time_since_epoch().count() != 0
                ? FormatTimeRemaining(credit.expiresAt)
                : "unknown";

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(left.c_str());

            ImGui::TableSetColumnIndex(1);
            std::string right = expires + " · " + remaining;
            float rightWidth = ImGui::GetContentRegionAvail().x;
            float rightTextWidth = ImGui::CalcTextSize(right.c_str()).x;

            if (rightTextWidth < rightWidth) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rightWidth - rightTextWidth);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
            ImGui::TextUnformatted(right.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

static void DrawCodexExtraUsageCard(const Codex::Snapshot& snapshot, float cardWidth) {
    ImGui::BeginChild("##codex_extra_usage_card", ImVec2(cardWidth, 74.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (ImGui::BeginTable("##codex_extra_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Credits", ImGuiTableColumnFlags_WidthStretch, 0.45f);

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
        ImGui::TextUnformatted(snapshot.extraUsage.spentText.c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
        ImGui::TextUnformatted("Extra usage");
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        float rightWidth = ImGui::GetContentRegionAvail().x;
        float rightTextWidth = ImGui::CalcTextSize(snapshot.extraUsage.balanceText.c_str()).x;
        if (rightTextWidth < rightWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rightWidth - rightTextWidth);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
        ImGui::TextUnformatted(snapshot.extraUsage.balanceText.c_str());
        ImGui::PopStyleColor();

        const char* caption = "Credits";
        float captionWidth = ImGui::CalcTextSize(caption).x;
        rightWidth = ImGui::GetContentRegionAvail().x;
        if (captionWidth < rightWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rightWidth - captionWidth);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
        ImGui::TextUnformatted(caption);
        ImGui::PopStyleColor();

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

static std::vector<UiBar> BuildCodexBars(const Codex::Snapshot& snapshot) {
    std::vector<UiBar> bars;

    for (const Codex::UsageBar& b : snapshot.bars) {
        UiBar row;
        row.label = b.label;
        row.rightText = BuildCodexRightText(b);
        row.resetAtUnixSeconds = b.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(b.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = true;
        row.thin = false;
        bars.push_back(row);
    }

    if (snapshot.extraUsage.valid) {
        UiBar row;
        row.label = snapshot.extraUsage.spentText;
        row.sublabel = "Extra usage";
        row.rightText = snapshot.extraUsage.balanceText;
        row.usedPercent = snapshot.extraUsage.usedPercent;
        row.valid = true;
        row.red = false;
        row.white = true;
        row.thin = false;
        bars.push_back(row);
    }

    return bars;
}

static std::vector<UiBar> BuildClaudeBars(const Claude::Snapshot& snapshot) {
    std::vector<UiBar> bars;

    if (snapshot.currentSession.valid) {
        UiBar row;
        row.label = snapshot.currentSession.title;
        row.sublabel = snapshot.currentSession.subtitle;
        row.rightText = BuildResetRightText(snapshot.currentSession.resetText, snapshot.currentSession.usedPercent, snapshot.currentSession.resetAtUnixSeconds);
        row.resetAtUnixSeconds = snapshot.currentSession.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(snapshot.currentSession.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = false;
        row.thin = true;
        bars.push_back(row);
    }

    if (snapshot.weeklyAllModels.valid) {
        UiBar row;
        row.label = snapshot.weeklyAllModels.title;
        row.sublabel = snapshot.weeklyAllModels.subtitle;
        row.rightText = BuildResetRightText(snapshot.weeklyAllModels.resetText, snapshot.weeklyAllModels.usedPercent, snapshot.weeklyAllModels.resetAtUnixSeconds);
        row.resetAtUnixSeconds = snapshot.weeklyAllModels.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(snapshot.weeklyAllModels.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = false;
        row.thin = true;
        bars.push_back(row);
    }

    if (snapshot.weeklySonnet.valid) {
        UiBar row;
        row.label = snapshot.weeklySonnet.title;
        row.sublabel = snapshot.weeklySonnet.subtitle;
        row.rightText = BuildResetRightText(snapshot.weeklySonnet.resetText, snapshot.weeklySonnet.usedPercent, snapshot.weeklySonnet.resetAtUnixSeconds);
        row.resetAtUnixSeconds = snapshot.weeklySonnet.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(snapshot.weeklySonnet.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = false;
        row.thin = true;
        bars.push_back(row);
    }

    if (snapshot.weeklyFable.valid) {
        UiBar row;
        row.label = snapshot.weeklyFable.title;
        row.sublabel = snapshot.weeklyFable.subtitle;
        row.rightText = BuildResetRightText(snapshot.weeklyFable.resetText, snapshot.weeklyFable.usedPercent, snapshot.weeklyFable.resetAtUnixSeconds);
        row.resetAtUnixSeconds = snapshot.weeklyFable.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(snapshot.weeklyFable.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = false;
        row.thin = true;
        bars.push_back(row);
    }

    if (snapshot.credits.valid && snapshot.credits.enabled) {
        UiBar row;
        row.label = snapshot.credits.spentText;
        row.sublabel = snapshot.credits.limitText;
        row.rightText = BuildResetRightText(snapshot.credits.resetText, snapshot.credits.usedPercent, snapshot.credits.resetAtUnixSeconds);
        row.resetAtUnixSeconds = snapshot.credits.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(snapshot.credits.usedPercent);
        row.valid = true;
        row.red = true;
        row.white = false;
        row.thin = true;
        row.detailValue1 = snapshot.credits.monthlyLimitText;
        row.detailLabel1 = "Monthly spend limit";
        row.detailValue2 = snapshot.credits.currentBalanceText;
        row.detailLabel2 = "Current balance";
        bars.push_back(row);
    }

    return bars;
}

static std::vector<UiBar> BuildZAiBars(const ZAi::Snapshot& snapshot) {
    std::vector<UiBar> bars;

    for (const ZAi::UsageBar& b : snapshot.bars) {
        if (!b.valid) {
            continue;
        }

        UiBar row;
        row.label = b.label;
        row.sublabel = b.sublabel;
        row.rightText = BuildResetRightText(b.resetText, b.usedPercent, b.resetAtUnixSeconds);
        row.resetAtUnixSeconds = b.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(b.usedPercent);
        row.valid = true;
        row.red = b.red;
        row.white = b.white;
        row.green = b.green;
        row.thin = b.thin;
        bars.push_back(row);
    }

    return bars;
}

static void DrawZAiDetailsCard(const ZAi::Snapshot& snapshot, float cardWidth) {
    if (snapshot.details.empty()) {
        return;
    }

    float rowHeight = 34.0f;
    float cardHeight = 32.0f + static_cast<float>(snapshot.details.size()) * rowHeight;

    ImGui::BeginChild("##zai_details_card", ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (ImGui::BeginTable("##zai_details_table", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        for (const ZAi::DetailRow& row : snapshot.details) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            DrawUsageDetailCell(row.leftValue, row.leftLabel);

            ImGui::TableSetColumnIndex(1);
            DrawUsageDetailCell(row.rightValue, row.rightLabel);
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

static void DrawCodexTab() {
    Codex::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(g_codexMutex);
        snapshot = g_codexState;
    }

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = contentWidth;

    ImGui::TextUnformatted("Codex usage");
    ImGui::SameLine();

    if (g_codexLoading) {
        ImGui::TextDisabled("Loading");
    }
    else {
        ImGui::TextDisabled(g_autoRefreshEnabled ? "Auto refresh on" : "Auto refresh off");
    }

    if (!snapshot.statusText.empty() && snapshot.statusText.rfind("Plan:", 0) != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Codex" : snapshot.plan;

    DrawUnifiedUsageCard(title.c_str(), BuildCodexBars(snapshot), cardWidth);

    ImGui::Spacing();

    DrawCodexResetCreditsCard(snapshot, cardWidth);
}

static void DrawClaudeTab() {
    Claude::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(g_claudeMutex);
        snapshot = g_claudeState;
    }

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = contentWidth;

    ImGui::TextUnformatted("Claude Desktop usage");
    ImGui::SameLine();

    if (g_claudeLoading) {
        ImGui::TextDisabled("Loading");
    }
    else {
        ImGui::TextDisabled(g_autoRefreshEnabled ? "Auto refresh on" : "Auto refresh off");
    }

    if (!snapshot.statusText.empty() && snapshot.statusText.rfind("Plan:", 0) != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Claude" : snapshot.plan;

    DrawUnifiedUsageCard(title.c_str(), BuildClaudeBars(snapshot), cardWidth);
}

static void DrawZAiTab() {
    ZAi::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(g_zaiMutex);
        snapshot = g_zaiState;
    }

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = contentWidth;

    ImGui::TextUnformatted("Z.Ai usage");
    ImGui::SameLine();

    if (g_zaiLoading) {
        ImGui::TextDisabled("Loading");
    }
    else {
        ImGui::TextDisabled(g_autoRefreshEnabled ? "Auto refresh on" : "Auto refresh off");
    }

    if (!snapshot.statusText.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Z.Ai" : snapshot.plan;
    std::vector<UiBar> bars = BuildZAiBars(snapshot);

    if (!bars.empty()) {
        DrawUnifiedUsageCard(title.c_str(), bars, cardWidth);
        ImGui::Spacing();
    }

    DrawZAiDetailsCard(snapshot, cardWidth);
}

static void DrawSettingsHeader(const char* title)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.86f, 0.86f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

static void DrawQuotaRuleSettings(const char* label, AppSettings::QuotaWarningRule& rule)
{
    ImGui::PushID(label);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox("##enabled", &rule.enabled);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(label);

    ImGui::TableSetColumnIndex(1);

    int displayPercent = rule.percent;

    if (g_showRemaining) {
        displayPercent = 100 - rule.percent;
    }

    displayPercent = Math::get_instance()->ClampPercent(displayPercent);

    ImGui::SetNextItemWidth(-1.0f);

    const char* sliderLabel = g_showRemaining ? "##remaining_threshold" : "##used_threshold";
    const char* format = g_showRemaining ? "%d%% remaining" : "%d%% used";

    if (ImGui::SliderInt(sliderLabel, &displayPercent, 1, 100, format)) {
        displayPercent = Math::get_instance()->ClampPercent(displayPercent);

        if (g_showRemaining) {
            rule.percent = 100 - displayPercent;
        }
        else {
            rule.percent = displayPercent;
        }

        rule.percent = Math::get_instance()->ClampPercent(rule.percent);
    }

    ImGui::PopID();
}

static void DrawPrepareMinutesControl(AppSettings::ProviderNotifications& settings)
{
    ImGui::BeginDisabled(!settings.prepareReset);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Prepare warning time");

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##prepare_minutes", &settings.prepareMinutes, 1, 120, "%d min before reset");

    ImGui::EndDisabled();
}

static void DrawProviderNotificationSettings(const char* label, AppSettings::ProviderNotifications& settings, bool showResetCredits)
{
    ImGui::PushID(label);
    DrawSettingsHeader(label);

    if (ImGui::BeginTable("##provider_notifications", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.48f);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Enabled", &settings.enabled);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled(settings.enabled ? "Notifications on" : "Notifications off");

        if (showResetCredits) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox("Reset credit reminders", &settings.resetCredits);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Codex reset credits");
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Prepare reset warning", &settings.prepareReset);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Before quota resets");

        DrawPrepareMinutesControl(settings);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Exact reset notification", &settings.exactReset);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("When quota resets");

        ImGui::EndTable();
    }

    ImGui::PopID();
}

static void DrawCodexNotificationCard()
{
    DrawProviderNotificationSettings("Codex notifications", g_codexNotifySettings, true);
}

static void DrawClaudeNotificationCard()
{
    DrawProviderNotificationSettings("Claude notifications", g_claudeNotifySettings, false);
}

static void DrawCodexQuotaCard()
{
    DrawSettingsHeader("Codex quota warnings");

    if (ImGui::BeginTable("##codex_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.47f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.53f);

        DrawQuotaRuleSettings("5-hour limit", g_codexQuotaWarnings.fiveHour);
        DrawQuotaRuleSettings("Weekly - all models", g_codexQuotaWarnings.weekly);

        ImGui::EndTable();
    }
}

static void DrawClaudeQuotaCard()
{
    DrawSettingsHeader("Claude quota warnings");

    if (ImGui::BeginTable("##claude_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.47f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.53f);

        DrawQuotaRuleSettings("Current session", g_claudeQuotaWarnings.currentSession);
        DrawQuotaRuleSettings("All models", g_claudeQuotaWarnings.allModels);
        DrawQuotaRuleSettings("Sonnet", g_claudeQuotaWarnings.sonnet);
        DrawQuotaRuleSettings("Fable", g_claudeQuotaWarnings.fable);
        DrawQuotaRuleSettings("Usage credits", g_claudeQuotaWarnings.credits);

        ImGui::EndTable();
    }
}

static void DrawSettingsGeneralCard(float contentWidth)
{
    DrawSettingsHeader("General");

    bool twoColumns = contentWidth >= 650.0f;

    if (twoColumns && ImGui::BeginTable("##general_table", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Show remaining", &g_showRemaining);
        ImGui::Checkbox("Show reset date details", &g_showResetDateDetails);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("Reset time mode", &g_resetDisplayMode, ResetTime::get_instance()->ModeNames(), ResetTime::get_instance()->ModeCount());
        g_resetDisplayMode = ResetTime::get_instance()->ClampMode(g_resetDisplayMode);

        if (ImGui::Checkbox("Show notifications inside window", &g_showNotificationsInsideWindow)) {
            NotifyGUI::SetInsideWindow(g_showNotificationsInsideWindow);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Notification position");
        ImGui::SetNextItemWidth(-1.0f);

        if (ImGui::Combo("##notification_position", &g_notifyPositionIndex, g_notifyPositionNames, R().notifyPositionCount)) {
            g_notifyPositionIndex = AppSettings::ClampNotificationPositionIndex(g_notifyPositionIndex);
            g_notifyPosition = g_notifyPositions[g_notifyPositionIndex];
            CodexNotifier::SetPosition(g_notifyPosition);
            ClaudeNotifier::SetPosition(g_notifyPosition);
            ZAiNotifier::SetPosition(g_notifyPosition);
        }

        ImGui::TextDisabled(g_showNotificationsInsideWindow ? "inside window" : "outside screen");

        ImGui::EndTable();
    }
    else {
        ImGui::Checkbox("Show remaining", &g_showRemaining);
        ImGui::Checkbox("Show reset date details", &g_showResetDateDetails);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("Reset time mode", &g_resetDisplayMode, ResetTime::get_instance()->ModeNames(), ResetTime::get_instance()->ModeCount());
        g_resetDisplayMode = ResetTime::get_instance()->ClampMode(g_resetDisplayMode);

        if (ImGui::Checkbox("Show notifications inside window", &g_showNotificationsInsideWindow)) {
            NotifyGUI::SetInsideWindow(g_showNotificationsInsideWindow);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Notification position");
        ImGui::SetNextItemWidth(-1.0f);

        if (ImGui::Combo("##notification_position", &g_notifyPositionIndex, g_notifyPositionNames, R().notifyPositionCount)) {
            g_notifyPositionIndex = AppSettings::ClampNotificationPositionIndex(g_notifyPositionIndex);
            g_notifyPosition = g_notifyPositions[g_notifyPositionIndex];
            CodexNotifier::SetPosition(g_notifyPosition);
            ClaudeNotifier::SetPosition(g_notifyPosition);
            ZAiNotifier::SetPosition(g_notifyPosition);
        }

        ImGui::TextDisabled(g_showNotificationsInsideWindow ? "inside window" : "outside screen");
    }
}

static void DrawSettingsSaveFooter()
{
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save Settings", ImVec2(132.0f, 28.0f))) {
        if (SaveAppSettings()) {
            NotifyGUI::Add("Settings saved to settings.ini", g_notifyPosition, 5.0f, NOTIFY_COL32(70, 230, 90, 255));
        }
        else {
            NotifyGUI::Add("Failed to save settings.ini", g_notifyPosition, 5.0f, NOTIFY_COL32(255, 90, 90, 255));
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("settings.ini in exe folder");
}


static void DrawModernSettingsSection(const char* title)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.88f, 0.88f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

static void DrawNotificationCell(const char* id, bool* value, const char* enabledText, const char* disabledText)
{
    ImGui::PushID(id);
    ImGui::Checkbox("##check", value);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextDisabled(*value ? enabledText : disabledText);
    ImGui::PopID();
}

static void DrawPrepareMinutesCell(const char* id, AppSettings::ProviderNotifications& settings)
{
    ImGui::PushID(id);
    ImGui::BeginDisabled(!settings.prepareReset);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##minutes", &settings.prepareMinutes, 1, 120, "%d min before reset");
    ImGui::EndDisabled();
    ImGui::PopID();
}

static void DrawModernNotificationsSettings()
{
    DrawModernSettingsSection("Notifications");

    if (ImGui::BeginTable("##notifications_table", 3,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Codex", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Claude", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Enabled");
        ImGui::TableSetColumnIndex(1); DrawNotificationCell("codex_enabled", &g_codexNotifySettings.enabled, "On", "Off");
        ImGui::TableSetColumnIndex(2); DrawNotificationCell("claude_enabled", &g_claudeNotifySettings.enabled, "On", "Off");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Reset credit reminders");
        ImGui::TableSetColumnIndex(1); DrawNotificationCell("codex_reset_credits", &g_codexNotifySettings.resetCredits, "On", "Off");
        ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("Not used by Claude");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Prepare reset warning");
        ImGui::TableSetColumnIndex(1); DrawNotificationCell("codex_prepare", &g_codexNotifySettings.prepareReset, "Before resets", "Off");
        ImGui::TableSetColumnIndex(2); DrawNotificationCell("claude_prepare", &g_claudeNotifySettings.prepareReset, "Before resets", "Off");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Prepare warning time");
        ImGui::TableSetColumnIndex(1); DrawPrepareMinutesCell("codex_minutes", g_codexNotifySettings);
        ImGui::TableSetColumnIndex(2); DrawPrepareMinutesCell("claude_minutes", g_claudeNotifySettings);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Exact reset notification");
        ImGui::TableSetColumnIndex(1); DrawNotificationCell("codex_exact", &g_codexNotifySettings.exactReset, "When resets", "Off");
        ImGui::TableSetColumnIndex(2); DrawNotificationCell("claude_exact", &g_claudeNotifySettings.exactReset, "When resets", "Off");

        ImGui::EndTable();
    }
}

static void DrawModernQuotaRow(const char* provider, const char* label, AppSettings::QuotaWarningRule& rule)
{
    ImGui::PushID(provider);
    ImGui::PushID(label);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(provider);

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(label);

    ImGui::TableSetColumnIndex(2);
    ImGui::Checkbox("##enabled", &rule.enabled);

    ImGui::TableSetColumnIndex(3);

    int displayPercent = rule.percent;

    if (g_showRemaining) {
        displayPercent = 100 - rule.percent;
    }

    displayPercent = Math::get_instance()->ClampPercent(displayPercent);

    ImGui::SetNextItemWidth(-1.0f);

    const char* format = g_showRemaining ? "%d%% remaining" : "%d%% used";

    if (ImGui::SliderInt("##threshold", &displayPercent, 1, 100, format)) {
        displayPercent = Math::get_instance()->ClampPercent(displayPercent);
        rule.percent = g_showRemaining ? 100 - displayPercent : displayPercent;
        rule.percent = Math::get_instance()->ClampPercent(rule.percent);
    }

    ImGui::PopID();
    ImGui::PopID();
}

static void DrawModernQuotaSettings()
{
    DrawModernSettingsSection("Quota warnings");

    if (ImGui::BeginTable("##quota_table", 4,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Provider", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableHeadersRow();

        DrawModernQuotaRow("Codex", "5-hour limit", g_codexQuotaWarnings.fiveHour);
        DrawModernQuotaRow("Codex", "Weekly - all models", g_codexQuotaWarnings.weekly);
        DrawModernQuotaRow("Claude", "Current session", g_claudeQuotaWarnings.currentSession);
        DrawModernQuotaRow("Claude", "All models", g_claudeQuotaWarnings.allModels);
        DrawModernQuotaRow("Claude", "Sonnet", g_claudeQuotaWarnings.sonnet);
        DrawModernQuotaRow("Claude", "Fable", g_claudeQuotaWarnings.fable);
        DrawModernQuotaRow("Claude", "Usage credits", g_claudeQuotaWarnings.credits);

        ImGui::EndTable();
    }
}

static void BeginCleanSettingsCard(const char* id, const char* title, ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.105f, 0.105f, 0.105f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.230f, 0.230f, 0.230f, 1.0f));
    ImGui::BeginChild(id, size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.86f, 0.86f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    ImGui::Separator();
    ImGui::Spacing();
}

static void EndCleanSettingsCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

static void DrawSettingsMutedText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

static void DrawSettingsSubHeader(const char* text)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.66f, 0.66f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

static void DrawNotificationToggleRow(const char* label, bool& value, const char* enabledText, const char* disabledText)
{
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::Checkbox(label, &value);

    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    DrawSettingsMutedText(value ? enabledText : disabledText);
}

static void DrawPrepareMinutesRow(AppSettings::ProviderNotifications& settings)
{
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    DrawSettingsMutedText("Warning time");

    ImGui::TableSetColumnIndex(1);
    ImGui::BeginDisabled(!settings.prepareReset);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##prepare_minutes", &settings.prepareMinutes, 1, 120, "%d min before reset");
    ImGui::EndDisabled();
}

static void DrawCleanProviderNotifications(AppSettings::ProviderNotifications& settings, bool showResetCredits)
{
    if (ImGui::BeginTable("##provider_notify_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.50f);

        DrawNotificationToggleRow("Enabled", settings.enabled, "Notifications on", "Notifications off");

        if (showResetCredits) {
            DrawNotificationToggleRow("Reset credit reminders", settings.resetCredits, "Codex reset credits", "Off");
        }

        DrawNotificationToggleRow("Prepare reset warning", settings.prepareReset, "Before quota resets", "Off");
        DrawPrepareMinutesRow(settings);
        DrawNotificationToggleRow("Exact reset notification", settings.exactReset, "When quota resets", "Off");

        ImGui::EndTable();
    }
}

static void DrawCleanQuotaWarnings(bool codex)
{
    if (ImGui::BeginTable("##provider_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.52f);

        if (codex) {
            DrawQuotaRuleSettings("5-hour limit", g_codexQuotaWarnings.fiveHour);
            DrawQuotaRuleSettings("Weekly - all models", g_codexQuotaWarnings.weekly);
        }
        else {
            DrawQuotaRuleSettings("Current session", g_claudeQuotaWarnings.currentSession);
            DrawQuotaRuleSettings("All models", g_claudeQuotaWarnings.allModels);
            DrawQuotaRuleSettings("Sonnet", g_claudeQuotaWarnings.sonnet);
            DrawQuotaRuleSettings("Fable", g_claudeQuotaWarnings.fable);
            DrawQuotaRuleSettings("Usage credits", g_claudeQuotaWarnings.credits);
        }

        ImGui::EndTable();
    }
}

static void DrawCleanZAiNotifications()
{
    if (ImGui::BeginTable("##zai_notify_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.50f);

        DrawNotificationToggleRow("Enabled", g_zaiNotifySettings.enabled, "Notifications on", "Notifications off");
        DrawNotificationToggleRow("Prepare reset warning", g_zaiNotifySettings.prepareReset, "Before quota resets", "Off");
        DrawPrepareMinutesRow(g_zaiNotifySettings);
        DrawNotificationToggleRow("Exact reset notification", g_zaiNotifySettings.exactReset, "When quota resets", "Off");

        ImGui::EndTable();
    }
}

static void DrawCleanZAiQuotaWarnings()
{
    if (ImGui::BeginTable("##zai_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.52f);

        DrawQuotaRuleSettings("GLM-5.2", g_zaiQuotaWarnings.glm52);
        DrawQuotaRuleSettings("GLM-5-Turbo", g_zaiQuotaWarnings.turbo);

        ImGui::EndTable();
    }
}

static bool DrawSettingsSaveButtonInline()
{
    bool saved = false;

    if (ImGui::Button("Save Settings", ImVec2(132.0f, 28.0f))) {
        saved = SaveAppSettings();

        if (saved) {
            NotifyGUI::Add("Settings saved to settings.ini", g_notifyPosition, 5.0f, NOTIFY_COL32(70, 230, 90, 255));
        }
        else {
            NotifyGUI::Add("Failed to save settings.ini", g_notifyPosition, 5.0f, NOTIFY_COL32(255, 90, 90, 255));
        }
    }

    return saved;
}

static void DrawAutoRefreshSettings()
{
    bool changed = ImGui::Checkbox("Enable auto refresh", &g_autoRefreshEnabled);

    if (changed && g_autoRefreshEnabled) {
        ClearAutoRefreshWarning();
    }

    ImGui::BeginDisabled(!g_autoRefreshEnabled);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##auto_refresh_minutes", &g_autoRefreshMinutes, 1, 120, "%d min interval");
    ImGui::EndDisabled();

    g_autoRefreshMinutes = AppSettings::ClampAutoRefreshMinutes(g_autoRefreshMinutes);

    std::string warning = GetAutoRefreshWarning();

    if (!warning.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.25f, 1.0f));
        ImGui::TextWrapped("%s", warning.c_str());
        ImGui::PopStyleColor();
    }
    else {
        DrawSettingsMutedText("Minimum 1 minute to avoid rate limits");
    }
}

static void DrawResetTimeModeSettings()
{
    ImGui::SetNextItemWidth(-1.0f);

    if (ImGui::Combo("##reset_time_mode", &g_resetDisplayMode, ResetTime::get_instance()->ModeNames(), ResetTime::get_instance()->ModeCount())) {
        g_resetDisplayMode = ResetTime::get_instance()->ClampMode(g_resetDisplayMode);
    }

    DrawSettingsMutedText(ResetTime::get_instance()->Description(g_resetDisplayMode));
}

static void DrawCleanGeneralSettings(float contentWidth)
{
    BeginCleanSettingsCard("##settings_general", "General", ImVec2(contentWidth, 218.0f));

    bool wide = contentWidth >= 900.0f;

    if (wide && ImGui::BeginTable("##settings_general_table", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Display", ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn("Auto", ImGuiTableColumnFlags_WidthStretch, 0.27f);
        ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch, 0.27f);
        ImGui::TableSetupColumn("Save", ImGuiTableColumnFlags_WidthStretch, 0.22f);

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        DrawSettingsMutedText("Display");
        ImGui::Checkbox("Show remaining", &g_showRemaining);
        ImGui::Checkbox("Show reset date details", &g_showResetDateDetails);
        ImGui::Spacing();
        DrawSettingsMutedText("Reset time mode");
        DrawResetTimeModeSettings();

        if (ImGui::Checkbox("Notifications inside window", &g_showNotificationsInsideWindow)) {
            NotifyGUI::SetInsideWindow(g_showNotificationsInsideWindow);
        }

        ImGui::TableSetColumnIndex(1);
        DrawSettingsMutedText("Auto refresh");
        DrawAutoRefreshSettings();

        ImGui::TableSetColumnIndex(2);
        DrawSettingsMutedText("Notification position");
        ImGui::SetNextItemWidth(-1.0f);

        if (ImGui::Combo("##notification_position", &g_notifyPositionIndex, g_notifyPositionNames, R().notifyPositionCount)) {
            g_notifyPositionIndex = AppSettings::ClampNotificationPositionIndex(g_notifyPositionIndex);
            g_notifyPosition = g_notifyPositions[g_notifyPositionIndex];
            CodexNotifier::SetPosition(g_notifyPosition);
            ClaudeNotifier::SetPosition(g_notifyPosition);
            ZAiNotifier::SetPosition(g_notifyPosition);
        }

        DrawSettingsMutedText(g_showNotificationsInsideWindow ? "Inside the app window" : "Outside the app window");

        ImGui::TableSetColumnIndex(3);
        DrawSettingsMutedText("Settings file");
        DrawSettingsSaveButtonInline();
        DrawSettingsMutedText("settings.ini in exe folder");

        ImGui::EndTable();
    }
    else if (ImGui::BeginTable("##settings_general_table", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        DrawSettingsMutedText("Display");
        ImGui::Checkbox("Show remaining", &g_showRemaining);
        ImGui::Checkbox("Show reset date details", &g_showResetDateDetails);
        ImGui::Spacing();
        DrawSettingsMutedText("Reset time mode");
        DrawResetTimeModeSettings();

        if (ImGui::Checkbox("Notifications inside window", &g_showNotificationsInsideWindow)) {
            NotifyGUI::SetInsideWindow(g_showNotificationsInsideWindow);
        }

        ImGui::Spacing();
        DrawSettingsMutedText("Auto refresh");
        DrawAutoRefreshSettings();

        ImGui::TableSetColumnIndex(1);
        DrawSettingsMutedText("Notification position");
        ImGui::SetNextItemWidth(-1.0f);

        if (ImGui::Combo("##notification_position", &g_notifyPositionIndex, g_notifyPositionNames, R().notifyPositionCount)) {
            g_notifyPositionIndex = AppSettings::ClampNotificationPositionIndex(g_notifyPositionIndex);
            g_notifyPosition = g_notifyPositions[g_notifyPositionIndex];
            CodexNotifier::SetPosition(g_notifyPosition);
            ClaudeNotifier::SetPosition(g_notifyPosition);
            ZAiNotifier::SetPosition(g_notifyPosition);
        }

        DrawSettingsMutedText(g_showNotificationsInsideWindow ? "Inside the app window" : "Outside the app window");

        ImGui::Spacing();
        DrawSettingsSaveButtonInline();

        ImGui::EndTable();
    }

    EndCleanSettingsCard();
}

static void DrawProviderSettingsCard(const char* id, const char* title, AppSettings::ProviderNotifications& settings, bool codex, float width, float height)
{
    ImGui::PushID(id);
    BeginCleanSettingsCard(id, title, ImVec2(width, height));

    DrawSettingsSubHeader("Notifications");
    DrawCleanProviderNotifications(settings, codex);

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanQuotaWarnings(codex);

    EndCleanSettingsCard();
    ImGui::PopID();
}

static void DrawZAiSettingsCard(float width, float height)
{
    ImGui::PushID("##zai_settings_card");
    BeginCleanSettingsCard("##zai_settings_card", "Z.Ai", ImVec2(width, height));

    DrawSettingsSubHeader("Notifications");
    DrawCleanZAiNotifications();

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanZAiQuotaWarnings();

    EndCleanSettingsCard();
    ImGui::PopID();
}

static void DrawSettingsTab()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 5.0f));

    float contentWidth = ImGui::GetContentRegionAvail().x;

    DrawCleanGeneralSettings(contentWidth);
    ImGui::Spacing();

    float columnGap = 12.0f;
    bool threeColumns = contentWidth >= 930.0f;
    bool twoColumns = contentWidth >= 780.0f;
    float availableHeight = ImGui::GetContentRegionAvail().y;
    float providerHeight = std::max(390.0f, availableHeight - 2.0f);

    if (threeColumns) {
        float columnWidth = (contentWidth - columnGap * 2.0f) / 3.0f;

        DrawProviderSettingsCard("##codex_settings_card", "Codex", g_codexNotifySettings, true, columnWidth, providerHeight);
        ImGui::SameLine(0.0f, columnGap);
        DrawProviderSettingsCard("##claude_settings_card", "Claude", g_claudeNotifySettings, false, columnWidth, providerHeight);
        ImGui::SameLine(0.0f, columnGap);
        DrawZAiSettingsCard(columnWidth, providerHeight);
    }
    else if (twoColumns) {
        float columnWidth = (contentWidth - columnGap) * 0.5f;
        float firstRowHeight = providerHeight * 0.58f;
        float secondRowHeight = providerHeight - firstRowHeight - columnGap;

        DrawProviderSettingsCard("##codex_settings_card", "Codex", g_codexNotifySettings, true, columnWidth, firstRowHeight);
        ImGui::SameLine(0.0f, columnGap);
        DrawProviderSettingsCard("##claude_settings_card", "Claude", g_claudeNotifySettings, false, columnWidth, firstRowHeight);
        ImGui::Spacing();
        DrawZAiSettingsCard(contentWidth, std::max(180.0f, secondRowHeight));
    }
    else {
        DrawProviderSettingsCard("##codex_settings_card", "Codex", g_codexNotifySettings, true, contentWidth, providerHeight);
        ImGui::Spacing();
        DrawProviderSettingsCard("##claude_settings_card", "Claude", g_claudeNotifySettings, false, contentWidth, providerHeight);
        ImGui::Spacing();
        DrawZAiSettingsCard(contentWidth, 240.0f);
    }

    ApplySettingsToRuntime();

    ImGui::PopStyleVar(3);
}

void RenderMainUi(State& state)
{
    g_state = &state;
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("AI Quota Checker Root", nullptr, flags);

    DrawCustomTitleBar();

    ImGui::BeginChild("##main_panel", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar);

    if (ImGui::BeginTabBar("##provider_tabs")) {
        if (ImGui::BeginTabItem("Codex")) {
            DrawCodexTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Claude")) {
            DrawClaudeTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Z.Ai")) {
            DrawZAiTab();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings")) {
            DrawSettingsTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();
    ImGui::End();
}


}
