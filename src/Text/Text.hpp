#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

class Text final
{
public:
    static Text* get_instance();

    std::string ToLowerCopy(std::string text) const;
    // Both ends, spaces, tabs and line breaks.
    std::string TrimCopy(std::string text) const;
    std::wstring TrimCopy(std::wstring text) const;
    // Percent-encodes all but RFC 3986's unreserved characters (A-Z a-z 0-9
    // - . _ ~) and any in `keep`; a space is %20, never '+'.
    std::string UrlEncode(const std::string& text, std::string_view keep = {}) const;
    std::string FirstNonEmpty(std::initializer_list<std::string> values) const;
    std::string CompactLower(std::string text, bool keepDot = true) const;
    // UTF-8 <-> UTF-16 for Win32 calls and paths. Invalid sequences become
    // U+FFFD rather than failing, so text cut mid-character still shows.
    std::wstring Utf8ToWide(const std::string& text) const;
    std::string WideToUtf8(const std::wstring& text) const;
};
