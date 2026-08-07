#include "Global.hpp"

#include "ClaudeDesktopAuth.hpp"
#include "ClaudeCookieStore.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

using json = nlohmann::json;

namespace ClaudeDesktopAuth {
namespace {

    constexpr long long kChromeEpochOffsetMicroseconds = 11644473600000000LL;

    struct CookieRecord {
        std::string name;
        std::string hostKey;
        std::string value;
        long long expiresUtc = 0;
        long long lastAccessUtc = 0;
        long long creationUtc = 0;
    };

    struct DesktopIdentity {
        std::string organizationId;
        std::vector<std::pair<std::string, std::string>> sessionCookies;
        std::string rootDomain;
        bool cookieRowsFound = false;
        std::string detail;
    };

    static long long NowChromeMicroseconds() {
        using namespace std::chrono;
        const long long unixMicros = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
        return unixMicros + kChromeEpochOffsetMicroseconds;
    }

    static std::string ToLowerAscii(std::string text) {
        for (char& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    }

    static std::string WideToUtf8(const std::wstring& value) {
        if (value.empty()) {
            return {};
        }

        int needed = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

        if (needed <= 0) {
            throw std::runtime_error("Could not convert a Claude Desktop path to UTF-8");
        }

        std::string output(static_cast<size_t>(needed), '\0');

        if (WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            needed,
            nullptr,
            nullptr
        ) != needed) {
            throw std::runtime_error("Could not convert a Claude Desktop path to UTF-8");
        }

        return output;
    }


    class UniqueHandle final {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) : m_handle(handle) {}

        ~UniqueHandle() {
            Reset();
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept
            : m_handle(other.Release()) {
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) {
                Reset(other.Release());
            }
            return *this;
        }

        bool Valid() const {
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }

        HANDLE Get() const {
            return m_handle;
        }

        HANDLE Release() {
            HANDLE handle = m_handle;
            m_handle = INVALID_HANDLE_VALUE;
            return handle;
        }

        void Reset(HANDLE handle = INVALID_HANDLE_VALUE) {
            if (Valid()) {
                CloseHandle(m_handle);
            }
            m_handle = handle;
        }

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

    static std::string TrimWindowsMessage(std::wstring message) {
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' ||
                message.back() == L' ' || message.back() == L'\t')) {
            message.pop_back();
        }

