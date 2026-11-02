#include "Global.hpp"

#include "ClaudeDesktopAuth.hpp"
#include "ClaudeCookieStore.hpp"
#include "JsonUtils.hpp"
#include "Network.hpp"
#include "Text.hpp"

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

using json = nlohmann::json;

namespace ClaudeDesktopAuth {
namespace {

    constexpr long long kChromeEpochOffsetMicroseconds = 11644473600000000LL;
    constexpr std::uint64_t kMaximumJsonBytes = 64ULL * 1024ULL * 1024ULL;

    using Cookie = ClaudeCookieStore::RawCookie;

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

    static std::filesystem::path ClaudeDesktopUserDataPath() {
        // The first candidate that actually holds Desktop's sign-in state.
        for (const std::filesystem::path& root : UserDataCandidates()) {
            std::error_code ec;

            for (const std::filesystem::path& marker : { root / "config.json", root / "Network" / "Cookies", root / "Cookies" }) {
                if (std::filesystem::is_regular_file(marker, ec)) {
                    return root;
                }
            }
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
        SecureZeroMemory(output.pbData, output.cbData);
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

    static std::vector<unsigned char> LoadChromiumMasterKey(const std::filesystem::path& userDataPath) {
        // Fixed messages: a parse error would quote the file's bytes.
        const json root = JsonUtils::get_instance()->ParseOrNull(
            ClaudeCookieStore::ReadSharedText(userDataPath / "Local State", kMaximumJsonBytes)
        );

        if (!root.is_object()) {
            throw std::runtime_error("Claude Desktop Local State was not found or could not be read");
        }

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

    // The AES key for v10/v11 values, unwrapped at most once per
    // AcquireCurrentSession and only when a value actually needs it.
    class MasterKey final {
    public:
        explicit MasterKey(std::filesystem::path userDataPath)
            : m_userDataPath(std::move(userDataPath)) {
        }

        ~MasterKey() {
            SecureZeroMemory(m_key.data(), m_key.size());
        }

        MasterKey(const MasterKey&) = delete;
        MasterKey& operator=(const MasterKey&) = delete;

        // The first failure throws its real reason; later calls do not
        // retry the Local State read and DPAPI round trip.
        const std::vector<unsigned char>& Get() {
            if (!m_attempted) {
                m_attempted = true;
                m_key = LoadChromiumMasterKey(m_userDataPath);
            }

            if (m_key.empty()) {
                throw std::runtime_error("Claude Desktop encryption key is unavailable");
            }

            return m_key;
        }

    private:
        std::filesystem::path m_userDataPath;
        std::vector<unsigned char> m_key;
        bool m_attempted = false;
    };

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
        MasterKey& masterKey
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

            const unsigned char* nonce = encrypted.data() + prefixSize;
            const unsigned char* ciphertext = nonce + nonceSize;
            const size_t ciphertextSize = encrypted.size() - prefixSize - nonceSize - tagSize;
            const unsigned char* tag = encrypted.data() + encrypted.size() - tagSize;

            return AesGcmDecrypt(
                masterKey.Get(),
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
        const Cookie& cookie,
        int databaseVersion,
        MasterKey& masterKey
    ) {
        if (!cookie.value.empty()) {
            return cookie.value;
        }

        if (cookie.encryptedValue.empty()) {
            return {};
        }

        std::vector<unsigned char> decrypted =
            DecryptElectronBlob(cookie.encryptedValue, masterKey);

        // Chromium cookie database version 24+ binds the plaintext to
        // host_key by prefixing SHA-256(host_key). Remove the prefix only
        // after it verifies; otherwise preserve the decrypted bytes.
        if (databaseVersion >= 24 && decrypted.size() >= 32) {
            const std::array<unsigned char, 32> hostDigest = Sha256(cookie.hostKey);
            if (std::equal(hostDigest.begin(), hostDigest.end(), decrypted.begin())) {
                decrypted.erase(decrypted.begin(), decrypted.begin() + 32);
            }
        }

        return std::string(decrypted.begin(), decrypted.end());
    }

    // Unexpired cookies with their values decrypted in place.
    static std::vector<Cookie> ReadCookiesLive(
        const std::filesystem::path& cookiePath,
        MasterKey& masterKey
    ) {
        ClaudeCookieStore::ReadResult readResult =
            ClaudeCookieStore::ReadLive(cookiePath);

        std::vector<Cookie> records;
        std::string firstDecryptError;
        bool encryptedRowsSeen = false;

        for (Cookie& cookie : readResult.cookies) {
            if (!cookie.encryptedValue.empty()) {
                encryptedRowsSeen = true;
            }

            try {
                cookie.value = DecryptCookieValue(cookie, readResult.databaseVersion, masterKey);
            }
            catch (const std::exception& error) {
                if (firstDecryptError.empty()) {
                    firstDecryptError = error.what();
                }
                continue;
            }

            if (cookie.expiresUtc > 0 &&
                cookie.expiresUtc <= NowChromeMicroseconds()) {
                continue;
            }

            if (!cookie.value.empty()) {
                records.push_back(std::move(cookie));
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
        const std::string lower = Text::get_instance()->ToLowerCopy(hostKey);
        if (lower.find("claude.com") != std::string::npos) return "claude.com";
        if (lower.find("claude.ai") != std::string::npos) return "claude.ai";
        return {};
    }

    struct OAuthCandidate {
        std::string organizationId;
        std::string accountUuid;
        std::string accessToken;
        std::string subscriptionType;
        std::string rateLimitTier;
        double expiresAtMs = 0.0;
        int scopeScore = 0;
        bool scopedCache = false;
        bool canReadUsage = false;
    };

    // Desktop 4.4212 keys every cache entry by account:
    //   acct:<accountUuid>|<clientId>:<orgId>:<apiHost>:<scope>
    // Older builds wrote the same key without the "acct:...|" prefix.
    static std::string CacheAccountUuid(const std::string& cacheKey) {
        const size_t bar = cacheKey.find('|');
        return cacheKey.rfind("acct:", 0) == 0 && bar != std::string::npos ? cacheKey.substr(5, bar - 5) : std::string{};
    }

    static std::string CacheOrganizationId(std::string cacheKey) {
        if (cacheKey.rfind("acct:", 0) == 0) {
            const size_t bar = cacheKey.find('|');
            cacheKey = bar == std::string::npos ? std::string{} : cacheKey.substr(bar + 1);
        }

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
        // Desktop reads /api/oauth/usage with the Claude Code client's token.
        if (cacheKey.find("9d1c250a-e61b-44d9-88ed-5944d1962f5e") != std::string::npos) score += 8;
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
            candidate.accountUuid = CacheAccountUuid(it.key());
            // /api/oauth/usage rejects a token without user:profile (403).
            candidate.canReadUsage = it.key().find("user:profile") != std::string::npos;
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
        MasterKey& masterKey
    ) {
        if (!config.contains(key) || !config.at(key).is_string()) {
            return json();
        }

        const std::string encoded = config.at(key).get<std::string>();
        if (encoded.empty()) {
            return json();
        }

        // Electron safeStorage is not always a raw DPAPI blob. Current Windows
        // builds may store the same v10/v11 AES-GCM envelope used by Chromium,
        // with its master key protected in Local State. Reuse the Electron blob
        // decoder so the Desktop OAuth cache remains readable while Claude is
        // open and its Cookies database is busy.
        const std::vector<unsigned char> decrypted =
            DecryptElectronBlob(Base64Decode(encoded), masterKey);

        // A parse error message would quote the decrypted tokens.
        const json cache = JsonUtils::get_instance()->ParseOrNull(
            std::string(decrypted.begin(), decrypted.end())
        );

        if (cache.is_null()) {
            throw std::runtime_error("Claude Desktop token cache could not be parsed");
        }

        return cache;
    }

    // The subscription is an account fact, not a session one: every cache entry
    // for an organization carries it, and it stays true after the token itself
    // expires. Harvested separately from token selection so that a session
    // authenticated by cookies - which happens precisely BECAUSE no live token
    // is left - still knows which plan it is on.
    struct OAuthPlanHint
    {
        std::string subscriptionType;
        std::string rateLimitTier;
    };

    // Given an organization, only its own entries count: another account's
    // plan must never label this one.
    static OAuthPlanHint HarvestPlanHint(
        const std::vector<OAuthCandidate>& candidates,
        const std::string& organizationId
    ) {
        OAuthPlanHint hint;

        for (const OAuthCandidate& candidate : candidates) {
            if (!organizationId.empty() && candidate.organizationId != organizationId) {
                continue;
            }
            if (hint.subscriptionType.empty()) {
                hint.subscriptionType = candidate.subscriptionType;
            }
            if (hint.rateLimitTier.empty()) {
                hint.rateLimitTier = candidate.rateLimitTier;
            }
        }

        return hint;
    }

    // Every token-cache entry of the active account; config.json is read and
    // decrypted once per AcquireCurrentSession.
    static std::vector<OAuthCandidate> ReadOAuthCandidates(
        const std::filesystem::path& userDataPath,
        MasterKey& masterKey,
        bool& cacheEntriesFound
    ) {
        cacheEntriesFound = false;
        const std::filesystem::path configPath = userDataPath / "config.json";
        std::error_code ec;

        if (!std::filesystem::exists(configPath, ec)) {
            return {};
        }

        const json config = JsonUtils::get_instance()->ParseOrNull(
            ClaudeCookieStore::ReadSharedText(configPath, kMaximumJsonBytes)
        );

        if (!config.is_object()) {
            throw std::runtime_error("Claude Desktop config.json could not be read");
        }

        std::vector<OAuthCandidate> candidates;

        for (const auto& item : std::array<std::pair<const char*, bool>, 2>{
            std::pair<const char*, bool>{ "oauth:tokenCacheV2", true },
            std::pair<const char*, bool>{ "oauth:tokenCache", false }
        }) {
            if (!config.contains(item.first)) {
                continue;
            }

            cacheEntriesFound = CollectOAuthCandidates(
                DecryptSafeStorageJson(config, item.first, masterKey),
                item.second,
                candidates
            ) || cacheEntriesFound;
        }

        // Several accounts can be signed in; Desktop records the active one in
        // plaintext. Keep only its entries when any are present.
        const std::string activeAccount = JsonUtils::get_instance()->String(config, "lastKnownAccountUuid");

        if (!activeAccount.empty() &&
            std::any_of(candidates.begin(), candidates.end(), [&](const OAuthCandidate& candidate) {
                return candidate.accountUuid == activeAccount;
            })) {
            candidates.erase(
                std::remove_if(candidates.begin(), candidates.end(), [&](const OAuthCandidate& candidate) {
                    return !candidate.accountUuid.empty() && candidate.accountUuid != activeAccount;
                }),
                candidates.end()
            );
        }

        return candidates;
    }

    // The best live token that can read usage, for one organization or (with
    // an empty id) any. Desktop marks a token that drew a 401 dead by zeroing
    // expiresAt and only trusts now < expiresAt, so 0 or a missing expiry is
    // not usable.
    static std::optional<OAuthCandidate> BestUsableToken(
        const std::vector<OAuthCandidate>& candidates,
        const std::string& organizationId
    ) {
        using namespace std::chrono;
        const double nowMs = static_cast<double>(duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count());

        auto worse = [](const OAuthCandidate& a, const OAuthCandidate& b) {
            if (a.scopeScore != b.scopeScore) return a.scopeScore < b.scopeScore;
            if (a.scopedCache != b.scopedCache) return !a.scopedCache && b.scopedCache;
            return a.expiresAtMs < b.expiresAtMs;
        };

        const OAuthCandidate* best = nullptr;

        for (const OAuthCandidate& candidate : candidates) {
            if (candidate.expiresAtMs <= nowMs || !candidate.canReadUsage ||
                (!organizationId.empty() && candidate.organizationId != organizationId)) {
                continue;
            }

            if (!best || worse(*best, candidate)) {
                best = &candidate;
            }
        }

        return best ? std::optional<OAuthCandidate>(*best) : std::nullopt;
    }

    static long long CookieRecency(const Cookie& record) {
        return std::max(record.lastAccessUtc, record.creationUtc);
    }

    static DesktopIdentity ResolveIdentity(
        const std::filesystem::path& userDataPath,
        MasterKey& masterKey
    ) {
        DesktopIdentity identity;
        std::filesystem::path cookiePath;

        for (const auto& candidate : { userDataPath / "Network" / "Cookies", userDataPath / "Cookies" }) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                cookiePath = candidate;
                break;
            }
        }

        if (cookiePath.empty()) {
            identity.detail = "Claude Desktop Cookies database was not found";
            return identity;
        }

        std::vector<Cookie> records = ReadCookiesLive(cookiePath, masterKey);
        identity.cookieRowsFound = !records.empty();

        std::vector<const Cookie*> organizations;
        std::vector<const Cookie*> sessions;

        for (const Cookie& record : records) {
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

        const Cookie* activeOrganization = nullptr;

        // Prefer the newest active-organization cookie whose Claude root domain
        // also has at least one readable session cookie. This avoids combining
        // an old claude.ai organization with a newer claude.com session (or the
        // reverse) during domain migrations.
        for (const Cookie* organization : organizations) {
            const std::string root = CookieRoot(organization->hostKey);
            const bool hasMatchingSession = std::any_of(
                sessions.begin(),
                sessions.end(),
                [&](const Cookie* session) {
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
                [](const Cookie* a, const Cookie* b) {
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
                const Cookie* newest = nullptr;

                for (const Cookie* session : sessions) {
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
                [&](const Cookie* session) {
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

    std::error_code existsError;

    if (!std::filesystem::exists(userDataPath, existsError)) {
        Result result;
        result.kind = ResultKind::NoDesktopSession;
        result.detail = "Claude Desktop data directory was not found";
        return result;
    }

    MasterKey masterKey(userDataPath);
    DesktopIdentity identity;
    std::string cookieError;

    try {
        identity = ResolveIdentity(userDataPath, masterKey);
    }
    catch (const std::exception& e) {
        cookieError = e.what();
    }

    // Claude Desktop keeps its Cookies database open, and recent builds deny
    // shared reads outright (Win32 32). That is not a signed-out state: the
    // OAuth cache in config.json is still readable, it just needs to know which
    // organization is active. The newest plan-usage-history.json sample (the
    // gated tray poller's, see ReadLatestPlanUsage) names the organization that
    // was active when it was taken.
    if (identity.organizationId.empty()) {
        const PlanUsageSample planUsage = ReadLatestPlanUsage();

        if (planUsage.valid && !planUsage.organizationId.empty()) {
            identity.organizationId = planUsage.organizationId;
        }
    }

    bool cacheEntriesFound = false;
    std::vector<OAuthCandidate> candidates;
    std::string cacheError;

    try {
        candidates = ReadOAuthCandidates(userDataPath, masterKey, cacheEntriesFound);
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

        // Only this organization's own token and plan may ride along with its
        // cookie session; another account's would mislabel it.
        if (const std::optional<OAuthCandidate> oauth = BestUsableToken(candidates, identity.organizationId)) {
            result.accessToken = oauth->accessToken;
            result.subscriptionType = oauth->subscriptionType;
            result.rateLimitTier = oauth->rateLimitTier;
        }

        const OAuthPlanHint plan = HarvestPlanHint(candidates, identity.organizationId);
        if (result.subscriptionType.empty()) result.subscriptionType = plan.subscriptionType;
        if (result.rateLimitTier.empty()) result.rateLimitTier = plan.rateLimitTier;

        return result;
    }

    // Without cookies the token is the whole session. An organization with no
    // usable entry (stale sample, just-switched account) must not hide another
    // usable token; the result then follows that token's organization.
    std::optional<OAuthCandidate> oauth = BestUsableToken(candidates, identity.organizationId);

    if (!oauth && !identity.organizationId.empty()) {
        oauth = BestUsableToken(candidates, std::string{});
    }

    if (oauth) {
        const OAuthPlanHint plan = HarvestPlanHint(candidates, oauth->organizationId);

        Result result;
        result.kind = ResultKind::Success;
        result.source = AuthSource::OAuthCache;
        result.organizationId = oauth->organizationId;
        result.baseUrl = "https://claude.ai";
        result.accessToken = oauth->accessToken;
        result.subscriptionType = oauth->subscriptionType.empty()
            ? plan.subscriptionType : oauth->subscriptionType;
        result.rateLimitTier = oauth->rateLimitTier.empty()
            ? plan.rateLimitTier : oauth->rateLimitTier;
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

PlanUsageSample ReadLatestPlanUsage() {
    PlanUsageSample sample;

    std::filesystem::path userDataPath;

    try {
        userDataPath = ClaudeDesktopUserDataPath();
    }
    catch (const std::exception&) {
        return sample;
    }

    const json root = JsonUtils::get_instance()->ParseOrNull(
        ClaudeCookieStore::ReadSharedText(userDataPath / "plan-usage-history.json", kMaximumJsonBytes)
    );

    if (!root.is_object() || !root.contains("samples") || !root.at("samples").is_array()) {
        return sample;
    }

    const json& samples = root.at("samples");

    if (samples.empty()) {
        return sample;
    }

    // Short code -> the usage-window key the API (and ParseSnapshot) uses.
    // Taken verbatim from the ASAR's own mapping table.
    static const std::pair<const char*, const char*> kWindowCodes[] = {
        { "fh", "five_hour" },
        { "sd", "seven_day" },
        { "so", "seven_day_opus" },
        { "oa", "seven_day_oauth_apps" },
        { "cw", "seven_day_cowork" },
        { "om", "seven_day_omelette" },
        { "op", "omelette_promotional" },
        { "sn", "seven_day_sonnet" }
    };
    static const char* kExtraUsageCode = "xu";

    // Newest first; the writer appends, but do not assume ordering.
    const json* newest = nullptr;
    double newestTime = 0.0;

    for (const json& entry : samples) {
        if (!entry.is_object()) {
            continue;
        }

        const auto t = entry.find("t");

        if (t == entry.end() || !t->is_number()) {
            continue;
        }

        const double value = t->get<double>();

        if (!newest || value > newestTime) {
            newest = &entry;
            newestTime = value;
        }
    }

    if (!newest) {
        return sample;
    }

    sample.sampledAtUnixMs = static_cast<long long>(newestTime);

    const auto org = newest->find("org");

    if (org != newest->end() && org->is_string()) {
        sample.organizationId = org->get<std::string>();
    }

    const auto usage = newest->find("u");

    // Ordered series for the active organization, so a window boundary can be
    // located by where its own value falls.
    std::vector<std::pair<long long, const json*>> series;

    for (const json& entry : samples) {
        if (!entry.is_object()) {
            continue;
        }

        const auto t = entry.find("t");

        if (t == entry.end() || !t->is_number()) {
            continue;
        }

        if (!sample.organizationId.empty() &&
            JsonUtils::get_instance()->String(entry, "org") != sample.organizationId) {
            continue;
        }

        series.emplace_back(static_cast<long long>(t->get<double>()), &entry);
    }

    std::sort(series.begin(), series.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    // A usage window opens on the first request after the previous one lapsed,
    // so the series rises monotonically inside a window and falls at the
    // boundary. Walking back to the last fall (or the last zero) finds when the
    // current window opened; the window length then gives the reset instant.
    // Claude publishes no reset timestamp anywhere on disk, so this is the only
    // way to show one without a live request.
    const auto windowStartMs = [&](const char* code) -> long long {
        const auto valueAt = [&](size_t index) -> std::optional<double> {
            const auto u = series[index].second->find("u");
            const json* holder = (u != series[index].second->end() && u->is_object())
                ? &*u
                : series[index].second;
            const auto it = holder->find(code);

            if (it == holder->end() || !it->is_number()) {
                return std::nullopt;
            }

            return it->get<double>();
        };

        if (series.empty()) {
            return 0;
        }

        size_t i = series.size() - 1;
        const auto current = valueAt(i);

        if (!current || *current <= 0.0) {
            return 0;
        }

        while (i > 0) {
            const auto here = valueAt(i);
            const auto before = valueAt(i - 1);

            if (!here || !before) {
                break;
            }

            // A fall means the window reset between these two samples; a zero
            // means usage started here. Either way this sample opens it.
            if (*before > *here || *before <= 0.0) {
                break;
            }

            --i;
        }

        return series[i].first;
    };

    if (usage != newest->end() && usage->is_object()) {
        for (const auto& [code, key] : kWindowCodes) {
            const auto it = usage->find(code);

            if (it == usage->end() || !it->is_number()) {
                continue;
            }

            PlanUsageWindow window;
            window.key = key;
            window.usedPercent = it->get<double>();

            const long long startMs = windowStartMs(code);

            if (startMs > 0) {
                // five_hour is a 5h window; every seven_day_* variant is 7d.
                const long long lengthSeconds =
                    std::string(code) == "fh" ? 5 * 60 * 60 : 7 * 24 * 60 * 60;
                window.resetAtUnixSeconds = startMs / 1000 + lengthSeconds;
            }

            sample.windows.push_back(std::move(window));
        }

        const auto extra = usage->find(kExtraUsageCode);

        if (extra != usage->end() && extra->is_number()) {
            sample.hasExtraUsage = true;
            sample.extraUsagePercent = extra->get<double>();
        }
    }
    else {
        // Schema v1 kept fh/sd directly on the sample.
        for (const auto& [code, key] : { std::pair<const char*, const char*>{ "fh", "five_hour" },
                                         std::pair<const char*, const char*>{ "sd", "seven_day" } }) {
            const auto it = newest->find(code);

            if (it == newest->end() || !it->is_number()) {
                continue;
            }

            PlanUsageWindow window;
            window.key = key;
            window.usedPercent = it->get<double>();
            sample.windows.push_back(std::move(window));
        }
    }

    sample.valid = !sample.windows.empty() || !sample.organizationId.empty();
    return sample;
}

std::vector<std::filesystem::path> UserDataCandidates() {
    std::vector<std::filesystem::path> roots;

    const auto add = [&](const std::filesystem::path& path) {
        std::error_code ec;

        if (!path.empty() && std::filesystem::is_directory(path, ec) &&
            std::find(roots.begin(), roots.end(), path) == roots.end()) {
            roots.push_back(path);
        }
    };

    for (const char* variable : { "CLAUDE_DESKTOP_USER_DATA_DIR", "CLAUDE_USER_DATA_DIR" }) {
        const std::string value = Network::get_instance()->GetEnvText(variable);

        if (!value.empty()) {
            add(std::filesystem::path(value));
        }
    }

    const std::string localAppData = Network::get_instance()->GetEnvText("LOCALAPPDATA");
    const std::string appData = Network::get_instance()->GetEnvText("APPDATA");

    if (!localAppData.empty()) {
        const std::filesystem::path local(localAppData);

        for (const char* family : { "Claude_pzs8sxrjxfjjc", "AnthropicPBC.Claude_fnn82j28hfe8t" }) {
            add(local / "Packages" / family / "LocalCache" / "Roaming" / "Claude");
        }

        add(local / "Claude-Data");

        // Range-for would use the throwing increment.
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(local / "Claude-Data", ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            add(it->path());
        }
    }

    if (!appData.empty()) {
        add(std::filesystem::path(appData) / "Claude");
    }

    if (!localAppData.empty()) {
        add(std::filesystem::path(localAppData) / "Claude-3p");
    }

    return roots;
}

} // namespace ClaudeDesktopAuth
