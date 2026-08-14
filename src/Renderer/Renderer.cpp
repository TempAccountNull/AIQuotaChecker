#include "Global.hpp"

#include "Renderer.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"
#include "ResetTime.hpp"
#include "Network.hpp"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <wincodec.h>
#include <filesystem>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "NotifyGUI.hpp"
#include "Resource.h"
#include "CodexNotifier.hpp"
#include "ClaudeNotifier.hpp"
#include "ZAiNotifier.hpp"
#include "GrokNotifier.hpp"
#include "AppSettings.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

namespace Renderer
{
    static State* g_state = nullptr;

    static State& R()
    {
        return *g_state;
    }

    struct TabImage
    {
        ID3D11ShaderResourceView* srv = nullptr;
        int width = 0;
        int height = 0;
        bool attempted = false;
    };

    static TabImage g_codexTabImage;
    static TabImage g_claudeTabImage;
    static TabImage g_zaiTabImage;
    static TabImage g_grokTabImage;

    static std::array<char, 32768> g_codexCustomPathBuffer{};
    static std::string g_codexCustomPathBufferSynced;
    static bool g_codexCustomPathBufferDirty = false;

    template <std::size_t N>
    static void CopyStringToBuffer(std::array<char, N>& destination, const std::string& source)
    {
        static_assert(N > 0, "Destination buffer must not be empty");

        destination.fill('\0');
        const std::size_t count = (std::min)(source.size(), N - 1);

        if (count > 0) {
            std::memcpy(destination.data(), source.data(), count);
        }
    }

    static std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty()) {
            return {};
        }

        int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

        if (size <= 0) {
            return {};
        }

        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr
        );
        return result;
    }

    static bool BrowseForCodexAuthJson(std::string& selectedPath)
    {
        std::array<wchar_t, 32768> fileBuffer{};

        if (!selectedPath.empty()) {
            std::wstring current = Network::get_instance()->Utf8ToWide(selectedPath);
            wcsncpy_s(fileBuffer.data(), fileBuffer.size(), current.c_str(), _TRUNCATE);
        }

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFilter = L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.lpstrTitle = L"Select Codex auth.json";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

        if (!GetOpenFileNameW(&dialog)) {
            return false;
        }

        selectedPath = WideToUtf8(fileBuffer.data());
        return !selectedPath.empty();
    }

    static std::filesystem::path ExeDirectory()
    {
        wchar_t exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        if (len == 0 || len >= MAX_PATH) {
            return std::filesystem::current_path();
        }

        std::filesystem::path path(std::wstring(exePath, len));
        return path.parent_path();
    }

    static std::filesystem::path FindTabImagePath(const wchar_t* fileName)
    {
        std::filesystem::path exeDir = ExeDirectory();
        std::vector<std::filesystem::path> candidates = {
            exeDir / L"images" / fileName,
            exeDir / L"Resources" / L"images" / fileName,
            exeDir / L"src" / L"Resources" / L"images" / fileName,
            exeDir / L".." / L".." / L"src" / L"Resources" / L"images" / fileName,
            std::filesystem::current_path() / L"src" / L"Resources" / L"images" / fileName,
            std::filesystem::current_path() / L"Resources" / L"images" / fileName
        };

        for (const std::filesystem::path& candidate : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }

        return {};
    }

    static void ReleaseTabImage(TabImage& image)
    {
        if (image.srv) {
            image.srv->Release();
            image.srv = nullptr;
        }

        image.width = 0;
        image.height = 0;
        image.attempted = false;
    }

    void ReleaseTabImages()
    {
        ReleaseTabImage(g_codexTabImage);
        ReleaseTabImage(g_claudeTabImage);
        ReleaseTabImage(g_zaiTabImage);
        ReleaseTabImage(g_grokTabImage);
    }

    static bool LoadTextureFromFile(const std::filesystem::path& path, TabImage& image)
    {
        if (!R().device || path.empty()) {
            return false;
        }

        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)
        );

        if (FAILED(hr)) {
            return false;
        }

        hr = factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder
        );

        if (SUCCEEDED(hr)) {
            hr = decoder->GetFrame(0, &frame);
        }

        if (SUCCEEDED(hr)) {
            hr = factory->CreateFormatConverter(&converter);
        }

        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom
            );
        }

        UINT width = 0;
        UINT height = 0;

        if (SUCCEEDED(hr)) {
            hr = converter->GetSize(&width, &height);
        }

        std::vector<unsigned char> pixels;

        if (SUCCEEDED(hr) && width > 0 && height > 0) {
            pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
            hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
        }

        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA subResource{};
            subResource.pSysMem = pixels.data();
            subResource.SysMemPitch = width * 4;

            ID3D11Texture2D* texture = nullptr;
            hr = R().device->CreateTexture2D(&desc, &subResource, &texture);

            if (SUCCEEDED(hr)) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = desc.Format;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;

                hr = R().device->CreateShaderResourceView(texture, &srvDesc, &image.srv);
                texture->Release();
            }
        }

        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();

        if (FAILED(hr) || !image.srv) {
            return false;
        }

        image.width = static_cast<int>(width);
        image.height = static_cast<int>(height);
        return true;
    }

    static TabImage& EnsureTabImage(TabImage& image, const wchar_t* fileName)
    {
        if (!image.attempted) {
            image.attempted = true;
            LoadTextureFromFile(FindTabImagePath(fileName), image);
        }

        return image;
    }

#define g_shouldClose (*R().shouldClose)
#define g_codexMutex (*R().codexMutex)
#define g_claudeMutex (*R().claudeMutex)
#define g_zaiMutex (*R().zaiMutex)
#define g_grokMutex (*R().grokMutex)
#define g_codexState (*R().codexState)
#define g_claudeState (*R().claudeState)
#define g_zaiState (*R().zaiState)
#define g_grokState (*R().grokState)
#define g_codexLoading (*R().codexLoading)
#define g_claudeLoading (*R().claudeLoading)
#define g_zaiLoading (*R().zaiLoading)
#define g_grokLoading (*R().grokLoading)
#define g_showRemaining (*R().showRemaining)
#define g_showResetDateDetails (*R().showResetDateDetails)
#define g_resetDisplayMode (*R().resetDisplayMode)
#define g_showNotificationsInsideWindow (*R().showNotificationsInsideWindow)
#define g_autoRefreshEnabled (*R().autoRefreshEnabled)
#define g_autoRefreshMinutes (*R().autoRefreshMinutes)
#define g_codexAutoRefreshEnabled (*R().codexAutoRefreshEnabled)
#define g_claudeAutoRefreshEnabled (*R().claudeAutoRefreshEnabled)
#define g_zaiAutoRefreshEnabled (*R().zaiAutoRefreshEnabled)
#define g_grokAutoRefreshEnabled (*R().grokAutoRefreshEnabled)
#define g_claudeAccountSource (*R().claudeAccountSource)
#define g_claudeThinkingShimmerSpeedPercent (*R().claudeThinkingShimmerSpeedPercent)
#define g_codexAccountSource (*R().codexAccountSource)
#define g_codexCustomAuthPath (*R().codexCustomAuthPath)
#define g_notifyPositionIndex (*R().notifyPositionIndex)
#define g_notifyPosition (*R().notifyPosition)
#define g_notifyPositionNames (R().notifyPositionNames)
#define g_notifyPositions (R().notifyPositions)
#define g_codexNotifySettings (*R().codexNotifySettings)
#define g_claudeNotifySettings (*R().claudeNotifySettings)
#define g_zaiNotifySettings (*R().zaiNotifySettings)
#define g_grokNotifySettings (*R().grokNotifySettings)
#define g_codexQuotaWarnings (*R().codexQuotaWarnings)
#define g_claudeQuotaWarnings (*R().claudeQuotaWarnings)
#define g_zaiQuotaWarnings (*R().zaiQuotaWarnings)
#define g_grokQuotaWarnings (*R().grokQuotaWarnings)
#define RefreshCodexAsync (R().refreshCodexAsync)
#define RefreshClaudeAsync (R().refreshClaudeAsync)
#define RefreshZAiAsync (R().refreshZAiAsync)
#define RefreshGrokAsync (R().refreshGrokAsync)
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



static const char* AccessStateLabel(UsageTelemetry::AccessState state)
{
    switch (state) {
    case UsageTelemetry::AccessState::Available: return "AVAILABLE";
    case UsageTelemetry::AccessState::RateLimited: return "RATE LIMITED";
    case UsageTelemetry::AccessState::OutOfUsage: return "OUT OF USAGE";
    case UsageTelemetry::AccessState::Unavailable: return "UNAVAILABLE";
    default: return "";
    }
}

static ImVec4 AccessStateColor(UsageTelemetry::AccessState state)
{
    switch (state) {
    case UsageTelemetry::AccessState::Available: return ImVec4(0.27f, 0.84f, 0.50f, 1.0f);
    case UsageTelemetry::AccessState::RateLimited: return ImVec4(1.00f, 0.69f, 0.24f, 1.0f);
    case UsageTelemetry::AccessState::OutOfUsage: return ImVec4(0.96f, 0.30f, 0.30f, 1.0f);
    case UsageTelemetry::AccessState::Unavailable: return ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    default: return ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    }
}

static void DrawProviderAccessStatus(const UsageTelemetry::AccessStatus& access)
{
    const char* label = AccessStateLabel(access.state);

    if (!label || !*label) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
    ImGui::TextUnformatted("Status:");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, AccessStateColor(access.state));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    if (!access.detail.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", access.detail.c_str());
        ImGui::PopStyleColor();
    }
}

