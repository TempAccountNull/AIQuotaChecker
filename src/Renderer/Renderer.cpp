#include "Global.hpp"

#include "Renderer.hpp"
#include "Text.hpp"
#include "Math.hpp"
#include "Format.hpp"
#include "ResetTime.hpp"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <wincodec.h>
#include <filesystem>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include "NotifyGUI.hpp"
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

    static bool BrowseForCodexAuthJson(std::string& selectedPath)
    {
        std::array<wchar_t, 32768> fileBuffer{};

        if (!selectedPath.empty()) {
            std::wstring current = Text::get_instance()->Utf8ToWide(selectedPath);
            wcsncpy_s(fileBuffer.data(), fileBuffer.size(), current.c_str(), _TRUNCATE);
        }

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        // Owned, so it opens above the topmost Settings window and disables it
        // while the dialog runs its own modal loop.
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFilter = L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.lpstrTitle = L"Select Codex auth.json";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

        if (!GetOpenFileNameW(&dialog)) {
            return false;
        }

        selectedPath = Text::get_instance()->WideToUtf8(fileBuffer.data());
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
#define g_widgetPinned (*R().widgetPinned)
#define g_settingsWindowOpen (*R().settingsWindowOpen)
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

static ImU32 Color(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

// Widget-only sans fonts (Segoe UI), loaded by ReloadFonts before the DX11
// atlas is built. Null when unavailable, in which case the widget uses the
// default.
static ImFont* g_widgetFont = nullptr;
static ImFont* g_widgetBigFont = nullptr;

#ifndef IM_PI
#define IM_PI 3.14159265358979323846f
#endif

static int g_loadedFontSize = 0;
static bool g_fontReloadRequested = false;

// Load Segoe UI at `size` as the interface font used by the custom panels, plus
// a semibold face for titles. The default bitmap font stays as ImGui's default
// so nothing else shifts.
// Forward slashes deliberately: "C:\Windows\Fonts" written with single
// backslashes silently loses \W and \F as invalid escapes, which is how this
// once ended up falling back to the bitmap font.
static const char* InterfaceFontPath()
{
    static const char* const kCandidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf"
    };
    for (const char* path : kCandidates) {
        if (std::filesystem::exists(path)) return path;
    }
    return nullptr;
}

static void LoadInterfaceFonts(int size)
{
    ImGuiIO& io = ImGui::GetIO();

    if (io.Fonts->Fonts.Size == 0) {
        io.Fonts->AddFontDefault();
    }

    const char* body = InterfaceFontPath();

    const char* semibold = std::filesystem::exists("C:/Windows/Fonts/seguisb.ttf")
        ? "C:/Windows/Fonts/seguisb.ttf"
        : body;

    const float bodySize = static_cast<float>(size);

    g_widgetFont = body ? io.Fonts->AddFontFromFileTTF(body, bodySize) : nullptr;
    g_widgetBigFont = semibold
        ? io.Fonts->AddFontFromFileTTF(semibold, bodySize + 4.0f)
        : nullptr;

    g_loadedFontSize = size;
}

void ReloadFonts()
{
    const int size = (g_state && R().uiFontSize)
        ? AppSettings::ClampUiFontSize(*R().uiFontSize)
        : 14;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_widgetFont = nullptr;
    g_widgetBigFont = nullptr;
    LoadInterfaceFonts(size);
    io.Fonts->Build();
}

// The settings window runs on its own ImGui context, so it has its own atlas
// and cannot borrow g_widgetFont. Install the same face as that context's
// default instead - otherwise every stock control falls back to ProggyClean.
void LoadSettingsFont(int size)
{
    ImGuiIO& io = ImGui::GetIO();

    if (io.Fonts->Fonts.Size == 0) {
        io.Fonts->AddFontDefault();
    }

    const char* body = InterfaceFontPath();
    if (!body) return;

    ImFont* font = io.Fonts->AddFontFromFileTTF(
        body, static_cast<float>(AppSettings::ClampUiFontSize(size)));
    if (font) io.FontDefault = font;
}

bool ConsumeFontReloadRequest()
{
    const bool requested = g_fontReloadRequested;
    g_fontReloadRequested = false;
    return requested;
}

// Style only: the app calls ReloadFonts straight after, which builds the atlas.
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

static std::string FormatCompactTokenCount(long long value)
{
    value = std::max<long long>(0, value);

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);

    // Lifetime and yearly totals run into the billions.
    if (value >= 1000000000) {
        out << (static_cast<double>(value) / 1000000000.0) << "B";
    }
    else if (value >= 1000000) {
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

    // Measured frame-by-frame from ref/ezgif-split (283 frames at ~65ms):
    // the state text fades UNIFORMLY - every column of it reaches minimum on
    // the same frame and maximum on the same frame, so there is no travelling
    // band - between roughly 75% and 100% brightness, symmetrically (16 frames
    // down, 15 back up, i.e. a sine) over a ~2.0s period. The elapsed time and
    // token count beside it measured dead flat, so only this text pulses.
    constexpr float kPeriodSeconds = 2.0f;
    constexpr float kMinAlpha = 0.75f;

    const ImU32 kBase = Color(226, 232, 244);

    // Reduced motion: the references kill the animation outright rather than
    // slowing it, so a static label is the right fallback.
    BOOL animationsOn = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animationsOn, 0) && !animationsOn) {
        drawList->AddText(pos, kBase, text);
        ImGui::Dummy(size);
        return;
    }

    const int speedPercent = AppSettings::ClampClaudeThinkingShimmerSpeedPercent(
        g_claudeThinkingShimmerSpeedPercent
    );
    const float period = kPeriodSeconds / (static_cast<float>(speedPercent) / 100.0f);

    const float wave = 0.5f - 0.5f * std::cos(
        static_cast<float>(ImGui::GetTime()) * 6.2831853f / period
    );
    const float alpha = kMinAlpha + (1.0f - kMinAlpha) * wave;

    ImVec4 color = ImGui::ColorConvertU32ToFloat4(kBase);
    color.w *= alpha;

    drawList->AddText(pos, ImGui::ColorConvertFloat4ToU32(color), text);
    ImGui::Dummy(size);
}

static bool ProviderAutoRefreshActive(bool providerEnabled)
{
    return g_autoRefreshEnabled && providerEnabled;
}

// ---------------------------------------------------------------------------
// Provider panels (CodexBar-style, the widget's body)
//
// A flat, sectioned layout: a section title, a thin severity-coloured bar, then
// "X% used / left" on the left and the reset on the right, with an optional
// muted sub-line. Rendered in Segoe UI (falls back to default). Every element
// is drawn from primitives - no stock cards.
// ---------------------------------------------------------------------------

// Info card text, secondary text, and the two alert tones the status line
// and the pace forecast use.
static constexpr ImU32 kPanelInfoText = IM_COL32(178, 185, 199, 255);
static constexpr ImU32 kPanelMuted = IM_COL32(138, 146, 166, 255);
static constexpr ImU32 kPanelWarning = IM_COL32(228, 168, 72, 255);
static constexpr ImU32 kPanelDanger = IM_COL32(214, 96, 96, 255);

static ImU32 PanelSeverity(float usedPercent)
{
    // Green -> yellow -> orange -> red. Deliberately desaturated: a wall of neon
    // bars reads as noise, so these sit closer to the reference popovers' muted
    // accents while still being four clearly distinct steps.
    if (usedPercent >= 90.0f) return kPanelDanger;          // red
    if (usedPercent >= 75.0f) return Color(216, 138, 68);   // orange
    if (usedPercent >= 55.0f) return Color(214, 186, 84);   // yellow
    return Color(94, 168, 132);                             // green
}

// ResetTime bakes "Resets in" into all three mode formatters. A banked credit
// expires rather than resets, so relabel the verb here instead of threading a
// noun through every one of them.
static std::string PanelExpiry(long long expiresAtUnixSeconds)
{
    if (expiresAtUnixSeconds <= 0) return {};

    std::string text = ResetTime::get_instance()->Format(
        expiresAtUnixSeconds, g_resetDisplayMode, g_showResetDateDetails);

    const std::string prefix = "Resets in ";
    if (text.rfind(prefix, 0) == 0) return "Expires in " + text.substr(prefix.size());
    if (text.rfind("Reset available", 0) == 0) return "Expired";

    return text;
}

static long long UnixSecondsFrom(std::chrono::system_clock::time_point point)
{
    if (point.time_since_epoch().count() == 0) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(point.time_since_epoch()).count();
}

static std::string PanelReset(long long resetAtUnixSeconds, const std::string& fallback)
{
    if (resetAtUnixSeconds > 0) {
        std::string text = ResetTime::get_instance()->Format(
            resetAtUnixSeconds, g_resetDisplayMode, g_showResetDateDetails);
        if (!text.empty()) return text;
    }
    return fallback;
}

// Sections render as contained cards rather than bare text on the window
// background - that containment is what makes the reference popovers read as
// designed instead of a dump of labels.
// Layout derives from the configured interface font size so the whole panel
// scales with the Settings choice instead of being pinned to magic numbers.
static float PanelTitleSize() { return static_cast<float>(g_loadedFontSize) + 3.0f; }
static float PanelPad() { return 10.0f; }

static void PanelRule()
{
    // Cards provide the separation now; this is just the gap between them.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 7.0f));
}

static void PanelCardBackground(ImVec2 a, ImVec2 b)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, Color(31, 34, 43, 255), 10.0f);
    dl->AddRect(a, b, Color(50, 54, 68, 200), 10.0f, 0, 1.0f);
}

// A live status, split the way Claude renders it: the elapsed time and token
// count are static, and only the trailing state word pulses. Measured from the
// reference frames - the leading columns held dead flat while the state text
// swung 75%-100%.
struct LiveState
{
    std::string prefix;   // "1h 22m 50s \xc2\xb7 127.4k tokens"
    std::string state;    // "Running tools\xe2\x80\xa6"
    bool animated = true; // false for a settled result like "Thought for 12s"

    bool empty() const { return state.empty(); }
};

static LiveState LiveRunState(const UsageTelemetry::RunUsage& run,
    const UsageTelemetry::ContextUsage& context)
{
    LiveState live;
    long long since = 0;

    if (context.compacting) {
        live.state = "Compacting session\xe2\x80\xa6";
        since = context.compactionStartedAtUnixSeconds > 0
            ? context.compactionStartedAtUnixSeconds
            : run.startedAtUnixSeconds;
    }
    else if (!run.valid || !run.running) {
        return live;
    }
    else {
        since = run.startedAtUnixSeconds;

        const long long now = static_cast<long long>(std::time(nullptr));
        const long long elapsed = since > 0 && now > since ? now - since : 0;

        // Waiting on the API: the transcript has stopped growing while the
        // turn is still open, so nothing is being processed locally. This is
        // derived, not reported - Claude Code's own wording and thresholds
        // live in its cli.js, which is not in the reference set.
        const bool waiting = run.idleSeconds >= 10;

        if (waiting) {
            live.state = "Waiting for Claude\xe2\x80\xa6";
        }
        else if (run.thinking) {
            // The longer a thought runs, the more it warrants saying so.
            // This threshold is ours; the CLI's is not discoverable here.
            live.state = elapsed >= 90 ? "Still thinking\xe2\x80\xa6" : "Thinking\xe2\x80\xa6";
        }
        else if (run.lastThoughtDurationSeconds > 0 &&
            run.lastThoughtCompletedAtUnixSeconds > 0 &&
            now - run.lastThoughtCompletedAtUnixSeconds <= 5) {
            // A finished thought is a result, not an activity, so it does not
            // pulse - it just sits there until the next state replaces it.
            live.state = "Thought for " + FormatRunDuration(run.lastThoughtDurationSeconds);
            live.animated = false;
        }
        else {
            live.state = "Running tools\xe2\x80\xa6";
        }
    }

    if (since > 0) {
        const long long now = static_cast<long long>(std::time(nullptr));
        if (now > since) live.prefix = FormatRunDuration(now - since);
    }

    const long long tokens = run.tokens > 0 ? run.tokens : run.currentTokens;

    if (tokens > 0) {
        if (!live.prefix.empty()) live.prefix += "  \xc2\xb7  ";
        live.prefix += FormatCompactTokenCount(tokens) + " tokens";
    }

    if (run.activeTaskCount > 0) {
        if (!live.prefix.empty()) live.prefix += "  \xc2\xb7  ";
        live.prefix += std::to_string(run.activeTaskCount) +
            (run.activeTaskCount == 1 ? " task" : " tasks");
    }

    return live;
}

// What the selected host is doing right now, if anything. A quota bar's
// right-hand slot is more useful showing "Thinking" or "Compacting" than a
// reset countdown, which is not what you are watching for in that moment; the
// countdown returns as soon as the turn ends.
static LiveState g_panelLiveState;
static std::string g_panelLiveModel;

// Width the whole status needs, so callers can right-align it.
static float MeasureLiveState(const LiveState& live)
{
    float width = ImGui::CalcTextSize(live.state.c_str()).x;

    if (!live.prefix.empty()) {
        width += ImGui::CalcTextSize(live.prefix.c_str()).x +
            ImGui::CalcTextSize("  \xc2\xb7  ").x;
    }

    return width;
}

// Draws the static prefix then the state, pulsing only the latter.
static void DrawLiveState(ImVec2 pos, const LiveState& live)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = pos.x;

    if (!live.prefix.empty()) {
        dl->AddText(ImVec2(x, pos.y), Color(132, 140, 160), live.prefix.c_str());
        x += ImGui::CalcTextSize(live.prefix.c_str()).x;

        dl->AddText(ImVec2(x, pos.y), Color(132, 140, 160), "  \xc2\xb7  ");
        x += ImGui::CalcTextSize("  \xc2\xb7  ").x;
    }

    if (live.animated) {
        const ImVec2 restore = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
        DrawClaudeThinkingShimmer(live.state.c_str());
        ImGui::SetCursorScreenPos(restore);
    }
    else {
        dl->AddText(ImVec2(x, pos.y), Color(226, 232, 244), live.state.c_str());
    }
}

// Which window a live state belongs on.
//
// Only the rolling session pool is being consumed by the turn in progress - a
// weekly cap is not "thinking" - and a model-scoped window is only involved
// when that model is the one actually running. Comparing the window's model
// against the active model is what keeps Fable from claiming a turn spent on
// Opus.
static bool WindowTakesLiveState(const std::string& title, const std::string& activeModel)
{
    const std::string lower = Text::get_instance()->ToLowerCopy(title);

    // Titles arrive as either "Session" or "<model> · Session".
    std::string modelPart;
    std::string periodPart = lower;

    const size_t sep = lower.find("\xc2\xb7");

    if (sep != std::string::npos) {
        modelPart = lower.substr(0, sep);
        periodPart = lower.substr(sep + 2);
    }

    if (periodPart.find("session") == std::string::npos) {
        return false;
    }

    if (modelPart.find_first_not_of(" \t") == std::string::npos) {
        return true;
    }

    if (activeModel.empty()) {
        return false;
    }

    // Compare on letters and digits only: "GPT-5.3 Codex Spark" against
    // "gpt-5.3-codex-spark" differs only in punctuation.
    const std::string wanted = Text::get_instance()->CompactLower(modelPart, false);
    return !wanted.empty() &&
        Text::get_instance()->CompactLower(activeModel, false).find(wanted) != std::string::npos;
}

// A pin key, "<host>:<title>". The stored list is comma-separated, so a comma
// in a provider-supplied title would split the pin apart on the next load.
static std::string PinKey(const std::string& host, const std::string& title)
{
    std::string key = host + ":" + title;
    key.erase(std::remove(key.begin(), key.end(), ','), key.end());
    return key;
}

// Drag a card into the rolled-up bar to pin it there ("<provider>:<window>"
// payload). The hit area goes down first so the card's own painting covers
// it; the card background is drawn here too, outlined while dragging.
static void PanelCardDragSource(const std::string& dragKey, const char* title, ImVec2 c0, ImVec2 c1)
{
    // A key gets one drag source per frame. A second "##card_drag" under the
    // same ID is an ImGui ID conflict, and only the first card could be pinned
    // anyway: the candidate list keeps the first row with a given key.
    static int frame = -1;
    static std::vector<std::string> submitted;

    if (frame != ImGui::GetFrameCount()) {
        frame = ImGui::GetFrameCount();
        submitted.clear();
    }

    const bool source = !dragKey.empty() &&
        std::find(submitted.begin(), submitted.end(), dragKey) == submitted.end();
    bool dragging = false;

    if (source) {
        submitted.push_back(dragKey);
        ImGui::PushID(dragKey.c_str());
        ImGui::InvisibleButton("##card_drag", ImVec2(c1.x - c0.x, c1.y - c0.y));

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("AQC_BAR_KEY", dragKey.c_str(), dragKey.size() + 1);
            ImGui::Text("Pin %s to the bar", title);
            ImGui::EndDragDropSource();
            dragging = true;
        }
        else if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag onto the bar at the bottom to pin it");
        }

        ImGui::PopID();
        ImGui::SetCursorScreenPos(c0);
    }

    PanelCardBackground(c0, c1);

    if (dragging) {
        ImGui::GetWindowDrawList()->AddRect(c0, c1, Color(96, 156, 240, 220), 10.0f, 0, 1.5f);
    }
}

// "45% used", or "55% left" with Show remaining on.
static std::string UsageText(float usedPercent)
{
    return Format::get_instance()->Percent(g_showRemaining ? 100.0f - usedPercent : usedPercent) +
        (g_showRemaining ? " left" : " used");
}

