#include "Global.hpp"

#include "Network.hpp"
#include "Text.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
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

namespace
{
    constexpr int kMaxRedirects = 10;

    class InternetHandle final
    {
    public:
        explicit InternetHandle(HINTERNET handle) : m_handle(handle) {}
        InternetHandle(const InternetHandle&) = delete;
        InternetHandle& operator=(const InternetHandle&) = delete;
        ~InternetHandle()
        {
            if (m_handle) {
                WinHttpCloseHandle(m_handle);
            }
        }

        HINTERNET get() const { return m_handle; }

    private:
        HINTERNET m_handle = nullptr;
    };

    struct UrlParts
    {
        std::wstring scheme;
        std::wstring host;
        std::wstring path; // path plus query
        INTERNET_PORT port = 0;
        bool secure = false;
    };

    bool CrackUrl(const std::wstring& url, UrlParts& out)
    {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
            return false;
        }

        out.scheme.assign(parts.lpszScheme, parts.dwSchemeLength);
        out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
        out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        out.port = parts.nPort;
        out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

        if (out.path.empty()) {
            out.path = L"/";
        }

        return true;
    }

    std::wstring QueryLocation(HINTERNET request)
    {
        DWORD size = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
            WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
            return {};
        }

        std::wstring location(size / sizeof(wchar_t), L'\0');

        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                location.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
            return {};
        }

        location.resize(size / sizeof(wchar_t));
        return location;
    }

    // Absolute, scheme-relative, root-relative or path-relative Location.
    std::wstring ResolveLocation(const UrlParts& base, const std::wstring& location)
    {
        if (location.find(L"://") != std::wstring::npos) {
            return location;
        }

        if (location.starts_with(L"//")) {
            return base.scheme + L":" + location;
        }

        const bool defaultPort = base.port == (base.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
        const std::wstring origin = base.scheme + L"://" + base.host + (defaultPort ? L"" : L":" + std::to_wstring(base.port));

        if (location.starts_with(L"/")) {
            return origin + location;
        }

        const std::wstring directory = base.path.substr(0, base.path.find_last_of(L'/', base.path.find(L'?')) + 1);
        return origin + directory + location;
    }

    // A redirect to another host drops credentials, as browsers do: the
    // bearer token or session cookie belongs to the host it was meant for.
    std::wstring StripCredentialHeaders(const std::wstring& headers)
    {
        std::wstring kept;

        for (size_t start = 0; start < headers.size();) {
            size_t end = headers.find(L"\r\n", start);
            end = end == std::wstring::npos ? headers.size() : end;

            std::wstring line = headers.substr(start, end - start);
            std::wstring lower = line;
            CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));

            if (!line.empty() && !lower.starts_with(L"authorization:") && !lower.starts_with(L"cookie:")) {
                kept += line + L"\r\n";
            }

            start = end + 2;
        }

        return kept;
    }

    long long ServerUnixSeconds(HINTERNET request)
    {
        SYSTEMTIME serverDate{};
        DWORD dateSize = sizeof(serverDate);

        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                WINHTTP_HEADER_NAME_BY_INDEX, &serverDate, &dateSize, WINHTTP_NO_HEADER_INDEX)) {
            return 0;
        }

        FILETIME fileTime{};

        if (!SystemTimeToFileTime(&serverDate, &fileTime)) {
            return 0;
        }

        ULARGE_INTEGER ticks{};
        ticks.LowPart = fileTime.dwLowDateTime;
        ticks.HighPart = fileTime.dwHighDateTime;
        constexpr ULONGLONG epoch = 116444736000000000ULL;
        return ticks.QuadPart >= epoch ? static_cast<long long>((ticks.QuadPart - epoch) / 10000000ULL) : 0;
    }

    std::string ReadBody(HINTERNET request)
    {
        std::string body;

        for (;;) {
            DWORD available = 0;

            // A failure here is a dropped connection, not the end of the
            // body: returning what arrived would pass a truncated document
            // off as a complete 200.
            if (!WinHttpQueryDataAvailable(request, &available)) {
                throw std::runtime_error("The connection dropped while reading the response");
            }

            if (available == 0) {
                return body;
            }

            const size_t offset = body.size();
            body.resize(offset + available);
            DWORD read = 0;

            if (!WinHttpReadData(request, body.data() + offset, available, &read)) {
                throw std::runtime_error("The connection dropped while reading the response");
            }

            body.resize(offset + read);

            if (read == 0) {
                return body;
            }
        }
    }
}

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
            lower.find("too many requests") != std::string::npos;
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

    std::filesystem::path Client::EnvPath(const char* name) const
    {
        const std::wstring wideName = Text::get_instance()->Utf8ToWide(name ? name : "");
        wchar_t* value = nullptr;
        size_t length = 0;

        if (wideName.empty() || _wdupenv_s(&value, &length, wideName.c_str()) != 0 || !value) {
            return {};
        }

        std::filesystem::path path(value);
        free(value);
        return path;
    }

    std::string Client::EnvText(const char* name) const
    {
        return Text::get_instance()->WideToUtf8(EnvPath(name).wstring());
    }

    std::filesystem::path Client::UserProfilePath() const
    {
        // Wide environment: a profile folder outside the ANSI code page would
        // otherwise come back with '?' in it and every credential path breaks.
        if (std::filesystem::path profile = EnvPath("USERPROFILE"); !profile.empty()) {
            return profile;
        }

        const std::filesystem::path drive = EnvPath("HOMEDRIVE");
        const std::filesystem::path home = EnvPath("HOMEPATH");

        if (!drive.empty() && !home.empty()) {
            return std::filesystem::path(drive.wstring() + home.wstring());
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
            throw std::runtime_error("Could not open file: " + Text::get_instance()->WideToUtf8(path.wstring()));
        }

        return text;
    }

    std::string Client::ReadFileTail(const std::filesystem::path& path, unsigned long long maxBytes) const
    {
        HANDLE file = CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE) {
            return {};
        }

        LARGE_INTEGER size{};

        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
            CloseHandle(file);
            return {};
        }

        const unsigned long long fileSize = static_cast<unsigned long long>(size.QuadPart);
        const unsigned long long readSize = std::min(fileSize, maxBytes);
        const unsigned long long start = fileSize - readSize;

        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(start);

        if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
            CloseHandle(file);
            return {};
        }

        std::string text(static_cast<size_t>(readSize), '\0');
        size_t totalRead = 0;

        while (totalRead < text.size()) {
            const DWORD chunkSize = static_cast<DWORD>(std::min<size_t>(
                text.size() - totalRead,
                1024 * 1024
            ));
            DWORD read = 0;

            if (!ReadFile(file, text.data() + totalRead, chunkSize, &read, nullptr) || read == 0) {
                break;
            }

            totalRead += read;
        }

        CloseHandle(file);
        text.resize(totalRead);

        // A tail read can start in the middle of one JSONL record. Drop only
        // that partial first record; the source file is never modified/copied.
        if (start > 0) {
            const size_t firstNewline = text.find('\n');

            if (firstNewline == std::string::npos) {
                return {};
            }

            text.erase(0, firstNewline + 1);
        }

        return text;
    }

    std::wstring Client::Utf8ToWide(const std::string& value) const
    {
        return Text::get_instance()->Utf8ToWide(value);
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
            headers += "Origin: " + StripHeaderValue(origin) + "\r\n";
        }

        if (!referer.empty()) {
            headers += "Referer: " + StripHeaderValue(referer) + "\r\n";
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
        const std::string& body,
        bool followRedirects
    ) const
    {
        std::wstring currentUrl = Utf8ToWide(url);
        std::wstring currentMethod = Utf8ToWide(method);
        std::wstring currentHeaders = headers;
        std::string currentBody = body;

        for (int hop = 0;; ++hop) {
            UrlParts parts;

            if (!CrackUrl(currentUrl, parts)) {
                throw std::runtime_error("Could not parse URL");
            }

            // Automatic proxy: the system's own settings (including PAC and
            // per-user proxies), where DEFAULT_PROXY only honoured netsh.
            InternetHandle session(WinHttpOpen(L"AIQuotaChecker/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));

            if (!session.get()) {
                throw std::runtime_error("WinHttpOpen failed");
            }

            WinHttpSetTimeouts(session.get(), 10000, 10000, 10000, 20000);

            InternetHandle connect(WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));

            if (!connect.get()) {
                throw std::runtime_error("WinHttpConnect failed");
            }

            InternetHandle request(WinHttpOpenRequest(connect.get(), currentMethod.c_str(), parts.path.c_str(),
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0));

            if (!request.get()) {
                throw std::runtime_error("WinHttpOpenRequest failed");
            }

            // Redirects are followed by hand below, so credentials never go to
            // another host. ZCode passes followRedirects = false: it sends two
            // account-scoped headers and treats a redirect as an error.
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;

            if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) {
                throw std::runtime_error("Could not disable HTTP redirects");
            }

            DWORD decompressionFlags = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
            WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &decompressionFlags, sizeof(decompressionFlags));

            if (!currentHeaders.empty()) {
                WinHttpAddRequestHeaders(request.get(), currentHeaders.c_str(), static_cast<DWORD>(-1),
                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            }

            const DWORD bodySize = static_cast<DWORD>(currentBody.size());

            if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    currentBody.empty() ? nullptr : currentBody.data(), bodySize, bodySize, 0)) {
                throw std::runtime_error("WinHttpSendRequest failed");
            }

            if (!WinHttpReceiveResponse(request.get(), nullptr)) {
                throw std::runtime_error("WinHttpReceiveResponse failed");
            }

            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

            const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;

            if (followRedirects && redirect && hop < kMaxRedirects) {
                const std::wstring next = ResolveLocation(parts, QueryLocation(request.get()));
                UrlParts target;

                // Never downgrade https to http (WinHTTP's own default policy).
                if (!next.empty() && CrackUrl(next, target) && (target.secure || !parts.secure)) {
                    if (_wcsicmp(target.host.c_str(), parts.host.c_str()) != 0) {
                        currentHeaders = StripCredentialHeaders(currentHeaders);
                    }

                    if (status == 303 || ((status == 301 || status == 302) && currentMethod == L"POST")) {
                        currentMethod = L"GET";
                        currentBody.clear();
                    }

                    currentUrl = next;
                    continue;
                }
            }

            const long long serverUnixSeconds = ServerUnixSeconds(request.get());
            return { static_cast<int>(status), ReadBody(request.get()), serverUnixSeconds };
        }
    }

    HttpResponse Client::TryRequestUrl(
        const std::string& url,
        const std::string& method,
        const std::wstring& headers,
        const std::string& body,
        bool followRedirects
    ) const
    {
        try {
            return RequestUrl(url, method, headers, body, followRedirects);
        }
        catch (const std::exception&) {
            return {};
        }
    }

    Client* get_instance()
    {
        static Client client;
        return &client;
    }
}