static std::string FormatTokenCount(long long value)
{
    value = std::max<long long>(0, value);
    std::string digits = std::to_string(value);

    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<size_t>(i), 1, ',');
    }

    return digits;
}

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

struct FlipClockElement
{
    enum Kind
    {
        Digit,
        Separator,
        Text
    } kind = Text;

    char digit = '0';
    std::string text;
    std::string key;
};

struct FlipDigitVisualState
{
    char current = '\0';
    char previous = '\0';
    double changedAt = 0.0;
};

static std::unordered_map<std::string, FlipDigitVisualState> g_flipDigitStates;

static void PushFlipDigit(std::vector<FlipClockElement>& elements, const std::string& keyPrefix, int& digitIndex, char value)
{
    FlipClockElement element;
    element.kind = FlipClockElement::Digit;
    element.digit = value;
    element.key = keyPrefix + ":" + std::to_string(digitIndex++);
    elements.push_back(element);
}

static void PushFlipDigits(std::vector<FlipClockElement>& elements, const std::string& keyPrefix, int& digitIndex, const std::string& value)
{
    for (char ch : value) {
        PushFlipDigit(elements, keyPrefix, digitIndex, ch);
    }
}

static void PushFlipText(std::vector<FlipClockElement>& elements, const std::string& text)
{
    FlipClockElement element;
    element.kind = FlipClockElement::Text;
    element.text = text;
    elements.push_back(element);
}

static void PushFlipSeparator(std::vector<FlipClockElement>& elements, const std::string& text)
{
    FlipClockElement element;
    element.kind = FlipClockElement::Separator;
    element.text = text;
    elements.push_back(element);
}

static std::vector<FlipClockElement> BuildFlipClockElements(const UiBar& bar)
{
    long long secondsLeft = bar.resetAtUnixSeconds - static_cast<long long>(std::time(nullptr));

    if (secondsLeft < 0) {
        secondsLeft = 0;
    }

    long long days = secondsLeft / 86400;
    secondsLeft %= 86400;
    long long hours = secondsLeft / 3600;
    secondsLeft %= 3600;
    long long minutes = secondsLeft / 60;
    long long seconds = secondsLeft % 60;

    std::vector<FlipClockElement> elements;
    std::string keyPrefix = bar.label + ":" + std::to_string(bar.resetAtUnixSeconds);
    int digitIndex = 0;

    if (days > 0) {
        PushFlipDigits(elements, keyPrefix, digitIndex, std::to_string(days));
        PushFlipText(elements, "d ");
    }

    PushFlipDigits(elements, keyPrefix, digitIndex, TwoDigit(hours));
    PushFlipSeparator(elements, ":");
    PushFlipDigits(elements, keyPrefix, digitIndex, TwoDigit(minutes));
    PushFlipSeparator(elements, ":");
    PushFlipDigits(elements, keyPrefix, digitIndex, TwoDigit(seconds));
    return elements;
}

static float FlipDigitWidth()
{
    return 18.0f;
}

static float FlipDigitHeight()
{
    return 24.0f;
}

static float FlipElementWidth(const FlipClockElement& element)
{
    if (element.kind == FlipClockElement::Digit) {
        return FlipDigitWidth();
    }

    float textWidth = ImGui::CalcTextSize(element.text.c_str()).x;

    if (element.kind == FlipClockElement::Separator) {
        return textWidth + 6.0f;
    }

    return textWidth + 3.0f;
}

static float FlipClockWidth(const std::vector<FlipClockElement>& elements, const std::string& percentText)
{
    float width = ImGui::CalcTextSize("Resets in").x + 8.0f;

    for (const FlipClockElement& element : elements) {
        width += FlipElementWidth(element);
    }

    if (!percentText.empty()) {
        width += 8.0f + ImGui::CalcTextSize(percentText.c_str()).x;
    }

    return width;
}

static bool UpdateFlipDigitState(const std::string& key, char value, char& current, char& previous, float& phase)
{
    static constexpr double kFlipDurationSeconds = 0.58;

    double now = ImGui::GetTime();
    FlipDigitVisualState& state = g_flipDigitStates[key];

    if (state.current == '\0') {
        state.current = value;
        state.previous = value;
        state.changedAt = now - kFlipDurationSeconds;
    }
    else if (state.current != value) {
        state.previous = state.current;
        state.current = value;
        state.changedAt = now;
    }

    current = state.current;
    previous = state.previous;
    phase = static_cast<float>((now - state.changedAt) / kFlipDurationSeconds);

    if (phase < 0.0f) {
        phase = 0.0f;
    }
    else if (phase > 1.0f) {
        phase = 1.0f;
    }

    return phase < 1.0f && current != previous;
}

static void DrawDigitTextClipped(ImDrawList* draw, ImVec2 pos, float width, float height, char digit, ImVec2 clipMin, ImVec2 clipMax, ImU32 color)
{
    char text[2] = { digit, '\0' };
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImVec2 textPos(pos.x + (width - textSize.x) * 0.5f, pos.y + (height - textSize.y) * 0.5f);

    draw->PushClipRect(clipMin, clipMax, true);
    draw->AddText(textPos, color, text);
    draw->PopClipRect();
}

static void DrawFlipDigitBox(ImDrawList* draw, ImVec2 pos, float width, float height, const std::string& key, char value)
{
    const float rounding = 4.0f;
    const float halfHeight = height * 0.5f;
    ImVec2 max(pos.x + width, pos.y + height);
    ImVec2 topMax(pos.x + width, pos.y + halfHeight);
    ImVec2 bottomMin(pos.x, pos.y + halfHeight);
    ImVec2 midLeft(pos.x, pos.y + halfHeight);
    ImVec2 midRight(pos.x + width, pos.y + halfHeight);

    char current = value;
    char previous = value;
    float phase = 1.0f;
    bool flipping = UpdateFlipDigitState(key, value, current, previous, phase);

    char topDigit = current;
    char bottomDigit = current;

    if (flipping) {
        if (phase < 0.5f) {
            topDigit = previous;
            bottomDigit = current;
        }
        else {
            topDigit = current;
            bottomDigit = current;
        }
    }

    draw->AddRectFilled(pos, topMax, Color(43, 43, 43), rounding, ImDrawFlags_RoundCornersTop);
    draw->AddRectFilled(bottomMin, max, Color(25, 25, 25), rounding, ImDrawFlags_RoundCornersBottom);
    draw->AddRect(pos, max, Color(86, 86, 86), rounding);

    DrawDigitTextClipped(draw, pos, width, height, topDigit, pos, topMax, Color(238, 238, 238));
    DrawDigitTextClipped(draw, pos, width, height, bottomDigit, bottomMin, max, Color(238, 238, 238));

    if (flipping) {
        if (phase < 0.5f) {
            float local = phase * 2.0f;
            int alpha = static_cast<int>(70.0f + local * 85.0f);
            draw->AddRectFilled(pos, topMax, IM_COL32(0, 0, 0, alpha), rounding, ImDrawFlags_RoundCornersTop);

            float flapHeight = std::max(2.0f, halfHeight * (1.0f - local));
            draw->AddRectFilled(
                ImVec2(pos.x + 1.0f, pos.y + halfHeight - flapHeight),
                ImVec2(max.x - 1.0f, pos.y + halfHeight),
                IM_COL32(18, 18, 18, 185),
                2.0f
            );
            DrawDigitTextClipped(
                draw,
                pos,
                width,
                height,
                previous,
                ImVec2(pos.x, pos.y + halfHeight - flapHeight),
                ImVec2(max.x, pos.y + halfHeight),
                IM_COL32(238, 238, 238, 220)
            );
        }
        else {
            float local = (phase - 0.5f) * 2.0f;
            int alpha = static_cast<int>((1.0f - local) * 110.0f);
            draw->AddRectFilled(bottomMin, max, IM_COL32(255, 255, 255, alpha), rounding, ImDrawFlags_RoundCornersBottom);

            float flapHeight = std::max(2.0f, halfHeight * local);
            draw->AddRectFilled(
                bottomMin,
                ImVec2(max.x, bottomMin.y + flapHeight),
                IM_COL32(38, 38, 38, 185),
                2.0f
            );
            DrawDigitTextClipped(
                draw,
                pos,
                width,
                height,
                current,
                bottomMin,
                ImVec2(max.x, bottomMin.y + flapHeight),
                IM_COL32(238, 238, 238, 230)
            );
        }
    }

    draw->AddLine(midLeft, midRight, Color(9, 9, 9));
    draw->AddLine(ImVec2(pos.x + 1.0f, pos.y + halfHeight + 1.0f), ImVec2(pos.x + width - 1.0f, pos.y + halfHeight + 1.0f), Color(62, 62, 62));
}

static void DrawFlipElement(ImDrawList* draw, ImVec2 pos, const FlipClockElement& element)
{
    if (element.kind == FlipClockElement::Digit) {
        DrawFlipDigitBox(draw, pos, FlipDigitWidth(), FlipDigitHeight(), element.key, element.digit);
        return;
    }

    ImVec2 textSize = ImGui::CalcTextSize(element.text.c_str());
    float y = pos.y + (FlipDigitHeight() - textSize.y) * 0.5f;
    draw->AddText(ImVec2(pos.x, y), Color(190, 190, 190), element.text.c_str());
}