static void PanelBarSection(
    const char* title,
    bool valid,
    float usedPercent,
    const std::string& rightText,
    const std::string& subline,
    // Whether this bar tracks a window that resets on a clock. The context
    // meter does not - it moves with the conversation - so it must not be
    // annotated with a reset that will never come.
    bool resets = true,
    // "<provider>:<window>". When set, the card can be dragged into the
    // rolled-up bar to pin it there.
    const std::string& dragKey = std::string{},
    // Replaces "X% used" under the bar, for a row that is named rather than
    // read as a percentage (a credit grant's "Included credit").
    const std::string& leftText = std::string{}
)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float lineH = ImGui::GetTextLineHeight();
    const float pad = PanelPad();
    const float titleH = PanelTitleSize() + 3.0f;
    const float barH = 5.0f;

    // The sub-line may run to several lines ('\n'-separated).
    const float height = pad + titleH + 7.0f + barH + 8.0f + lineH
        + (subline.empty() ? 0.0f : ImGui::CalcTextSize(subline.c_str()).y + 3.0f) + pad;

    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 c1(c0.x + w, c0.y + height);

    PanelCardDragSource(dragKey, title, c0, c1);

    const float x = c0.x + pad;
    const float innerW = w - pad * 2.0f;
    float y = c0.y + pad;

    ImFont* tf = g_widgetBigFont ? g_widgetBigFont : ImGui::GetFont();
    dl->AddText(tf, PanelTitleSize(), ImVec2(x, y), Color(234, 238, 246), title);
    y += titleH + 7.0f;

    const float shown = valid ? (g_showRemaining ? 100.0f - usedPercent : usedPercent) : 0.0f;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    DrawThinBar(shown, innerW, valid ? PanelSeverity(usedPercent) : Color(60, 64, 76), barH,
        Color(48, 52, 63));
    y += barH + 8.0f;

    const std::string left = !valid ? "No data" : !leftText.empty() ? leftText : UsageText(usedPercent);
    dl->AddText(ImVec2(x, y), Color(206, 213, 227), left.c_str());

    // A percentage on its own is half the answer - when it resets matters as
    // much as how much is left. Claude omits resets_at on a five-hour window
    // until the first message of a session, so an empty slot here is the norm
    // at 0% rather than an error; say which case it is instead of nothing.
    if (resets && !g_panelLiveState.empty() &&
        WindowTakesLiveState(title, g_panelLiveModel)) {
        const float sw = MeasureLiveState(g_panelLiveState);
        DrawLiveState(ImVec2(x + innerW - sw, y), g_panelLiveState);
    }
    else {
        const std::string right = !rightText.empty() ? rightText
            : ((!valid || !resets) ? std::string{}
                : (usedPercent <= 0.0f ? "Not started yet" : "Reset time not reported"));

        if (!right.empty()) {
            const float rw = ImGui::CalcTextSize(right.c_str()).x;
            dl->AddText(ImVec2(x + innerW - rw, y), kPanelMuted, right.c_str());
        }
    }
    y += lineH + 3.0f;

    if (!subline.empty()) {
        dl->AddText(ImVec2(x, y), Color(126, 134, 154), subline.c_str());
    }

    ImGui::SetCursorScreenPos(ImVec2(c0.x, c1.y));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

// ZCode 3.14.0 reports the Coding Plan windows as "5-hour Coding credits"
// and "Weekly Coding credits".  Those are useful provider labels in the
// detail panel, but they are needlessly long and changed between ZCode
// releases.  Keep the widget names stable and human-readable while retaining
// the provider's identity as the drag/drop key.
static std::string ZAiWidgetDisplayLabel(const ZAi::UsageBar& bar)
{
    const std::string compact = Text::get_instance()->CompactLower(bar.label, false);

    if (compact.find("codingcredits") != std::string::npos) {
        if (compact.find("week") != std::string::npos) return "ZAi - Weekly";
        if (compact.find("hour") != std::string::npos || compact.find("session") != std::string::npos)
            return "ZAi - Session";
    }

    return bar.label.empty() ? bar.sublabel : bar.label;
}

// Letters and digits only, lowercased: the part of a name that survives a
// cosmetic rename. A name with none of them (a localized label) hashes its
// bytes instead, so such names cannot all collapse into one key.
static std::string CompactName(const std::string& name)
{
    std::string compact = Text::get_instance()->CompactLower(name, false);
    if (!compact.empty() || name.empty()) return compact;

    // FNV-1a: the same value on every build, unlike std::hash.
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : name) hash = (hash ^ c) * 16777619u;

    std::ostringstream out;
    out << 'u' << std::hex << hash;
    return out.str();
}

static std::string ZAiWidgetKey(const ZAi::UsageBar& bar, const std::string& fallbackLabel)
{
    // WidgetBarRows is comma-delimited. The former identity was a JSON array
    // containing commas, so one Z.Ai pin was split into several "No data"
    // rows. Use a stable display-derived key that is safe for that setting.
    const std::string& label = bar.label.empty() ? fallbackLabel : bar.label;
    return "Z.Ai:bar-" + (label.empty() ? std::string("usage") : CompactName(label));
}

// Lines wrap to the card, so a sentence such as the weekly pace stays whole.
// Lines from `mutedFrom` on are secondary and drawn muted.
static void PanelInfoSection(const char* title, const std::vector<std::string>& lines,
    const std::string& dragKey = std::string{}, ImU32 color = kPanelInfoText,
    size_t mutedFrom = SIZE_MAX)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = PanelPad();
    const float titleH = PanelTitleSize() + 3.0f;
    const float wrapW = w - pad * 2.0f;

    float height = pad + titleH + 5.0f + pad - 4.0f;
    for (const std::string& line : lines) {
        height += ImGui::CalcTextSize(line.c_str(), nullptr, false, wrapW).y + 4.0f;
    }

    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 c1(c0.x + w, c0.y + height);
    PanelCardDragSource(dragKey, title, c0, c1);

    const float x = c0.x + pad;
    float y = c0.y + pad;

    ImFont* tf = g_widgetBigFont ? g_widgetBigFont : ImGui::GetFont();
    dl->AddText(tf, PanelTitleSize(), ImVec2(x, y), Color(234, 238, 246), title);
    y += titleH + 5.0f;

    for (size_t i = 0; i < lines.size(); ++i) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
            i < mutedFrom ? color : kPanelMuted, lines[i].c_str(), nullptr, wrapW);
        y += ImGui::CalcTextSize(lines[i].c_str(), nullptr, false, wrapW).y + 4.0f;
    }

    ImGui::SetCursorScreenPos(ImVec2(c0.x, c1.y));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

static bool PanelShows(int bit);

static void PanelStatusLine(const UsageTelemetry::AccessStatus& access, const std::string& statusText)
{
    if (!PanelShows(AppSettings::WidgetSectionStatus)) return;

    std::string text;
    if (!access.detail.empty() && access.state != UsageTelemetry::AccessState::Available) {
        text = access.detail;
    }
    else if (!statusText.empty() && statusText.rfind("Plan:", 0) != 0) {
        text = statusText;
    }
    if (text.empty()) return;

    const float w = ImGui::GetContentRegionAvail().x;
    const ImU32 color = access.state == UsageTelemetry::AccessState::Available
        ? Color(120, 128, 148)
        : kPanelWarning;

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(w, 8.0f));
}

// Section visibility, driven by the Settings picker.
static bool PanelShows(int bit)
{
    return R().widgetSections ? ((*R().widgetSections) & bit) != 0 : true;
}

// An info card pinnable as "<host>:<title>"; nothing is drawn without lines.
// The same line builders feed the bar's candidates, so a pinned row always
// reads exactly like its card.
static void PanelInfoCard(const std::string& title, const std::vector<std::string>& lines,
    const std::string& host, ImU32 color = kPanelInfoText, size_t mutedFrom = SIZE_MAX)
{
    if (lines.empty() || lines.front().empty()) return;
    PanelInfoSection(title.c_str(), lines, PinKey(host, title), color, mutedFrom);
    PanelRule();
}

// Live token telemetry for the current turn - the numbers Claude Code and the
// Codex app-server publish per request but which we were throwing away.
static std::vector<std::string> ActivityCardLines(const UsageTelemetry::RunUsage& run,
    const UsageTelemetry::ContextUsage& context)
{
    std::vector<std::string> lines;

    // Compaction is worth announcing on its own: it happens precisely when the
    // token stats are least meaningful, so gating the whole card on them would
    // hide the one moment the user most wants to see.
    if (!context.compacting && (!run.valid || !run.tokenStatsValid)) return lines;

    if (run.running && run.startedAtUnixSeconds > 0) {
        const long long now = static_cast<long long>(std::time(nullptr));
        lines.push_back("Elapsed: " + FormatRunDuration(
            now >= run.startedAtUnixSeconds ? now - run.startedAtUnixSeconds : 0));
    }
    if (run.inputTokens > 0 || run.currentTokens > 0) {
        lines.push_back("In " + FormatCompactTokenCount(run.inputTokens) +
            "  ·  Out " + FormatCompactTokenCount(run.currentTokens));
    }
    if (run.cacheReadInputTokens > 0 || run.cacheCreationInputTokens > 0) {
        lines.push_back("Cache read " + FormatCompactTokenCount(run.cacheReadInputTokens) +
            "  ·  write " + FormatCompactTokenCount(run.cacheCreationInputTokens));
    }
    if (run.reasoningOutputTokens > 0) {
        lines.push_back("Reasoning: " + FormatCompactTokenCount(run.reasoningOutputTokens));
    }
    if (run.tokens > 0) {
        lines.push_back("Turn total: " + FormatCompactTokenCount(run.tokens));
    }
    if (!run.thinking && run.lastThoughtDurationSeconds > 0) {
        lines.push_back("Thought for " + FormatRunDuration(run.lastThoughtDurationSeconds));
    }

    return lines;
}

static void PanelActivitySection(const UsageTelemetry::RunUsage& run,
    const UsageTelemetry::ContextUsage& context, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionActivity)) return;
    PanelInfoCard("Activity", ActivityCardLines(run, context), host);
}

// Session facts Claude Code only publishes through its statusLine payload.
static void PanelSessionSection(const UsageTelemetry::RunUsage& run, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionSession)) return;
    PanelInfoCard("Session info", run.sessionDetails, host);
}

// The model actually in use and the context limit resolved for it.
static std::vector<std::string> ModelCardLines(const UsageTelemetry::ContextUsage& context)
{
    std::vector<std::string> lines;
    if (!context.model.empty()) lines.push_back(context.model);
    if (context.contextWindowTokens > 0) {
        lines.push_back("Context limit: " + FormatCompactTokenCount(context.contextWindowTokens));
    }
    if (!lines.empty() && !context.sourceLabel.empty()) lines.push_back(context.sourceLabel);
    return lines;
}

static void PanelModelSection(const UsageTelemetry::ContextUsage& context, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionModel)) return;
    PanelInfoCard("Model", ModelCardLines(context), host);
}

static void PanelContextSection(const UsageTelemetry::ContextUsage& context, const char* host)
{
    if (!PanelShows(AppSettings::WidgetSectionContext)) return;
    if (!context.valid || context.contextWindowTokens <= 0) return;

    const float used = Math::get_instance()->PercentUsed(
        static_cast<double>(context.usedTokens),
        static_cast<double>(context.contextWindowTokens));

    std::string right;
    if (context.compacting) right = "Compacting...";
    else if (context.autoCompactPercentValid)
        right = "Auto compact @ " + std::to_string(context.autoCompactPercentLeft) + "%";

    const std::string tokens = FormatCompactTokenCount(context.usedTokens) + " / " +
        FormatCompactTokenCount(context.contextWindowTokens) + " tokens";

    PanelBarSection("Context", true, used, right, tokens, /*resets=*/false, PinKey(host, "Context"));
    PanelRule();
}

// The subscription actually in force. Providers default this string to their
// own name, so a bare "Codex"/"Claude" means the plan has not been resolved -
// say so rather than presenting the fallback as if it were the answer.
static std::vector<std::string> PlanCardLines(const char* provider, const std::string& plan,
    const std::string& statusText, const std::string& renewal = std::string{})
{
    std::vector<std::string> lines;

    const bool resolved = !plan.empty() && plan != provider;
    lines.push_back(resolved ? plan : "Not reported");

    // Second, so a pinned Plan row reads "Renews Nov 3" beside the plan.
    if (!renewal.empty()) {
        lines.push_back(renewal);
    }

    // Providers put "Plan: <x> | Source: <y>" in statusText; the source half is
    // worth keeping here because it says which credential the plan came from.
    const std::string marker = "Source: ";
    const size_t at = statusText.find(marker);
    if (at != std::string::npos) {
        lines.push_back(statusText.substr(at + marker.size()));
    }

    return lines;
}

static void PanelPlanSection(const char* provider, const std::vector<std::string>& lines,
    size_t mutedFrom = SIZE_MAX)
{
    if (!PanelShows(AppSettings::WidgetSectionPlan)) return;
    PanelInfoCard("Plan", lines, provider, kPanelInfoText, mutedFrom);
}

// Provider-native cache telemetry. ZCode publishes both the latest request's
// hit ratio and a cumulative average; every other provider leaves this unset,
// so the card simply does not appear for them.
static std::vector<std::string> CacheCardLines(const UsageTelemetry::ContextUsage& context)
{
    std::vector<std::string> lines;
    if (!context.cacheStatsValid) return lines;

    if (context.latestCacheHitPercentValid) {
        lines.push_back("Latest hit: " +
            Format::get_instance()->Percent(static_cast<float>(context.latestCacheHitPercent)));
    }
    if (context.averageCacheHitPercentValid) {
        lines.push_back("Average hit: " +
            Format::get_instance()->Percent(static_cast<float>(context.averageCacheHitPercent)));
    }
    if (context.cacheReadTokens > 0 || context.cacheWriteTokens > 0) {
        lines.push_back("Read " + FormatCompactTokenCount(context.cacheReadTokens) +
            "  ·  write " + FormatCompactTokenCount(context.cacheWriteTokens));
    }
    if (context.totalCacheReadTokens > 0 || context.totalCacheWriteTokens > 0) {
        lines.push_back("Total read " + FormatCompactTokenCount(context.totalCacheReadTokens) +
            "  ·  write " + FormatCompactTokenCount(context.totalCacheWriteTokens));
    }

    return lines;
}

static void PanelCacheSection(const UsageTelemetry::ContextUsage& context, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionCache)) return;
    PanelInfoCard("Cache", CacheCardLines(context), host);
}

// What is actually filling the context window - messages, system prompt, tool
// definitions. ZCode reports this natively; the provider normalises it into
// label/value/percent so nothing provider-specific lands here.
static std::vector<std::string> BreakdownCardLines(const UsageTelemetry::ContextUsage& context)
{
    std::vector<std::string> lines;
    for (const UsageTelemetry::ContextBreakdownEntry& entry : context.breakdown) {
        if (entry.label.empty()) continue;
        std::string line = entry.label + ": " + FormatCompactTokenCount(entry.value);
        if (entry.percent > 0.0) {
            line += "  (" + Format::get_instance()->Percent(static_cast<float>(entry.percent)) + ")";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

static void PanelBreakdownSection(const UsageTelemetry::ContextUsage& context, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionBreakdown)) return;
    PanelInfoCard("Context breakdown", BreakdownCardLines(context), host);
}

// The last completed compaction. Claude writes an explicit boundary record and
// Codex reports a retained count; both land in compactionSavedTokens.
static std::vector<std::string> CompactionCardLines(const UsageTelemetry::ContextUsage& context)
{
    std::vector<std::string> lines;
    if (context.compactionSavedTokens <= 0) return lines;

    lines.push_back("Freed " + FormatCompactTokenCount(context.compactionSavedTokens));

    if (context.compactionPreTokens > 0) {
        lines.push_back("Was " + FormatCompactTokenCount(context.compactionPreTokens));
    }
    if (context.compactionCompletedAtUnixSeconds > 0) {
        // ResetTime::Format is built for future reset points and degrades to
        // "Reset available" once the timestamp is in the past, which is every
        // compaction. Elapsed-since is the useful reading here anyway.
        const long long now = static_cast<long long>(std::time(nullptr));
        if (now > context.compactionCompletedAtUnixSeconds) {
            lines.push_back(FormatRunDuration(now - context.compactionCompletedAtUnixSeconds) + " ago");
        }
    }

    return lines;
}

static void PanelCompactionSection(const UsageTelemetry::ContextUsage& context, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionCompaction)) return;
    PanelInfoCard("Last compaction", CompactionCardLines(context), host);
}

// Wall time vs time actually spent in the API, from the statusLine payload.
static std::vector<std::string> TimingCardLines(const UsageTelemetry::RunUsage& run)
{
    std::vector<std::string> lines;
    if (run.sessionDurationMs > 0) {
        lines.push_back("Session: " + FormatRunDuration(run.sessionDurationMs / 1000));
    }
    if (run.sessionApiDurationMs > 0) {
        lines.push_back("API time: " + FormatRunDuration(run.sessionApiDurationMs / 1000));
    }
    return lines;
}

static void PanelTimingSection(const UsageTelemetry::RunUsage& run, const std::string& host)
{
    if (!PanelShows(AppSettings::WidgetSectionTiming)) return;
    PanelInfoCard("Timing", TimingCardLines(run), host);
}

// A custom refresh icon button (circular arrow), drawn from primitives.
static bool PanelRefreshButton(const char* id, ImVec2 pos, float size, bool loading)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##refresh", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = loading ? Color(38, 41, 52, 235)
        : (hovered ? Color(48, 88, 150, 255) : Color(38, 41, 52, 235));
    dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), fill, 6.0f);
    dl->AddRect(pos, ImVec2(pos.x + size, pos.y + size),
        hovered ? Color(90, 150, 235, 255) : Color(54, 58, 72, 220), 6.0f, 0, 1.0f);

    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = 6.0f;
    const ImU32 ink = Color(226, 232, 242);
    dl->PathArcTo(c, r, IM_PI * 0.4f, IM_PI * 1.95f, 24);
    dl->PathStroke(ink, 0, 1.7f);
    dl->AddTriangleFilled(
        ImVec2(c.x + r * 0.95f, c.y - r * 1.0f),
        ImVec2(c.x + r * 1.85f, c.y - r * 0.55f),
        ImVec2(c.x + r * 0.75f, c.y - r * 0.05f), ink);

    return clicked && !loading;
}

