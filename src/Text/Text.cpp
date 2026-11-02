#include "Global.hpp"
#include "Text.hpp"

#include <algorithm>
#include <cctype>

Text* Text::get_instance()
{
    static Text instance;
    return &instance;
}

std::string Text::ToLowerCopy(std::string text) const
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Text::TrimCopy(std::string text) const
{
    const size_t first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) {
        return {};
    }

    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::wstring Text::TrimCopy(std::wstring text) const
{
    const size_t first = text.find_first_not_of(L" \t\r\n");

    if (first == std::wstring::npos) {
        return {};
    }

    return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}

std::string Text::UrlEncode(const std::string& text, std::string_view keep) const
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);

    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
            (c < 0x80 && keep.find(static_cast<char>(c)) != std::string_view::npos)) {
            out.push_back(static_cast<char>(c));
        }
        else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }

    return out;
}

std::string Text::FirstNonEmpty(std::initializer_list<std::string> values) const
{
    for (const std::string& value : values) {
        if (!value.empty()) {
            return value;
        }
    }

    return {};
}

std::wstring Text::Utf8ToWide(const std::string& text) const
{
    if (text.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0,
        text.data(), static_cast<int>(text.size()), nullptr, 0);

    if (length <= 0) {
        return {};
    }

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Text::WideToUtf8(const std::wstring& text) const
{
    if (text.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);

    if (length <= 0) {
        return {};
    }

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::string Text::CompactLower(std::string text, bool keepDot) const
{
    std::string out;
    out.reserve(text.size());

    for (unsigned char c : text) {
        if (std::isalnum(c) || (keepDot && c == '.')) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }

    return out;
}
