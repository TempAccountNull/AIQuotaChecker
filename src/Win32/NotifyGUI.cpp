#include "Global.hpp"
#include "NotifyGUI.hpp"


#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>
#include "KSharedClock.hpp"

// ---------------------------------------------------------------------------
// KUSER_SHARED_DATA � the kernel maps this read-only page into every process
// at 0x7FFE0000.  InterruptTime is a 64-bit counter in 100-nanosecond units
// (10,000,000 ticks per second).  We define only the fields we need; no
// winternl.h or ntddk.h required, and zero import-table entries are added.
// ---------------------------------------------------------------------------
namespace
{
    // Tear-safe 64-bit read of InterruptTime using the High1/Low/High2
    // spinlock exactly as NTDLL does internally.
    static float GetNow()
    {
        return KSharedClock::InterruptSeconds();
    }

    // ---------------------------------------------------------------------------
    // Notification ring buffer
    // ---------------------------------------------------------------------------
    struct Notification
    {
        char           message[256];
        NotifyPosition position;
        float          startTime;  // seconds (InterruptTime epoch)
        float          duration;   // total display time in seconds
        ImU32          color;      // text colour, ImGui ABGR packing
    };

    struct PendingNotification
    {
        char           message[256];
        NotifyPosition position;
        float          duration;
        std::uint32_t  color;
        bool           insideWindow;
    };

    struct NativeNotification
    {
        HWND           hwnd = nullptr;
        std::wstring   message;
        NotifyPosition position = NotifyPosition::BOTTOM_RIGHT;
        ULONGLONG      startTick = 0;
        DWORD          durationMs = 5000;
        COLORREF       textColor = RGB(255, 255, 255);
        int            width = 260;
        int            height = 42;
    };

    static constexpr int kMax = 32;
    static Notification g_Notifs[kMax];
    static int g_Count = 0;
    static std::mutex g_Mutex;

    static std::vector<PendingNotification> g_Pending;
    static std::mutex g_PendingMutex;

    static std::vector<NativeNotification*> g_NativeNotifs;
    static constexpr wchar_t kNativeClassName[] = L"AIQuotaCheckerNotifyToast";
    static bool g_NativeClassRegistered = false;

    static std::atomic_bool g_ShowInsideWindow = false;

    // Scale the alpha component of an ImU32 colour by [0..1].
    static ImU32 ScaleAlpha(ImU32 col, float factor)
    {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
        c.w *= factor;
        return ImGui::ColorConvertFloat4ToU32(c);
    }

    static BYTE GetB(std::uint32_t c) { return static_cast<BYTE>((c >> 16) & 0xFF); }
    static BYTE GetG(std::uint32_t c) { return static_cast<BYTE>((c >> 8) & 0xFF); }
    static BYTE GetR(std::uint32_t c) { return static_cast<BYTE>(c & 0xFF); }

    static COLORREF ToColorRef(std::uint32_t col)
    {
        return RGB(GetR(col), GetG(col), GetB(col));
    }

    static constexpr std::uint32_t kProviderCodexColor = NOTIFY_COL32(0, 220, 255, 255);
    static constexpr std::uint32_t kProviderClaudeColor = NOTIFY_COL32(255, 154, 60, 255);
    static constexpr std::uint32_t kProviderZAiColor = NOTIFY_COL32(68, 215, 128, 255);

    static bool StartsWithAscii(const char* text, const char* prefix)
    {
        if (!text || !prefix) return false;

        while (*prefix) {
            if (*text != *prefix) return false;
            ++text;
            ++prefix;
        }

        return true;
    }

    static bool StartsWithWide(const std::wstring& text, const wchar_t* prefix)
    {
        if (!prefix) return false;

        size_t len = wcslen(prefix);

        if (text.size() < len) {
            return false;
        }

        return text.compare(0, len, prefix) == 0;
    }