// The custom provider header: big name, muted "Updated ..." sub-line, plan on
// the right, and a refresh button. Replaces the old stock refresh header.
static void DrawProviderHeader(
    const char* name,
    const std::string& updated,
    const std::string& plan,
    bool providerEnabled,
    bool loading,
    void (*refresh)(),
    const char* refreshId
)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImFont* bf = g_widgetBigFont ? g_widgetBigFont : ImGui::GetFont();
    dl->AddText(bf, PanelTitleSize() + 4.0f, p, Color(236, 240, 248), name);

    std::string sub = loading ? "Refreshing..."
        : (updated.empty() || updated == "never"
            ? (ProviderAutoRefreshActive(providerEnabled) ? "Auto refresh on" : "Auto refresh off")
            : "Updated " + updated);
    dl->AddText(ImVec2(p.x, p.y + PanelTitleSize() + 8.0f), Color(128, 136, 156), sub.c_str());

    // plan, right-aligned on the title row (leaving room for the refresh button)
    const float btn = 30.0f;
    if (!plan.empty()) {
        const float ps = PanelTitleSize() - 2.0f;
        const float pw = bf->CalcTextSizeA(ps, FLT_MAX, 0.0f, plan.c_str()).x;
        dl->AddText(bf, ps, ImVec2(p.x + w - btn - 12.0f - pw, p.y + 4.0f),
            Color(160, 168, 186), plan.c_str());
    }

    const bool clicked = PanelRefreshButton(refreshId, ImVec2(p.x + w - btn, p.y), btn, loading);
    if (clicked && refresh) refresh();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + PanelTitleSize() + 26.0f));
    PanelRule();
}

// ---- per-provider section bodies (no header) ----
//
// Every card carries a "<host>:<title>" pin key, and the *CardLines builders
// below also feed WidgetPeekCandidates, so anything the panel shows can be
// dragged into - or ticked for - the rolled-up bar.

// "1 day", "1,204 turns".
static std::string Plural(long long count, const char* noun)
{
    return Format::get_instance()->IntegerWithCommas(static_cast<double>(count)) + " " + noun +
        (count == 1 ? "" : "s");
}

static std::string Capitalized(std::string text)
{
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

// A server code in words: "plus-1-month-free" -> "Plus 1 month free".
static std::string Humanized(std::string code)
{
    std::replace(code.begin(), code.end(), '_', ' ');
    std::replace(code.begin(), code.end(), '-', ' ');
    return Capitalized(std::move(code));
}

// "2026-10-03" -> "Oct 3"; anything else is returned as it came. The date is
// a calendar day, so it is read at local noon, which no zone shift moves off it.
static std::string ShortDate(const std::string& isoDate)
{
    std::tm day{};
    std::istringstream in(isoDate);
    in >> std::get_time(&day, "%Y-%m-%d");
    if (in.fail()) return isoDate;

    day.tm_hour = 12;
    day.tm_isdst = -1;
    const std::time_t at = std::mktime(&day);
    return at == -1 ? isoDate : Format::get_instance()->MonthDay(at);
}

// "267.66 credits", "1,200 credits", as the provider writes its own amounts.
static std::string CreditCount(double credits)
{
    return Format::get_instance()->CreditAmount(credits) + " credits";
}

// "GLM-5.3 1.2M · GLM-5.3-Flash 300.0k": the three biggest, biggest first.
static std::string TopEntries(std::vector<std::pair<std::string, long long>> entries)
{
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });

    std::string text;
    for (size_t i = 0; i < entries.size() && i < 3; ++i) {
        if (!text.empty()) text += " · ";
        text += entries[i].first + " " + FormatCompactTokenCount(entries[i].second);
    }
    return text;
}

// "Streak: 4 days · best 12 days". -1 is "not reported".
static std::string StreakLine(int current, int longest)
{
    std::string line = "Streak: " + (current >= 0 ? Plural(current, "day") : std::string("--"));
    if (longest >= 0) line += " · best " + Plural(longest, "day");
    return line;
}

// "Nov 30, 10:59 PM CST", how Desktop dates a grant's expiry.
static std::string ExpiryDate(long long unixSeconds)
{
    return Format::get_instance()->LocalTime(unixSeconds, "%b %d, %I:%M %p %Z");
}

// A window's name. Pin keys keep the raw name, so a nameless window still
// resolves the pin it always had while the card reads "Usage".
static std::string CodexBarName(const Codex::UsageBar& bar)
{
    return !bar.label.empty() ? bar.label : bar.sublabel;
}

static std::string CodexExtraUsageTitle(const Codex::Snapshot& s)
{
    return s.extraUsage.label.empty() ? "Extra usage" : s.extraUsage.label;
}

static std::string CodexExtraUsageLine(const Codex::Snapshot& s)
{
    if (!s.extraUsage.valid || s.extraUsage.spentText.empty()) return {};
    return "This month: " + s.extraUsage.spentText +
        (s.extraUsage.limitText.empty() ? "" : " / " + s.extraUsage.limitText);
}

// Desktop only blocks on a HARD_CAP; any other cap just warns.
static std::string CodexCapNote(const Codex::Snapshot& s)
{
    return s.extraUsage.enforced ? std::string{} : "(warning only)";
}

// The month's spend against the cap, then the note, which the panel mutes.
static std::vector<std::string> CodexExtraUsageCardLines(const Codex::Snapshot& s)
{
    std::vector<std::string> lines{ CodexExtraUsageLine(s) };
    if (const std::string note = CodexCapNote(s); !note.empty()) lines.push_back(note);
    return lines;
}

// The balance, then what it buys ("~9,531–49,563 local · ~1,525–9,531
// cloud messages").
static std::vector<std::string> CodexCreditCardLines(const Codex::Snapshot& s)
{
    if (!s.creditBalance.valid || s.creditBalance.balanceText.empty()) return {};

    std::vector<std::string> lines{ s.creditBalance.unlimited ? "Unlimited" : s.creditBalance.balanceText };
    if (!s.creditBalance.approxMessagesText.empty()) lines.push_back(s.creditBalance.approxMessagesText);
    return lines;
}

// Credit auto-reload: "On · below 125 credits, refill to 500 credits ·
// monthly limit 2,000 credits". No monthly limit means no cap.
static std::vector<std::string> CodexAutoTopUpCardLines(const Codex::AutoTopUp& topUp)
{
    if (!topUp.valid) return {};
    if (!topUp.enabled) return { "Off" };

    std::string line = "On";
    if (!topUp.thresholdText.empty()) line += " · below " + topUp.thresholdText;
    if (!topUp.targetText.empty()) {
        line += (topUp.thresholdText.empty() ? " · " : ", ") + std::string("refill to ") + topUp.targetText;
    }
    line += topUp.monthlyLimitText.empty() ? " · no monthly limit" : " · monthly limit " + topUp.monthlyLimitText;
    return { line };
}

// Credit spend by day and surface, newest first: "Oct 3 · Desktop App · 267.7 credits".
static std::vector<std::string> CodexCreditUsageCardLines(const Codex::Snapshot& s)
{
    std::vector<std::string> lines;
    for (const Codex::CreditEvent& event : s.creditEvents) {
        std::string line = ShortDate(event.date);
        if (!event.surface.empty()) line += " · " + event.surface;
        lines.push_back(line + " · " + CreditCount(event.credits));
    }
    return lines;
}

static std::vector<std::string> CodexResetCreditCardLines(const Codex::Snapshot& s)
{
    if (s.resetCreditsAvailableCount < 0 && s.resetCreditsEarnedCount < 0) return {};

    std::vector<std::string> lines;
    if (s.resetCreditsAvailableCount >= 0) {
        lines.push_back(std::to_string(s.resetCreditsAvailableCount) + " banked");
    }
    if (s.resetCreditsEarnedCount >= 0) {
        lines.push_back(std::to_string(s.resetCreditsEarnedCount) + " earned in total");
    }

    // Each banked credit carries its own expiry, already sorted soonest first
    // by the provider. A bare count hides the only actionable part.
    for (const Codex::ResetCredit& credit : s.resetCredits) {
        const std::string when = PanelExpiry(UnixSecondsFrom(credit.expiresAt));
        if (when.empty() && credit.title.empty()) continue;

        std::string line = credit.title.empty() ? std::string("Credit") : credit.title;
        if (!credit.resetType.empty()) line += " (" + credit.resetType + ")";
        if (!when.empty()) line += "  ·  " + when;
        lines.push_back(std::move(line));
    }

    return lines;
}

// Banked-reset activity over the last 30 days, newest first.
static std::vector<std::string> CodexResetHistoryCardLines(const Codex::Snapshot& s)
{
    constexpr size_t kShown = 5;

    std::vector<Codex::ResetCredit> events = s.resetHistory;
    std::stable_sort(events.begin(), events.end(), [](const Codex::ResetCredit& a, const Codex::ResetCredit& b) {
        return a.grantedAt > b.grantedAt;
    });

    std::vector<std::string> lines;
    for (size_t i = 0; i < events.size() && i < kShown; ++i) {
        std::string line = events[i].title.empty() ? std::string("Reset") : events[i].title;
        if (!events[i].resetType.empty()) line += " (" + events[i].resetType + ")";
        if (const long long at = UnixSecondsFrom(events[i].grantedAt); at > 0) {
            line += " · " + Format::get_instance()->LocalTime(at, "%b %d, %I:%M %p");
        }
        lines.push_back(std::move(line));
    }
    if (events.size() > kShown) {
        lines.push_back("+" + std::to_string(events.size() - kShown) + " earlier");
    }

    return lines;
}

// Past plan windows, newest first: "5-hour · Nov 1 · 63%",
// "Weekly · Oct 29–Nov 5 · 58%".
static std::vector<std::string> CodexPlanLimitHistoryCardLines(const Codex::Snapshot& s)
{
    constexpr size_t kShown = 7;
    constexpr int kWeekMinutes = 7 * 24 * 60;

    std::vector<std::string> lines;
    for (auto period = s.planLimitHistory.rbegin();
        period != s.planLimitHistory.rend() && lines.size() < kShown; ++period) {
        if (period->startsAtUnixSeconds <= 0) continue;

        const bool weekly = period->windowMinutes >= kWeekMinutes;
        std::string line = weekly ? std::string("Weekly")
            : std::to_string(std::max(1, period->windowMinutes / 60)) + "-hour";
        line += " · " + Format::get_instance()->MonthDay(period->startsAtUnixSeconds);
        if (weekly && period->endsAtUnixSeconds > 0) {
            line += "\xe2\x80\x93" + Format::get_instance()->MonthDay(period->endsAtUnixSeconds);
        }
        // -1: the window has not been accounted for yet.
        line += " · " + (period->usedPercent < 0.0f ? std::string("pending")
            : Format::get_instance()->Percent(period->usedPercent));
        lines.push_back(std::move(line));
    }

    return lines;
}

// Each surface's share of the last 7 days, like Claude's weekly split.
static std::vector<std::string> CodexSurfaceShareCardLines(const Codex::Snapshot& s)
{
    std::vector<std::string> lines;
    for (const Codex::SurfaceShare& share : s.surfaceShare) {
        lines.push_back(share.surface + "  " + Format::get_instance()->Percent(share.percent));
    }
    return lines;
}

// The 30-day total, then the last 7 days, newest first:
// "Nov 1 · 1.2M tokens · 18 turns · 350 credits".
static std::vector<std::string> CodexDailyUsageCardLines(const Codex::Snapshot& s)
{
    constexpr size_t kDays = 7;
    if (s.dailyUsage.empty()) return {};

    const auto line = [](const std::string& when, const Codex::DailyUsage& usage) {
        return when + " · " + FormatCompactTokenCount(usage.tokens) + " tokens · " +
            Plural(usage.turns, "turn") + " · " + CreditCount(usage.credits);
    };

    Codex::DailyUsage total;
    for (const Codex::DailyUsage& day : s.dailyUsage) {
        total.tokens += day.tokens;
        total.turns += day.turns;
        total.credits += day.credits;
    }

    std::vector<std::string> lines{ line("30 days", total) };
    for (auto day = s.dailyUsage.rbegin(); day != s.dailyUsage.rend() && lines.size() <= kDays; ++day) {
        lines.push_back(line(ShortDate(day->date), *day));
    }
    return lines;
}

// Codex Desktop's profile page (GET /wham/profiles/me). -1 is "not
// reported", so each figure stands or falls on its own. A stats failure is
// the last line, so the panel can mute it.
static std::vector<std::string> CodexProfileCardLines(const Codex::ProfileStats& p)
{
    std::vector<std::string> lines;
    if (!p.valid && !p.statsError) return lines;

    if (p.lifetimeTokens >= 0) lines.push_back("Lifetime: " + FormatCompactTokenCount(p.lifetimeTokens) + " tokens");
    if (p.todayTokens >= 0) lines.push_back("Today: " + FormatCompactTokenCount(p.todayTokens) + " tokens");
    if (p.peakDailyTokens >= 0) lines.push_back("Peak day: " + FormatCompactTokenCount(p.peakDailyTokens) + " tokens");
    if (p.longestTaskSeconds >= 0) lines.push_back("Longest task: " + FormatRunDuration(p.longestTaskSeconds));
    if (p.currentStreakDays >= 0 || p.longestStreakDays >= 0) {
        lines.push_back(StreakLine(p.currentStreakDays, p.longestStreakDays));
    }
    if (p.totalThreads >= 0) lines.push_back("Threads: " + Format::get_instance()->IntegerWithCommas(static_cast<double>(p.totalThreads)));
    if (!p.reasoningEffort.empty()) {
        lines.push_back("Reasoning: " + Capitalized(p.reasoningEffort) + (p.reasoningEffortPercent >= 0.0
            ? " (" + Format::get_instance()->Percent(p.reasoningEffortPercent) + ")" : std::string{}));
    }
    if (p.fastModePercent >= 0.0) lines.push_back("Fast mode: " + Format::get_instance()->Percent(p.fastModePercent));
    if (!p.topInvocations.empty()) lines.push_back("Most used: " + TopEntries(p.topInvocations));
    // -1 is a personal account, which has no workspace to rank in.
    if (p.workspaceRank >= 0 && p.workspaceUsers > 0) {
        lines.push_back("Rank " + std::to_string(p.workspaceRank) + " of " +
            std::to_string(p.workspaceUsers) + " in the workspace");
    }
    if (!p.statsAsOf.empty()) lines.push_back("As of " + ShortDate(p.statsAsOf));
    if (p.statsError) lines.push_back("Stats could not be computed");

    return lines;
}

static std::vector<std::string> CodexAccountsCardLines(const Codex::Snapshot& s)
{
    if (s.signedInAccounts <= 0) return {};
    return { Plural(s.signedInAccounts, "account") + " signed in to Codex" };
}

// What Codex is serving requests from, and the notices behind it ("Main
// limit reached - running on Luna Reserve").
static std::vector<std::string> CodexUsageSourceCardLines(const Codex::Snapshot& s)
{
    std::vector<std::string> lines;
    if (!s.usageSource.empty()) {
        lines.push_back(s.usageSource == "included" ? std::string("Included usage")
            : s.usageSource == "reserve" ? std::string("Luna Reserve")
            : Capitalized(s.usageSource));
    }
    if (!s.usageNotice.empty()) lines.push_back(s.usageNotice);
    return lines;
}

// The plan, its renewal ("Renews Oct 5 · Monthly"), the credential, then the
// promotion it runs on, if any ("Plus 1 month free"), which the panel mutes.
static std::vector<std::string> CodexPlanCardLines(const Codex::Snapshot& s)
{
    const Codex::Subscription& subscription = s.subscription;
    std::string renewal = subscription.valid ? subscription.text : std::string{};
    if (!renewal.empty() && !subscription.billingPeriod.empty()) {
        renewal += " · " + Capitalized(subscription.billingPeriod);
    }

    std::vector<std::string> lines = PlanCardLines("Codex", s.plan, s.statusText, renewal);
    if (!s.promoCampaign.empty()) lines.push_back(Humanized(s.promoCampaign));
    return lines;
}

