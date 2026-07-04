#pragma once

#include <filesystem>
#include <string>

namespace Network
{
    struct HttpResponse
    {
        int statusCode = 0;
        std::string body;
    };

    class Client final
    {
    public:
        std::string ToLowerCopy(std::string text) const;
        bool IsRateLimitText(const std::string& text) const;

        std::string GetEnvText(const char* name) const;
        std::filesystem::path UserProfilePath() const;
        std::string ReadTextFile(const std::filesystem::path& path) const;
        std::string ReadRequiredTextFile(const std::filesystem::path& path) const;
        std::wstring Utf8ToWide(const std::string& value) const;
        std::string StripHeaderValue(std::string value) const;

        std::wstring JsonHeaders(
            const std::string& authorizationHeader,
            const std::string& origin,
            const std::string& referer
        ) const;

        std::wstring BearerJsonHeaders(
            const std::string& token,
            const std::string& origin,
            const std::string& referer
        ) const;

        std::wstring RawAuthorizationJsonHeaders(
            const std::string& token,
            const std::string& origin,
            const std::string& referer
        ) const;

        HttpResponse RequestUrl(
            const std::string& url,
            const std::string& method,
            const std::wstring& headers,
            const std::string& body = {}
        ) const;
    };

    Client* get_instance();

    // Compatibility wrappers for older call sites while refactors move to get_instance()->.
    std::string ToLowerCopy(std::string text);
    bool IsRateLimitText(const std::string& text);
    std::string GetEnvText(const char* name);
    std::filesystem::path UserProfilePath();
    std::string ReadTextFile(const std::filesystem::path& path);
    std::string ReadRequiredTextFile(const std::filesystem::path& path);
    std::wstring Utf8ToWide(const std::string& value);
    std::string StripHeaderValue(std::string value);
    std::wstring JsonHeaders(const std::string& authorizationHeader, const std::string& origin, const std::string& referer);
    std::wstring BearerJsonHeaders(const std::string& token, const std::string& origin, const std::string& referer);
    std::wstring RawAuthorizationJsonHeaders(const std::string& token, const std::string& origin, const std::string& referer);
    HttpResponse RequestUrl(const std::string& url, const std::string& method, const std::wstring& headers, const std::string& body = {});
}