    static bool GetProviderPrefixStyle(const char* text, size_t& prefixLen, ImU32& prefixColor)
    {
        if (StartsWithAscii(text, "Codex")) {
            prefixLen = 5;
            prefixColor = static_cast<ImU32>(kProviderCodexColor);
            return true;
        }

        if (StartsWithAscii(text, "Claude")) {
            prefixLen = 6;
            prefixColor = static_cast<ImU32>(kProviderClaudeColor);
            return true;
        }

        if (StartsWithAscii(text, "Z.Ai")) {
            prefixLen = 4;
            prefixColor = static_cast<ImU32>(kProviderZAiColor);
            return true;
        }

        return false;
    }

    static bool GetProviderPrefixStyle(const std::wstring& text, size_t& prefixLen, COLORREF& prefixColor)
    {
        if (StartsWithWide(text, L"Codex")) {
            prefixLen = 5;
            prefixColor = ToColorRef(kProviderCodexColor);
            return true;
        }

        if (StartsWithWide(text, L"Claude")) {
            prefixLen = 6;
            prefixColor = ToColorRef(kProviderClaudeColor);
            return true;
        }

        if (StartsWithWide(text, L"Z.Ai")) {
            prefixLen = 4;
            prefixColor = ToColorRef(kProviderZAiColor);
            return true;
        }

        return false;
    }