static void RenderCodexSections(const Codex::Snapshot& snapshot)
{
    g_panelLiveState = LiveRunState(snapshot.run, snapshot.context);
    g_panelLiveModel = snapshot.context.model;
    PanelStatusLine(snapshot.access, snapshot.statusText);

    const std::vector<std::string> plan = CodexPlanCardLines(snapshot);
    PanelPlanSection("Codex", plan, snapshot.promoCampaign.empty() ? SIZE_MAX : plan.size() - 1);
    if (PanelShows(AppSettings::WidgetSectionPlan)) {
        PanelInfoCard("Accounts", CodexAccountsCardLines(snapshot), "Codex");
    }

    for (const Codex::UsageBar& bar : snapshot.bars) {
        if (!bar.valid || !PanelShows(AppSettings::WidgetSectionQuota)) continue;
        const std::string name = CodexBarName(bar);
        PanelBarSection(name.empty() ? "Usage" : name.c_str(), true, bar.usedPercent,
            PanelReset(bar.resetAtUnixSeconds, std::string{}), std::string{},
            true, PinKey("Codex", name));
        PanelRule();
    }

    if (PanelShows(AppSettings::WidgetSectionExtra) && snapshot.extraUsage.valid) {
        const std::string title = CodexExtraUsageTitle(snapshot);

        // Either way "(warning only)" sits muted beside the cap: the bar card's
        // sub-line is muted throughout.
        if (snapshot.extraUsage.hasUsedPercent) {
            const std::string line = CodexExtraUsageLine(snapshot);
            const std::string note = CodexCapNote(snapshot);
            PanelBarSection(title.c_str(), true, snapshot.extraUsage.usedPercent,
                PanelReset(snapshot.extraUsage.resetAtUnixSeconds, snapshot.extraUsage.resetText),
                line.empty() || note.empty() ? line + note : line + " " + note,
                true, PinKey("Codex", title));
            PanelRule();
        }
        else {
            PanelInfoCard(title, CodexExtraUsageCardLines(snapshot), "Codex", kPanelInfoText, 1);
        }
    }

    if (PanelShows(AppSettings::WidgetSectionExtra)) {
        PanelInfoCard("Usage source", CodexUsageSourceCardLines(snapshot), "Codex");
    }

    PanelContextSection(snapshot.context, "Codex");

    if (PanelShows(AppSettings::WidgetSectionExtra)) {
        PanelInfoCard("Credits", CodexCreditCardLines(snapshot), "Codex");
        PanelInfoCard("Workspace credits", { snapshot.workspaceCredits }, "Codex");
        PanelInfoCard("Auto top-up", CodexAutoTopUpCardLines(snapshot.autoTopUp), "Codex");
        PanelInfoCard("Credit usage", CodexCreditUsageCardLines(snapshot), "Codex");
    }
    if (PanelShows(AppSettings::WidgetSectionDetails)) {
        PanelInfoCard("Reset credits", CodexResetCreditCardLines(snapshot), "Codex");
        PanelInfoCard("Reset history", CodexResetHistoryCardLines(snapshot), "Codex");
        PanelInfoCard("Plan limit history", CodexPlanLimitHistoryCardLines(snapshot), "Codex");
        PanelInfoCard("Usage by surface", CodexSurfaceShareCardLines(snapshot), "Codex");
        PanelInfoCard("Daily usage", CodexDailyUsageCardLines(snapshot), "Codex");

        const std::vector<std::string> profile = CodexProfileCardLines(snapshot.profile);
        PanelInfoCard("Profile", profile, "Codex", kPanelInfoText,
            snapshot.profile.statsError ? profile.size() - 1 : SIZE_MAX);
    }

    PanelActivitySection(snapshot.run, snapshot.context, "Codex");
    PanelBreakdownSection(snapshot.context, "Codex");
    PanelCacheSection(snapshot.context, "Codex");
    PanelCompactionSection(snapshot.context, "Codex");
    PanelModelSection(snapshot.context, "Codex");
}

// Provider plans are brand-prefixed by their formatters ("Claude Max 5x",
// "Z.Ai GLM Coding Pro"). Beside a header that already names the provider that
// reads twice, so drop the prefix there and keep the full name in the card.
static std::string PlanBeside(const char* provider, const std::string& plan)
{
    if (plan.empty() || plan == provider) return {};

    const std::string prefix = std::string(provider) + " ";
    return plan.rfind(prefix, 0) == 0 ? plan.substr(prefix.size()) : plan;
}

static std::string ClaudeCreditsTitle(const Claude::Snapshot& s)
{
    return s.credits.label.empty() ? "Usage credits" : s.credits.label;
}

// "$0.00 spent / $50.00", or "$0.00 spent / Off" with credits off - compact,
// as Desktop's "Usage credits $0" beside a switched-off toggle. A block that
// reports nothing but "Off" says it once.
static std::string ClaudeCreditsLine(const Claude::Snapshot& s)
{
    if (!s.credits.valid || s.credits.spentText.empty()) return {};

    const std::string& limit = s.credits.monthlyLimitText;
    return s.credits.spentText + (limit.empty() || limit == s.credits.spentText ? "" : " / " + limit);
}

// The server's code for why usage credits are off, in words; empty when it
// gives none.
static std::string ClaudeCreditsDisabledText(const Claude::UsageCredits& credits)
{
    const std::string& code = credits.disabledReason;
    if (code == "out_of_credits") return "Out of credits";
    if (code == "org_level_disabled") return "Turned off for the organization";
    return Humanized(code);
}

// The credits line, then why they are off; the panel mutes the reason.
static std::vector<std::string> ClaudeCreditsCardLines(const Claude::Snapshot& s)
{
    std::vector<std::string> lines{ ClaudeCreditsLine(s) };
    if (const std::string reason = ClaudeCreditsDisabledText(s.credits); !reason.empty()) {
        lines.push_back(reason);
    }
    return lines;
}

// The plan, then "Renews Nov 3" ("Ends Nov 3" for a plan that will not renew).
static std::vector<std::string> ClaudePlanCardLines(const Claude::Snapshot& s)
{
    std::string renewal;
    if (s.planRenewsAtUnixSeconds > 0) {
        renewal = (s.planEnds ? "Ends " : "Renews ") +
            Format::get_instance()->MonthDay(s.planRenewsAtUnixSeconds);
    }
    return PlanCardLines("Claude", s.plan, s.statusText, renewal);
}

static std::vector<std::string> ClaudeCostCardLines(const UsageTelemetry::RunUsage& run)
{
    if (!run.costValid) return {};

    std::vector<std::string> lines{ "Session: " + Format::get_instance()->Dollars(run.sessionCostUsd) };
    if (run.sessionLinesAdded > 0 || run.sessionLinesRemoved > 0) {
        lines.push_back("+" + std::to_string(run.sessionLinesAdded) +
            " / -" + std::to_string(run.sessionLinesRemoved) + " lines");
    }
    if (run.sessionDurationMs > 0) {
        lines.push_back("Over " + FormatRunDuration(run.sessionDurationMs / 1000));
    }
    return lines;
}

// Desktop's weekly pace forecast. Recomputed every frame rather than cached:
// the wording ("run out Wednesday night") moves with the clock.
static Claude::Pace ClaudeWeeklyPace(const Claude::Snapshot& s, long long now)
{
    if (!s.weeklyAllModels.valid) return {};
    return Claude::ComputePace(s.weeklyAllModels.usedPercent, s.weeklyAllModels.resetAtUnixSeconds, now);
}

static ImU32 PaceColor(Claude::Pace::State state)
{
    if (state == Claude::Pace::State::HeadsUp) return kPanelWarning;
    if (state == Claude::Pace::State::LimitHit) return kPanelDanger;
    return kPanelMuted;
}

// Free limit resets, read-only. Claiming one is a POST this app never sends,
// so the card carries what Desktop's does minus its "Reset for free" button.
static std::vector<std::string> ClaudeLimitResetCardLines(const Claude::LimitResets& resets, long long now)
{
    std::vector<std::string> lines;

    for (const Claude::LimitReset& grant : resets.grants) {
        std::string line = grant.title + " · " + std::to_string(grant.left) + " left";
        if (grant.endsAtUnixSeconds > 0) line += " · Expires " + ExpiryDate(grant.endsAtUnixSeconds);
        if (grant.paused) line += " · Paused";
        if (grant.usableNow) line += " · Usable now";
        lines.push_back(std::move(line));
    }

    if (!lines.empty() && resets.cooldownUntilUnixSeconds > now) {
        lines.push_back("Cooling down until " + ExpiryDate(resets.cooldownUntilUnixSeconds));
    }

    return lines;
}

// One-time credit grants. Their titles repeat ("Promotional credit"), so pins
// use the usage field name the provider keeps for exactly that.
static std::string PromoCreditKey(const Claude::PromoCredit& credit)
{
    return PinKey("Claude", credit.key.empty() ? credit.title : credit.key);
}

// "$250 of $250 left"; empty without dollar figures.
static std::string PromoCreditLeft(const Claude::PromoCredit& credit)
{
    if (!credit.hasDollars) return {};
    return Format::get_instance()->Dollars(credit.remainingDollars, 0) + " of " +
        Format::get_instance()->Dollars(credit.limitDollars, 0) + " left";
}

// "Expires 1:59 AM CST, November 5", then the lock, if any.
static std::string PromoCreditSubline(const Claude::PromoCredit& credit)
{
    std::string text;
    if (credit.expiresAtUnixSeconds > 0) {
        text = "Expires " + Format::get_instance()->LocalTime(credit.expiresAtUnixSeconds, "%I:%M %p %Z, %B %d");
    }
    if (!credit.lockedReason.empty()) {
        text += (text.empty() ? "" : "\n") + credit.lockedReason;
    }
    return text;
}

// This week's usage by surface, shown with the weekly window it splits.
static std::vector<std::string> ClaudeBreakdownCardLines(const Claude::Snapshot& s)
{
    std::vector<std::string> lines;
    if (!s.weeklyAllModels.valid) return lines;

    for (const Claude::BreakdownRow& row : s.weeklyBreakdown) {
        lines.push_back(row.name + "  " + Format::get_instance()->Percent(row.percent));
    }
    return lines;
}

// Desktop sessions parked until the five-hour limit resets.
static std::vector<std::string> ClaudeAutoResumeCardLines(const Claude::AutoResume& resume)
{
    if (!resume.valid) return {};

    std::string line = Plural(resume.waitingSessions, "session") + " waiting";
    if (resume.resetAtUnixSeconds > 0) {
        line += " · resumes " + Format::get_instance()->UnixResetTime(resume.resetAtUnixSeconds);
    }

    std::vector<std::string> lines{ line };
    if (resume.optedIn) lines.push_back("Auto-resume on");
    return lines;
}

static std::string ClaudeLedgerTitle(const Claude::UsageLedger& ledger)
{
    return "Usage (" + std::to_string(ledger.windowDays) + " days)";
}

// The local usage ledger Desktop's 3P builds keep, summed over its window.
static std::vector<std::string> ClaudeLedgerCardLines(const Claude::UsageLedger& ledger)
{
    if (!ledger.valid) return {};

    std::vector<std::string> lines{
        Plural(ledger.turns, "turn") + " · " + Plural(ledger.errorTurns, "error"),
        "In " + FormatCompactTokenCount(ledger.inputTokens) +
            " · Out " + FormatCompactTokenCount(ledger.outputTokens),
        "Cache read " + FormatCompactTokenCount(ledger.cacheReadTokens) +
            " · write " + FormatCompactTokenCount(ledger.cacheWriteTokens)
    };
    if (ledger.costValid) lines.push_back("Cost " + Format::get_instance()->Dollars(ledger.costUsd));
    for (const auto& [surface, tokens] : ledger.surfaceTokens) {
        lines.push_back(Capitalized(surface) + ": " + FormatCompactTokenCount(tokens) + " tokens");
    }
    return lines;
}

static void RenderClaudeSections(const Claude::Snapshot& snapshot)
{
    g_panelLiveState = LiveRunState(snapshot.run, snapshot.context);
    // Without this the previous host's model decided which row shows the live state.
    g_panelLiveModel = snapshot.context.model;
    PanelStatusLine(snapshot.access, snapshot.statusText);

    PanelPlanSection("Claude", ClaudePlanCardLines(snapshot));

    const long long now = static_cast<long long>(std::time(nullptr));

    // An invalid window draws nothing at all: Ultra has no five-hour limit, so
    // its session window arrives as null rather than as a placeholder. The
    // sub-line is the window's subtitle ("Up to 50% of weekly limit"), except
    // the session's "Starts when a message is sent", which the right-hand slot
    // already says as "Not started yet".
    const Claude::UsageWindow* windows[] = {
        &snapshot.currentSession, &snapshot.weeklyAllModels,
        &snapshot.weeklySonnet, &snapshot.weeklyFable
    };
    for (const Claude::UsageWindow* w : windows) {
        if (!w->valid || !PanelShows(AppSettings::WidgetSectionQuota)) continue;
        PanelBarSection(w->title.empty() ? "Usage" : w->title.c_str(), true, w->usedPercent,
            PanelReset(w->resetAtUnixSeconds, w->resetText),
            w == &snapshot.currentSession ? std::string{} : w->subtitle,
            true, PinKey("Claude", w->title));
        PanelRule();

        // The forecast and the split by surface read as part of the week.
        if (w == &snapshot.weeklyAllModels) {
            const Claude::Pace pace = ClaudeWeeklyPace(snapshot, now);
            PanelInfoCard("Pace", { Claude::PaceText(pace, now) }, "Claude", PaceColor(pace.state));
            PanelInfoCard("This week by surface", ClaudeBreakdownCardLines(snapshot), "Claude");
        }
    }
    for (const Claude::UsageWindow& w : snapshot.additionalLimits) {
        if (!w.valid || !PanelShows(AppSettings::WidgetSectionQuota)) continue;
        PanelBarSection(w.title.empty() ? "Usage" : w.title.c_str(), true, w.usedPercent,
            PanelReset(w.resetAtUnixSeconds, w.resetText), w.subtitle,
            true, PinKey("Claude", w.title));
        PanelRule();
    }

    if (PanelShows(AppSettings::WidgetSectionDetails)) {
        PanelInfoCard("Limit resets", ClaudeLimitResetCardLines(snapshot.limitResets, now), "Claude");
    }

    if (PanelShows(AppSettings::WidgetSectionExtra)) {
        for (const Claude::PromoCredit& credit : snapshot.promoCredits) {
            const std::string left = PromoCreditLeft(credit);
            PanelBarSection(credit.title.c_str(), true, credit.usedPercent,
                left.empty() ? UsageText(credit.usedPercent) : left, PromoCreditSubline(credit),
                /*resets=*/false, PromoCreditKey(credit), credit.label);
            PanelRule();
        }
    }

    PanelContextSection(snapshot.context, "Claude");

    if (PanelShows(AppSettings::WidgetSectionExtra) && snapshot.credits.valid) {
        const std::string title = ClaudeCreditsTitle(snapshot);

        // Either way the disabled reason sits muted beside the credits line:
        // the bar card's sub-line is muted throughout.
        if (snapshot.credits.hasUsedPercent) {
            const std::string reason = ClaudeCreditsDisabledText(snapshot.credits);
            PanelBarSection(title.c_str(), true, snapshot.credits.usedPercent,
                PanelReset(snapshot.credits.resetAtUnixSeconds, snapshot.credits.resetText),
                ClaudeCreditsLine(snapshot) + (reason.empty() ? "" : " · " + reason),
                true, PinKey("Claude", title));
            PanelRule();
        }
        else {
            PanelInfoCard(title, ClaudeCreditsCardLines(snapshot), "Claude", kPanelInfoText, 1);
        }
    }

    if (PanelShows(AppSettings::WidgetSectionCost)) {
        PanelInfoCard("Cost", ClaudeCostCardLines(snapshot.run), "Claude");
    }
    if (PanelShows(AppSettings::WidgetSectionDetails)) {
        PanelInfoCard("Auto-resume", ClaudeAutoResumeCardLines(snapshot.autoResume), "Claude");
        PanelInfoCard(ClaudeLedgerTitle(snapshot.ledger), ClaudeLedgerCardLines(snapshot.ledger), "Claude");
    }

    PanelActivitySection(snapshot.run, snapshot.context, "Claude");
    PanelSessionSection(snapshot.run, "Claude");
    PanelTimingSection(snapshot.run, "Claude");
    PanelBreakdownSection(snapshot.context, "Claude");
    PanelCacheSection(snapshot.context, "Claude");
    PanelCompactionSection(snapshot.context, "Claude");
    PanelModelSection(snapshot.context, "Claude");
}

// A provider detail row as its own card, titled by what it is ("Subscription",
// "5-hour quota resets") so each can be pinned separately.
static std::vector<std::string> ZAiDetailCardLines(const ZAi::DetailRow& detail)
{
    std::vector<std::string> lines;
    if (!detail.leftValue.empty()) lines.push_back(detail.leftValue);
    if (!detail.rightLabel.empty() && !detail.rightValue.empty()) {
        lines.push_back(detail.rightLabel + ": " + detail.rightValue);
    }
    return lines;
}

// Banked 5-hour / weekly resets, soonest expiry first, and when each kind was
// last spent - the reading ZCode's own reset panel gives.
static std::vector<std::string> ZAiResetCardLines(const ZAi::Snapshot& s)
{
    if (!s.resetsReported) return {};

    std::vector<std::string> lines;
    lines.push_back(std::to_string(s.resets.size()) + " available");

    for (const ZAi::QuotaReset& reset : s.resets) {
        const std::string when = PanelExpiry(reset.expiresAtUnixSeconds);
        lines.push_back(reset.type + " reset" + (when.empty() ? "" : "  ·  " + when));
    }

    const long long now = static_cast<long long>(std::time(nullptr));
    for (const auto& [type, usedAt] : { std::pair<const char*, long long>{ "5-hour", s.lastFiveHourResetUnixSeconds },
                                        std::pair<const char*, long long>{ "weekly", s.lastWeeklyResetUnixSeconds } }) {
        if (usedAt > 0 && usedAt <= now) {
            lines.push_back("Last " + std::string(type) + " reset used " + FormatRunDuration(now - usedAt) + " ago");
        }
    }

    return lines;
}

