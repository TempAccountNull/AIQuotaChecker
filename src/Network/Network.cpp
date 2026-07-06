#include "Global.hpp"

#include "Network.hpp"
#include "Text.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")

#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_GZIP
#define WINHTTP_DECOMPRESSION_FLAG_GZIP 0x00000001
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_DEFLATE
#define WINHTTP_DECOMPRESSION_FLAG_DEFLATE 0x00000002
#endif

namespace Network
{
    std::string Client::ToLowerCopy(std::string text) const
    {
        return Text::get_instance()->ToLowerCopy(text);
    }

    bool Client::IsRateLimitText(const std::string& text) const
    {
        std::string lower = ToLowerCopy(text);

        return lower.find("rate limited") != std::string::npos ||
            lower.find("rate limit") != std::string::npos ||
            lower.find("http 429") != std::string::npos ||
            lower.find("too many requests") != std::string::npos ||
            lower.find("insufficient balance") != std::string::npos ||
            lower.find("no resource package") != std::string::npos;
    }

    std::string Client::GetEnvText(const char* name) const
    {
        char* value = nullptr;
        size_t len = 0;

        if (_dupenv_s(&value, &len, name) != 0 || !value) {
            return {};
        }

        std::string out(value);
        free(value);
        return out;
    }

    std::filesystem::path Client::UserProfilePath() const
    {
        std::string userProfile = GetEnvText("USERPROFILE");

        if (!userProfile.empty()) {
            return std::filesystem::path(userProfile);
        }

        std::string homeDrive = GetEnvText("HOMEDRIVE");
        std::string homePath = GetEnvText("HOMEPATH");

        if (!homeDrive.empty() && !homePath.empty()) {
            return std::filesystem::path(homeDrive + homePath);
        }

        throw std::runtime_error("Could not resolve USERPROFILE");
    }

    std::string Client::ReadTextFile(const std::filesystem::path& path) const
    {
        std::ifstream file(path, std::ios::binary);

        if (!file) {
            return {};
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::string Client::ReadRequiredTextFile(const std::filesystem::path& path) const
    {
        std::string text = ReadTextFile(path);

        if (text.empty()) {
            throw std::runtime_error("Could not open file: " + path.string());
        }

        return text;
    }

    std::wstring Client::Utf8ToWide(const std::string& value) const
    {
        if (value.empty()) {
            return {};
        }

        int needed = MultiByteToWideChar(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0
        );

        std::wstring out(needed, L'\0');

        MultiByteToWideChar(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            out.data(),
            needed
        );

        return out;
    }

    std::string Client::StripHeaderValue(std::string value) const
    {
        value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
        return value;
    }

    std::wstring Client::JsonHeaders(
        const std::string& authorizationHeader,
        const std::string& origin,
        const std::string& referer
    ) const
    {
        std::string headers;

        if (!authorizationHeader.empty()) {
            headers += "Authorization: " + StripHeaderValue(authorizationHeader) + "\r\n";
        }

        headers += "Accept: application/json\r\n";
        headers += "Content-Type: application/json\r\n";

        if (!origin.empty()) {
            headers += "Origin: " + origin + "\r\n";
        }

        if (!referer.empty()) {
            headers += "Referer: " + referer + "\r\n";
        }

        return Utf8ToWide(headers);
    }

    std::wstring Client::BearerJsonHeaders(
        const std::string& token,
        const std::string& origin,
        const std::string& referer
    ) const
    {
        return JsonHeaders("Bearer " + StripHeaderValue(token), origin, referer);
    }

    std::wstring Client::RawAuthorizationJsonHeaders(
        const std::string& token,
        const std::string& origin,
        const std::string& referer
    ) const
    {
        return JsonHeaders(StripHeaderValue(token), origin, referer);
    }

    HttpResponse Client::RequestUrl(
        const std::string& url,
        const std::string& method,
        const std::wstring& headers,
        const std::string& body
    ) const
    {
        std::wstring wideUrl = Utf8ToWide(url);

        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
            throw std::runtime_error("Could not parse URL");
        }

        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);

        if (parts.dwExtraInfoLength > 0) {
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        }

        if (path.empty()) {
            path = L"/";
        }

        HINTERNET session = WinHttpOpen(
            L"AIQuotaChecker/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (!session) {
            throw std::runtime_error("WinHttpOpen failed");
        }

        WinHttpSetTimeouts(session, 10000, 10000, 10000, 20000);

        HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);

        if (!connect) {
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpConnect failed");
        }

        std::wstring wideMethod = Utf8ToWide(method);

        HINTERNET request = WinHttpOpenRequest(
            connect,
            wideMethod.c_str(),
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
        );

        if (!request) {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpOpenRequest failed");
        }

        DWORD decompressionFlags = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompressionFlags, sizeof(decompressionFlags));

        if (!headers.empty()) {
            WinHttpAddRequestHeaders(
                request,
                headers.c_str(),
                static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE
            );
        }

        const void* bodyPtr = body.empty() ? nullptr : body.data();
        DWORD bodySize = static_cast<DWORD>(body.size());

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<void*>(bodyPtr), bodySize, bodySize, 0)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpSendRequest failed");
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpReceiveResponse failed");
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);

        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX
        );

        std::string responseBody;

        for (;;) {
            DWORD available = 0;

            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                break;
            }

            std::string chunk(available, '\0');
            DWORD read = 0;

            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                break;
            }

            chunk.resize(read);
            responseBody += chunk;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);

        return { static_cast<int>(status), responseBody };
    }
    Client* get_instance()
    {
        static Client client;
        return &client;
    }

    std::string ToLowerCopy(std::string text) { return get_instance()->ToLowerCopy(text); }
    bool IsRateLimitText(const std::string& text) { return get_instance()->IsRateLimitText(text); }
    std::string GetEnvText(const char* name) { return get_instance()->GetEnvText(name); }
    std::filesystem::path UserProfilePath() { return get_instance()->UserProfilePath(); }
    std::string ReadTextFile(const std::filesystem::path& path) { return get_instance()->ReadTextFile(path); }
    std::string ReadRequiredTextFile(const std::filesystem::path& path) { return get_instance()->ReadRequiredTextFile(path); }
    std::wstring Utf8ToWide(const std::string& value) { return get_instance()->Utf8ToWide(value); }
    std::string StripHeaderValue(std::string value) { return get_instance()->StripHeaderValue(value); }
    std::wstring JsonHeaders(const std::string& authorizationHeader, const std::string& origin, const std::string& referer) { return get_instance()->JsonHeaders(authorizationHeader, origin, referer); }
    std::wstring BearerJsonHeaders(const std::string& token, const std::string& origin, const std::string& referer) { return get_instance()->BearerJsonHeaders(token, origin, referer); }
    std::wstring RawAuthorizationJsonHeaders(const std::string& token, const std::string& origin, const std::string& referer) { return get_instance()->RawAuthorizationJsonHeaders(token, origin, referer); }
    HttpResponse RequestUrl(const std::string& url, const std::string& method, const std::wstring& headers, const std::string& body) { return get_instance()->RequestUrl(url, method, headers, body); }
}