    static std::wstring Utf8ToWide(const char* text)
    {
        if (!text || !*text) return L"";

        int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (needed <= 0) return L"";

        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), needed);

        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    static RECT GetWorkArea()
    {
        RECT rc{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
        return rc;
    }

    static int GetNativeStackOffset(const NativeNotification* n)
    {
        int offset = 0;
        int gap = 8;

        for (NativeNotification* item : g_NativeNotifs) {
            if (item == n) return offset;

            if (item && item->position == n->position) {
                offset += item->height + gap;
            }
        }

        return offset;
    }

    static void PositionNative(NativeNotification* n)
    {
        if (!n || !n->hwnd) return;

        RECT rc = GetWorkArea();
        int margin = 16;
        int stackOffset = GetNativeStackOffset(n);

        int x = rc.right - n->width - margin;
        int y = rc.bottom - n->height - margin - stackOffset;

        switch (n->position)
        {
        case NotifyPosition::TOP_LEFT:
            x = rc.left + margin;
            y = rc.top + margin + stackOffset;
            break;

        case NotifyPosition::TOP_RIGHT:
            x = rc.right - n->width - margin;
            y = rc.top + margin + stackOffset;
            break;

        case NotifyPosition::BOTTOM_LEFT:
            x = rc.left + margin;
            y = rc.bottom - n->height - margin - stackOffset;
            break;

        case NotifyPosition::CENTER:
            x = rc.left + ((rc.right - rc.left) - n->width) / 2;
            y = rc.top + ((rc.bottom - rc.top) - n->height) / 2 + stackOffset;
            break;

        case NotifyPosition::BOTTOM_RIGHT:
        default:
            x = rc.right - n->width - margin;
            y = rc.bottom - n->height - margin - stackOffset;
            break;
        }

        SetWindowPos(n->hwnd, HWND_TOPMOST, x, y, n->width, n->height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    static void RepositionNativeAll()
    {
        for (NativeNotification* n : g_NativeNotifs) PositionNative(n);
    }

    static void DestroyNative(NativeNotification* n)
    {
        if (!n) return;

        auto it = std::find(g_NativeNotifs.begin(), g_NativeNotifs.end(), n);
        if (it != g_NativeNotifs.end()) g_NativeNotifs.erase(it);

        HWND hwnd = n->hwnd;
        n->hwnd = nullptr;

        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }

        delete n;
        RepositionNativeAll();
    }

    static void DrawNative(HWND hwnd, NativeNotification* n)
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(12, 12, 12));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, n ? n->textColor : RGB(255, 255, 255));

        HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, font);

        RECT textRc = rc;
        textRc.left += 12;
        textRc.right -= 12;
        textRc.top += 10;
        textRc.bottom -= 10;

        if (n) {
            size_t prefixLen = 0;
            COLORREF providerColor = RGB(255, 255, 255);

            if (GetProviderPrefixStyle(n->message, prefixLen, providerColor)) {
                std::wstring prefix = n->message.substr(0, prefixLen);
                std::wstring rest = n->message.substr(prefixLen);

                SIZE prefixSize{};
                GetTextExtentPoint32W(hdc, prefix.c_str(), static_cast<int>(prefix.size()), &prefixSize);

                SetTextColor(hdc, providerColor);
                TextOutW(hdc, textRc.left, textRc.top, prefix.c_str(), static_cast<int>(prefix.size()));

                RECT restRc = textRc;
                restRc.left += prefixSize.cx;

                SetTextColor(hdc, n->textColor);
                DrawTextW(hdc, rest.c_str(), -1, &restRc, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
            }
            else {
                DrawTextW(hdc, n->message.c_str(), -1, &textRc, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
            }
        }

        SelectObject(hdc, oldFont);
        DeleteObject(font);

        EndPaint(hwnd, &ps);
    }

    static LRESULT CALLBACK NativeWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        NativeNotification* n = reinterpret_cast<NativeNotification*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_NCCREATE:
        {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }

        case WM_TIMER:
        {
            n = reinterpret_cast<NativeNotification*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!n) return 0;

            ULONGLONG now = GetTickCount64();
            ULONGLONG elapsed = now - n->startTick;

            if (elapsed >= n->durationMs) {
                KillTimer(hwnd, 1);
                DestroyNative(n);
                return 0;
            }

            DWORD fadeMs = 800;
            BYTE alpha = 230;

            if (n->durationMs > fadeMs && elapsed > n->durationMs - fadeMs) {
                float t = static_cast<float>(n->durationMs - elapsed) / static_cast<float>(fadeMs);
                t = std::clamp(t, 0.0f, 1.0f);
                alpha = static_cast<BYTE>(230.0f * t);
            }

            SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
            return 0;
        }

        case WM_PAINT:
            DrawNative(hwnd, n);
            return 0;

        case WM_CLOSE:
            if (n) DestroyNative(n);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    static void RegisterNativeClass()
    {
        if (g_NativeClassRegistered) return;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = NativeWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kNativeClassName;

        ATOM atom = RegisterClassExW(&wc);
        if (atom || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) g_NativeClassRegistered = true;
    }

    static void MeasureNativeSize(const std::wstring& text, NotifyPosition position, int& width, int& height)
    {
        RECT work = GetWorkArea();
        int workWidth = work.right - work.left;
        int maxWidth = workWidth - 64;

        if (maxWidth < 260) maxWidth = 260;
        if (maxWidth > 780) maxWidth = 780;

        HDC hdc = GetDC(nullptr);
        if (!hdc) {
            width = 420;
            height = 42;
            return;
        }

        HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, font);

        SIZE oneLine{};
        GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &oneLine);

        width = static_cast<int>(oneLine.cx) + 28;

        if (width < 220) width = 220;
        if (width > maxWidth) width = maxWidth;

        // Center notifications need a stable stack width, otherwise each toast
        // centers itself using a different width and the stack looks crooked.
        if (position == NotifyPosition::CENTER) {
            width = std::min(620, maxWidth);
        }

        RECT calc{};
        calc.left = 0;
        calc.top = 0;
        calc.right = width - 24;
        calc.bottom = 0;

        DrawTextW(hdc, text.c_str(), -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);

        height = (calc.bottom - calc.top) + 20;

        if (height < 42) height = 42;
        if (height > 220) height = 220;

        SelectObject(hdc, oldFont);
        DeleteObject(font);
        ReleaseDC(nullptr, hdc);
    }

    static void AddNativeNow(const char* message, NotifyPosition position, float duration, std::uint32_t color)
    {
        std::wstring text = Utf8ToWide(message);
        if (text.empty()) return;

        RegisterNativeClass();
        if (!g_NativeClassRegistered) return;

        NativeNotification* n = new NativeNotification();
        n->message = text;
        n->position = position;
        n->startTick = GetTickCount64();
        n->durationMs = static_cast<DWORD>((duration > 0.0f ? duration : 5.0f) * 1000.0f);
        n->textColor = ToColorRef(color);
        MeasureNativeSize(text, position, n->width, n->height);

        DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
        HWND hwnd = CreateWindowExW(exStyle, kNativeClassName, L"", WS_POPUP, 0, 0, n->width, n->height, nullptr, nullptr, GetModuleHandleW(nullptr), n);

        if (!hwnd) {
            delete n;
            return;
        }

        n->hwnd = hwnd;
        g_NativeNotifs.push_back(n);

        HRGN rgn = CreateRoundRectRgn(0, 0, n->width + 1, n->height + 1, 8, 8);
        SetWindowRgn(hwnd, rgn, TRUE);

        SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);
        PositionNative(n);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        SetTimer(hwnd, 1, 16, nullptr);
    }

    static void AddInsideNow(const char* message, NotifyPosition position, float duration, std::uint32_t color)
    {
        if (!message || !*message) return;

        std::lock_guard<std::mutex> lk(g_Mutex);

        if (g_Count >= kMax) {
            for (int i = 1; i < kMax; ++i) g_Notifs[i - 1] = g_Notifs[i];
            g_Count = kMax - 1;
        }

        Notification& n = g_Notifs[g_Count++];
        strncpy_s(n.message, sizeof(n.message), message, _TRUNCATE);
        n.position = position;
        n.startTime = GetNow();
        n.duration = duration > 0.0f ? duration : 5.0f;
        n.color = static_cast<ImU32>(color); // ImU32 = unsigned int, same layout
    }

    static void DrainPending()
    {
        std::vector<PendingNotification> pending;

        {
            std::lock_guard<std::mutex> lk(g_PendingMutex);
            if (g_Pending.empty()) return;
            pending.swap(g_Pending);
        }

        for (const PendingNotification& n : pending) {
            if (n.insideWindow) AddInsideNow(n.message, n.position, n.duration, n.color);
            else AddNativeNow(n.message, n.position, n.duration, n.color);
        }
    }
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace NotifyGUI
{
    void SetInsideWindow(bool enabled)
    {
        g_ShowInsideWindow.store(enabled);
    }

    bool GetInsideWindow()
    {
        return g_ShowInsideWindow.load();
    }

    void Add(const char* message, NotifyPosition position, float duration, std::uint32_t color)
    {
        if (!message || !*message) return;

        PendingNotification n{};
        strncpy_s(n.message, sizeof(n.message), message, _TRUNCATE);
        n.position = position;
        n.duration = duration > 0.0f ? duration : 5.0f;
        n.color = color;
        n.insideWindow = g_ShowInsideWindow.load();

        std::lock_guard<std::mutex> lk(g_PendingMutex);
        g_Pending.push_back(n);
    }

    void Render()
    {
        DrainPending();

        // Snapshot inside the lock, then drop it before touching ImGui (font
        // atlas access from CalcTextSize must not contend with scanner threads).
        Notification local[kMax];
        int count;
        {
            std::lock_guard<std::mutex> lk(g_Mutex);
            if (g_Count == 0) return;

            // Expire in-place, compact survivors.
            float now = GetNow();
            int dst = 0;

            for (int i = 0; i < g_Count; ++i) {
                if (now - g_Notifs[i].startTime < g_Notifs[i].duration) g_Notifs[dst++] = g_Notifs[i];
            }

            g_Count = count = dst;
            if (count == 0) return;

            memcpy(local, g_Notifs, sizeof(Notification) * static_cast<size_t>(count));
        }

        // -----------------------------------------------------------------------
        // Draw — mutex released; all accesses below are render-thread-only.
        // -----------------------------------------------------------------------
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        const float now = GetNow();
        const float screenW = io.DisplaySize.x;
        const float screenH = io.DisplaySize.y;

        const float margin = 10.0f;
        const float padX = 10.0f;
        const float padY = 8.0f;
        const float gap = 5.0f;
        const float rounding = 3.0f;
        const float maxBoxW = std::min(720.0f, screenW - margin * 2.0f);

        float offTL = 50.0f;
        float offTR = 50.0f;
        float offBL = 50.0f;
        float offBR = 50.0f;
        float offCenter = 0.0f;

        for (int i = 0; i < count; ++i) {
            const Notification& n = local[i];
            float elapsed = now - n.startTime;
            if (elapsed >= n.duration) continue; // expired between snapshot and draw; skip

            // Fade out over the final 1 second of life.
            float alpha = 1.0f;
            float fadeStart = n.duration - 1.0f;

            if (fadeStart > 0.0f && elapsed > fadeStart) alpha = 1.0f - ((elapsed - fadeStart) / 1.0f);
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;

            float oneLineW = ImGui::CalcTextSize(n.message).x + padX * 2.0f;
            float boxW = std::clamp(oneLineW, 180.0f, maxBoxW);

            // Center notifications need a stable stack width, otherwise each toast
            // centers itself using a different width and the stack looks crooked.
            if (n.position == NotifyPosition::CENTER) {
                boxW = std::min(620.0f, maxBoxW);
            }

            float wrapW = boxW - padX * 2.0f;

            ImVec2 textSz = ImGui::CalcTextSize(n.message, nullptr, false, wrapW);
            float boxH = std::clamp(textSz.y + padY * 2.0f, 32.0f, 220.0f);

            float boxX, boxY;

            switch (n.position) {
            case NotifyPosition::TOP_LEFT:
                boxX = margin;
                boxY = offTL;
                offTL += boxH + gap;
                break;

            case NotifyPosition::TOP_RIGHT:
                boxX = screenW - boxW - margin;
                boxY = offTR;
                offTR += boxH + gap;
                break;

            case NotifyPosition::BOTTOM_LEFT:
                boxX = margin;
                boxY = screenH - offBL - boxH;
                offBL += boxH + gap;
                break;

            case NotifyPosition::CENTER:
                boxX = (screenW - boxW) * 0.5f;
                boxY = (screenH - boxH) * 0.5f + offCenter;
                offCenter += boxH + gap;
                break;

            default: // BOTTOM_RIGHT
                boxX = screenW - boxW - margin;
                boxY = screenH - offBR - boxH;
                offBR += boxH + gap;
                break;
            }

            ImU32 bgCol = IM_COL32(10, 10, 10, static_cast<int>(200.0f * alpha));
            ImU32 brCol = IM_COL32(70, 70, 70, static_cast<int>(255.0f * alpha));
            ImU32 txCol = ScaleAlpha(n.color, alpha);

            ImVec2 pMin(boxX, boxY);
            ImVec2 pMax(boxX + boxW, boxY + boxH);

            dl->AddRectFilled(pMin, pMax, bgCol, rounding);
            dl->AddRect(pMin, pMax, brCol, rounding, 0, 1.0f);

            size_t prefixLen = 0;
            ImU32 providerColor = 0;

            if (GetProviderPrefixStyle(n.message, prefixLen, providerColor)) {
                const char* prefixEnd = n.message + prefixLen;
                ImU32 prefixCol = ScaleAlpha(providerColor, alpha);
                ImVec2 textPos(boxX + padX, boxY + padY);
                ImVec2 prefixSize = ImGui::CalcTextSize(n.message, prefixEnd);

                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPos, prefixCol, n.message, prefixEnd);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(textPos.x + prefixSize.x, textPos.y), txCol, prefixEnd, nullptr, wrapW - prefixSize.x);
            }
            else {
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(boxX + padX, boxY + padY), txCol, n.message, nullptr, wrapW);
            }
        }
    }
} // namespace NotifyGUI