        return WideToUtf8(message);
    }

    static std::string Win32Failure(
        const std::string& operation,
        const std::filesystem::path& path,
        DWORD error
    ) {
        wchar_t* rawMessage = nullptr;
        DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS;

        DWORD length = FormatMessageW(
            flags,
            nullptr,
            error,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&rawMessage),
            0,
            nullptr
        );

        std::wstring message;
        if (length > 0 && rawMessage) {
            message.assign(rawMessage, rawMessage + length);
        }

        if (rawMessage) {
            LocalFree(rawMessage);
        }

        std::ostringstream detail;
        detail << operation << " \"" << path.string() << "\" failed (Win32 "
               << error << ")";

        if (!message.empty()) {
            detail << ": " << TrimWindowsMessage(std::move(message));
        }

        return detail.str();
    }

    static UniqueHandle OpenSharedReadHandle(
        const std::filesystem::path& path,
        bool optional
    ) {
        const std::wstring nativePath = path.wstring();

        HANDLE handle = CreateFileW(
            nativePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();

            if (optional &&
                (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
                return {};
            }

            throw std::runtime_error(
                Win32Failure("Opening shared Claude Desktop file", path, error)
            );
        }

        return UniqueHandle(handle);
    }

    static std::string ReadTextFileWithSharedRead(
        const std::filesystem::path& path,
        size_t maximumBytes
    ) {
        UniqueHandle source = OpenSharedReadHandle(path, false);

        LARGE_INTEGER sourceSize{};
        if (!GetFileSizeEx(source.Get(), &sourceSize) || sourceSize.QuadPart < 0) {
            throw std::runtime_error(
                Win32Failure(
                    "Reading Claude Desktop file size",
                    path,
                    GetLastError()
                )
            );
        }

        if (static_cast<unsigned long long>(sourceSize.QuadPart) >
            static_cast<unsigned long long>(maximumBytes)) {
            throw std::runtime_error(
                "Claude Desktop file is unexpectedly large: " + path.string()
            );
        }

        std::string output(
            static_cast<size_t>(sourceSize.QuadPart),
            '\0'
        );

        size_t offset = 0;
        while (offset < output.size()) {
            DWORD request = static_cast<DWORD>(
                std::min<size_t>(
                    output.size() - offset,
                    1024 * 1024
                )
            );

            DWORD bytesRead = 0;
            if (!ReadFile(
                source.Get(),
                output.data() + offset,
                request,
                &bytesRead,
                nullptr
            )) {
                throw std::runtime_error(
                    Win32Failure(
                        "Reading shared Claude Desktop file",
                        path,
                        GetLastError()
                    )
                );
            }

            if (bytesRead == 0) {
                throw std::runtime_error(
                    "Claude Desktop changed \"" + path.string() +
                    "\" while it was being read"
                );
            }

            offset += static_cast<size_t>(bytesRead);
        }

        return output;
    }

    static std::filesystem::path ClaudeDesktopUserDataPath() {
        std::string overridePath = Network::get_instance()->GetEnvText("CLAUDE_DESKTOP_USER_DATA_DIR");

        if (!overridePath.empty()) {
            return std::filesystem::path(overridePath);
        }

        std::string appData = Network::get_instance()->GetEnvText("APPDATA");

        if (appData.empty()) {
            throw std::runtime_error("APPDATA is not available, so Claude Desktop data cannot be located");
        }

        return std::filesystem::path(appData) / "Claude";
    }

    static std::vector<unsigned char> Base64Decode(const std::string& value) {
        if (value.empty()) {
            return {};
        }

        DWORD size = 0;

        if (!CryptStringToBinaryA(
            value.c_str(),
            static_cast<DWORD>(value.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &size,
            nullptr,
            nullptr
        )) {
            throw std::runtime_error("Could not decode Claude Desktop base64 data");
        }

        std::vector<unsigned char> output(size);

        if (!CryptStringToBinaryA(
            value.c_str(),
            static_cast<DWORD>(value.size()),
            CRYPT_STRING_BASE64,
            output.data(),
            &size,
            nullptr,
            nullptr
        )) {
            throw std::runtime_error("Could not decode Claude Desktop base64 data");
        }

        output.resize(size);
        return output;
    }

    static std::vector<unsigned char> DpapiDecrypt(const unsigned char* data, size_t size) {
        if (!data || size == 0 || size > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
            throw std::runtime_error("Claude Desktop encrypted data is empty or invalid");
        }

        DATA_BLOB input{};
        input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data));
        input.cbData = static_cast<DWORD>(size);

        DATA_BLOB output{};

        if (!CryptUnprotectData(
            &input,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output
        )) {
            throw std::runtime_error("Windows could not decrypt Claude Desktop data (DPAPI error " + std::to_string(GetLastError()) + ")");
        }

        std::vector<unsigned char> result(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return result;
    }

    static std::array<unsigned char, 32> Sha256(const unsigned char* data, size_t size) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> hashObject;
        std::array<unsigned char, 32> digest{};

        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0
        );

        if (status < 0) {
            throw std::runtime_error("Could not open Windows SHA-256 provider");
        }

        DWORD objectLength = 0;
        DWORD resultLength = 0;

        status = BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),
            &resultLength,
            0
        );

        if (status < 0 || objectLength == 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("Could not initialize Windows SHA-256 provider");
        }

        hashObject.resize(objectLength);

        status = BCryptCreateHash(
            algorithm,
            &hash,
            hashObject.data(),
            static_cast<ULONG>(hashObject.size()),
            nullptr,
            0,
            0
        );

        if (status >= 0 && size > 0) {
            if (size > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
                status = static_cast<NTSTATUS>(0xC000000DL);
            }
            else {
                status = BCryptHashData(
                    hash,
                    const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
                    static_cast<ULONG>(size),
                    0
                );
            }
        }

        if (status >= 0) {
            status = BCryptFinishHash(
                hash,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0
            );
        }

        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);

        if (status < 0) {
            throw std::runtime_error("Could not calculate SHA-256 for Claude Desktop data");
        }

        return digest;
    }

    static std::array<unsigned char, 32> Sha256(const std::string& value) {
        return Sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size());
    }

    static std::vector<unsigned char> RandomBytes(size_t size) {
        if (size > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
            throw std::runtime_error("Claude Desktop random request is too large");
        }

        std::vector<unsigned char> output(size);

        if (size > 0) {
            NTSTATUS status = BCryptGenRandom(
                nullptr,
                output.data(),
                static_cast<ULONG>(output.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG
            );

            if (status < 0) {
                throw std::runtime_error("Could not generate secure random data for Claude Desktop OAuth");
            }
        }

        return output;
    }

    static std::vector<unsigned char> LoadChromiumMasterKey(const std::filesystem::path& userDataPath) {
        std::filesystem::path localStatePath = userDataPath / "Local State";

        if (!std::filesystem::exists(localStatePath)) {
            throw std::runtime_error("Claude Desktop Local State was not found");
        }

        json root = JsonUtils::get_instance()->ParseRequired(
            ReadTextFileWithSharedRead(localStatePath, 64ULL * 1024ULL * 1024ULL)
        );

        if (!root.contains("os_crypt") || !root.at("os_crypt").is_object()) {
            throw std::runtime_error("Claude Desktop Local State has no os_crypt section");
        }

        const json& osCrypt = root.at("os_crypt");
        std::string encodedKey = JsonUtils::get_instance()->String(osCrypt, "encrypted_key");

        if (encodedKey.empty()) {
            throw std::runtime_error("Claude Desktop Local State has no encrypted_key");
        }

        std::vector<unsigned char> protectedKey = Base64Decode(encodedKey);
        static const unsigned char kDpapiPrefix[] = { 'D', 'P', 'A', 'P', 'I' };

        if (protectedKey.size() >= sizeof(kDpapiPrefix) &&
            std::equal(std::begin(kDpapiPrefix), std::end(kDpapiPrefix), protectedKey.begin())) {
            protectedKey.erase(protectedKey.begin(), protectedKey.begin() + sizeof(kDpapiPrefix));
        }

        std::vector<unsigned char> key = DpapiDecrypt(protectedKey.data(), protectedKey.size());

        if (key.size() != 32) {
            throw std::runtime_error("Claude Desktop encryption key has an unexpected size");
        }

        return key;
    }

    static std::vector<unsigned char> AesGcmDecrypt(
        const std::vector<unsigned char>& key,
        const unsigned char* nonce,
        size_t nonceSize,
        const unsigned char* ciphertext,
        size_t ciphertextSize,
        const unsigned char* tag,
        size_t tagSize
    ) {
        if (key.empty() || !nonce || nonceSize == 0 || !tag || tagSize == 0 ||
            ciphertextSize > static_cast<size_t>(std::numeric_limits<ULONG>::max()) ||
            nonceSize > static_cast<size_t>(std::numeric_limits<ULONG>::max()) ||
            tagSize > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
            throw std::runtime_error("Claude Desktop AES-GCM data is invalid");
        }

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE keyHandle = nullptr;
        std::vector<unsigned char> keyObject;
        std::vector<unsigned char> output(ciphertextSize);
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_AES_ALGORITHM,
            nullptr,
            0
        );

        if (status < 0) {
            throw std::runtime_error("Could not open Windows AES provider");
        }

        status = BCryptSetProperty(
            algorithm,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
            0
        );

        DWORD objectLength = 0;
        DWORD resultLength = 0;

        if (status >= 0) {
            status = BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength),
                &resultLength,
                0
            );
        }

        if (status >= 0) {
            keyObject.resize(objectLength);
            status = BCryptGenerateSymmetricKey(
                algorithm,
                &keyHandle,
                keyObject.data(),
                static_cast<ULONG>(keyObject.size()),
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
                static_cast<ULONG>(key.size()),
                0
            );
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(nonce));
        authInfo.cbNonce = static_cast<ULONG>(nonceSize);
        authInfo.pbTag = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(tag));
        authInfo.cbTag = static_cast<ULONG>(tagSize);

        ULONG plainSize = 0;

        if (status >= 0) {
            status = BCryptDecrypt(
                keyHandle,
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(ciphertext)),
                static_cast<ULONG>(ciphertextSize),
                &authInfo,
                nullptr,
                0,
                output.empty() ? nullptr : output.data(),
                static_cast<ULONG>(output.size()),
                &plainSize,
                0
            );
        }

        if (keyHandle) BCryptDestroyKey(keyHandle);
        BCryptCloseAlgorithmProvider(algorithm, 0);

        if (status < 0) {
            throw std::runtime_error("Windows could not decrypt Claude Desktop AES-GCM data");
        }

        output.resize(plainSize);
        return output;
    }

    static bool HasPrefix(const std::vector<unsigned char>& data, const char* prefix) {
        const size_t length = std::strlen(prefix);
        return data.size() >= length && std::equal(prefix, prefix + length, data.begin());
    }

    static std::vector<unsigned char> DecryptElectronBlob(
        const std::vector<unsigned char>& encrypted,
        const std::filesystem::path& userDataPath
    ) {
        if (encrypted.empty()) {
            return {};
        }

        if (HasPrefix(encrypted, "v20")) {
            throw std::runtime_error(
                "Claude Desktop uses Chromium v20 app-bound encryption, which this standalone checker cannot decrypt"
            );
        }

        if (HasPrefix(encrypted, "v10") || HasPrefix(encrypted, "v11")) {
            constexpr size_t prefixSize = 3;
            constexpr size_t nonceSize = 12;
            constexpr size_t tagSize = 16;

            if (encrypted.size() < prefixSize + nonceSize + tagSize) {
                throw std::runtime_error("Claude Desktop encrypted value is truncated");
            }

            std::vector<unsigned char> key = LoadChromiumMasterKey(userDataPath);
            const unsigned char* nonce = encrypted.data() + prefixSize;
            const unsigned char* ciphertext = nonce + nonceSize;
            const size_t ciphertextSize = encrypted.size() - prefixSize - nonceSize - tagSize;
            const unsigned char* tag = encrypted.data() + encrypted.size() - tagSize;

            return AesGcmDecrypt(
                key,
                nonce,
                nonceSize,
                ciphertext,
                ciphertextSize,
                tag,
                tagSize
            );
        }

        return DpapiDecrypt(encrypted.data(), encrypted.size());
    }

    static std::string DecryptCookieValue(
        const std::string& plainValue,
        const std::vector<unsigned char>& encryptedValue,
        const std::string& hostKey,
        int databaseVersion,
        const std::filesystem::path& userDataPath
    ) {
        if (!plainValue.empty()) {
            return plainValue;
        }

        if (encryptedValue.empty()) {
            return {};
        }

        std::vector<unsigned char> decrypted =
            DecryptElectronBlob(encryptedValue, userDataPath);

        // Chromium cookie database version 24+ binds the plaintext to
        // host_key by prefixing SHA-256(host_key). Remove the prefix only
        // after it verifies; otherwise preserve the decrypted bytes.
        if (databaseVersion >= 24 && decrypted.size() >= 32) {
            const std::array<unsigned char, 32> hostDigest = Sha256(hostKey);
            if (std::equal(hostDigest.begin(), hostDigest.end(), decrypted.begin())) {
                decrypted.erase(decrypted.begin(), decrypted.begin() + 32);
            }
        }

        return std::string(decrypted.begin(), decrypted.end());
    }

    static std::vector<CookieRecord> ReadCookiesLive(
        const std::filesystem::path& cookiePath,
        const std::filesystem::path& userDataPath
    ) {
        ClaudeCookieStore::ReadResult readResult =
            ClaudeCookieStore::ReadLive(cookiePath);
        std::vector<ClaudeCookieStore::RawCookie>& rawCookies = readResult.cookies;

        std::vector<CookieRecord> records;
        std::string firstDecryptError;
        bool encryptedRowsSeen = false;

        for (ClaudeCookieStore::RawCookie& raw : rawCookies) {
            if (!raw.encryptedValue.empty()) {
                encryptedRowsSeen = true;
            }

            CookieRecord record;
            record.name = std::move(raw.name);
            record.hostKey = std::move(raw.hostKey);
            record.expiresUtc = raw.expiresUtc;
            record.lastAccessUtc = raw.lastAccessUtc;
            record.creationUtc = raw.creationUtc;

            try {
                record.value = DecryptCookieValue(
                    raw.value,
                    raw.encryptedValue,
                    record.hostKey,
                    readResult.databaseVersion,
                    userDataPath
                );
            }
            catch (const std::exception& error) {
                if (firstDecryptError.empty()) {
                    firstDecryptError = error.what();
                }
                continue;
            }

            if (record.expiresUtc > 0 &&
                record.expiresUtc <= NowChromeMicroseconds()) {
                continue;
            }

            if (!record.value.empty()) {
                records.push_back(std::move(record));
            }
        }

        if (records.empty() && encryptedRowsSeen && !firstDecryptError.empty()) {
            throw std::runtime_error(firstDecryptError);
        }

        return records;
    }

    static bool IsOrganizationId(const std::string& value) {
        if (value.size() != 36) {
            return false;
        }

        for (size_t i = 0; i < value.size(); ++i) {
            const char c = value[i];
            const bool hyphenPosition = i == 8 || i == 13 || i == 18 || i == 23;

            if (hyphenPosition) {
                if (c != '-') return false;
            }
            else if (!std::isxdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }

        return true;
    }

    static std::string CookieRoot(const std::string& hostKey) {
        const std::string lower = ToLowerAscii(hostKey);
        if (lower.find("claude.com") != std::string::npos) return "claude.com";
        if (lower.find("claude.ai") != std::string::npos) return "claude.ai";
        return {};
    }

    struct OAuthCandidate {
        std::string organizationId;
        std::string accessToken;
        std::string subscriptionType;
        std::string rateLimitTier;
        double expiresAtMs = 0.0;
        int scopeScore = 0;
        bool scopedCache = false;
    };

    static std::string CacheOrganizationId(const std::string& cacheKey) {
        const size_t first = cacheKey.find(':');
        if (first == std::string::npos) {
            return {};
        }

        const size_t second = cacheKey.find(':', first + 1);
        if (second == std::string::npos || second <= first + 1) {
            return {};
        }

        std::string organizationId = cacheKey.substr(first + 1, second - first - 1);
        return IsOrganizationId(organizationId) ? organizationId : std::string();
    }

    static int CacheScopeScore(const std::string& cacheKey) {
        int score = 0;
        if (cacheKey.find("user:inference") != std::string::npos) score += 4;
        if (cacheKey.find("user:profile") != std::string::npos) score += 2;
        if (cacheKey.find("user:sessions:claude_code") != std::string::npos) score += 1;
        return score;
    }

    static bool CollectOAuthCandidates(
        const json& cache,
        bool scopedCache,
        std::vector<OAuthCandidate>& candidates
    ) {
        if (!cache.is_object()) {
            return false;
        }

        const bool entriesPresent = cache.begin() != cache.end();

        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }

            const json& entry = it.value();
            const std::string token = JsonUtils::get_instance()->String(entry, "token");
            const std::string organizationId = CacheOrganizationId(it.key());

            if (token.empty() || organizationId.empty()) {
                continue;
            }

            OAuthCandidate candidate;
            candidate.organizationId = organizationId;
            candidate.accessToken = token;
            candidate.subscriptionType = JsonUtils::get_instance()->String(entry, "subscriptionType");
            candidate.rateLimitTier = JsonUtils::get_instance()->String(entry, "rateLimitTier");
            candidate.expiresAtMs = JsonUtils::get_instance()->Number(entry, "expiresAt", 0.0);
            candidate.scopeScore = CacheScopeScore(it.key());
            candidate.scopedCache = scopedCache;
            candidates.push_back(std::move(candidate));
        }

        return entriesPresent;
    }

    static json DecryptSafeStorageJson(
        const json& config,
        const char* key,
        const std::filesystem::path& userDataPath
    ) {
        if (!config.contains(key) || !config.at(key).is_string()) {
            return json();
        }

        const std::string encoded = config.at(key).get<std::string>();
        if (encoded.empty()) {
            return json();
        }

        std::vector<unsigned char> encrypted = Base64Decode(encoded);

        // Electron safeStorage is not always a raw DPAPI blob. Current Windows
        // builds may store the same v10/v11 AES-GCM envelope used by Chromium,
        // with its master key protected in Local State. Reuse the Electron blob
        // decoder so the Desktop OAuth cache remains readable while Claude is
        // open and its Cookies database is busy.
        std::vector<unsigned char> decrypted =
            DecryptElectronBlob(encrypted, userDataPath);

        std::string text(decrypted.begin(), decrypted.end());
        return JsonUtils::get_instance()->ParseRequired(text);
    }

    static std::optional<OAuthCandidate> ReadOAuthCache(
        const std::filesystem::path& userDataPath,
        const std::string& preferredOrganizationId,
        bool& cacheEntriesFound
    ) {
        cacheEntriesFound = false;
        const std::filesystem::path configPath = userDataPath / "config.json";

        if (!std::filesystem::exists(configPath)) {
            return std::nullopt;
        }

        json config = JsonUtils::get_instance()->ParseRequired(
            ReadTextFileWithSharedRead(configPath, 64ULL * 1024ULL * 1024ULL)
        );

        std::vector<OAuthCandidate> candidates;

        for (const auto& item : std::array<std::pair<const char*, bool>, 2>{
            std::pair<const char*, bool>{ "oauth:tokenCacheV2", true },
            std::pair<const char*, bool>{ "oauth:tokenCache", false }
        }) {
            if (!config.contains(item.first)) {
                continue;
            }

            cacheEntriesFound = CollectOAuthCandidates(
                DecryptSafeStorageJson(config, item.first, userDataPath),
                item.second,
                candidates
            ) || cacheEntriesFound;
        }

        if (candidates.empty()) {
            return std::nullopt;
        }

        using namespace std::chrono;
        const double nowMs = static_cast<double>(duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count());

        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [&](const OAuthCandidate& candidate) {
                    return candidate.expiresAtMs > 0.0 && candidate.expiresAtMs <= nowMs;
                }
            ),
            candidates.end()
        );

        if (!preferredOrganizationId.empty()) {
            candidates.erase(
                std::remove_if(
                    candidates.begin(),
                    candidates.end(),
                    [&](const OAuthCandidate& candidate) {
                        return candidate.organizationId != preferredOrganizationId;
                    }
                ),
                candidates.end()
            );
        }

        if (candidates.empty()) {
            return std::nullopt;
        }

        auto better = [](const OAuthCandidate& a, const OAuthCandidate& b) {
            if (a.scopeScore != b.scopeScore) return a.scopeScore < b.scopeScore;
            if (a.scopedCache != b.scopedCache) return !a.scopedCache && b.scopedCache;
            return a.expiresAtMs < b.expiresAtMs;
        };

        return *std::max_element(candidates.begin(), candidates.end(), better);
    }

    static long long CookieRecency(const CookieRecord& record) {
        return std::max(record.lastAccessUtc, record.creationUtc);
    }

    static DesktopIdentity ResolveIdentity(const std::filesystem::path& userDataPath) {
        DesktopIdentity identity;
        std::vector<std::filesystem::path> candidates = {
            userDataPath / "Network" / "Cookies",
            userDataPath / "Cookies"
        };

        std::filesystem::path cookiePath;
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                cookiePath = candidate;
                break;
            }
        }

        if (cookiePath.empty()) {
            identity.detail = "Claude Desktop Cookies database was not found";
            return identity;
        }

        std::vector<CookieRecord> records = ReadCookiesLive(cookiePath, userDataPath);
        identity.cookieRowsFound = !records.empty();

        std::vector<const CookieRecord*> organizations;
        std::vector<const CookieRecord*> sessions;

        for (const CookieRecord& record : records) {
            if (record.name == "lastActiveOrg" && IsOrganizationId(record.value)) {
                organizations.push_back(&record);
            }
            else if ((record.name == "sessionKeyV2" ||
                      record.name == "sessionKey" ||
                      record.name == "sessionKeyV3") &&
                     !record.value.empty()) {
                sessions.push_back(&record);
            }
        }

        const CookieRecord* activeOrganization = nullptr;

        // Prefer the newest active-organization cookie whose Claude root domain
        // also has at least one readable session cookie. This avoids combining
        // an old claude.ai organization with a newer claude.com session (or the
        // reverse) during domain migrations.
        for (const CookieRecord* organization : organizations) {
            const std::string root = CookieRoot(organization->hostKey);
            const bool hasMatchingSession = std::any_of(
                sessions.begin(),
                sessions.end(),
                [&](const CookieRecord* session) {
                    return CookieRoot(session->hostKey) == root;
                }
            );

            if (!hasMatchingSession) {
                continue;
            }

            if (!activeOrganization ||
                CookieRecency(*organization) > CookieRecency(*activeOrganization)) {
                activeOrganization = organization;
            }
        }

        if (!activeOrganization && !organizations.empty()) {
            activeOrganization = *std::max_element(
                organizations.begin(),
                organizations.end(),
                [](const CookieRecord* a, const CookieRecord* b) {
                    return CookieRecency(*a) < CookieRecency(*b);
                }
            );
        }

        if (activeOrganization) {
            identity.organizationId = activeOrganization->value;
            identity.rootDomain = CookieRoot(activeOrganization->hostKey);

            // A real browser sends all differently named session cookies for the
            // selected domain. Preserve that behavior instead of betting on only
            // one cookie generation. The order matches Claude Desktop's own list.
            for (const char* cookieName : { "sessionKeyV2", "sessionKey", "sessionKeyV3" }) {
                const CookieRecord* newest = nullptr;

                for (const CookieRecord* session : sessions) {
                    if (session->name != cookieName ||
                        CookieRoot(session->hostKey) != identity.rootDomain) {
                        continue;
                    }

                    if (!newest || CookieRecency(*session) > CookieRecency(*newest)) {
                        newest = session;
                    }
                }

                if (newest) {
                    identity.sessionCookies.emplace_back(newest->name, newest->value);
                }
            }
        }

        if (identity.organizationId.empty() && identity.sessionCookies.empty()) {
            identity.detail = records.empty()
                ? "Claude Desktop is not signed in"
                : "Claude Desktop login cookies were present but did not contain a valid active organization";
        }
        else if (identity.organizationId.empty()) {
            identity.detail = "Claude Desktop has session cookies but no valid lastActiveOrg cookie";
        }
        else if (identity.sessionCookies.empty()) {
            const bool sessionsOnAnotherDomain = std::any_of(
                sessions.begin(),
                sessions.end(),
                [&](const CookieRecord* session) {
                    return CookieRoot(session->hostKey) != identity.rootDomain;
                }
            );

            identity.detail = sessionsOnAnotherDomain
                ? "Claude Desktop organization and session cookies belonged to different domains"
                : "Claude Desktop has an active organization but its session cookies could not be read";
        }

        return identity;
    }


} // namespace

