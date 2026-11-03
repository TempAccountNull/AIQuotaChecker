#pragma once

#include <filesystem>
#include <string>

namespace Network
{
    struct HttpResponse
    {
        int statusCode = 0;
        std::string body;
        long long serverUnixSeconds = 0; // Optional HTTP Date; used for server-clock expiry.
    };

    class Client final
    {
    public:
        std::string ToLowerCopy(std::string text) const;
        bool IsRateLimitText(const std::string& text) const;

        std::string GetEnvText(const char* name) const;
        // Path-valued variables, read from the wide environment so folders
        // outside the ANSI code page survive. Empty when unset.
        std::filesystem::path EnvPath(const char* name) const;
        // Text-valued variables, likewise read wide, returned as UTF-8.
        std::string EnvText(const char* name) const;
        std::filesystem::path UserProfilePath() const;
        std::string ReadTextFile(const std::filesystem::path& path) const;
        std::string ReadRequiredTextFile(const std::filesystem::path& path) const;
        // The last maxBytes of a file another process is writing (opened with
        // full sharing, read-only), starting at the first whole line; empty
        // when it can't be read.
        std::string ReadFileTail(const std::filesystem::path& path, unsigned long long maxBytes) const;
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
            const std::string& body = {},
            bool followRedirects = true
        ) const;

        // RequestUrl for best-effort calls: a transport failure (offline,
        // timeout, TLS) comes back as status 0 instead of an exception.
        HttpResponse TryRequestUrl(
            const std::string& url,
            const std::string& method,
            const std::wstring& headers,
            const std::string& body = {},
            bool followRedirects = true
        ) const;
    };

    Client* get_instance();
}
