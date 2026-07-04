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

std::string Text::TrimRightCopy(std::string text) const
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }

    return text;
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