Result AcquireCurrentSession() {
    std::filesystem::path userDataPath;

    try {
        userDataPath = ClaudeDesktopUserDataPath();
    }
    catch (const std::exception& e) {
        Result result;
        result.kind = ResultKind::Error;
        result.detail = e.what();
        return result;
    }

    if (!std::filesystem::exists(userDataPath)) {
        Result result;
        result.kind = ResultKind::NoDesktopSession;
        result.detail = "Claude Desktop data directory was not found";
        return result;
    }

    DesktopIdentity identity;
    std::string cookieError;

    try {
        identity = ResolveIdentity(userDataPath);
    }
    catch (const std::exception& e) {
        cookieError = e.what();
    }

    bool cacheEntriesFound = false;
    std::optional<OAuthCandidate> oauth;
    std::string cacheError;

    try {
        oauth = ReadOAuthCache(userDataPath, identity.organizationId, cacheEntriesFound);
    }
    catch (const std::exception& e) {
        cacheError = e.what();
    }

    if (!identity.organizationId.empty() && !identity.sessionCookies.empty()) {
        std::string cookieHeader;

        for (const auto& [name, value] : identity.sessionCookies) {
            if (value.find('\r') != std::string::npos ||
                value.find('\n') != std::string::npos ||
                value.find(';') != std::string::npos) {
                Result result;
                result.kind = ResultKind::Error;
                result.detail = "Claude Desktop session cookie contains an invalid header character";
                return result;
            }

            if (!cookieHeader.empty()) {
                cookieHeader += "; ";
            }
            cookieHeader += name + "=" + value;
        }

        if (!cookieHeader.empty()) {
            cookieHeader += "; ";
        }
        cookieHeader += "lastActiveOrg=" + identity.organizationId;

        Result result;
        result.kind = ResultKind::Success;
        result.source = AuthSource::BrowserCookies;
        result.organizationId = identity.organizationId;
        result.baseUrl = identity.rootDomain == "claude.com"
            ? "https://claude.com"
            : "https://claude.ai";
        result.cookieHeader = std::move(cookieHeader);

        if (oauth) {
            result.accessToken = oauth->accessToken;
            result.subscriptionType = oauth->subscriptionType;
            result.rateLimitTier = oauth->rateLimitTier;
        }

        return result;
    }

    if (oauth) {
        Result result;
        result.kind = ResultKind::Success;
        result.source = AuthSource::OAuthCache;
        result.organizationId = oauth->organizationId;
        result.baseUrl = "https://claude.ai";
        result.accessToken = oauth->accessToken;
        result.subscriptionType = oauth->subscriptionType;
        result.rateLimitTier = oauth->rateLimitTier;
        return result;
    }

    if (!cookieError.empty() || !cacheError.empty() || identity.cookieRowsFound || cacheEntriesFound) {
        Result result;
        result.kind = ResultKind::Error;

        if (!cookieError.empty()) {
            result.detail = cookieError;
        }
        else if (!cacheError.empty()) {
            result.detail = cacheError;
        }
        else if (!identity.detail.empty()) {
            result.detail = identity.detail;
        }
        else {
            result.detail = "Claude Desktop credentials were present but no current session could be selected";
        }

        return result;
    }

    Result result;
    result.kind = ResultKind::NoDesktopSession;
    result.detail = identity.detail.empty() ? "Claude Desktop is not signed in" : identity.detail;
    return result;
}

} // namespace ClaudeDesktopAuth
