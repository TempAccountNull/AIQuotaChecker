#pragma once

#include <initializer_list>
#include <string>

class Text final
{
public:
    static Text* get_instance();

    std::string ToLowerCopy(std::string text) const;
    std::string TrimRightCopy(std::string text) const;
    std::string FirstNonEmpty(std::initializer_list<std::string> values) const;
    std::string CompactLower(std::string text, bool keepDot = true) const;
};
