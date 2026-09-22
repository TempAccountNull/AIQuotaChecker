// Deliberately independent of the GUI precompiled header for offline crypto tests.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include "ZAiCredentials.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib") // FOLDERID_Profile used by SHGetKnownFolderPath.

namespace ZAi::Credentials
{
    namespace
    {
        std::wstring Environment(const wchar_t* name)
        {
            DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
            if (!count || count > 65536) return {};
            std::wstring value(count, L'\0');
            DWORD actual = GetEnvironmentVariableW(name, value.data(), count);
            if (!actual || actual >= count) return {};
            value.resize(actual);
            return value;
        }

        std::string Utf8(const std::wstring& text)
        {
            if (text.empty()) return {};
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!count) return {};
            std::string result(static_cast<size_t>(count), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                result.data(), count, nullptr, nullptr)) return {};
            return result;
        }

        std::wstring TrimPath(std::wstring text)
        {
            const auto first = text.find_first_not_of(L" \t\r\n");
            return first == std::wstring::npos ? std::wstring{} :
                text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
        }

        std::filesystem::path OsHome()
        {
            // Match Node os.homedir() on Windows, not HOME or a migrated data path.
            const auto profile = Environment(L"USERPROFILE");
            if (!profile.empty()) return std::filesystem::path(profile);
            PWSTR folder = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &folder)))
                throw std::runtime_error("Could not resolve the Windows user profile for ZCode");
            std::filesystem::path result(folder);
            CoTaskMemFree(folder);
            return result;
        }

        Protocol::Json ReadObject(const std::filesystem::path& path)
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            if (ec) throw std::runtime_error("Could not access a ZCode settings or credential file");
            if (!exists) return Protocol::Json::object();
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > 8 * 1024 * 1024) throw std::runtime_error("ZCode JSON file is unreadable or too large");
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("Could not read a ZCode settings or credential file");
            std::string text(static_cast<size_t>(size), '\0');
            if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size())))
                throw std::runtime_error("ZCode JSON file changed while reading; refresh again");
            auto root = Protocol::Json::parse(text, nullptr, false);
            if (!text.empty()) SecureZeroMemory(text.data(), text.size());
            if (!root.is_object()) throw std::runtime_error("ZCode settings or credentials contain invalid JSON");
            return root;
        }

        std::string DefaultSecret()
        {
            auto overrideSecret = Environment(L"ZCODE_CREDENTIAL_SECRET");
            if (!overrideSecret.empty()) return Utf8(overrideSecret); // No trim: ZCode hashes the exact value.
            std::array<wchar_t, 257> buffer{};
            DWORD size = static_cast<DWORD>(buffer.size());
            const std::string username = GetUserNameW(buffer.data(), &size)
                ? Utf8(std::wstring(buffer.data())) : "unknown";
            return "zcode-credential-fallback:win32:" + Utf8(OsHome().wstring()) + ":" + username;
        }

        struct Algorithm
        {
            BCRYPT_ALG_HANDLE handle = nullptr;
            ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
        };
        struct Hash
        {
            BCRYPT_HASH_HANDLE handle = nullptr;
            ~Hash() { if (handle) BCryptDestroyHash(handle); }
        };
        struct Key
        {
            BCRYPT_KEY_HANDLE handle = nullptr;
            ~Key() { if (handle) BCryptDestroyKey(handle); }
        };
        struct SecretBytes
        {
            std::vector<unsigned char> value;
            explicit SecretBytes(size_t size) : value(size) {}
            ~SecretBytes() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
        };
    }

    std::optional<std::string> Decrypt(const std::string& value, const std::string& secret)
    {
        if (value.rfind("enc:", 0) != 0) return value; // Legacy plaintext store.
        const auto parsed = CredentialFormat::ParseEnvelope(value);
        if (!parsed || secret.empty() || secret.size() > 65536) return {};
        // enc:v1:base64url(iv).base64url(tag).base64url(ciphertext), SHA256(secret).
        Algorithm sha, aes;
        if (BCryptOpenAlgorithmProvider(&sha.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptOpenAlgorithmProvider(&aes.handle, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return {};
        DWORD hashObjectSize = 0, keyObjectSize = 0, received = 0;
        if (BCryptGetProperty(sha.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjectSize),
            sizeof(hashObjectSize), &received, 0) < 0) return {};
        SecretBytes hashObject(hashObjectSize), digest(32);
        Hash hash;
        if (BCryptCreateHash(sha.handle, &hash.handle, hashObject.value.data(), hashObjectSize, nullptr, 0, 0) < 0 ||
            BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(const_cast<char*>(secret.data())),
                static_cast<ULONG>(secret.size()), 0) < 0 ||
            BCryptFinishHash(hash.handle, digest.value.data(), 32, 0) < 0) return {};
        if (BCryptSetProperty(aes.handle, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0 ||
            BCryptGetProperty(aes.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&keyObjectSize),
                sizeof(keyObjectSize), &received, 0) < 0) return {};
        SecretBytes keyObject(keyObjectSize);
        Key key;
        if (BCryptGenerateSymmetricKey(aes.handle, &key.handle, keyObject.value.data(), keyObjectSize,
            digest.value.data(), static_cast<ULONG>(digest.value.size()), 0) < 0) return {};
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(parsed->iv.data());
        info.cbNonce = static_cast<ULONG>(parsed->iv.size());
        info.pbTag = const_cast<PUCHAR>(parsed->tag.data());
        info.cbTag = static_cast<ULONG>(parsed->tag.size());
        SecretBytes plaintext(parsed->ciphertext.size());
        ULONG length = 0;
        const auto status = BCryptDecrypt(key.handle, const_cast<PUCHAR>(parsed->ciphertext.data()),
            static_cast<ULONG>(parsed->ciphertext.size()), &info, nullptr, 0, plaintext.value.data(),
            static_cast<ULONG>(plaintext.value.size()), &length, 0);
        // Authentication failure must never return plaintext or fall back to ciphertext.
        if (status < 0 || length > plaintext.value.size()) return {};
        return std::string(reinterpret_cast<const char*>(plaintext.value.data()), length);
    }

    std::filesystem::path DataRoot()
    {
        auto home = TrimPath(Environment(L"ZCODE_DESKTOP_HOME_DIR"));
        if (home.empty()) home = TrimPath(Environment(L"HOME"));
        if (home.empty()) home = TrimPath(Environment(L"USERPROFILE"));
        if (home.empty()) home = OsHome().wstring();
        const auto setting = ReadObject(std::filesystem::path(home) / L".zcode" / L"v2" / L"setting.json");
        const auto configured = Protocol::String(setting, "dataBaseDir");
        if (!configured.empty()) return std::filesystem::u8path(configured) / L".zcode";
        auto base = TrimPath(Environment(L"ZCODE_DATA_BASE_DIR"));
        if (base.empty()) base = TrimPath(Environment(L"HOME"));
        if (base.empty()) base = OsHome().wstring();
        return std::filesystem::path(base) / L".zcode";
    }

    CredentialFormat::Selection Load()
    {
        CredentialFormat::Selection result;
        try {
            const auto root = DataRoot() / L"v2";
            const auto store = ReadObject(root / L"credentials.json");
            auto config = Protocol::Json::object();
            std::string configWarning;
            try { config = ReadObject(root / L"config.json"); }
            catch (...) { configWarning = "ZCode config.json could not be read; using only the active credential store."; }
            auto secret = DefaultSecret();
            // An existing empty store represents a signed-out desktop, not a
            // reason to recover stale provider keys from config.json.
            const bool allowLegacyConfig = !std::filesystem::exists(root / L"credentials.json");
            result = CredentialFormat::Select(store, config,
                [&](const std::string& value) { return Decrypt(value, secret); }, allowLegacyConfig);
            if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size());
            if (result.diagnostic.empty()) result.diagnostic = std::move(configWarning);
        }
        catch (const std::exception& error) { result.diagnostic = error.what(); }
        return result;
    }
}
