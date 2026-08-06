#include "Global.hpp"

#include "CodexAppServer.hpp"
#include "Network.hpp"

#include <windows.h>

#include <array>
#include <cwctype>
#include <limits>
#include <unordered_map>

using json = nlohmann::json;

namespace CodexAppServer {
namespace {

    constexpr DWORD kRequestTimeoutMilliseconds = 15000;
    constexpr size_t kMaximumBufferedOutput = 4u * 1024u * 1024u;
    constexpr size_t kMaximumDiagnosticOutput = 16u * 1024u;

    class UniqueHandle final {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE value) : m_value(value) {}

        ~UniqueHandle() {
            Reset();
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.Release()) {}

        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) {
                Reset(other.Release());
            }
            return *this;
        }

        HANDLE Get() const {
            return m_value;
        }

        bool Valid() const {
            return m_value != nullptr && m_value != INVALID_HANDLE_VALUE;
        }

        HANDLE Release() {
            HANDLE value = m_value;
            m_value = nullptr;
            return value;
        }

        void Reset(HANDLE value = nullptr) {
            if (Valid()) {
                CloseHandle(m_value);
            }
            m_value = value;
        }

    private:
        HANDLE m_value = nullptr;
    };

    static std::wstring TrimQuotes(std::wstring value) {
        while (!value.empty() && std::iswspace(value.front())) {
            value.erase(value.begin());
        }

        while (!value.empty() && std::iswspace(value.back())) {
            value.pop_back();
        }

        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
            value = value.substr(1, value.size() - 2);
        }

        return value;
    }

    static std::wstring SearchExecutable(const std::wstring& fileName) {
        std::vector<wchar_t> buffer(32768, L'\0');
        DWORD length = SearchPathW(
            nullptr,
            fileName.c_str(),
            nullptr,
            static_cast<DWORD>(buffer.size()),
            buffer.data(),
            nullptr
        );

        if (length == 0 || length >= buffer.size()) {
            return {};
        }

        return std::wstring(buffer.data(), length);
    }

    static std::wstring FindCodexExecutable() {
        std::string configured = Network::get_instance()->GetEnvText("CODEX_EXECUTABLE");

        if (!configured.empty()) {
            std::wstring candidate = TrimQuotes(Network::get_instance()->Utf8ToWide(configured));

            if (!candidate.empty() && std::filesystem::exists(candidate)) {
                return candidate;
            }

            std::wstring found = SearchExecutable(candidate);
            if (!found.empty()) {
                return found;
            }
        }

        std::wstring found = SearchExecutable(L"codex.exe");
        if (!found.empty()) {
            return found;
        }

        std::string localAppData = Network::get_instance()->GetEnvText("LOCALAPPDATA");
        if (!localAppData.empty()) {
            std::filesystem::path candidate =
                std::filesystem::path(localAppData) /
                "Programs" / "OpenAI" / "Codex" / "bin" / "codex.exe";

            if (std::filesystem::exists(candidate)) {
                return candidate.wstring();
            }
        }

        std::filesystem::path localBin =
            Network::get_instance()->UserProfilePath() / ".local" / "bin" / "codex.exe";

        if (std::filesystem::exists(localBin)) {
            return localBin.wstring();
        }

        return {};
    }

    static std::string LastErrorText(const char* operation) {
        return std::string(operation) + " failed with Windows error " + std::to_string(GetLastError());
    }

    static bool WriteAll(HANDLE handle, const std::string& text, std::string& detail) {
        size_t offset = 0;

        while (offset < text.size()) {
            const size_t remaining = text.size() - offset;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;

            if (!WriteFile(handle, text.data() + offset, chunk, &written, nullptr)) {
                detail = LastErrorText("Writing to Codex app-server");
                return false;
            }

            if (written == 0) {
                detail = "Writing to Codex app-server returned zero bytes";
                return false;
            }

            offset += written;
        }

        return true;
    }

    static void AppendBounded(std::string& target, const char* data, size_t size, size_t maximum) {
        if (size == 0 || maximum == 0) {
            return;
        }

        if (size >= maximum) {
            target.assign(data + (size - maximum), maximum);
            return;
        }

        if (target.size() + size > maximum) {
            target.erase(0, target.size() + size - maximum);
        }

        target.append(data, size);
    }

    static bool DrainPipe(HANDLE pipe, std::string& output, size_t maximum, bool& broken) {
        broken = false;

        for (;;) {
            DWORD available = 0;

            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                    broken = true;
                    return true;
                }
                return false;
            }

            if (available == 0) {
                return true;
            }

            std::array<char, 4096> buffer{};
            DWORD read = 0;
            DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));

            if (!ReadFile(pipe, buffer.data(), requested, &read, nullptr)) {
                DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                    broken = true;
                    return true;
                }
                return false;
            }

            if (read == 0) {
                return true;
            }

            AppendBounded(output, buffer.data(), read, maximum);
        }
    }

    static std::string ErrorMessage(const json& response) {
        if (!response.is_object() || !response.contains("error")) {
            return {};
        }

        const json& error = response.at("error");
        if (error.is_string()) {
            return error.get<std::string>();
        }

        if (error.is_object()) {
            if (error.contains("message") && error.at("message").is_string()) {
                return error.at("message").get<std::string>();
            }
            return error.dump();
        }

        return error.dump();
    }

    static bool LooksLikeUnsupportedMethod(const std::string& message) {
        std::string lower = Network::get_instance()->ToLowerCopy(message);
        return lower.find("method not found") != std::string::npos ||
            lower.find("unknown method") != std::string::npos ||
            lower.find("unsupported method") != std::string::npos ||
            lower.find("not supported") != std::string::npos;
    }

    class AppServerProcess final {
    public:
        ~AppServerProcess() {
            Stop();
        }

        bool Start(const std::wstring& executable, std::string& detail) {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.bInheritHandle = TRUE;

            HANDLE childStdinRead = nullptr;
            HANDLE parentStdinWrite = nullptr;
            HANDLE parentStdoutRead = nullptr;
            HANDLE childStdoutWrite = nullptr;
            HANDLE parentStderrRead = nullptr;
            HANDLE childStderrWrite = nullptr;

            if (!CreatePipe(&childStdinRead, &parentStdinWrite, &security, 0)) {
                detail = LastErrorText("Creating Codex stdin pipe");
                return false;
            }

            UniqueHandle childInput(childStdinRead);
            m_stdinWrite.Reset(parentStdinWrite);

            if (!SetHandleInformation(m_stdinWrite.Get(), HANDLE_FLAG_INHERIT, 0)) {
                detail = LastErrorText("Protecting Codex stdin pipe");
                return false;
            }

            if (!CreatePipe(&parentStdoutRead, &childStdoutWrite, &security, 0)) {
                detail = LastErrorText("Creating Codex stdout pipe");
                return false;
            }

            m_stdoutRead.Reset(parentStdoutRead);
            UniqueHandle childOutput(childStdoutWrite);

            if (!SetHandleInformation(m_stdoutRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
                detail = LastErrorText("Protecting Codex stdout pipe");
                return false;
            }

            if (!CreatePipe(&parentStderrRead, &childStderrWrite, &security, 0)) {
                detail = LastErrorText("Creating Codex stderr pipe");
                return false;
            }

            m_stderrRead.Reset(parentStderrRead);
            UniqueHandle childError(childStderrWrite);

            if (!SetHandleInformation(m_stderrRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
                detail = LastErrorText("Protecting Codex stderr pipe");
                return false;
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = childInput.Get();
            startup.hStdOutput = childOutput.Get();
            startup.hStdError = childError.Get();

            PROCESS_INFORMATION processInfo{};
            std::wstring commandLine = L"\"" + executable + L"\" app-server";
            std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
            mutableCommand.push_back(L'\0');

            BOOL created = CreateProcessW(
                executable.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                nullptr,
                nullptr,
                &startup,
                &processInfo
            );

            if (!created) {
                detail = LastErrorText("Starting Codex app-server");
                return false;
            }

            m_process.Reset(processInfo.hProcess);
            UniqueHandle thread(processInfo.hThread);

            // The parent must close its copies of the child ends or broken-pipe
            // detection will never work.
            childInput.Reset();
            childOutput.Reset();
            childError.Reset();
            return true;
        }

        bool Send(const json& message, std::string& detail) {
            if (!m_stdinWrite.Valid()) {
                detail = "Codex app-server stdin is not available";
                return false;
            }

            return WriteAll(m_stdinWrite.Get(), message.dump() + "\n", detail);
        }

        bool WaitForResponse(long long id, json& response, DWORD timeoutMilliseconds, std::string& detail) {
            const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;

            for (;;) {
                if (!DrainOutput(detail)) {
                    return false;
                }

                auto found = m_responses.find(id);
                if (found != m_responses.end()) {
                    response = std::move(found->second);
                    m_responses.erase(found);
                    return true;
                }

                DWORD exitCode = STILL_ACTIVE;
                if (!GetExitCodeProcess(m_process.Get(), &exitCode)) {
                    detail = LastErrorText("Reading Codex app-server process state");
                    return false;
                }

                if (exitCode != STILL_ACTIVE) {
                    DrainOutput(detail);
                    found = m_responses.find(id);
                    if (found != m_responses.end()) {
                        response = std::move(found->second);
                        m_responses.erase(found);
                        return true;
                    }

                    detail = "Codex app-server exited with code " + std::to_string(exitCode);
                    if (!m_stderrBuffer.empty()) {
                        detail += ": " + m_stderrBuffer;
                    }
                    return false;
                }

                if (GetTickCount64() >= deadline) {
                    detail = "Timed out waiting for Codex app-server response";
                    if (!m_stderrBuffer.empty()) {
                        detail += ": " + m_stderrBuffer;
                    }
                    return false;
                }

                Sleep(10);
            }
        }

        void Stop() {
            m_stdinWrite.Reset();

            if (m_process.Valid()) {
                DWORD exitCode = STILL_ACTIVE;
                if (GetExitCodeProcess(m_process.Get(), &exitCode) && exitCode == STILL_ACTIVE) {
                    if (WaitForSingleObject(m_process.Get(), 250) == WAIT_TIMEOUT) {
                        TerminateProcess(m_process.Get(), 0);
                        WaitForSingleObject(m_process.Get(), 1000);
                    }
                }
            }

            m_stdoutRead.Reset();
            m_stderrRead.Reset();
            m_process.Reset();
        }

    private:
        bool ParseStdoutLines(std::string& detail) {
            for (;;) {
                size_t newline = m_stdoutBuffer.find('\n');
                if (newline == std::string::npos) {
                    if (m_stdoutBuffer.size() >= kMaximumBufferedOutput) {
                        detail = "Codex app-server produced an unterminated response larger than the safety limit";
                        return false;
                    }
                    return true;
                }

                std::string line = m_stdoutBuffer.substr(0, newline);
                m_stdoutBuffer.erase(0, newline + 1);

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.empty()) {
                    continue;
                }

                json message = json::parse(line, nullptr, false);
                if (message.is_discarded() || !message.is_object()) {
                    // stdout should be JSONL, but do not let one diagnostic line
                    // hide a later valid response.
                    continue;
                }

                if (!message.contains("id")) {
                    continue;
                }

                const json& idValue = message.at("id");
                if (idValue.is_number_integer()) {
                    m_responses[idValue.get<long long>()] = std::move(message);
                }
                else if (idValue.is_number_unsigned()) {
                    unsigned long long value = idValue.get<unsigned long long>();
                    if (value <= static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
                        m_responses[static_cast<long long>(value)] = std::move(message);
                    }
                }
            }
        }

        bool DrainOutput(std::string& detail) {
            bool stdoutBroken = false;
            bool stderrBroken = false;

            if (!DrainPipe(m_stdoutRead.Get(), m_stdoutBuffer, kMaximumBufferedOutput, stdoutBroken)) {
                detail = LastErrorText("Reading Codex app-server stdout");
                return false;
            }

            if (!DrainPipe(m_stderrRead.Get(), m_stderrBuffer, kMaximumDiagnosticOutput, stderrBroken)) {
                detail = LastErrorText("Reading Codex app-server stderr");
                return false;
            }

            return ParseStdoutLines(detail);
        }

        UniqueHandle m_process;
        UniqueHandle m_stdinWrite;
        UniqueHandle m_stdoutRead;
        UniqueHandle m_stderrRead;
        std::string m_stdoutBuffer;
        std::string m_stderrBuffer;
        std::unordered_map<long long, json> m_responses;
    };

    static Result Failure(ResultKind kind, const std::string& detail) {
        Result result;
        result.kind = kind;
        result.detail = detail;
        return result;
    }

} // namespace

Result ReadCurrentAccountRateLimits() {
    std::wstring executable = FindCodexExecutable();

    if (executable.empty()) {
        return Failure(ResultKind::Unavailable, "Codex executable was not found");
    }

    AppServerProcess server;
    std::string detail;

    if (!server.Start(executable, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    const json initialize = {
        { "method", "initialize" },
        { "id", 1 },
        { "params", {
            { "clientInfo", {
                { "name", "ai_quota_checker" },
                { "title", "AI Quota Checker" },
                { "version", "1.0.0" }
            } }
        } }
    };

    if (!server.Send(initialize, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    json initializeResponse;
    if (!server.WaitForResponse(1, initializeResponse, kRequestTimeoutMilliseconds, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    std::string initializeError = ErrorMessage(initializeResponse);
    if (!initializeError.empty()) {
        ResultKind kind = LooksLikeUnsupportedMethod(initializeError)
            ? ResultKind::Unavailable
            : ResultKind::Error;
        return Failure(kind, "Codex app-server initialization failed: " + initializeError);
    }

    if (!server.Send(json{ { "method", "initialized" }, { "params", json::object() } }, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    if (!server.Send(json{
        { "method", "account/read" },
        { "id", 2 },
        { "params", { { "refreshToken", true } } }
    }, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    json accountResponse;
    if (!server.WaitForResponse(2, accountResponse, kRequestTimeoutMilliseconds, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    // Account metadata is useful for the plan label, but an older app-server
    // may still support rate limits even if this optional read shape differs.
    json accountResult;
    if (!accountResponse.contains("error") && accountResponse.contains("result")) {
        accountResult = accountResponse.at("result");
    }

    if (!server.Send(json{
        { "method", "account/rateLimits/read" },
        { "id", 3 },
        { "params", json::object() }
    }, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    json rateLimitsResponse;
    if (!server.WaitForResponse(3, rateLimitsResponse, kRequestTimeoutMilliseconds, detail)) {
        return Failure(ResultKind::Error, detail);
    }

    std::string rateLimitsError = ErrorMessage(rateLimitsResponse);
    if (!rateLimitsError.empty()) {
        ResultKind kind = LooksLikeUnsupportedMethod(rateLimitsError)
            ? ResultKind::Unavailable
            : ResultKind::Error;
        return Failure(kind, "Codex rate-limit read failed: " + rateLimitsError);
    }

    if (!rateLimitsResponse.contains("result") || !rateLimitsResponse.at("result").is_object()) {
        return Failure(ResultKind::Error, "Codex app-server returned no rate-limit result");
    }

    Result result;
    result.kind = ResultKind::Success;
    result.accountResult = std::move(accountResult);
    result.rateLimitsResult = rateLimitsResponse.at("result");
    return result;
}

} // namespace CodexAppServer