// Coding Plan usage history. -1 is "not reported", so each figure stands or
// falls on its own.
static std::vector<std::string> ZAiHistoryCardLines(const ZAi::UsageHistory& h)
{
    std::vector<std::string> lines;
    if (!h.valid) return lines;

    std::string today;
    const auto add = [&today](const std::string& part) {
        today += (today.empty() ? "" : " · ") + part;
    };
    if (h.todayTokens >= 0) add(FormatCompactTokenCount(h.todayTokens) + " tokens");
    if (h.todayModelCalls >= 0) add(Plural(h.todayModelCalls, "model call"));
    if (h.todayToolCalls >= 0) add(Plural(h.todayToolCalls, "tool call"));
    if (!today.empty()) lines.push_back("Today: " + today);

    if (h.totalTokens >= 0) lines.push_back("Past year: " + FormatCompactTokenCount(h.totalTokens) + " tokens");

    // "Usage time 123 h · Decode Pro 42 tok/s, Lite 88 tok/s": the year's
    // time, then the latest daily decode speed per tier.
    std::string decode;
    if (h.proDecodeTps >= 0.0) {
        decode = "Pro " + Format::get_instance()->IntegerWithCommas(h.proDecodeTps) + " tok/s";
    }
    if (h.liteDecodeTps >= 0.0) {
        decode += (decode.empty() ? "" : ", ") + std::string("Lite ") +
            Format::get_instance()->IntegerWithCommas(h.liteDecodeTps) + " tok/s";
    }
    std::string speed = h.totalUsageSeconds >= 0
        ? "Usage time " + Format::get_instance()->IntegerWithCommas(static_cast<double>(h.totalUsageSeconds / 3600)) + " h"
        : std::string{};
    if (!decode.empty()) speed += (speed.empty() ? "Decode " : " · Decode ") + decode;
    if (!speed.empty()) lines.push_back(speed);

    if (h.peakDailyTokens >= 0) {
        lines.push_back("Peak day: " + FormatCompactTokenCount(h.peakDailyTokens) + " tokens" +
            (h.peakDate.empty() ? "" : " · " + ShortDate(h.peakDate)));
    }
    if (h.currentStreakDays >= 0 || h.longestStreakDays >= 0) {
        lines.push_back(StreakLine(h.currentStreakDays, h.longestStreakDays));
    }
    if (!h.models.empty()) lines.push_back("Models, 7 days: " + TopEntries(h.models));
    if (!h.tools.empty()) lines.push_back("Tools, 7 days: " + TopEntries(h.tools));

    return lines;
}

static void RenderZAiSections(const ZAi::Snapshot& snapshot)
{
    g_panelLiveState = LiveRunState(snapshot.run, snapshot.context);
    g_panelLiveModel = snapshot.context.model;
    PanelStatusLine(snapshot.access, snapshot.statusText);

    PanelPlanSection("Z.Ai", PlanCardLines("Z.Ai", snapshot.plan, snapshot.statusText));

    for (const ZAi::UsageBar& bar : snapshot.bars) {
        if (!bar.valid || !PanelShows(AppSettings::WidgetSectionQuota)) continue;
        const std::string title = ZAiWidgetDisplayLabel(bar);
        PanelBarSection(title.c_str(), true, bar.usedPercent,
            PanelReset(bar.resetAtUnixSeconds, bar.resetText), bar.sublabel,
            true, ZAiWidgetKey(bar, bar.label.empty() ? title : bar.label));
        PanelRule();
    }

    PanelContextSection(snapshot.context, "Z.Ai");

    if (PanelShows(AppSettings::WidgetSectionDetails)) {
        for (const ZAi::DetailRow& detail : snapshot.details) {
            PanelInfoCard(detail.leftLabel.empty() ? "Details" : detail.leftLabel,
                ZAiDetailCardLines(detail), "Z.Ai");
        }
        PanelInfoCard("Quota resets", ZAiResetCardLines(snapshot), "Z.Ai");
        PanelInfoCard("Usage history", ZAiHistoryCardLines(snapshot.history), "Z.Ai");
    }

    if (PanelShows(AppSettings::WidgetSectionMcp) && snapshot.mcp.valid) {
        const float used = snapshot.mcp.limit > 0
            ? Math::get_instance()->PercentUsed(
                static_cast<double>(snapshot.mcp.used),
                static_cast<double>(snapshot.mcp.limit))
            : 0.0f;

        std::string right;
        if (snapshot.mcp.nextRefreshAtUnixSeconds > 0) {
            right = PanelReset(snapshot.mcp.nextRefreshAtUnixSeconds, std::string{});
        }

        std::string sub = std::to_string(snapshot.mcp.used) + " / " +
            std::to_string(snapshot.mcp.limit) + " calls";
        if (snapshot.mcp.remaining > 0) {
            sub += "  ·  " + std::to_string(snapshot.mcp.remaining) + " left";
        }
        if (!snapshot.mcp.level.empty()) {
            sub += "  ·  " + snapshot.mcp.level;
        }

        PanelBarSection("MCP Usage", snapshot.mcp.limit > 0, used, right, sub,
            true, PinKey("Z.Ai", "MCP"));
        PanelRule();
    }

    PanelActivitySection(snapshot.run, snapshot.context, "Z.Ai");
    PanelBreakdownSection(snapshot.context, "Z.Ai");
    PanelCacheSection(snapshot.context, "Z.Ai");
    PanelCompactionSection(snapshot.context, "Z.Ai");
    PanelModelSection(snapshot.context, "Z.Ai");
}

// What the CLI's /usage prints: prepaid credits with their auto top-up, then
// the legacy pay-as-you-go spend against its cap.
static std::vector<std::string> GrokCreditCardLines(const Grok::Snapshot& s)
{
    const Grok::ExtraCredits& credits = s.extraCredits;
    if (!credits.valid) return {};

    std::vector<std::string> lines;
    if (!credits.balanceText.empty()) {
        lines.push_back("Credits: " + credits.balanceText);

        const Grok::AutoTopUp& topUp = credits.autoTopUp;
        if (topUp.valid && !topUp.enabled) {
            lines.push_back("Auto top-up: off");
        }
        else if (topUp.valid) {
            std::string line = "Auto top-up: " + (topUp.amountText.empty() ? std::string("on") : topUp.amountText);
            if (!topUp.monthlyLimitText.empty()) line += " · max " + topUp.monthlyLimitText + " a month";
            lines.push_back(std::move(line));
        }
    }
    if (!credits.capText.empty()) {
        lines.push_back("Pay-as-you-go: " + credits.usedText + " of " + credits.capText);
    }
    return lines;
}

