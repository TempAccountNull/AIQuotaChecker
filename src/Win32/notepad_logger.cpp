#include "notepad_logger.hpp"

#include <cstring>

// Global variable definition. Now caches the *text control* handle (classic Edit
// on Win10, RichEditD2DPT on Win11), revalidated with IsWindow() on each call.
HWND CachedHWND = nullptr;

namespace {

// EnumChildWindows callback: stop at the first Edit / RichEditD2DPT descendant.
BOOL CALLBACK FindEditProc(HWND child, LPARAM lparam)
{
    char cls[64] = {};
    GetClassNameA(child, cls, sizeof(cls));
    if (std::strcmp(cls, "Edit") == 0 ||            // classic Notepad (Win10)
        std::strcmp(cls, "RichEditD2DPT") == 0)     // new Notepad (Win11)
    {
        *reinterpret_cast<HWND*>(lparam) = child;
        return FALSE;   // found it - stop enumerating
    }
    return TRUE;
}

} // namespace

HWND notepad_logger_find_target()
{
    // Possible Notepad window titles (EN and DE)
    const char* notepadTitles[] = {
        "Unbenannt - Editor",
        "*Unbenannt - Editor",
        "Untitled - Notepad",
        "*Untitled - Notepad"
    };
    HWND top = nullptr;
    for (const char* title : notepadTitles)
    {
        top = FindWindowA(nullptr, title);
        if (top) break;
    }
    if (!top)
        return nullptr; // No Notepad found

    // Fast path: classic Notepad exposes the Edit control as a direct child.
    HWND edit = FindWindowExA(top, nullptr, "Edit", nullptr);
    if (edit)
        return edit;

    // New Notepad nests RichEditD2DPT deep in the tree - walk all descendants.
    HWND found = nullptr;
    EnumChildWindows(top, FindEditProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

// Function implementation
void notepad_logger(const char* str, ...)
{
    char buf[262144];
    va_list ap;
    va_start(ap, str);
    vsprintf_s(buf, sizeof(buf), str, ap);
    va_end(ap);

    if (!IsWindow(CachedHWND))
        CachedHWND = notepad_logger_find_target();
    if (!CachedHWND)
        return; // No Notepad edit control found

    // Normalise trailing newlines: strip any \r\n / \n already in buf, then append
    // a single \r\n so the control gets exactly one line-break per call.
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
    strcat_s(buf, sizeof(buf), "\r\n");

    // Insert at the caret (as the original did). EM_REPLACESEL leaves the caret
    // after the inserted text, so lines append top-to-bottom - matching the
    // original logger and the console - for both the classic Edit and RichEditD2DPT.
    SendMessageA(CachedHWND, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(buf));
}