static void DrawFlipResetClock(ImDrawList* draw, const ImVec2& rowStart, float barWidth, const UiBar& bar)
{
    std::vector<FlipClockElement> elements = BuildFlipClockElements(bar);
    std::string percentText = DisplayPercentFromUiBar(bar);

    float totalWidth = FlipClockWidth(elements, percentText);
    float x = rowStart.x + std::max(0.0f, barWidth - totalWidth);
    float y = rowStart.y - 3.0f;

    const char* prefix = "Resets in";
    ImVec2 prefixSize = ImGui::CalcTextSize(prefix);
    draw->AddText(ImVec2(x, rowStart.y), Color(190, 190, 190), prefix);
    x += prefixSize.x + 8.0f;

    for (const FlipClockElement& element : elements) {
        DrawFlipElement(draw, ImVec2(x, y), element);
        x += FlipElementWidth(element);
    }

    if (!percentText.empty()) {
        x += 8.0f;
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
            cardHeight += 72.0f;
        }
    }

    // The card is owner-drawn while the row content still advances ImGui's
    // layout cursor. Use a conservative footer/row pad so Claude's credit
    // details and Grok's extra-credit row cannot clip at the card bottom.
    cardHeight += 36.0f + static_cast<float>(bars.size()) * 18.0f;

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

    const bool hasValidBar = std::any_of(
        bars.begin(),
        bars.end(),
        [](const UiBar& bar) { return bar.valid; }
    );

    if (!hasValidBar) {
        // Do not move the cursor to the first row unless a row will actually
        // submit an item. On startup an empty usage vector used to leave a raw
        // SetCursorPos() as the final layout operation, which triggers ImGui's
        // cursor-extent assertion in EndChild().
        ImGui::EndChild();
        return;
    }

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
    const bool showCredits = snapshot.creditBalance.valid;
    const bool showSpendDetails = snapshot.extraUsage.valid && !snapshot.extraUsage.hasUsedPercent;

    if (!showCredits && !showSpendDetails) {
        return;
    }

    const int rows = (showCredits ? 1 : 0) + (showSpendDetails ? 1 : 0);
    const float cardHeight = 24.0f + static_cast<float>(rows) * 50.0f;

    ImGui::BeginChild("##codex_extra_usage_card", ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (ImGui::BeginTable("##codex_extra_table", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        if (showCredits) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawUsageDetailCell(snapshot.creditBalance.balanceText, "Credit balance");
            ImGui::TableSetColumnIndex(1);
            DrawUsageDetailCell(
                snapshot.creditBalance.unlimited ? "Unlimited" : (snapshot.creditBalance.hasCredits ? "Available" : "Unavailable"),
                "Extra usage credits"
            );
        }

        if (showSpendDetails) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawUsageDetailCell(
                snapshot.extraUsage.spentText.empty() ? snapshot.extraUsage.limitText : snapshot.extraUsage.spentText,
                snapshot.extraUsage.label
            );
            ImGui::TableSetColumnIndex(1);
            DrawUsageDetailCell(
                snapshot.extraUsage.remainingText.empty() ? snapshot.extraUsage.limitText : snapshot.extraUsage.remainingText,
                "Remaining"
            );
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

static void DrawClaudeExtraUsageCard(const Claude::Snapshot& snapshot, float cardWidth) {
    // The Usage credits section remains visible even when Claude omits spend
    // data. This keeps the section independent from Session/Weekly exhaustion
    // without inventing a balance or utilization value.
    if (snapshot.credits.valid && snapshot.credits.enabled && snapshot.credits.hasUsedPercent) {
        return;
    }

    const float cardHeight = 74.0f;

    ImGui::BeginChild(
        "##claude_extra_usage_card",
        ImVec2(cardWidth, cardHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    if (ImGui::BeginTable(
        "##claude_extra_usage_table",
        2,
        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings
    )) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        DrawUsageDetailCell(
            snapshot.credits.spentText.empty() ? "Unavailable" : snapshot.credits.spentText,
            snapshot.credits.label.empty() ? "Usage credits" : snapshot.credits.label.c_str()
        );

        ImGui::TableSetColumnIndex(1);
        const std::string creditLimit = !snapshot.credits.monthlyLimitText.empty()
            ? snapshot.credits.monthlyLimitText
            : (!snapshot.credits.limitText.empty() ? snapshot.credits.limitText : "Unavailable");
        DrawUsageDetailCell(
            creditLimit,
            snapshot.credits.resetText.empty()
                ? (snapshot.credits.reported ? "Monthly spend limit" : "Not returned by provider")
                : snapshot.credits.resetText
        );

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

static std::vector<UiBar> BuildCodexBars(const Codex::Snapshot& snapshot) {
    std::vector<UiBar> bars;

    for (const Codex::UsageBar& b : snapshot.bars) {
        if (!b.valid) {
            continue;
        }

        UiBar row;
        row.label = b.label;
        row.sublabel = b.sublabel;
        row.rightText = BuildCodexRightText(b);
        row.resetAtUnixSeconds = b.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(b.usedPercent);
        row.valid = b.valid;
        row.red = false;
        row.white = true;
        row.thin = false;
        bars.push_back(row);
    }

    if (snapshot.extraUsage.valid && snapshot.extraUsage.hasUsedPercent) {
        UiBar row;
        row.label = snapshot.extraUsage.label;
        row.sublabel = snapshot.extraUsage.spentText;
        row.rightText = BuildResetRightText(
            snapshot.extraUsage.resetText,
            snapshot.extraUsage.usedPercent,
            snapshot.extraUsage.resetAtUnixSeconds
        );
        row.resetAtUnixSeconds = snapshot.extraUsage.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(snapshot.extraUsage.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = true;
        row.thin = false;
        row.detailValue1 = snapshot.extraUsage.limitText;
        row.detailLabel1 = snapshot.extraUsage.limitText.empty() ? "" : "Monthly limit";
        row.detailValue2 = snapshot.extraUsage.remainingText;
        row.detailLabel2 = snapshot.extraUsage.remainingText.empty() ? "" : "Remaining";
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

    for (const Claude::UsageWindow& limit : snapshot.additionalLimits) {
        if (!limit.valid) {
            continue;
        }

        UiBar row;
        row.label = limit.title;
        row.sublabel = limit.subtitle;
        row.rightText = BuildResetRightText(limit.resetText, limit.usedPercent, limit.resetAtUnixSeconds);
        row.resetAtUnixSeconds = limit.resetAtUnixSeconds;
        row.usedPercent = DisplayPercentValue(limit.usedPercent);
        row.valid = true;
        row.red = false;
        row.white = false;
        row.thin = true;
        bars.push_back(row);
    }

    if (snapshot.credits.valid && snapshot.credits.enabled && snapshot.credits.hasUsedPercent) {
        UiBar row;
        row.label = snapshot.credits.label.empty() ? "Usage credits" : snapshot.credits.label;
        row.sublabel = snapshot.credits.spentText;
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

static std::vector<UiBar> BuildGrokBars(const Grok::Snapshot& snapshot) {
    std::vector<UiBar> bars;

    if (snapshot.weeklyLimit.valid) {
        UiBar row;
        row.label = snapshot.weeklyLimit.title;
        row.sublabel = snapshot.weeklyLimit.subtitle;
        row.rightText = BuildResetRightText(snapshot.weeklyLimit.resetText, snapshot.weeklyLimit.usedPercent, snapshot.weeklyLimit.resetAtUnixSeconds);
        row.resetAtUnixSeconds = snapshot.weeklyLimit.resetAtUnixSeconds;
        row.usedPercent = snapshot.weeklyLimit.usedPercent;
        row.valid = true;
        row.red = false;
        row.white = true;
        row.thin = false;
        bars.push_back(row);
    }

    if (snapshot.extraCredits.valid) {
        UiBar row;
        row.label = snapshot.extraCredits.balanceText;
        row.sublabel = "Extra Usage Credits";
        row.rightText = "";
        row.usedPercent = snapshot.extraCredits.usedPercent;
        row.valid = true;
        row.red = false;
        row.white = true;
        row.thin = true;
        row.detailValue1 = snapshot.extraCredits.usedText;
        row.detailLabel1 = "On-demand used";
        row.detailValue2 = snapshot.extraCredits.balanceText;
        row.detailLabel2 = "Credit balance";
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

static std::string FormatCompactTokenCount(long long value)
{
    value = std::max<long long>(0, value);

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);

    if (value >= 1000000) {
        out << (static_cast<double>(value) / 1000000.0) << "M";
    }
    else if (value >= 1000) {
        out << (static_cast<double>(value) / 1000.0) << "k";
    }
    else {
        return std::to_string(value);
    }

    return out.str();
}


static std::string FormatRunDuration(long long seconds)
{
    seconds = std::max<long long>(0, seconds);

    const long long hours = seconds / 3600;
    const long long minutes = (seconds % 3600) / 60;
    const long long remainingSeconds = seconds % 60;

    std::ostringstream out;
    if (hours > 0) {
        out << hours << "h ";
    }
    if (hours > 0 || minutes > 0) {
        out << minutes << "m ";
    }
    out << remainingSeconds << "s";
    return out.str();
}

static void DrawClaudeThinkingShimmer(const char* text)
{
    if (!text || !*text) {
        return;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Claude-style thinking label: muted gray base with a narrow white band
    // moving across it. Draw directly so only the status word animates.
    drawList->AddText(
        pos,
        ImGui::GetColorU32(ImVec4(0.60f, 0.60f, 0.60f, 1.0f)),
        text
    );

    const float bandWidth = 13.0f;
    const float travelWidth = size.x + bandWidth * 2.0f;
    const int speedPercent = AppSettings::ClampClaudeThinkingShimmerSpeedPercent(
        g_claudeThinkingShimmerSpeedPercent
    );
    // 60 px/s sits between the original slow sweep and the later 130 px/s
    // version. The Claude setting scales it from 25%-250%.
    const float pixelsPerSecond = 60.0f * (static_cast<float>(speedPercent) / 100.0f);
    const float phase = std::fmod(
        static_cast<float>(ImGui::GetTime()) * pixelsPerSecond,
        travelWidth
    );
    const float bandCenter = pos.x - bandWidth + phase;

    drawList->PushClipRect(
        ImVec2(bandCenter - bandWidth * 0.5f, pos.y),
        ImVec2(bandCenter + bandWidth * 0.5f, pos.y + size.y),
        true
    );
    drawList->AddText(
        pos,
        ImGui::GetColorU32(ImVec4(0.96f, 0.96f, 0.96f, 1.0f)),
        text
    );
    drawList->PopClipRect();

    ImGui::Dummy(size);
}

static void DrawClaudeStatusValue(
    const char* label,
    bool thinking,
    bool thoughtComplete,
    bool compacting,
    UsageTelemetry::AccessState accessState
)
{
    if (thinking) {
        DrawClaudeThinkingShimmer(label);
        return;
    }

    const ImVec4 color = thoughtComplete
        ? ImVec4(0.64f, 0.64f, 0.64f, 1.0f)
        : (compacting
            ? ImVec4(1.00f, 0.58f, 0.24f, 1.0f)
            : AccessStateColor(accessState));

    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

static void DrawClaudeAccessStatus(
    const UsageTelemetry::AccessStatus& access,
    const UsageTelemetry::ContextUsage& context,
    const UsageTelemetry::RunUsage& run
)
{
    const bool canOverride =
        access.state == UsageTelemetry::AccessState::Available ||
        access.state == UsageTelemetry::AccessState::Unknown;
    const bool compacting = canOverride && context.compacting;
    const bool activeRun = canOverride && !compacting && run.valid && run.running;
    const long long nowUnix = static_cast<long long>(std::time(nullptr));
    constexpr long long kThoughtNoticeSeconds = 5;
    const bool thoughtComplete = activeRun &&
        run.lastThoughtDurationSeconds > 0 &&
        run.lastThoughtCompletedAtUnixSeconds > 0 &&
        nowUnix >= run.lastThoughtCompletedAtUnixSeconds &&
        nowUnix - run.lastThoughtCompletedAtUnixSeconds <= kThoughtNoticeSeconds;
    const bool thinking = activeRun && run.thinking && !thoughtComplete;

    std::string stateLabel;
    if (compacting) {
        stateLabel = "COMPACTING";
    }
    else if (thoughtComplete) {
        // Keep this visible briefly even if a tool_result has already started
        // the next reasoning cycle. After five seconds it flips back to the
        // live THINKING state automatically.
        stateLabel = "THOUGHT FOR " + FormatRunDuration(run.lastThoughtDurationSeconds);
    }
    else if (thinking || activeRun) {
        stateLabel = "THINKING";
    }
    else {
        const char* accessLabel = AccessStateLabel(access.state);
        stateLabel = accessLabel ? accessLabel : "";
    }

    if (stateLabel.empty()) {
        return;
    }

    // Keep "Status:" white. Only the current state value carries state color.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
    ImGui::TextUnformatted("Status:");
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 5.0f);
    DrawClaudeStatusValue(
        stateLabel.c_str(),
        thinking,
        thoughtComplete,
        compacting,
        access.state
    );

    const bool transientState = compacting || activeRun;
    if (transientState && run.valid && run.running) {
        const long long timerStart = compacting && context.compactionStartedAtUnixSeconds > 0
            ? context.compactionStartedAtUnixSeconds
            : run.startedAtUnixSeconds;
        const long long elapsed = timerStart > 0 && nowUnix >= timerStart
            ? nowUnix - timerStart
            : 0;

        std::string telemetry = FormatRunDuration(elapsed);
        if (run.tokenStatsValid) {
            telemetry += " · Current Tokens: " + FormatCompactTokenCount(run.currentTokens);
        }
        if (run.tokens > 0) {
            telemetry += " · Total Tokens: " + FormatCompactTokenCount(run.tokens);
        }

        // Match the right-aligned reset/usage text used by the quota rows.
        ImGui::SameLine();
        const float telemetryWidth = ImGui::CalcTextSize(telemetry.c_str()).x;
        const float currentX = ImGui::GetCursorPosX();
        const float targetX = currentX + ImGui::GetContentRegionAvail().x - telemetryWidth;
        if (targetX > currentX) {
            ImGui::SetCursorPosX(targetX);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
        ImGui::TextUnformatted(telemetry.c_str());
        ImGui::PopStyleColor();

        // Current Tokens above is already Claude's output_tokens, so do not
        // duplicate it as "Out". "In" is the effective prompt input including
        // cache reads/creation. Only render this row when real token telemetry
        // is available so placeholder characters never appear in the UI.
        if (run.tokenStatsValid) {
            const std::string tokenStats =
                "In: " + FormatCompactTokenCount(run.inputTokens) +
                " · Cache Read: " + FormatCompactTokenCount(run.cacheReadInputTokens) +
                " · Cache Create: " + FormatCompactTokenCount(run.cacheCreationInputTokens);

            // Right-align the token breakdown to match the other detail text.
            const float tokenStatsWidth = ImGui::CalcTextSize(tokenStats.c_str()).x;
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            if (tokenStatsWidth < availableWidth) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableWidth - tokenStatsWidth);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
            ImGui::TextUnformatted(tokenStats.c_str());
            ImGui::PopStyleColor();
        }
    }

    if (!transientState && !access.detail.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", access.detail.c_str());
        ImGui::PopStyleColor();
    }
}

static void DrawCodexAccessStatus(
    const UsageTelemetry::AccessStatus& access,
    const UsageTelemetry::ContextUsage& context,
    const UsageTelemetry::RunUsage& run
)
{
    // Capture the full row bounds before drawing any right-aligned telemetry.
    // GetContentRegionAvail() becomes very small after the first right-aligned
    // item, which previously caused the second token row to begin at the far
    // right edge and wrap one character per line.
    const float contentStartX = ImGui::GetCursorPosX();
    const float contentRightX = contentStartX + ImGui::GetContentRegionAvail().x;

    const bool canOverride =
        access.state == UsageTelemetry::AccessState::Available ||
        access.state == UsageTelemetry::AccessState::Unknown;
    const bool compacting = canOverride && context.compacting;
    const bool activeRun = canOverride && !compacting && run.valid && run.running;

    std::string stateLabel;
    if (compacting) {
        stateLabel = "COMPACTING";
    }
    else if (activeRun) {
        stateLabel = "THINKING";
    }
    else {
        const char* accessLabel = AccessStateLabel(access.state);
        stateLabel = accessLabel ? accessLabel : "";
    }

    if (stateLabel.empty()) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
    ImGui::TextUnformatted("Status:");
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 5.0f);
    DrawClaudeStatusValue(
        stateLabel.c_str(),
        activeRun,
        false,
        compacting,
        access.state
    );

    const long long nowUnix = static_cast<long long>(std::time(nullptr));
    const bool runStats = activeRun && run.tokenStatsValid;
    // Do not label the previous request's context counters as live spend
    // while a new Codex turn is still waiting for its first token update.
    const bool contextStats = !activeRun && !compacting && context.valid &&
        (context.inputTokens > 0 || context.outputTokens > 0 ||
            context.cachedInputTokens > 0);
    const bool haveStats = runStats || contextStats;

    long long rawInput = 0;
    long long netInput = 0;
    long long cachedInput = 0;
    long long output = 0;
    long long reasoning = 0;
    long long spent = 0;

    if (runStats) {
        rawInput = run.rawInputTokens;
        netInput = run.inputTokens;
        cachedInput = run.cacheReadInputTokens;
        output = run.currentTokens;
        reasoning = run.reasoningOutputTokens;
        spent = run.tokens > 0
            ? run.tokens
            : std::max(0LL, rawInput + output);
    }
    else if (contextStats) {
        rawInput = context.inputTokens;
        cachedInput = context.cachedInputTokens;
        netInput = std::max(0LL, rawInput - cachedInput);
        output = context.outputTokens;
        reasoning = context.reasoningOutputTokens;
        spent = std::max(0LL, rawInput + output);
    }

    if (haveStats || compacting || activeRun) {
        std::string telemetry;

        if (compacting || activeRun) {
            const long long timerStart = compacting && context.compactionStartedAtUnixSeconds > 0
                ? context.compactionStartedAtUnixSeconds
                : run.startedAtUnixSeconds;
            const long long elapsed = timerStart > 0 && nowUnix >= timerStart
                ? nowUnix - timerStart
                : 0;
            telemetry = FormatRunDuration(elapsed);
        }

        if (haveStats) {
            if (!telemetry.empty()) {
                telemetry += " · ";
            }
            telemetry += (activeRun ? "Spent: " : "Last request: ") +
                FormatCompactTokenCount(spent) + " tokens";
        }

        if (!telemetry.empty()) {
            ImGui::SameLine();
            const float telemetryWidth = ImGui::CalcTextSize(telemetry.c_str()).x;
            const float currentX = ImGui::GetCursorPosX();
            const float targetX = contentRightX - telemetryWidth;
            if (targetX > currentX) {
                ImGui::SetCursorPosX(targetX);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
            ImGui::TextUnformatted(telemetry.c_str());
            ImGui::PopStyleColor();
        }

        if (haveStats) {
            std::string tokenStats =
                "In: " + FormatCompactTokenCount(netInput) +
                " · Out: " + FormatCompactTokenCount(output) +
                " · Cache: " + FormatCompactTokenCount(cachedInput);

            if (reasoning > 0) {
                tokenStats += " · Reasoning: " + FormatCompactTokenCount(reasoning);
            }

            // Always align against the original full row, not the remaining
            // width after the telemetry line above. This keeps the token row
            // on one line instead of wrapping vertically at the right edge.
            const float tokenStatsWidth = ImGui::CalcTextSize(tokenStats.c_str()).x;
            const float targetX = std::max(contentStartX, contentRightX - tokenStatsWidth);
            ImGui::SetCursorPosX(targetX);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
            ImGui::TextUnformatted(tokenStats.c_str());
            ImGui::PopStyleColor();
        }
    }

    const bool transientState = compacting || activeRun;
    // OUT OF USAGE is already the complete user-facing Codex state. Keep the
    // underlying quota detail for detection/debugging, but do not clutter the
    // status row with redundant text such as "Usage exhausted: Weekly".
    if (!transientState && access.state != UsageTelemetry::AccessState::OutOfUsage && !access.detail.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", access.detail.c_str());
        ImGui::PopStyleColor();
    }
}

enum class ContextMeterStyle
{
    ClaudeBlueThin,
    CodexWhiteStandard
};

static ImU32 ClaudeContextFillColor(
    const UsageTelemetry::ContextUsage& context,
    float usedPercent
) {
    // Claude's normal context meter is blue. Enter the warning color in the
    // same 20k pre-auto-compact band used by Claude Code, then use red only
    // once the calculated auto-compact threshold has actually been reached.
    // Severity always follows USED context, so the Remaining checkbox never
    // reverses the warning color.
    if (context.autoCompactPercentValid && context.autoCompactThresholdTokens > 0) {
        if (context.usedTokens >= context.autoCompactThresholdTokens) {
            return Color(235, 87, 87);
        }

        if (context.compacting ||
            context.usedTokens >= context.autoCompactThresholdTokens - 20000) {
            return Color(245, 190, 55);
        }

        return Color(38, 132, 255);
    }

    // Fallback for a valid context where an auto-compact threshold could not
    // be calculated. This is visual only and never changes token values.
    if (usedPercent >= 90.0f) {
        return Color(235, 87, 87);
    }
    if (usedPercent >= 80.0f) {
        return Color(245, 190, 55);
    }
    return Color(38, 132, 255);
}

static void DrawContextUsageCard(
    const UsageTelemetry::ContextUsage& context,
    float cardWidth,
    bool showWhenUnavailable = false,
    ContextMeterStyle style = ContextMeterStyle::ClaudeBlueThin
) {
    const bool available = context.valid &&
        context.usedTokens >= 0 &&
        context.contextWindowTokens > 0;
    const bool codexStyle = style == ContextMeterStyle::CodexWhiteStandard;

    if (!available && !showWhenUnavailable) {
        return;
    }

    const float cardHeight = available ? 70.0f : 48.0f;
    ImGui::BeginChild(
        "##context_usage_card",
        ImVec2(cardWidth, cardHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );

    ImGui::TextUnformatted("Context window");

    std::string usageText = context.compacting
        ? "Compacting Conversation..."
        : "Unavailable";
    float displayPercent = 0.0f;
    float usedPercent = 0.0f;

    if (available) {
        const long long clampedUsed = std::clamp<long long>(
            context.usedTokens,
            0,
            context.contextWindowTokens
        );
        const long long remainingTokens = context.contextWindowTokens - clampedUsed;
        usedPercent = Math::get_instance()->ClampPercentFloat(
            static_cast<float>(
                (static_cast<double>(clampedUsed) /
                    static_cast<double>(context.contextWindowTokens)) * 100.0
            )
        );

        displayPercent = g_showRemaining ? 100.0f - usedPercent : usedPercent;
        const long long displayTokens = g_showRemaining ? remainingTokens : clampedUsed;

        if (context.compacting) {
            usageText = "Compacting Conversation...";
        }
        else {
            const long long nowUnix = static_cast<long long>(std::time(nullptr));
            const bool showCompactionNotice =
                context.compactionSavedTokens > 0 &&
                context.compactionCompletedAtUnixSeconds > 0 &&
                nowUnix >= context.compactionCompletedAtUnixSeconds &&
                nowUnix - context.compactionCompletedAtUnixSeconds <= 15;

            if (showCompactionNotice) {
                usageText = "Compacted Conversation · You saved " +
                    FormatCompactTokenCount(context.compactionSavedTokens) + " tokens!";
            }
            else {
                usageText = FormatCompactTokenCount(displayTokens) + " / " +
                    FormatCompactTokenCount(context.contextWindowTokens) + " (" +
                    Format::get_instance()->Percent(displayPercent) +
                    (g_showRemaining ? " remaining)" : " used)");

                if (!codexStyle && context.autoCompactPercentValid) {
                    usageText += " · Auto Compact after: " +
                        std::to_string(context.autoCompactPercentLeft) + "%";
                }
            }
        }
    }

    if (!usageText.empty()) {
        ImGui::SameLine();
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float textWidth = ImGui::CalcTextSize(usageText.c_str()).x;

        if (textWidth < availableWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableWidth - textWidth);
        }

        if (!available && !context.compacting) {
            ImGui::TextDisabled("%s", usageText.c_str());
            ImGui::EndChild();
            return;
        }

        if (context.compacting) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.75f, 0.22f, 1.0f));
            ImGui::TextUnformatted(usageText.c_str());
            ImGui::PopStyleColor();
        }
        else {
            ImGui::TextUnformatted(usageText.c_str());
        }
    }
    else if (!available) {
        ImGui::EndChild();
        return;
    }

    if (available) {
        const ImU32 fillColor = codexStyle
            ? Color(235, 235, 235)
            : ClaudeContextFillColor(context, usedPercent);
        const float barHeight = codexStyle ? 7.0f : 3.0f;
        DrawThinBar(displayPercent, ImGui::GetContentRegionAvail().x, fillColor, barHeight);
    }

    ImGui::EndChild();
}

static bool ProviderAutoRefreshActive(bool providerEnabled)
{
    return g_autoRefreshEnabled && providerEnabled;
}

static void DrawProviderRefreshHeader(
    const char* label,
    bool providerEnabled,
    bool loading,
    void (*refresh)(),
    const char* buttonId
) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    if (loading) {
        ImGui::TextDisabled("Loading");
    }
    else {
        ImGui::TextDisabled(ProviderAutoRefreshActive(providerEnabled) ? "Auto refresh on" : "Auto refresh off");
    }

    const float buttonWidth = 72.0f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), right - buttonWidth));
    ImGui::BeginDisabled(loading || refresh == nullptr);

    ImGui::PushID(buttonId);
    if (ImGui::Button("Refresh", ImVec2(buttonWidth, 0.0f)) && refresh) {
        refresh();
    }
    ImGui::PopID();

    ImGui::EndDisabled();
}

static void DrawCodexTab() {
    Codex::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(g_codexMutex);
        snapshot = g_codexState;
    }

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = contentWidth;

    DrawProviderRefreshHeader(
        "Codex usage",
        g_codexAutoRefreshEnabled,
        g_codexLoading,
        RefreshCodexAsync,
        "codex_manual_refresh"
    );

    if (!snapshot.statusText.empty() &&
        snapshot.statusText.rfind("Plan:", 0) != 0 &&
        snapshot.statusText != snapshot.access.detail) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    DrawCodexAccessStatus(snapshot.access, snapshot.context, snapshot.run);
    ImGui::Spacing();

    DrawContextUsageCard(
        snapshot.context,
        cardWidth,
        true,
        ContextMeterStyle::CodexWhiteStandard
    );
    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Codex" : snapshot.plan;
    DrawUnifiedUsageCard(title.c_str(), BuildCodexBars(snapshot), cardWidth);

    if (snapshot.creditBalance.valid || (snapshot.extraUsage.valid && !snapshot.extraUsage.hasUsedPercent)) {
        ImGui::Spacing();
        DrawCodexExtraUsageCard(snapshot, cardWidth);
    }

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

    DrawProviderRefreshHeader(
        snapshot.usageHeading.empty() ? "Claude usage" : snapshot.usageHeading.c_str(),
        g_claudeAutoRefreshEnabled,
        g_claudeLoading,
        RefreshClaudeAsync,
        "claude_manual_refresh"
    );

    if (!snapshot.statusText.empty() && snapshot.statusText.rfind("Plan:", 0) != 0 && snapshot.statusText != snapshot.access.detail) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    DrawClaudeAccessStatus(snapshot.access, snapshot.context, snapshot.run);
    ImGui::Spacing();

    DrawContextUsageCard(
        snapshot.context,
        cardWidth,
        true,
        ContextMeterStyle::ClaudeBlueThin
    );
    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Claude" : snapshot.plan;
    std::vector<UiBar> bars = BuildClaudeBars(snapshot);

    if (!bars.empty()) {
        DrawUnifiedUsageCard(title.c_str(), bars, cardWidth);
    }

    ImGui::Spacing();
    DrawClaudeExtraUsageCard(snapshot, cardWidth);
}

static void DrawZAiTab() {
    ZAi::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(g_zaiMutex);
        snapshot = g_zaiState;
    }

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = contentWidth;

    DrawProviderRefreshHeader(
        "Z.Ai usage",
        g_zaiAutoRefreshEnabled,
        g_zaiLoading,
        RefreshZAiAsync,
        "zai_manual_refresh"
    );

    if (!snapshot.statusText.empty() && snapshot.statusText != snapshot.access.detail) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    DrawProviderAccessStatus(snapshot.access);
    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Z.Ai" : snapshot.plan;
    std::vector<UiBar> bars = BuildZAiBars(snapshot);

    if (!bars.empty()) {
        DrawUnifiedUsageCard(title.c_str(), bars, cardWidth);
        ImGui::Spacing();
    }

    DrawZAiDetailsCard(snapshot, cardWidth);

    if (snapshot.context.valid) {
        ImGui::Spacing();
        DrawContextUsageCard(snapshot.context, cardWidth);
    }
}

static void DrawGrokTab() {
    Grok::Snapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(g_grokMutex);
        snapshot = g_grokState;
    }

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = contentWidth;

    DrawProviderRefreshHeader(
        "Grok / xAI usage",
        g_grokAutoRefreshEnabled,
        g_grokLoading,
        RefreshGrokAsync,
        "grok_manual_refresh"
    );

    if (!snapshot.statusText.empty() && snapshot.statusText != "Updated" && snapshot.statusText != snapshot.access.detail) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", snapshot.statusText.c_str());
        ImGui::PopStyleColor();
    }

    DrawProviderAccessStatus(snapshot.access);
    ImGui::Spacing();

    std::string title = snapshot.plan.empty() ? "Grok" : snapshot.plan;
    std::vector<UiBar> bars = BuildGrokBars(snapshot);

    if (!bars.empty()) {
        DrawUnifiedUsageCard(title.c_str(), bars, cardWidth);
    }

    if (snapshot.context.valid) {
        ImGui::Spacing();
        DrawContextUsageCard(snapshot.context, cardWidth);
    }
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

static Codex::Snapshot CopyCodexSnapshotForSettings()
{
    std::lock_guard<std::mutex> lock(g_codexMutex);
    return g_codexState;
}

static Claude::Snapshot CopyClaudeSnapshotForSettings()
{
    std::lock_guard<std::mutex> lock(g_claudeMutex);
    return g_claudeState;
}

static ZAi::Snapshot CopyZAiSnapshotForSettings()
{
    std::lock_guard<std::mutex> lock(g_zaiMutex);
    return g_zaiState;
}

static Grok::Snapshot CopyGrokSnapshotForSettings()
{
    std::lock_guard<std::mutex> lock(g_grokMutex);
    return g_grokState;
}

static bool HasCodexWindow(const Codex::Snapshot& snapshot, const char* label)
{
    for (const Codex::UsageBar& bar : snapshot.bars) {
        if (bar.valid && bar.label == label) {
            return true;
        }
    }

    return false;
}

static bool HasZAiModelWindow(const ZAi::Snapshot& snapshot, const char* compactModel)
{
    for (const ZAi::UsageBar& bar : snapshot.bars) {
        if (bar.valid && Text::get_instance()->CompactLower(bar.label) == compactModel) {
            return true;
        }
    }

    return false;
}

static void DrawCodexQuotaCard()
{
    DrawSettingsHeader("Codex quota warnings");
    Codex::Snapshot snapshot = CopyCodexSnapshotForSettings();
    bool hasSession = HasCodexWindow(snapshot, "Session");
    bool hasWeekly = HasCodexWindow(snapshot, "Weekly");

    if (!hasSession && !hasWeekly) {
        ImGui::TextDisabled("No quota windows are currently available.");
        return;
    }

    if (ImGui::BeginTable("##codex_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.47f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.53f);

        if (hasSession) {
            DrawQuotaRuleSettings("5-hour limit", g_codexQuotaWarnings.fiveHour);
        }

        if (hasWeekly) {
            DrawQuotaRuleSettings("Weekly - all models", g_codexQuotaWarnings.weekly);
        }

        ImGui::EndTable();
    }
}

static void DrawClaudeQuotaCard()
{
    DrawSettingsHeader("Claude quota warnings");
    Claude::Snapshot snapshot = CopyClaudeSnapshotForSettings();
    bool any = snapshot.currentSession.valid || snapshot.weeklyAllModels.valid || snapshot.weeklySonnet.valid ||
        snapshot.weeklyFable.valid || (snapshot.credits.valid && snapshot.credits.enabled && snapshot.credits.hasUsedPercent);

    if (!any) {
        ImGui::TextDisabled("No quota windows are currently available.");
        return;
    }

    if (ImGui::BeginTable("##claude_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.47f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.53f);

        if (snapshot.currentSession.valid) DrawQuotaRuleSettings("Current session", g_claudeQuotaWarnings.currentSession);
        if (snapshot.weeklyAllModels.valid) DrawQuotaRuleSettings("All models", g_claudeQuotaWarnings.allModels);
        if (snapshot.weeklySonnet.valid) DrawQuotaRuleSettings("Sonnet", g_claudeQuotaWarnings.sonnet);
        if (snapshot.weeklyFable.valid) DrawQuotaRuleSettings("Fable", g_claudeQuotaWarnings.fable);
        if (snapshot.credits.valid && snapshot.credits.enabled && snapshot.credits.hasUsedPercent) DrawQuotaRuleSettings("Usage credits", g_claudeQuotaWarnings.credits);

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
            GrokNotifier::SetPosition(g_notifyPosition);
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
            GrokNotifier::SetPosition(g_notifyPosition);
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

        Codex::Snapshot codexSnapshot = CopyCodexSnapshotForSettings();
        Claude::Snapshot claudeSnapshot = CopyClaudeSnapshotForSettings();

        if (HasCodexWindow(codexSnapshot, "Session")) DrawModernQuotaRow("Codex", "5-hour limit", g_codexQuotaWarnings.fiveHour);
        if (HasCodexWindow(codexSnapshot, "Weekly")) DrawModernQuotaRow("Codex", "Weekly - all models", g_codexQuotaWarnings.weekly);
        if (claudeSnapshot.currentSession.valid) DrawModernQuotaRow("Claude", "Current session", g_claudeQuotaWarnings.currentSession);
        if (claudeSnapshot.weeklyAllModels.valid) DrawModernQuotaRow("Claude", "All models", g_claudeQuotaWarnings.allModels);
        if (claudeSnapshot.weeklySonnet.valid) DrawModernQuotaRow("Claude", "Sonnet", g_claudeQuotaWarnings.sonnet);
        if (claudeSnapshot.weeklyFable.valid) DrawModernQuotaRow("Claude", "Fable", g_claudeQuotaWarnings.fable);
        if (claudeSnapshot.credits.valid && claudeSnapshot.credits.enabled && claudeSnapshot.credits.hasUsedPercent) DrawModernQuotaRow("Claude", "Usage credits", g_claudeQuotaWarnings.credits);

        ImGui::EndTable();
    }
}

static void BeginCleanSettingsCard(const char* id, const char* title, ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.105f, 0.105f, 0.105f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.230f, 0.230f, 0.230f, 1.0f));
    ImGui::BeginChild(id, size, true);

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

static void DrawSettingsMutedWrappedText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.58f, 0.58f, 1.0f));
    ImGui::TextWrapped("%s", text);
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
    bool codexSession = false;
    bool codexWeekly = false;
    Claude::Snapshot claudeSnapshot;

    if (codex) {
        Codex::Snapshot snapshot = CopyCodexSnapshotForSettings();
        codexSession = HasCodexWindow(snapshot, "Session");
        codexWeekly = HasCodexWindow(snapshot, "Weekly");

        if (!codexSession && !codexWeekly) {
            DrawSettingsMutedText("No quota windows are currently available.");
            return;
        }
    }
    else {
        claudeSnapshot = CopyClaudeSnapshotForSettings();
        bool any = claudeSnapshot.currentSession.valid || claudeSnapshot.weeklyAllModels.valid ||
            claudeSnapshot.weeklySonnet.valid || claudeSnapshot.weeklyFable.valid ||
            (claudeSnapshot.credits.valid && claudeSnapshot.credits.enabled && claudeSnapshot.credits.hasUsedPercent);

        if (!any) {
            DrawSettingsMutedText("No quota windows are currently available.");
            return;
        }
    }

    if (ImGui::BeginTable("##provider_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.52f);

        if (codex) {
            if (codexSession) DrawQuotaRuleSettings("5-hour limit", g_codexQuotaWarnings.fiveHour);
            if (codexWeekly) DrawQuotaRuleSettings("Weekly - all models", g_codexQuotaWarnings.weekly);
        }
        else {
            if (claudeSnapshot.currentSession.valid) DrawQuotaRuleSettings("Current session", g_claudeQuotaWarnings.currentSession);
            if (claudeSnapshot.weeklyAllModels.valid) DrawQuotaRuleSettings("All models", g_claudeQuotaWarnings.allModels);
            if (claudeSnapshot.weeklySonnet.valid) DrawQuotaRuleSettings("Sonnet", g_claudeQuotaWarnings.sonnet);
            if (claudeSnapshot.weeklyFable.valid) DrawQuotaRuleSettings("Fable", g_claudeQuotaWarnings.fable);
            if (claudeSnapshot.credits.valid && claudeSnapshot.credits.enabled && claudeSnapshot.credits.hasUsedPercent) DrawQuotaRuleSettings("Usage credits", g_claudeQuotaWarnings.credits);
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
    ZAi::Snapshot snapshot = CopyZAiSnapshotForSettings();
    bool hasGlm52 = HasZAiModelWindow(snapshot, "glm5.2");
    bool hasTurbo = HasZAiModelWindow(snapshot, "glm5turbo");

    if (!hasGlm52 && !hasTurbo) {
        DrawSettingsMutedText("No quota windows are currently available.");
        return;
    }

    if (ImGui::BeginTable("##zai_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.52f);

        if (hasGlm52) DrawQuotaRuleSettings("GLM-5.2", g_zaiQuotaWarnings.glm52);
        if (hasTurbo) DrawQuotaRuleSettings("GLM-5-Turbo", g_zaiQuotaWarnings.turbo);

        ImGui::EndTable();
    }
}

static void DrawCleanGrokNotifications()
{
    if (ImGui::BeginTable("##grok_notify_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Option", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.50f);

        DrawNotificationToggleRow("Enabled", g_grokNotifySettings.enabled, "Notifications on", "Notifications off");
        DrawNotificationToggleRow("Prepare reset warning", g_grokNotifySettings.prepareReset, "Before quota resets", "Off");
        DrawPrepareMinutesRow(g_grokNotifySettings);
        DrawNotificationToggleRow("Exact reset notification", g_grokNotifySettings.exactReset, "When quota resets", "Off");

        ImGui::EndTable();
    }
}

static void DrawCleanGrokQuotaWarnings()
{
    Grok::Snapshot snapshot = CopyGrokSnapshotForSettings();

    if (!snapshot.weeklyLimit.valid) {
        DrawSettingsMutedText("No quota windows are currently available.");
        return;
    }

    if (ImGui::BeginTable("##grok_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.52f);

        DrawQuotaRuleSettings("Weekly limit", g_grokQuotaWarnings.weekly);

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

static void DrawCleanGeneralSettings(float contentWidth, float contentHeight)
{
    float cardHeight = std::max(340.0f, contentHeight);
    BeginCleanSettingsCard("##settings_general", "General", ImVec2(contentWidth, cardHeight));

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
            GrokNotifier::SetPosition(g_notifyPosition);
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
            GrokNotifier::SetPosition(g_notifyPosition);
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

static void SyncCodexCustomPathBuffer()
{
    if (g_codexCustomPathBufferDirty || g_codexCustomPathBufferSynced == g_codexCustomAuthPath) {
        return;
    }

    CopyStringToBuffer(g_codexCustomPathBuffer, g_codexCustomAuthPath);
    g_codexCustomPathBufferSynced = g_codexCustomAuthPath;
}

static bool SaveAndApplyCodexAccountSource(bool commitPathBuffer)
{
    if (commitPathBuffer) {
        g_codexCustomAuthPath = g_codexCustomPathBuffer.data();
        g_codexCustomPathBufferSynced = g_codexCustomAuthPath;
        g_codexCustomPathBufferDirty = false;
    }

    bool saved = AppSettings::SaveCodexAccountSource(
        g_codexAccountSource,
        g_codexCustomAuthPath
    );

    if (!saved) {
        NotifyGUI::Add(
            "Failed to save the Codex account source",
            g_notifyPosition,
            5.0f,
            NOTIFY_COL32(255, 90, 90, 255)
        );
    }

    ApplySettingsToRuntime();
    RefreshCodexAsync();
    return saved;
}

static const char* CodexAccountSourceDescription(int source)
{
    switch (AppSettings::ClampCodexAccountSource(source)) {
    case 1:
        return "Uses only the account Codex currently exposes through app-server. It never reads auth.json.";
    case 2:
        return "Directly parses CODEX_HOME\\auth.json, or %USERPROFILE%\\.codex\\auth.json when CODEX_HOME is unset. It never starts app-server.";
    case 3:
        return "Directly parses only the selected auth.json file. No app-server or default-file fallback is used.";
    default:
        return "Uses the active Codex app-server account first. The default auth.json is used only when app-server is unavailable, never after an app-server error.";
    }
}

static void DrawProviderAutoRefreshSetting(
    const char* checkboxId,
    bool& enabled
) {
    const bool previous = enabled;
    ImGui::Checkbox(checkboxId, &enabled);

    if (enabled != previous) {
        ClearAutoRefreshWarning();

        if (!SaveAppSettings()) {
            NotifyGUI::Add(
                "Failed to save provider auto-refresh setting",
                g_notifyPosition,
                5.0f,
                NOTIFY_COL32(255, 90, 90, 255)
            );
        }
    }

    DrawSettingsMutedWrappedText(
        g_autoRefreshEnabled
            ? (enabled ? "This provider refreshes automatically at the global interval." : "Automatic requests for this provider are disabled. Use Refresh on its tab when needed.")
            : "Global auto refresh is off. This provider setting will apply when the global switch is enabled."
    );
}

static void DrawCodexSettingsCard(float width, float height)
{
    ImGui::PushID("##codex_settings_card");
    BeginCleanSettingsCard("##codex_settings_card", "Codex", ImVec2(width, height));

    DrawSettingsSubHeader("Account source");
    ImGui::SetNextItemWidth(-1.0f);

    static const char* sourceNames[] = {
        "Auto - active account, then auth.json",
        "Active Codex account only",
        "Codex auth.json only",
        "Custom Codex auth.json"
    };

    SyncCodexCustomPathBuffer();

    int selectedSource = AppSettings::ClampCodexAccountSource(g_codexAccountSource);

    if (ImGui::Combo("##codex_account_source", &selectedSource, sourceNames, 4)) {
        g_codexAccountSource = AppSettings::ClampCodexAccountSource(selectedSource);
        SaveAndApplyCodexAccountSource(g_codexCustomPathBufferDirty);
    }

    DrawSettingsMutedWrappedText(CodexAccountSourceDescription(g_codexAccountSource));

    if (g_codexAccountSource == 2) {
        DrawSettingsMutedText("Default file: %CODEX_HOME%\\auth.json or %USERPROFILE%\\.codex\\auth.json");
        DrawSettingsMutedWrappedText("This explicit mode honors the file even when config.toml uses auto, keyring, or ephemeral; the main Codex page warns that the file may be stale.");
    }
    else if (g_codexAccountSource == 3) {
        ImGui::Spacing();
        DrawSettingsMutedText("Custom auth.json path");
        ImGui::SetNextItemWidth(-1.0f);

        if (ImGui::InputText(
            "##codex_custom_auth_path",
            g_codexCustomPathBuffer.data(),
            g_codexCustomPathBuffer.size()
        )) {
            g_codexCustomPathBufferDirty = true;
        }

        if (ImGui::Button("Browse...", ImVec2(96.0f, 26.0f))) {
            std::string selectedPath = g_codexCustomPathBuffer.data();

            if (BrowseForCodexAuthJson(selectedPath)) {
                CopyStringToBuffer(g_codexCustomPathBuffer, selectedPath);
                g_codexCustomPathBufferDirty = true;
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!g_codexCustomPathBufferDirty);

        if (ImGui::Button("Apply path", ImVec2(104.0f, 26.0f))) {
            SaveAndApplyCodexAccountSource(true);
        }

        ImGui::EndDisabled();
        DrawSettingsMutedText("Environment variables such as %USERPROFILE% are expanded. Relative paths are resolved to an absolute path.");
    }

    DrawSettingsMutedText("The source selection is saved automatically.");

    ImGui::Spacing();
    DrawSettingsSubHeader("Auto refresh");
    DrawProviderAutoRefreshSetting("Refresh Codex automatically", g_codexAutoRefreshEnabled);

    ImGui::Spacing();
    DrawSettingsSubHeader("Notifications");
    DrawCleanProviderNotifications(g_codexNotifySettings, true);

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanQuotaWarnings(true);

    EndCleanSettingsCard();
    ImGui::PopID();
}

static const char* ClaudeAccountSourceDescription(int source)
{
    switch (AppSettings::ClampClaudeAccountSource(source)) {
    case 1:
        return "Uses only the account signed in to Claude Desktop. It reads the live Desktop cookie/OAuth session; Claude Code files and environment tokens are never read.";
    case 2:
        return "Uses only Claude Code .credentials.json. This is a separate Claude Code login: signing in or switching accounts in Claude Desktop does not update this file.";
    case 3:
        return "Uses only CLAUDE_CODE_OAUTH_TOKEN from this process environment. Claude Desktop and .credentials.json are ignored.";
    default:
        return "Uses Claude Desktop first. If Desktop has no session, it uses .credentials.json; the environment token is used only when that file is absent. Read or decryption errors never silently switch accounts.";
    }
}

static void DrawClaudeSettingsCard(float width, float height)
{
    ImGui::PushID("##claude_settings_card");
    BeginCleanSettingsCard("##claude_settings_card", "Claude", ImVec2(width, height));

    DrawSettingsSubHeader("Account source");
    ImGui::SetNextItemWidth(-1.0f);

    static const char* sourceNames[] = {
        "Auto - Desktop, credentials file, then environment",
        "Claude Desktop only",
        "Claude Code .credentials.json only",
        "Claude Code environment token only"
    };

    int selectedSource = AppSettings::ClampClaudeAccountSource(g_claudeAccountSource);

    if (ImGui::Combo("##claude_account_source", &selectedSource, sourceNames, 4)) {
        g_claudeAccountSource = AppSettings::ClampClaudeAccountSource(selectedSource);

        if (!AppSettings::SaveClaudeAccountSource(g_claudeAccountSource)) {
            NotifyGUI::Add(
                "Failed to save the Claude account source",
                g_notifyPosition,
                5.0f,
                NOTIFY_COL32(255, 90, 90, 255)
            );
        }

        ApplySettingsToRuntime();
        RefreshClaudeAsync();
    }

    DrawSettingsMutedWrappedText(ClaudeAccountSourceDescription(g_claudeAccountSource));

    if (g_claudeAccountSource == 2) {
        DrawSettingsMutedText("File: %CLAUDE_CONFIG_DIR%\\.credentials.json or %USERPROFILE%\\.claude\\.credentials.json");
    }
    else if (g_claudeAccountSource == 3) {
        DrawSettingsMutedText("Environment variable: CLAUDE_CODE_OAUTH_TOKEN");
    }

    DrawSettingsMutedText("This selection is saved automatically.");

    ImGui::Spacing();
    DrawSettingsSubHeader("Thinking animation");
    int shimmerSpeed = AppSettings::ClampClaudeThinkingShimmerSpeedPercent(
        g_claudeThinkingShimmerSpeedPercent
    );
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt(
        "##claude_thinking_shimmer_speed",
        &shimmerSpeed,
        25,
        250,
        "%d%% speed"
    )) {
        g_claudeThinkingShimmerSpeedPercent =
            AppSettings::ClampClaudeThinkingShimmerSpeedPercent(shimmerSpeed);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && R().saveAppSettings) {
        R().saveAppSettings();
    }
    DrawSettingsMutedText("Controls the white sweep through THINKING. Default: 100%.");

    ImGui::Spacing();
    DrawSettingsSubHeader("Auto refresh");
    DrawProviderAutoRefreshSetting("Refresh Claude automatically", g_claudeAutoRefreshEnabled);

    ImGui::Spacing();
    DrawSettingsSubHeader("Notifications");
    DrawCleanProviderNotifications(g_claudeNotifySettings, false);

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanQuotaWarnings(false);

    EndCleanSettingsCard();
    ImGui::PopID();
}

static void DrawZAiSettingsCard(float width, float height)
{
    ImGui::PushID("##zai_settings_card");
    BeginCleanSettingsCard("##zai_settings_card", "Z.Ai", ImVec2(width, height));

    DrawSettingsSubHeader("Auto refresh");
    DrawProviderAutoRefreshSetting("Refresh Z.Ai automatically", g_zaiAutoRefreshEnabled);

    ImGui::Spacing();
    DrawSettingsSubHeader("Notifications");
    DrawCleanZAiNotifications();

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanZAiQuotaWarnings();

    EndCleanSettingsCard();
    ImGui::PopID();
}

static void DrawGrokSettingsCard(float width, float height)
{
    ImGui::PushID("##grok_settings_card");
    BeginCleanSettingsCard("##grok_settings_card", "Grok", ImVec2(width, height));

    DrawSettingsSubHeader("Auto refresh");
    DrawProviderAutoRefreshSetting("Refresh Grok automatically", g_grokAutoRefreshEnabled);

    ImGui::Spacing();
    DrawSettingsSubHeader("Notifications");
    DrawCleanGrokNotifications();

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanGrokQuotaWarnings();

    EndCleanSettingsCard();
    ImGui::PopID();
}

enum class ActiveSettingsTab
{
    General,
    Codex,
    Claude,
    Grok,
    ZAi
};

static ActiveSettingsTab g_activeSettingsTab = ActiveSettingsTab::General;

static void DrawSettingsSideTab(const char* label, ActiveSettingsTab tab)
{
    bool selected = g_activeSettingsTab == tab;

    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.150f, 0.310f, 0.470f, 1.0f) : ImVec4(0.130f, 0.130f, 0.130f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.180f, 0.370f, 0.560f, 1.0f) : ImVec4(0.190f, 0.220f, 0.250f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.170f, 0.350f, 0.540f, 1.0f));

    if (ImGui::Button(label, ImVec2(-1.0f, 32.0f))) {
        g_activeSettingsTab = tab;
    }

    ImGui::PopStyleColor(4);
    ImGui::PopID();
}

static void DrawSelectedSettingsPage(float width, float height)
{
    height = std::max(260.0f, height);

    switch (g_activeSettingsTab) {
    case ActiveSettingsTab::General:
        DrawCleanGeneralSettings(width, height);
        break;
    case ActiveSettingsTab::Codex:
        DrawCodexSettingsCard(width, height);
        break;
    case ActiveSettingsTab::Claude:
        DrawClaudeSettingsCard(width, height);
        break;
    case ActiveSettingsTab::Grok:
        DrawGrokSettingsCard(width, height);
        break;
    case ActiveSettingsTab::ZAi:
        DrawZAiSettingsCard(width, height);
        break;
    }
}

static void DrawSettingsTab()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 5.0f));

    float contentWidth = ImGui::GetContentRegionAvail().x;
    float contentHeight = ImGui::GetContentRegionAvail().y;
    float sideWidth = std::min(170.0f, std::max(150.0f, contentWidth * 0.16f));

    ImGui::BeginChild("##settings_side_tabs", ImVec2(sideWidth, contentHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawSettingsSideTab("General", ActiveSettingsTab::General);
    DrawSettingsSideTab("Codex", ActiveSettingsTab::Codex);
    DrawSettingsSideTab("Claude", ActiveSettingsTab::Claude);
    DrawSettingsSideTab("Grok", ActiveSettingsTab::Grok);
    DrawSettingsSideTab("Z.Ai", ActiveSettingsTab::ZAi);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##settings_page", ImVec2(std::max(320.0f, contentWidth - sideWidth - ImGui::GetStyle().ItemSpacing.x), contentHeight), true);
    float pageWidth = ImGui::GetContentRegionAvail().x;
    float pageHeight = ImGui::GetContentRegionAvail().y;
    DrawSelectedSettingsPage(pageWidth, pageHeight);
    ImGui::EndChild();

    ApplySettingsToRuntime();

    ImGui::PopStyleVar(3);
}

enum class ActiveMainTab
{
    Codex,
    Claude,
    ZAi,
    Grok,
    Settings
};

static ActiveMainTab g_activeTab = ActiveMainTab::Codex;

static bool DrawIconTabButton(const char* id, const char* tooltip, TabImage& image, ActiveMainTab tab)
{
    bool selected = g_activeTab == tab;

    ImGui::PushID(id);

    ImVec2 buttonSize(46.0f, 34.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##tab_button", buttonSize);
    bool hovered = ImGui::IsItemHovered();

    if (clicked) {
        g_activeTab = tab;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 fill = selected ? Color(38, 79, 120) : Color(34, 34, 34);

    if (hovered && !selected) {
        fill = Color(48, 58, 68);
    }

    draw->AddRectFilled(pos, ImVec2(pos.x + buttonSize.x, pos.y + buttonSize.y), fill, 6.0f);
    draw->AddRect(pos, ImVec2(pos.x + buttonSize.x, pos.y + buttonSize.y), Color(70, 70, 70), 6.0f);

    if (image.srv) {
        ImVec2 imageSize(24.0f, 24.0f);
        ImVec2 imagePos(pos.x + (buttonSize.x - imageSize.x) * 0.5f, pos.y + (buttonSize.y - imageSize.y) * 0.5f);
        draw->AddImage(
            reinterpret_cast<ImTextureID>(image.srv),
            imagePos,
            ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y)
        );
    }
    else {
        ImVec2 textSize = ImGui::CalcTextSize(tooltip);
        draw->AddText(
            ImVec2(pos.x + (buttonSize.x - textSize.x) * 0.5f, pos.y + (buttonSize.y - textSize.y) * 0.5f),
            Color(235, 235, 235),
            tooltip
        );
    }

    if (hovered) {
        ImGui::SetTooltip("%s", tooltip);
    }

    ImGui::PopID();

    return clicked;
}

static void DrawProviderTabStrip()
{
    TabImage& codex = EnsureTabImage(g_codexTabImage, L"Codex.ico");
    TabImage& claude = EnsureTabImage(g_claudeTabImage, L"Claude.ico");
    TabImage& zai = EnsureTabImage(g_zaiTabImage, L"ZAi.ico");
    TabImage& grok = EnsureTabImage(g_grokTabImage, L"Grok.ico");

    DrawIconTabButton("codex", "Codex", codex, ActiveMainTab::Codex);
    ImGui::SameLine();
    DrawIconTabButton("claude", "Claude", claude, ActiveMainTab::Claude);
    ImGui::SameLine();
    DrawIconTabButton("zai", "Z.Ai", zai, ActiveMainTab::ZAi);
    ImGui::SameLine();
    DrawIconTabButton("grok", "Grok", grok, ActiveMainTab::Grok);
    ImGui::SameLine();

    bool selected = g_activeTab == ActiveMainTab::Settings;
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.150f, 0.310f, 0.470f, 1.0f) : ImVec4(0.135f, 0.135f, 0.135f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.210f, 0.270f, 0.330f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.180f, 0.360f, 0.540f, 1.0f));

    if (ImGui::Button("Settings", ImVec2(96.0f, 34.0f))) {
        g_activeTab = ActiveMainTab::Settings;
    }

    ImGui::PopStyleColor(3);

    ImGui::Separator();
    ImGui::Spacing();
}

static void DrawActiveMainTab()
{
    switch (g_activeTab) {
    case ActiveMainTab::Codex:
        DrawCodexTab();
        break;
    case ActiveMainTab::Claude:
        DrawClaudeTab();
        break;
    case ActiveMainTab::ZAi:
        DrawZAiTab();
        break;
    case ActiveMainTab::Grok:
        DrawGrokTab();
        break;
    case ActiveMainTab::Settings:
        DrawSettingsTab();
        break;
    }
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

    DrawProviderTabStrip();
    DrawActiveMainTab();

    ImGui::EndChild();
    ImGui::End();
}


}