static void RenderGrokSections(const Grok::Snapshot& snapshot)
{
    // Grok publishes no per-turn run telemetry, so only compaction can be live.
    g_panelLiveState = LiveRunState(UsageTelemetry::RunUsage{}, snapshot.context);
    g_panelLiveModel = snapshot.context.model;
    PanelStatusLine(snapshot.access, snapshot.statusText);

    PanelPlanSection("Grok", PlanCardLines("Grok", snapshot.plan, snapshot.statusText));

    if (PanelShows(AppSettings::WidgetSectionQuota) && snapshot.weeklyLimit.valid) {
        PanelBarSection(snapshot.weeklyLimit.title.empty() ? "Weekly" : snapshot.weeklyLimit.title.c_str(),
            true, snapshot.weeklyLimit.usedPercent,
            PanelReset(snapshot.weeklyLimit.resetAtUnixSeconds, snapshot.weeklyLimit.resetText),
            std::string{}, true, PinKey("Grok", snapshot.weeklyLimit.title));
        PanelRule();
    }
    for (const Grok::ProductUsage& product : snapshot.products) {
        if (!PanelShows(AppSettings::WidgetSectionQuota)) break;
        PanelBarSection(product.product.empty() ? "Product" : product.product.c_str(),
            true, product.usagePercent, std::string{}, std::string{},
            true, PinKey("Grok", product.product));
        PanelRule();
    }
    if (PanelShows(AppSettings::WidgetSectionExtra)) {
        PanelInfoCard("Credits", GrokCreditCardLines(snapshot), "Grok");
        PanelInfoCard("Billing history", snapshot.history, "Grok");
    }

    PanelContextSection(snapshot.context, "Grok");
    PanelBreakdownSection(snapshot.context, "Grok");
    PanelCacheSection(snapshot.context, "Grok");
    PanelModelSection(snapshot.context, "Grok");
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

struct QuotaRuleRow
{
    const char* label;
    AppSettings::QuotaWarningRule& rule;
};

static void DrawCleanQuotaWarnings(std::initializer_list<QuotaRuleRow> rules)
{
    // Every rule this provider defines is always listed. A warning threshold is
    // a saved preference: gating the row on the window being present in the
    // current snapshot made it impossible to configure warnings for exactly the
    // account that is rate limited, signed out, or on a local fallback.
    if (ImGui::BeginTable("##provider_quota_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Quota", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("Threshold", ImGuiTableColumnFlags_WidthStretch, 0.52f);

        for (const QuotaRuleRow& row : rules) {
            DrawQuotaRuleSettings(row.label, row.rule);
        }

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

    ImGui::Spacing();
    DrawSettingsMutedText("Interface font size");

    if (R().uiFontSize) {
        int size = AppSettings::ClampUiFontSize(*R().uiFontSize);

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##ui_font_size", &size, 10, 22, "%d pt")) {
            *R().uiFontSize = AppSettings::ClampUiFontSize(size);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Rebuilding the atlas has to happen outside the frame, so flag it
            // and let the app loop do it between frames.
            g_fontReloadRequested = true;
            SaveAppSettings();
        }
    }

    DrawSettingsMutedText("Font used by the provider panels and the widget.");

    DrawSettingsMutedText("Pick the data cards under View.");
}

// ---- View tab: one checkbox per data card ----------------------------------
//
// Cards only render when their provider actually supplies the data, so a
// checked box with nothing behind it stays invisible rather than drawing an
// empty card.
struct ViewToggle { const char* label; int bit; const char* hint; };

static const ViewToggle kViewToggles[] = {
    { "Quota limits",      AppSettings::WidgetSectionQuota,
      "Session, weekly and per-model limit bars, with the weekly pace" },
    { "Context window",    AppSettings::WidgetSectionContext,
      "Tokens used against the resolved context limit" },
    { "Context breakdown", AppSettings::WidgetSectionBreakdown,
      "What is filling the window: messages, system prompt, tools" },
    { "Cache",             AppSettings::WidgetSectionCache,
      "Cache hit rate and read/write token totals" },
    { "Last compaction",   AppSettings::WidgetSectionCompaction,
      "Tokens freed by the most recent compaction" },
    { "Extra usage",       AppSettings::WidgetSectionExtra,
      "Credits, balances and overage spend" },
    { "Session cost",      AppSettings::WidgetSectionCost,
      "Spend and lines changed (needs the statusLine hook)" },
    { "Timing",            AppSettings::WidgetSectionTiming,
      "Session wall time and time spent inside the API" },
    { "Activity",          AppSettings::WidgetSectionActivity,
      "Live in/out/cache token counts for the running turn" },
    { "Session info",      AppSettings::WidgetSectionSession,
      "Output style, project, agent, PR, branch, vim mode" },
    { "Model",             AppSettings::WidgetSectionModel,
      "Model id and the resolved context limit" },
    { "MCP calls",         AppSettings::WidgetSectionMcp,
      "Z.Ai MCP call quota and refresh countdown" },
    { "Provider detail",   AppSettings::WidgetSectionDetails,
      "Reset credits, limit resets, usage history and other provider-specific rows" },
    { "Status messages",   AppSettings::WidgetSectionStatus,
      "Errors and access warnings above the cards" },
    { "Plan",              AppSettings::WidgetSectionPlan,
      "The subscription in force and which credential it came from" }
};

static void DrawViewSectionToggles(float contentWidth)
{
    if (!R().widgetSections) return;

    int mask = *R().widgetSections;
    bool changed = false;

    // Two columns once there is room; a single 14-row list is a lot of scroll.
    const int columns = contentWidth >= 520.0f ? 2 : 1;
    const int rows = (static_cast<int>(IM_ARRAYSIZE(kViewToggles)) + columns - 1) / columns;

    if (ImGui::BeginTable("##view_toggles", columns,
        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {

        for (int row = 0; row < rows; ++row) {
            ImGui::TableNextRow();
            for (int column = 0; column < columns; ++column) {
                const int index = column * rows + row;
                if (index >= IM_ARRAYSIZE(kViewToggles)) continue;

                const ViewToggle& toggle = kViewToggles[index];
                ImGui::TableSetColumnIndex(column);

                bool on = (mask & toggle.bit) != 0;
                ImGui::PushID(toggle.bit);
                if (ImGui::Checkbox(toggle.label, &on)) {
                    mask = on ? (mask | toggle.bit) : (mask & ~toggle.bit);
                    changed = true;
                }
                ImGui::PopID();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", toggle.hint);
                }
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    if (ImGui::Button("Show all")) {
        mask = AppSettings::kWidgetSectionAll;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide all")) {
        // Quota is the reason the widget exists; leave it on so "hide all"
        // cannot produce a panel with nothing in it.
        mask = AppSettings::WidgetSectionQuota;
        changed = true;
    }

    if (changed) {
        *R().widgetSections = mask;
        SaveAppSettings();
    }
}

// A miniature of the screen: its six parking spots, top and bottom thirds, as
// a 2x3 grid. Far quicker to read than a dropdown of position names.
static void DrawWidgetAnchorGrid()
{
    if (!R().widgetAnchor) return;

    const float cell = 38.0f;
    const float gap = 4.0f;
    const int rows = 2;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    int anchor = AppSettings::ClampWidgetAnchor(*R().widgetAnchor);
    bool changed = false;

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < 3; ++col) {
            const ImVec2 a(origin.x + col * (cell + gap), origin.y + row * (cell + gap));
            const ImVec2 b(a.x + cell, a.y + cell);

            const int value = AppSettings::WidgetAnchorFrom(row, col);
            const bool selected = value == anchor;

            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(row * 3 + col);
            const bool pressed = ImGui::InvisibleButton("##cell", ImVec2(cell, cell));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            dl->AddRectFilled(a, b,
                selected ? Color(40, 74, 122) : (hovered ? Color(44, 48, 60) : Color(31, 34, 43)), 5.0f);
            dl->AddRect(a, b,
                selected ? Color(96, 156, 240) : Color(54, 58, 72), 5.0f, 0, 1.0f);

            // A slab against the edge this spot rolls into, so the cell shows
            // the retract direction and not just the position.
            const float inset = 7.0f;
            ImVec2 s0(a.x + inset, a.y + inset);
            ImVec2 s1(b.x - inset, b.y - inset);
            if (row == 0) s1.y = a.y + inset + 6.0f;
            else s0.y = b.y - inset - 6.0f;

            dl->AddRectFilled(s0, s1,
                selected ? Color(150, 195, 255) : Color(96, 104, 124), 2.0f);

            if (pressed) { anchor = value; changed = true; }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (cell + gap)));
    ImGui::Dummy(ImVec2(3 * cell + 2 * gap, 0.0f));

    if (changed) {
        *R().widgetAnchor = anchor;
        SaveAppSettings();
        if (R().repositionWidget) R().repositionWidget();
    }
}

static void DrawWidgetPlacementSettings(float contentWidth)
{
    DrawSettingsMutedText("Monitor");

    if (R().widgetMonitor && R().widgetMonitorNames && R().widgetMonitorCount > 0) {
        int monitor = std::clamp(*R().widgetMonitor, 0, R().widgetMonitorCount - 1);
        ImGui::SetNextItemWidth(std::min(320.0f, contentWidth));
        if (ImGui::Combo("##widget_monitor", &monitor,
                R().widgetMonitorNames, R().widgetMonitorCount)) {
            *R().widgetMonitor = monitor;
            SaveAppSettings();
            if (R().repositionWidget) R().repositionWidget();
        }
    }
    else {
        DrawSettingsMutedText("No monitors enumerated");
    }

    ImGui::Spacing();

    if (R().widgetAlwaysOnTop) {
        if (ImGui::Checkbox("Always on top", R().widgetAlwaysOnTop)) {
            SaveAppSettings();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", *R().widgetAlwaysOnTop
                ? "The widget floats above other windows."
                : "The widget sits in the normal z-order and can be covered.");
        }
    }

    ImGui::Spacing();
    DrawSettingsMutedText("Parking spot");
    DrawWidgetAnchorGrid();
    DrawSettingsMutedText("Snapping and rolling away are relative to this monitor only.");
}

// One row of the collapsed bar: a quota window or an info card from some host.
struct WidgetPeek
{
    bool valid = false;
    std::string host;
    std::string window;
    float usedPercent = 0.0f;
    long long resetAtUnixSeconds = 0;
    std::string resetText;
    // The host's plan, brand prefix already trimmed ("Max 5x").
    std::string plan;
    // Right-hand text for rows that do not reset on a clock (the context
    // meter's token counts, say). Wins over the reset time when set.
    std::string detail;
    bool resets = true;
    // Info rows (balances, plan, reset credits...) have no percentage: they
    // show valueText where the percent goes and draw no bar.
    bool hasPercent = true;
    std::string valueText;
    // Whether the row can stand for its host when nothing is pinned, and on
    // its tab's underline: the quota windows can, the context meter, credit
    // balances and info rows cannot.
    bool primary = true;
    // "Thinking..." / "Compacting..." while that host is mid-turn; empty when
    // idle, in which case the reset countdown shows instead.
    LiveState liveState;
    // "<provider>:<window>", stable across restarts.
    std::string key;
    // Previous versions used the visible Z.Ai label as the key. Keep it as an
    // alias so an existing settings.ini pin resolves after the identity-based
    // drag/drop key fix.
    std::string legacyKey;
};

static const std::vector<WidgetPeek>& WidgetPeekCandidates();
static const WidgetPeek* ResolveKey(const std::vector<WidgetPeek>& candidates, const std::string& key);
static std::vector<std::string> WidgetBarKeys();
static void SetWidgetBarKeys(const std::vector<std::string>& keys);

static std::string WidgetPeekLabel(const WidgetPeek& peek)
{
    return peek.window.empty() ? peek.host : peek.host + "  " + peek.window;
}

// The collapsed bar's contents: tick the windows you want, drag to reorder.
// Candidates come from whatever the hosts are actually reporting right now, so
// a signed-out host simply has no rows to offer.
static void DrawWidgetBarRowPicker(float contentWidth)
{
    if (!R().widgetBarRows) return;

    const std::vector<WidgetPeek>& candidates = WidgetPeekCandidates();

    // The current selection, in order. Keys are kept even when their host is
    // not reporting right now - a host that has not refreshed yet, or is signed
    // out, must not silently delete a pick and then save the deletion.
    std::vector<std::string> chosen = WidgetBarKeys();

    if (candidates.empty() && chosen.empty()) {
        DrawSettingsMutedText("No quota windows reported yet - refresh a host first.");
        return;
    }

    // What each pick resolves to, the same way the bar resolves it.
    std::vector<const WidgetPeek*> picked;
    for (const std::string& key : chosen) {
        picked.push_back(ResolveKey(candidates, key));
    }

    bool changed = false;

    // Picked rows first, in order, each draggable onto another to reorder.
    // Edits apply after the loop, so the rows stay paired with `picked`.
    int removeAt = -1;
    int moveFrom = -1;
    int moveTo = -1;

    for (size_t i = 0; i < chosen.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        bool on = true;
        if (ImGui::Checkbox("##pick", &on)) {
            removeAt = static_cast<int>(i);
        }

        ImGui::SameLine();

        std::string label;
        if (picked[i]) {
            label = WidgetPeekLabel(*picked[i]);
        }
        else {
            label = chosen[i];
            const size_t colon = label.find(':');
            if (colon != std::string::npos) label[colon] = ' ';
            label += "   (not reporting)";
        }
        ImGui::Selectable(("≡  " + label).c_str(), false, 0, ImVec2(contentWidth * 0.6f, 0.0f));

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            const int from = static_cast<int>(i);
            ImGui::SetDragDropPayload("AQC_BAR_ROW", &from, sizeof(from));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AQC_BAR_ROW")) {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = static_cast<int>(i);
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
    }

    if (removeAt >= 0) {
        chosen.erase(chosen.begin() + removeAt);
        changed = true;
    }
    else if (moveFrom >= 0 && moveFrom < static_cast<int>(chosen.size()) && moveFrom != moveTo) {
        const std::string moved = chosen[static_cast<size_t>(moveFrom)];
        chosen.erase(chosen.begin() + moveFrom);
        chosen.insert(chosen.begin() + moveTo, moved);
        changed = true;
    }

    if (!chosen.empty()) {
        ImGui::Spacing();
        DrawSettingsMutedText("Not shown");
    }

    // Everything else, unticked.
    for (const WidgetPeek& candidate : candidates) {
        if (std::find(picked.begin(), picked.end(), &candidate) != picked.end()) continue;

        ImGui::PushID(candidate.key.c_str());
        bool on = false;
        if (ImGui::Checkbox(WidgetPeekLabel(candidate).c_str(), &on)) {
            chosen.push_back(candidate.key);
            changed = true;
        }
        ImGui::PopID();
    }

    if (candidates.empty()) {
        DrawSettingsMutedText("Refresh a host to see its other windows here.");
    }

    ImGui::Spacing();
    if (ImGui::Button("Follow selected host")) {
        // Empty means "whatever tab is open", the original behaviour.
        chosen.clear();
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear the picks and let the bar track the open host tab");
    }

    if (changed) {
        SetWidgetBarKeys(chosen);
    }
}

static void DrawCleanViewSettings(float contentWidth, float contentHeight)
{
    BeginCleanSettingsCard("##settings_view", "View",
        ImVec2(contentWidth, std::max(340.0f, contentHeight)));

    DrawSettingsMutedText("Data cards shown in the widget and the provider tabs");
    ImGui::Spacing();

    DrawViewSectionToggles(contentWidth);

    ImGui::Spacing();
    DrawSettingsMutedText("The widget resizes itself to whatever is turned on here.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    DrawWidgetPlacementSettings(contentWidth);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    DrawSettingsMutedText("Rolled-up bar");
    DrawWidgetBarRowPicker(contentWidth);

    EndCleanSettingsCard();
}

// The inside-window flag and the position take effect through
// ApplySettingsToRuntime, which DrawSettingsTab runs every frame: it clamps
// the index and hands the position to every notifier.
static void DrawDisplaySettings()
{
    DrawSettingsMutedText("Display");
    ImGui::Checkbox("Show remaining", &g_showRemaining);
    ImGui::Checkbox("Show reset date details", &g_showResetDateDetails);
    ImGui::Spacing();
    DrawSettingsMutedText("Reset time mode");
    DrawResetTimeModeSettings();
    ImGui::Checkbox("Notifications inside window", &g_showNotificationsInsideWindow);
}

static void DrawNotificationPositionSettings()
{
    DrawSettingsMutedText("Notification position");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("##notification_position", &g_notifyPositionIndex, g_notifyPositionNames, R().notifyPositionCount);
    DrawSettingsMutedText(g_showNotificationsInsideWindow ? "Inside the app window" : "Outside the app window");
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
        DrawDisplaySettings();

        ImGui::TableSetColumnIndex(1);
        DrawSettingsMutedText("Auto refresh");
        DrawAutoRefreshSettings();

        ImGui::TableSetColumnIndex(2);
        DrawNotificationPositionSettings();

        ImGui::TableSetColumnIndex(3);
        DrawSettingsMutedText("Settings file");
        DrawSettingsSaveButtonInline();
        DrawSettingsMutedText("settings.ini in exe folder");

        ImGui::EndTable();
    }
    else if (ImGui::BeginTable("##settings_general_table", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        DrawDisplaySettings();

        ImGui::Spacing();
        DrawSettingsMutedText("Auto refresh");
        DrawAutoRefreshSettings();

        ImGui::TableSetColumnIndex(1);
        DrawNotificationPositionSettings();

        ImGui::Spacing();
        DrawSettingsSaveButtonInline();

        ImGui::EndTable();
    }

    EndCleanSettingsCard();
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

// Auto refresh, notifications and quota warnings: the part of every provider
// page that differs only in which settings it edits.
static void DrawProviderAlertSettings(const char* name, bool& autoRefresh,
    AppSettings::ProviderNotifications& notify, bool resetCredits,
    std::initializer_list<QuotaRuleRow> rules)
{
    DrawSettingsSubHeader("Auto refresh");
    DrawProviderAutoRefreshSetting(("Refresh " + std::string(name) + " automatically").c_str(), autoRefresh);

    ImGui::Spacing();
    DrawSettingsSubHeader("Notifications");
    DrawCleanProviderNotifications(notify, resetCredits);

    ImGui::Spacing();
    DrawSettingsSubHeader("Quota warnings");
    DrawCleanQuotaWarnings(rules);
}

// Z.Ai and Grok have no account picker, so their page is just that.
static void DrawProviderSettingsCard(const char* name, bool& autoRefresh,
    AppSettings::ProviderNotifications& notify, std::initializer_list<QuotaRuleRow> rules,
    float width, float height)
{
    ImGui::PushID(name);
    BeginCleanSettingsCard("##provider_settings_card", name, ImVec2(width, height));
    DrawProviderAlertSettings(name, autoRefresh, notify, false, rules);
    EndCleanSettingsCard();
    ImGui::PopID();
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
    DrawProviderAlertSettings("Codex", g_codexAutoRefreshEnabled, g_codexNotifySettings, true, {
        { "5-hour limit", g_codexQuotaWarnings.fiveHour },
        { "Weekly - all models", g_codexQuotaWarnings.weekly }
    });

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
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        SaveAppSettings();
    }
    DrawSettingsMutedText("How fast the live status text pulses. Default: 100%.");

    ImGui::Spacing();
    DrawProviderAlertSettings("Claude", g_claudeAutoRefreshEnabled, g_claudeNotifySettings, false, {
        { "Current session", g_claudeQuotaWarnings.currentSession },
        { "All models", g_claudeQuotaWarnings.allModels },
        { "Sonnet", g_claudeQuotaWarnings.sonnet },
        { "Fable", g_claudeQuotaWarnings.fable },
        { "Usage credits", g_claudeQuotaWarnings.credits }
    });

    EndCleanSettingsCard();
    ImGui::PopID();
}

enum class ActiveSettingsTab
{
    General,
    View,
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
    case ActiveSettingsTab::View:
        DrawCleanViewSettings(width, height);
        break;
    case ActiveSettingsTab::Codex:
        DrawCodexSettingsCard(width, height);
        break;
    case ActiveSettingsTab::Claude:
        DrawClaudeSettingsCard(width, height);
        break;
    case ActiveSettingsTab::Grok:
        DrawProviderSettingsCard("Grok", g_grokAutoRefreshEnabled, g_grokNotifySettings, {
            { "Weekly limit", g_grokQuotaWarnings.weekly }
        }, width, height);
        break;
    case ActiveSettingsTab::ZAi:
        // One rule for every Coding Plan window (GLM-5.3, GLM-5.3-Flash...);
        // the stored setting keeps its old glm52 name.
        DrawProviderSettingsCard("Z.Ai", g_zaiAutoRefreshEnabled, g_zaiNotifySettings, {
            { "Coding Plan / GLM", g_zaiQuotaWarnings.glm52 },
            { "GLM-5-Turbo", g_zaiQuotaWarnings.turbo }
        }, width, height);
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
    DrawSettingsSideTab("View", ActiveSettingsTab::View);
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

// The host the widget shows: picked from its icon tabs, and what the bar
// follows when nothing is pinned.
enum class HostTab
{
    Codex,
    Claude,
    ZAi,
    Grok
};

static HostTab g_widgetTab = HostTab::Claude;

static const char* HostName(HostTab tab)
{
    switch (tab) {
    case HostTab::Codex: return "Codex";
    case HostTab::Claude: return "Claude";
    case HostTab::ZAi: return "Z.Ai";
    default: return "Grok";
    }
}

// Measured every widget frame; the app loop resizes the OS window to match so
// the panel is exactly as tall as the cards that are actually turned on.
static float g_widgetContentHeight = 0.0f;
// Height of the bottom summary strip, including the panel padding below it.
// This is how much of the window stays on screen while retracted.
static float g_widgetPeekHeight = 0.0f;
// Widest bar row measured this frame, untrimmed. The window widens to this so
// a long model name is shown rather than ellipsised.
static float g_widgetContentWidth = 0.0f;
static float g_widgetContentWidthAccum = 0.0f;

// ---- retracted peek strip -------------------------------------------------
//
// When the widget rolls up, the only thing left on screen is the bottom few
// pixels. Those used to be whatever the footer menu happened to end on - a
// half-cut "Quit" - which told you nothing. This strip is drawn last so it IS
// that remaining band: the selected host's session quota and reset, readable
// without pulling the panel back down.

// A percentage row: a quota window, or anything else read as a fill level.
static WidgetPeek BarPeek(const std::string& host, const std::string& window, bool valid,
    float usedPercent, long long resetAtUnixSeconds, const std::string& resetText,
    const std::string& plan)
{
    WidgetPeek peek;
    peek.valid = valid;
    peek.host = host;
    peek.window = window;
    peek.plan = plan;
    peek.usedPercent = usedPercent;
    peek.resetAtUnixSeconds = resetAtUnixSeconds;
    peek.resetText = resetText;
    return peek;
}

// The context meter as a pinnable row. It has a percentage and a bar like any
// quota window, but nothing resets on a clock, so the right-hand slot carries
// the token counts instead of a countdown.
static WidgetPeek ContextPeek(const std::string& host, const std::string& plan,
    const UsageTelemetry::ContextUsage& context)
{
    WidgetPeek peek = BarPeek(host, "Context", context.valid && context.contextWindowTokens > 0,
        Math::get_instance()->PercentUsed(
            static_cast<double>(context.usedTokens),
            static_cast<double>(context.contextWindowTokens)),
        0, std::string{}, plan);
    peek.resets = false;
    peek.primary = false;
    peek.detail = FormatCompactTokenCount(context.usedTokens) + " / " +
        FormatCompactTokenCount(context.contextWindowTokens);
    return peek;
}

// Every row the bar can show across every host, as stable "<provider>:<window>"
// keys: each quota window and every info card the panel draws. The key has to
// survive a restart - hence the window title rather than an index into a list
// that reorders.
static std::vector<WidgetPeek> BuildWidgetPeekCandidates()
{
    std::vector<WidgetPeek> out;
    const long long now = static_cast<long long>(std::time(nullptr));

    LiveState liveState;
    std::string liveModel;

    // Keys default to "<host>:<window>". Providers can report the same window
    // twice (a server-driven limit plus its legacy named field); first wins.
    const auto push = [&out, &liveState, &liveModel](WidgetPeek peek) {
        if (!peek.valid || (peek.host.empty() && peek.window.empty())) return;
        if (peek.key.empty()) peek.key = PinKey(peek.host, peek.window);

        // A renamed Z.Ai row carries its window in the host ("ZAi - Session").
        if (peek.resets && WindowTakesLiveState(peek.window.empty() ? peek.host : peek.window, liveModel)) {
            peek.liveState = liveState;
        }

        for (const WidgetPeek& existing : out) {
            if (existing.key == peek.key) return;
        }
        out.push_back(std::move(peek));
    };

    // Info cards, keyed like PanelInfoCard: the first line is the value and
    // the second the right-hand detail.
    const auto addInfo = [&push](const std::string& host, const std::string& title,
        const std::vector<std::string>& lines, const std::string& plan) {
        if (lines.empty() || lines.front().empty() || title.empty()) return;

        WidgetPeek peek;
        peek.valid = true;
        peek.hasPercent = false;
        peek.resets = false;
        peek.primary = false;
        peek.host = host;
        peek.window = title;
        peek.plan = plan;
        peek.valueText = lines.front();
        if (lines.size() > 1) peek.detail = lines[1];
        push(std::move(peek));
    };

    // The cards every host can show.
    const auto addCommonInfo = [&addInfo](const char* host, const std::string& plan,
        const std::vector<std::string>& planLines,
        const UsageTelemetry::RunUsage& run, const UsageTelemetry::ContextUsage& context) {
        addInfo(host, "Plan", planLines, plan);
        addInfo(host, "Activity", ActivityCardLines(run, context), plan);
        addInfo(host, "Session info", run.sessionDetails, plan);
        addInfo(host, "Timing", TimingCardLines(run), plan);
        addInfo(host, "Context breakdown", BreakdownCardLines(context), plan);
        addInfo(host, "Cache", CacheCardLines(context), plan);
        addInfo(host, "Last compaction", CompactionCardLines(context), plan);
        addInfo(host, "Model", ModelCardLines(context), plan);
    };

    {
        std::lock_guard<std::mutex> lock(g_codexMutex);
        const Codex::Snapshot& s = g_codexState;
        const std::string plan = PlanBeside("Codex", s.plan);
        liveState = LiveRunState(s.run, s.context);
        liveModel = s.context.model;

        for (const Codex::UsageBar& b : s.bars) {
            push(BarPeek("Codex", CodexBarName(b), b.valid, b.usedPercent,
                b.resetAtUnixSeconds, std::string{}, plan));
        }
        push(ContextPeek("Codex", plan, s.context));

        if (s.extraUsage.valid && s.extraUsage.hasUsedPercent) {
            WidgetPeek extra = BarPeek("Codex", CodexExtraUsageTitle(s), true, s.extraUsage.usedPercent,
                s.extraUsage.resetAtUnixSeconds, s.extraUsage.resetText, plan);
            extra.primary = false;
            push(std::move(extra));
        }
        else if (s.extraUsage.valid) {
            // The bar row shows "(warning only)" as its muted detail.
            addInfo("Codex", CodexExtraUsageTitle(s), CodexExtraUsageCardLines(s), plan);
        }
        addInfo("Codex", "Accounts", CodexAccountsCardLines(s), plan);
        addInfo("Codex", "Usage source", CodexUsageSourceCardLines(s), plan);
        addInfo("Codex", "Credits", CodexCreditCardLines(s), plan);
        addInfo("Codex", "Workspace credits", { s.workspaceCredits }, plan);
        addInfo("Codex", "Auto top-up", CodexAutoTopUpCardLines(s.autoTopUp), plan);
        addInfo("Codex", "Credit usage", CodexCreditUsageCardLines(s), plan);
        addInfo("Codex", "Reset credits", CodexResetCreditCardLines(s), plan);
        addInfo("Codex", "Reset history", CodexResetHistoryCardLines(s), plan);
        addInfo("Codex", "Plan limit history", CodexPlanLimitHistoryCardLines(s), plan);
        addInfo("Codex", "Usage by surface", CodexSurfaceShareCardLines(s), plan);
        addInfo("Codex", "Daily usage", CodexDailyUsageCardLines(s), plan);
        addInfo("Codex", "Profile", CodexProfileCardLines(s.profile), plan);
        addCommonInfo("Codex", plan, CodexPlanCardLines(s), s.run, s.context);
    }
    {
        std::lock_guard<std::mutex> lock(g_claudeMutex);
        const Claude::Snapshot& s = g_claudeState;
        const std::string plan = PlanBeside("Claude", s.plan);
        liveState = LiveRunState(s.run, s.context);
        liveModel = s.context.model;

        // An invalid window adds no row: Ultra's null session window included.
        for (const Claude::UsageWindow* w : { &s.currentSession, &s.weeklyAllModels, &s.weeklySonnet, &s.weeklyFable }) {
            push(BarPeek("Claude", w->title, w->valid, w->usedPercent,
                w->resetAtUnixSeconds, w->resetText, plan));
        }
        for (const Claude::UsageWindow& w : s.additionalLimits) {
            push(BarPeek("Claude", w.title, w.valid, w.usedPercent,
                w.resetAtUnixSeconds, w.resetText, plan));
        }
        addInfo("Claude", "Pace", { Claude::PaceText(ClaudeWeeklyPace(s, now), now) }, plan);
        addInfo("Claude", "This week by surface", ClaudeBreakdownCardLines(s), plan);
        addInfo("Claude", "Limit resets", ClaudeLimitResetCardLines(s.limitResets, now), plan);

        for (const Claude::PromoCredit& credit : s.promoCredits) {
            WidgetPeek peek = BarPeek("Claude", credit.title, true, credit.usedPercent, 0, std::string{}, plan);
            peek.key = PromoCreditKey(credit);
            peek.detail = PromoCreditLeft(credit);
            peek.resets = false;
            peek.primary = false;
            push(std::move(peek));
        }
        push(ContextPeek("Claude", plan, s.context));

        if (s.credits.valid && s.credits.hasUsedPercent) {
            WidgetPeek credits = BarPeek("Claude", ClaudeCreditsTitle(s), true, s.credits.usedPercent,
                s.credits.resetAtUnixSeconds, s.credits.resetText, plan);
            credits.primary = false;
            push(std::move(credits));
        }
        else if (s.credits.valid) {
            // The bar row shows the disabled reason as its muted detail.
            addInfo("Claude", ClaudeCreditsTitle(s), ClaudeCreditsCardLines(s), plan);
        }
        addInfo("Claude", "Cost", ClaudeCostCardLines(s.run), plan);
        addInfo("Claude", "Auto-resume", ClaudeAutoResumeCardLines(s.autoResume), plan);
        addInfo("Claude", ClaudeLedgerTitle(s.ledger), ClaudeLedgerCardLines(s.ledger), plan);
        addCommonInfo("Claude", plan, ClaudePlanCardLines(s), s.run, s.context);
    }
    {
        std::lock_guard<std::mutex> lock(g_zaiMutex);
        const ZAi::Snapshot& s = g_zaiState;
        const std::string plan = PlanBeside("Z.Ai", s.plan);
        liveState = LiveRunState(s.run, s.context);
        liveModel = s.context.model;

        for (const ZAi::UsageBar& b : s.bars) {
            const std::string rawLabel = !b.label.empty() ? b.label : b.sublabel;
            const std::string displayLabel = ZAiWidgetDisplayLabel(b);
            const bool renamed = displayLabel == "ZAi - Session" || displayLabel == "ZAi - Weekly";

            WidgetPeek peek = BarPeek(renamed ? displayLabel : "Z.Ai", renamed ? std::string{} : rawLabel,
                b.valid, b.usedPercent, b.resetAtUnixSeconds, b.resetText,
                renamed ? std::string{} : plan);
            peek.key = ZAiWidgetKey(b, rawLabel);
            peek.legacyKey = "Z.Ai:" + rawLabel;
            // A spend balance is money, not the plan's quota window.
            peek.primary = !b.spendBalance;
            push(std::move(peek));
        }
        if (s.mcp.valid && s.mcp.limit > 0) {
            WidgetPeek mcp = BarPeek("MCP Usage", std::string{}, true,
                Math::get_instance()->PercentUsed(
                    static_cast<double>(s.mcp.used), static_cast<double>(s.mcp.limit)),
                s.mcp.nextRefreshAtUnixSeconds, std::string{}, std::string{});
            mcp.key = PinKey("Z.Ai", "MCP");
            push(std::move(mcp));
        }
        push(ContextPeek("Z.Ai", plan, s.context));

        for (const ZAi::DetailRow& detail : s.details) {
            addInfo("Z.Ai", detail.leftLabel.empty() ? "Details" : detail.leftLabel,
                ZAiDetailCardLines(detail), plan);
        }
        addInfo("Z.Ai", "Quota resets", ZAiResetCardLines(s), plan);
        addInfo("Z.Ai", "Usage history", ZAiHistoryCardLines(s.history), plan);
        addCommonInfo("Z.Ai", plan, PlanCardLines("Z.Ai", s.plan, s.statusText), s.run, s.context);
    }
    {
        std::lock_guard<std::mutex> lock(g_grokMutex);
        const Grok::Snapshot& s = g_grokState;
        const std::string plan = PlanBeside("Grok", s.plan);
        liveState = LiveRunState(UsageTelemetry::RunUsage{}, s.context);
        liveModel = s.context.model;

        push(BarPeek("Grok", s.weeklyLimit.title, s.weeklyLimit.valid, s.weeklyLimit.usedPercent,
            s.weeklyLimit.resetAtUnixSeconds, s.weeklyLimit.resetText, plan));
        for (const Grok::ProductUsage& p : s.products) {
            push(BarPeek("Grok", p.product, true, p.usagePercent, 0, std::string{}, plan));
        }
        push(ContextPeek("Grok", plan, s.context));

        addInfo("Grok", "Credits", GrokCreditCardLines(s), plan);
        addInfo("Grok", "Billing history", s.history, plan);
        addCommonInfo("Grok", plan, PlanCardLines("Grok", s.plan, s.statusText), UsageTelemetry::RunUsage{}, s.context);
    }

    return out;
}

// Built once per frame of the current ImGui context: the bar, its height, the
// host tabs and the Settings picker all ask, and every build locks all four
// provider mutexes. The widget and Settings windows are separate contexts
// with their own frame counters.
static const std::vector<WidgetPeek>& WidgetPeekCandidates()
{
    static ImGuiContext* cachedContext = nullptr;
    static int cachedFrame = -1;
    static std::vector<WidgetPeek> cached;

    if (cachedContext != ImGui::GetCurrentContext() || cachedFrame != ImGui::GetFrameCount()) {
        cachedContext = ImGui::GetCurrentContext();
        cachedFrame = ImGui::GetFrameCount();
        cached = BuildWidgetPeekCandidates();
    }

    return cached;
}

// Punctuation- and case-insensitive form of a pin key, used only as a fallback
// when an exact match fails.
static std::string CompactKey(const std::string& key)
{
    const size_t colon = key.find(':');
    if (colon == std::string::npos) return CompactName(key);
    return CompactName(key.substr(0, colon)) + CompactName(key.substr(colon + 1));
}

// The candidate a stored pin key stands for: an exact key or legacy alias,
// else one that differs only in punctuation and case. Keys embed the window's
// display name, so relabelling a window would otherwise orphan every pin that
// referenced it - fixing "GPT 5 3" to "GPT 5.3" silently broke saved rows.
// The bar, the Settings picker and the drop handler all resolve through here,
// so they cannot disagree about which row a key means.
static const WidgetPeek* ResolveKey(const std::vector<WidgetPeek>& candidates, const std::string& key)
{
    for (const WidgetPeek& candidate : candidates) {
        if (candidate.key == key || (!candidate.legacyKey.empty() && candidate.legacyKey == key)) {
            return &candidate;
        }
    }

    const std::string wanted = CompactKey(key);
    for (const WidgetPeek& candidate : candidates) {
        if (CompactKey(candidate.key) == wanted ||
            (!candidate.legacyKey.empty() && CompactKey(candidate.legacyKey) == wanted)) {
            return &candidate;
        }
    }

    return nullptr;
}

// The stored pick list, unresolved. Kept separate from WidgetPeekRows because
// reordering has to move the keys, not the rows they happen to resolve to.
static std::vector<std::string> WidgetBarKeys()
{
    std::vector<std::string> keys;
    if (!R().widgetBarRows) return keys;

    std::stringstream stream(*R().widgetBarRows);
    std::string key;
    while (std::getline(stream, key, ',')) {
        if (key.empty()) continue;
        // Old comma-containing Z.Ai identities were split into fragments.
        // Valid persisted widget keys always contain a provider separator;
        // discard the orphan numeric/JSON fragments during migration.
        if (key.find(':') == std::string::npos) continue;
        if (key.rfind("Z.Ai:coding:[", 0) == 0 && key.find(']') == std::string::npos) continue;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    return keys;
}

static void SetWidgetBarKeys(const std::vector<std::string>& keys)
{
    if (!R().widgetBarRows) return;

    std::string joined;
    for (const std::string& key : keys) {
        if (!joined.empty()) joined.push_back(',');
        joined += key;
    }

    *R().widgetBarRows = joined;
    SaveAppSettings();
}

// The row that stands for a host: its first quota window, in the order the
// panel lists them. A context meter or a balance never does.
static const WidgetPeek* PrimaryCandidate(const std::vector<WidgetPeek>& candidates, HostTab tab)
{
    // Every key of the host starts "<host>:", the renamed Z.Ai rows included.
    const std::string prefix = std::string(HostName(tab)) + ":";

    for (const WidgetPeek& candidate : candidates) {
        if (candidate.primary && candidate.key.rfind(prefix, 0) == 0) return &candidate;
    }
    return nullptr;
}

// Nothing pinned: the bar follows the widget's open host tab, reading "No
// data" until that host reports a window.
static WidgetPeek WidgetPeekSummary()
{
    if (const WidgetPeek* primary = PrimaryCandidate(WidgetPeekCandidates(), g_widgetTab)) {
        return *primary;
    }

    WidgetPeek peek;
    peek.host = HostName(g_widgetTab);
    return peek;
}

// The rows the collapsed bar should show: the user's picks in their order,
// falling back to the selected host's primary window when nothing is picked.
//
// Cached for the frame: the height helpers ask for this several times per
// frame.
static std::vector<WidgetPeek> WidgetPeekRowsUncached()
{
    std::vector<WidgetPeek> rows;

    const std::vector<std::string> storedKeys = WidgetBarKeys();

    if (!storedKeys.empty()) {
        const std::vector<WidgetPeek>& all = WidgetPeekCandidates();

        for (const std::string& key : storedKeys) {
            if (const WidgetPeek* found = ResolveKey(all, key)) {
                rows.push_back(*found);
                continue;
            }

            // Hold the slot. A host that has not answered yet resolves nothing,
            // and dropping the row would shift everything below it up - the
            // order visibly rearranging itself as each host reports in. It also
            // keeps rows 1:1 with the stored keys, which the reorder and unpin
            // handlers index by position.
            WidgetPeek placeholder;
            placeholder.valid = false;
            placeholder.key = key;

            const size_t colon = key.find(':');
            placeholder.host = colon == std::string::npos ? key : key.substr(0, colon);
            placeholder.window = colon == std::string::npos
                ? std::string{} : key.substr(colon + 1);

            rows.push_back(std::move(placeholder));
        }
    }

    // Nothing picked at all: follow whichever host tab is open.
    if (rows.empty()) {
        rows.push_back(WidgetPeekSummary());
    }

    return rows;
}

static const std::vector<WidgetPeek>& WidgetPeekRows()
{
    static int cachedFrame = -1;
    static std::vector<WidgetPeek> cached;

    const int frame = ImGui::GetFrameCount();
    if (frame != cachedFrame) {
        cachedFrame = frame;
        cached = WidgetPeekRowsUncached();
    }

    return cached;
}

// Height of the strip for the current font. The app reads this to decide how
// much of the window to leave on screen when retracted, so it has to be
// knowable without drawing.
static float WidgetPeekStripPad() { return 6.0f; }
static float WidgetPeekBarHeight() { return 5.0f; }

static float WidgetPeekRowHeight()
{
    return WidgetPeekStripPad() + ImGui::GetTextLineHeight() + 5.0f
        + WidgetPeekBarHeight() + WidgetPeekStripPad();
}

// The bar is as tall as its rows: the picks, or the one followed window.
static float WidgetPeekStripHeight()
{
    return WidgetPeekRowHeight() * static_cast<float>(WidgetPeekRows().size());
}

// `text` cut to `maxWidth` on a whole UTF-8 code point and ended with an
// ellipsis; unchanged when it fits, empty when not even the ellipsis does.
static std::string Ellipsize(std::string text, float maxWidth)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;

    const std::string ellipsis = "\xe2\x80\xa6";
    while (!text.empty() && ImGui::CalcTextSize((text + ellipsis).c_str()).x > maxWidth) {
        // The continuation bytes first, then the lead byte they belong to.
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
            text.pop_back();
        }
        if (!text.empty()) text.pop_back();
    }
    return text.empty() ? text : text + ellipsis;
}

static void DrawWidgetPeekRow(const WidgetPeek& peek, float width)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float pad = WidgetPeekStripPad();
    const float lineH = ImGui::GetTextLineHeight();
    const float barH = WidgetPeekBarHeight();
    const float height = WidgetPeekRowHeight();

    const float x = c0.x + 13.0f;
    const float innerW = width - 26.0f;
    const float labelX = x + 13.0f;
    float y = c0.y + pad;

    const ImU32 severity = peek.valid && peek.hasPercent ? PanelSeverity(peek.usedPercent)
        : (peek.valid ? Color(110, 150, 220) : Color(70, 76, 92));

    // Row: severity dot, host + window, then the reset, then the headline
    // number hard against the right edge.
    dl->AddCircleFilled(ImVec2(x + 3.5f, y + lineH * 0.5f), 3.5f, severity);

    const std::string value = !peek.valid ? "No data"
        : !peek.valueText.empty() ? peek.valueText
        : UsageText(peek.usedPercent);

    // Reset without the date suffix: 360px is not enough for both, and the
    // relative time is the part worth reading at a glance.
    std::string right = peek.detail;

    if (right.empty() && peek.resetAtUnixSeconds > 0) {
        right = ResetTime::get_instance()->Format(peek.resetAtUnixSeconds, g_resetDisplayMode, false);
    }
    if (right.empty()) right = peek.resetText;
    if (right.empty() && peek.valid && peek.resets) {
        right = peek.usedPercent <= 0.0f ? "Not started yet" : "Reset not reported";
    }

    const bool showLive = !peek.liveState.empty();
    float rightW = showLive
        ? MeasureLiveState(peek.liveState)
        : (right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x);

    // "Codex (Pro 20x)  ·  Weekly", degrading by what matters least first.
    const std::string sep = "  ·  ";
    const std::string planned = peek.plan.empty()
        ? peek.host : peek.host + " (" + peek.plan + ")";

    // What this row would need to show everything untrimmed. Accumulated across
    // rows so the window can widen to the longest one.
    {
        const std::string widest = peek.window.empty()
            ? planned : planned + sep + peek.window;

        const float needed = 13.0f + 13.0f + ImGui::CalcTextSize(widest.c_str()).x
            + 10.0f + rightW + 12.0f + ImGui::CalcTextSize(value.c_str()).x + 13.0f;

        g_widgetContentWidthAccum = (std::max)(g_widgetContentWidthAccum, needed);
    }

    // When the window cannot widen that far: the value is the headline and
    // clips only when it alone overflows the row (a long info line). The
    // reset comes next. The label yields first: a percentage without a reset
    // time is half an answer, whereas the window name is recoverable from the
    // panel.
    const std::string shownValue = Ellipsize(value, x + innerW - labelX);
    const float valueX = x + innerW - ImGui::CalcTextSize(shownValue.c_str()).x;
    dl->AddText(ImVec2(valueX, y), Color(228, 234, 246), shownValue.c_str());

    if (showLive) {
        DrawLiveState(ImVec2(valueX - 12.0f - rightW, y), peek.liveState);
    }
    else if (!right.empty()) {
        right = Ellipsize(right, valueX - 12.0f - labelX);
        rightW = right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(valueX - 12.0f - rightW, y), Color(132, 140, 160), right.c_str());
    }

    const float labelRoom = (valueX - 12.0f - rightW) - labelX - 10.0f;
    const auto fits = [labelRoom](const std::string& text) {
        return ImGui::CalcTextSize(text.c_str()).x <= labelRoom;
    };

    // The window name is what tells two rows of the same host apart, so it is
    // the LAST thing to go - dropping it first left two Codex rows both
    // reading just "Codex". Order of sacrifice: plan, then host, then finally
    // truncate the window name itself.
    std::string label;
    if (peek.window.empty()) label = planned;
    else if (fits(planned + sep + peek.window)) label = planned + sep + peek.window;
    else if (fits(peek.host + sep + peek.window)) label = peek.host + sep + peek.window;
    else label = peek.window;

    // Still too wide: clip with an ellipsis rather than print nothing.
    label = Ellipsize(label, labelRoom);

    if (!label.empty()) {
        dl->AddText(ImVec2(labelX, y), Color(158, 166, 186), label.c_str());
    }

    y += lineH + 5.0f;

    // The same bar the cards use, so the strip is a real reading and not a
    // second visual language for the same number. Info rows have no number.
    if (peek.hasPercent) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        const float shown = peek.valid
            ? (g_showRemaining ? 100.0f - peek.usedPercent : peek.usedPercent)
            : 0.0f;
        DrawThinBar(shown, innerW, severity, barH, Color(48, 52, 63));
    }

    ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + height));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

static void DrawWidgetPeekStrip(float width, bool onTop)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const float height = WidgetPeekStripHeight();

    // Its own slightly lighter ground so the bar still reads as a distinct
    // band when it is the only thing left on screen.
    dl->AddRectFilled(c0, ImVec2(c0.x + width, c0.y + height), Color(26, 28, 36, 252));

    // The divider faces the panel body, which is above the bar at the bottom
    // edge and below it at the top edge.
    const float ruleY = onTop ? c0.y + height : c0.y;
    dl->AddLine(ImVec2(c0.x, ruleY), ImVec2(c0.x + width, ruleY), Color(52, 57, 70), 1.0f);

    const std::vector<WidgetPeek>& rows = WidgetPeekRows();
    std::vector<std::string> keys = WidgetBarKeys();

    // Applied after the loop: mutating the list mid-iteration invalidates the
    // indices the drop handlers were computed from. Drop positions are slots
    // between rows: the upper half of a row drops before it, the lower half
    // after, so the last row's lower half appends.
    bool keysChanged = false;
    int dropAt = -1;
    std::string dropKey;
    int removeAt = -1;
    int moveFrom = -1;
    int moveTo = -1;
    float dropLineY = -1.0f;

    const float rowH = WidgetPeekRowHeight();

    for (size_t i = 0; i < rows.size(); ++i) {
        const ImVec2 rowTop(c0.x, c0.y + rowH * static_cast<float>(i));

        if (i > 0) {
            dl->AddLine(ImVec2(rowTop.x + 13.0f, rowTop.y),
                ImVec2(rowTop.x + width - 13.0f, rowTop.y), Color(42, 46, 58), 1.0f);
        }

        // Hit area first, painting second, so the row draws over it.
        ImGui::SetCursorScreenPos(rowTop);
        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##bar_row", ImVec2(width, rowH),
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

        const bool hovered = ImGui::IsItemHovered();

        // Right-click unpins. Only meaningful once the list is explicit.
        if (!keys.empty() && i < keys.size() &&
            ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            removeAt = static_cast<int>(i);
        }

        if (!keys.empty() && i < keys.size() &&
            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const int from = static_cast<int>(i);
            ImGui::SetDragDropPayload("AQC_BAR_MOVE", &from, sizeof(from));
            ImGui::TextUnformatted(WidgetPeekLabel(rows[i]).c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget()) {
            const bool after = ImGui::GetMousePos().y >= rowTop.y + rowH * 0.5f;
            const int slot = static_cast<int>(i) + (after ? 1 : 0);

            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AQC_BAR_KEY")) {
                dropKey.assign(static_cast<const char*>(p->Data));
                dropAt = slot;
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AQC_BAR_MOVE")) {
                moveFrom = *static_cast<const int*>(p->Data);
                moveTo = slot;
            }
            // Drawn after the loop, or the next row's divider would cover it.
            dropLineY = after ? rowTop.y + rowH : rowTop.y;
            ImGui::EndDragDropTarget();
        }
        else if (hovered && !keys.empty()) {
            dl->AddRectFilled(rowTop, ImVec2(rowTop.x + width, rowTop.y + rowH),
                Color(255, 255, 255, 10));
            ImGui::SetTooltip("Drag to reorder  ·  right-click to unpin");
        }

        ImGui::PopID();

        ImGui::SetCursorScreenPos(rowTop);
        DrawWidgetPeekRow(rows[i], width);
    }

    if (dropLineY >= 0.0f) {
        dl->AddLine(ImVec2(c0.x, dropLineY), ImVec2(c0.x + width, dropLineY),
            Color(96, 156, 240), 2.0f);
    }

    if (!dropKey.empty()) {
        // Dropping onto the implicit "follow the open tab" bar makes the list
        // explicit, starting with what was just dropped. A pin that already
        // stands for the same row gives way to it, and every one removed from
        // above the slot moves the slot up with it.
        const std::vector<WidgetPeek>& candidates = WidgetPeekCandidates();
        const WidgetPeek* dropped = ResolveKey(candidates, dropKey);

        size_t at = std::min(static_cast<size_t>(std::max(0, dropAt)), keys.size());
        for (size_t k = keys.size(); k-- > 0;) {
            if (keys[k] == dropKey || (dropped && ResolveKey(candidates, keys[k]) == dropped)) {
                if (k < at) --at;
                keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(k));
            }
        }

        keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(at), dropKey);
        keysChanged = true;
    }
    else if (moveFrom >= 0 && moveFrom < static_cast<int>(keys.size()) &&
        moveTo >= 0 && moveTo <= static_cast<int>(keys.size())) {
        // Taking the row out first shifts every slot below it up by one.
        const int to = moveTo > moveFrom ? moveTo - 1 : moveTo;
        if (to != moveFrom) {
            const std::string moved = keys[static_cast<size_t>(moveFrom)];
            keys.erase(keys.begin() + moveFrom);
            keys.insert(keys.begin() + to, moved);
            keysChanged = true;
        }
    }
    else if (removeAt >= 0 && removeAt < static_cast<int>(keys.size())) {
        keys.erase(keys.begin() + removeAt);
        keysChanged = true;
    }

    if (keysChanged) {
        SetWidgetBarKeys(keys);
    }

    ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + height));
    ImGui::Dummy(ImVec2(width, 0.0f));

    g_widgetContentWidth = g_widgetContentWidthAccum;
    g_widgetContentWidthAccum = 0.0f;
}

// One custom icon-only pill tab: provider icon, selected highlight, and a tiny
// underline showing the host's primary window.
static void DrawProviderTabPill(const char* id, TabImage& image, HostTab tab, const WidgetPeek* primary)
{
    const bool selected = g_widgetTab == tab;
    const float iconS = 18.0f;
    const float padX = 9.0f;
    const bool showIcon = image.srv != nullptr;
    const float pillW = padX + (showIcon ? iconS : 0.0f) + padX;
    const float pillH = 36.0f;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##pill", ImVec2(pillW, pillH));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (clicked) g_widgetTab = tab;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = selected ? Color(46, 92, 150, 255)
        : (hovered ? Color(40, 44, 56, 255) : Color(28, 30, 38, 235));
    dl->AddRectFilled(pos, ImVec2(pos.x + pillW, pos.y + pillH), fill, 9.0f);
    dl->AddRect(pos, ImVec2(pos.x + pillW, pos.y + pillH),
        selected ? Color(96, 156, 240, 255) : Color(52, 56, 70, 200), 9.0f, 0, selected ? 1.4f : 1.0f);

    const float yShift = primary ? 2.0f : 0.0f;
    const ImVec2 ic(pos.x + padX, pos.y + (pillH - iconS) * 0.5f - yShift);
    if (showIcon) {
        dl->AddImage(reinterpret_cast<ImTextureID>(image.srv), ic, ImVec2(ic.x + iconS, ic.y + iconS));
    }

    if (primary) {
        const float bw = pillW - padX * 2.0f;
        const float bx = pos.x + padX;
        const float by = pos.y + pillH - 7.0f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + 3.0f), Color(60, 64, 78, 255), 1.5f);
        const float shown = g_showRemaining ? 100.0f - primary->usedPercent : primary->usedPercent;
        dl->AddRectFilled(ImVec2(bx, by),
            ImVec2(bx + bw * std::clamp(shown / 100.0f, 0.0f, 1.0f), by + 3.0f),
            PanelSeverity(primary->usedPercent), 1.5f);
    }
}

// ---------------------------------------------------------------------------
// Widget mode
//
// The CodexBar-style sectioned panel in a docked column: a compact icon tab
// strip picks the host, then that host's header and sections render
// underneath. Everything is drawn from primitives.
// ---------------------------------------------------------------------------

// Small custom icon button. `icon`: 0 pin, 1 restore, 2 close.
static bool WidgetIconButton(const char* id, int icon, ImVec2 size, ImU32 accent, bool activeState)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 base = activeState ? accent
        : (hovered ? Color(48, 88, 150, 255) : Color(34, 37, 47, 235));
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), base, 6.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
        hovered || activeState ? Color(96, 156, 240, 255) : Color(54, 58, 72, 220), 6.0f, 0, 1.0f);

    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const ImU32 ink = Color(230, 235, 244);
    const float r = 5.0f;

    switch (icon) {
    case 0: // pin: a tack
        dl->AddCircleFilled(ImVec2(c.x, c.y - 2.0f), 3.2f, ink);
        dl->AddLine(ImVec2(c.x, c.y + 1.0f), ImVec2(c.x, c.y + 6.0f), ink, 1.6f);
        break;
    case 1: // restore: a square
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ink, 1.5f, 0, 1.6f);
        break;
    default: // close: an X
        dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ink, 1.8f);
        dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), ink, 1.8f);
        break;
    }

    return pressed;
}

// A footer menu row, like the reference popover's "Settings... / About / Quit".
// Custom drawn: hover highlight, no stock widget.
static bool DrawWidgetMenuRow(const char* id, const char* label, ImU32 color)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeight() + 12.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), Color(44, 48, 60, 255), 7.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    dl->AddText(ImVec2(pos.x + 10.0f, pos.y + 6.0f), color, label);

    return pressed;
}

static void DrawWidgetUi()
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 panelMin = ImGui::GetWindowPos();
    const ImVec2 panelSize = ImGui::GetWindowSize();
    const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);

    draw->AddRectFilledMultiColor(panelMin, panelMax,
        Color(24, 26, 34, 252), Color(24, 26, 34, 252),
        Color(15, 16, 22, 252), Color(15, 16, 22, 252));
    draw->AddRect(panelMin, panelMax, Color(54, 58, 72, 220), 8.0f, 0, 1.0f);

    if (g_widgetFont) ImGui::PushFont(g_widgetFont);

    // A bottom-row anchor rolls the panel downwards, so the peek strip lives on
    // the top edge there and the header has to start below it.
    const int widgetAnchor = R().widgetAnchor
        ? *R().widgetAnchor : AppSettings::WidgetAnchorTopRight;
    const bool peekOnTop = AppSettings::WidgetAnchorRow(widgetAnchor) == 1;
    const float peekStripHeight = WidgetPeekStripHeight();

    // Rolled away: the window has been collapsed to the peek band, so draw only
    // that. The full layout does not fit and BeginChild with a negative height
    // reservation would not survive it anyway.
    if (panelSize.y < peekStripHeight + 24.0f) {
        ImGui::SetCursorScreenPos(ImVec2(panelMin.x,
            peekOnTop ? panelMin.y : panelMax.y - peekStripHeight));
        DrawWidgetPeekStrip(panelSize.x, peekOnTop);
        g_widgetPeekHeight = peekStripHeight;
        if (g_widgetFont) ImGui::PopFont();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(13.0f, 11.0f));
    ImGui::BeginChild("##widget_panel", ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* inner = ImGui::GetWindowDrawList();
    const float contentWidth = ImGui::GetContentRegionAvail().x;

    if (peekOnTop) {
        ImGui::Dummy(ImVec2(contentWidth, peekStripHeight - 4.0f));
    }

    const ImVec2 headerOrigin = ImGui::GetCursorScreenPos();

    // ---- title row ----
    {
        const ImVec2 badgeA(headerOrigin.x, headerOrigin.y);
        const ImVec2 badgeB(badgeA.x + 24.0f, badgeA.y + 24.0f);
        inner->AddRectFilledMultiColor(badgeA, badgeB,
            Color(88, 176, 255), Color(150, 110, 240),
            Color(120, 90, 220), Color(60, 140, 220));
        DrawGenericQuotaIcon(ImVec2((badgeA.x + badgeB.x) * 0.5f, (badgeA.y + badgeB.y) * 0.5f), 12.0f);

        ImFont* tf = g_widgetBigFont ? g_widgetBigFont : ImGui::GetFont();
        inner->AddText(tf, 17.0f, ImVec2(headerOrigin.x + 32.0f, headerOrigin.y + 1.0f),
            Color(236, 240, 248), g_widgetPinned ? "AI Quota - pinned" : "AI Quota");
    }

    const float btnW = 26.0f;
    const float btnH = 22.0f;
    ImGui::SetCursorScreenPos(ImVec2(headerOrigin.x + contentWidth - (btnW * 2.0f + 6.0f), headerOrigin.y));

    const bool pin = WidgetIconButton("##w_pin", 0, ImVec2(btnW, btnH), Color(196, 148, 44, 255), g_widgetPinned);
    ImGui::SameLine(0.0f, 6.0f);
    const bool close = WidgetIconButton("##w_close", 2, ImVec2(btnW, btnH), Color(170, 54, 54, 255), false);

    if (pin) {
        g_widgetPinned = !g_widgetPinned;
        SaveAppSettings();
    }
    if (close) {
        g_shouldClose = true;
    }

    ImGui::SetCursorScreenPos(ImVec2(headerOrigin.x, headerOrigin.y + 32.0f));

    // ---- compact icon tab strip ----
    // Each underline is the window the bar would follow for that host.
    {
        const std::vector<WidgetPeek>& candidates = WidgetPeekCandidates();
        const auto pill = [&candidates](const char* id, TabImage& image, const wchar_t* file, HostTab tab) {
            DrawProviderTabPill(id, EnsureTabImage(image, file), tab, PrimaryCandidate(candidates, tab));
        };

        pill("wCodex", g_codexTabImage, L"Codex.ico", HostTab::Codex);
        ImGui::SameLine(0.0f, 7.0f);
        pill("wClaude", g_claudeTabImage, L"Claude.ico", HostTab::Claude);
        ImGui::SameLine(0.0f, 7.0f);
        pill("wZAi", g_zaiTabImage, L"ZAi.ico", HostTab::ZAi);
        ImGui::SameLine(0.0f, 7.0f);
        pill("wGrok", g_grokTabImage, L"Grok.ico", HostTab::Grok);
    }

    ImGui::Dummy(ImVec2(contentWidth, 9.0f));
    {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        inner->AddLine(rp, ImVec2(rp.x + contentWidth, rp.y), Color(44, 48, 60), 1.0f);
        ImGui::Dummy(ImVec2(contentWidth, 10.0f));
    }

    // ---- selected host: header + sections ----
    // Reserve room for the pinned footer menu; otherwise it scrolls off the
    // bottom with the sections and can never be clicked.
    const float footerHeight = 2.0f * (ImGui::GetTextLineHeight() + 12.0f) + 20.0f
        + (peekOnTop ? 0.0f : peekStripHeight);

    const float bodyTopScreenY = ImGui::GetCursorScreenPos().y;

    ImGui::BeginChild("##widget_body", ImVec2(0.0f, -footerHeight), false,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);

    switch (g_widgetTab) {
    case HostTab::Codex: {
        Codex::Snapshot s;
        { std::lock_guard<std::mutex> lock(g_codexMutex); s = g_codexState; }
        DrawProviderHeader("Codex", s.lastUpdated, PlanBeside("Codex", s.plan),
            g_codexAutoRefreshEnabled, g_codexLoading.load(), RefreshCodexAsync, "w_codex_refresh");
        RenderCodexSections(s);
        break;
    }
    case HostTab::Claude: {
        Claude::Snapshot s;
        { std::lock_guard<std::mutex> lock(g_claudeMutex); s = g_claudeState; }
        DrawProviderHeader("Claude", s.lastUpdated, PlanBeside("Claude", s.plan),
            g_claudeAutoRefreshEnabled, g_claudeLoading.load(), RefreshClaudeAsync, "w_claude_refresh");
        RenderClaudeSections(s);
        break;
    }
    case HostTab::ZAi: {
        ZAi::Snapshot s;
        { std::lock_guard<std::mutex> lock(g_zaiMutex); s = g_zaiState; }
        DrawProviderHeader("Z.Ai", s.lastUpdated, PlanBeside("Z.Ai", s.plan),
            g_zaiAutoRefreshEnabled, g_zaiLoading.load(), RefreshZAiAsync, "w_zai_refresh");
        RenderZAiSections(s);
        break;
    }
    default: {
        Grok::Snapshot s;
        { std::lock_guard<std::mutex> lock(g_grokMutex); s = g_grokState; }
        DrawProviderHeader("Grok", s.lastUpdated, PlanBeside("Grok", s.plan),
            g_grokAutoRefreshEnabled, g_grokLoading.load(), RefreshGrokAsync, "w_grok_refresh");
        RenderGrokSections(s);
        break;
    }
    }

    // Content extent of the body, in child-local coordinates and so unaffected
    // by any scroll. This is what the window sizes itself to.
    g_widgetContentHeight = (bodyTopScreenY - panelMin.y)
        + ImGui::GetCursorPosY()
        + footerHeight
        + ImGui::GetStyle().WindowPadding.y;

    ImGui::EndChild();

    // ---- footer menu (matches the reference popover), pinned to the bottom ----
    {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(rp, ImVec2(rp.x + contentWidth, rp.y),
            Color(44, 48, 60), 1.0f);
        ImGui::Dummy(ImVec2(contentWidth, 7.0f));
    }

    if (DrawWidgetMenuRow("m_settings", "Settings...", Color(214, 220, 232))) {
        g_settingsWindowOpen = true;
    }
    if (DrawWidgetMenuRow("m_quit", "Quit", Color(214, 220, 232))) {
        g_shouldClose = true;
    }

    // Pinned to whichever edge the panel rolls into, not to wherever the cursor
    // happened to land: the sliver left on screen IS this strip, so it has to
    // sit on the edge that stays visible. Bottom-row anchors roll downwards,
    // so there the strip belongs at the top.
    {
        const float stripHeight = peekStripHeight;

        ImGui::SetCursorScreenPos(peekOnTop
            ? ImVec2(panelMin.x, panelMin.y)
            : ImVec2(panelMin.x, panelMax.y - stripHeight));
        DrawWidgetPeekStrip(panelSize.x, peekOnTop);
        g_widgetPeekHeight = stripHeight;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (g_widgetFont) ImGui::PopFont();
}

int WidgetDesiredHeight()
{
    return static_cast<int>(g_widgetContentHeight + 0.5f);
}

int WidgetPeekHeight()
{
    return static_cast<int>(g_widgetPeekHeight + 0.5f);
}

int WidgetDesiredWidth()
{
    return static_cast<int>(g_widgetContentWidth + 0.5f);
}

void RenderSettingsUi(State& state)
{
    g_state = &state;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("AI Quota Settings Root", nullptr, flags);
    ImGui::PopStyleVar(2);

    // Title strip
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + 40.0f);
        dl->AddRectFilled(a, b, Color(22, 24, 31), 0.0f);
        dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), Color(44, 48, 60), 1.0f);
        dl->AddText(ImVec2(a.x + 16.0f, a.y + 12.0f), Color(232, 236, 244), "Settings");

        ImGui::SetCursorScreenPos(ImVec2(a.x + ImGui::GetWindowSize().x - 84.0f, a.y + 8.0f));
        if (ImGui::Button("Close", ImVec2(70.0f, 24.0f))) {
            g_settingsWindowOpen = false;
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 8.0f));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::BeginChild("##settings_body", ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();

    DrawSettingsTab();

    ImGui::EndChild();
    ImGui::End();
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

    // The root window is the app frame itself - there is no OS chrome behind it.
    // Its default padding was letterboxing the whole UI in a band of WindowBg,
    // which reads as black bars down every edge. Let the panel reach the glass.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    ImGui::Begin("AI Quota Checker Root", nullptr, flags);

    ImGui::PopStyleVar(2);

    DrawWidgetUi();

    ImGui::End();
}


}
