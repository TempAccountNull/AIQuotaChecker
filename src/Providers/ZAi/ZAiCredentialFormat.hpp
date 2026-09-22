#pragma once

#include "ZAiProtocol.hpp"
#include <functional>
#include <optional>
#include <vector>

namespace ZAi::CredentialFormat
{
    using Json = nlohmann::json;
    using Decoder = std::function<std::optional<std::string>(const std::string&)>;

    struct Envelope
    {
        std::vector<unsigned char> iv, tag, ciphertext;
    };

    inline std::optional<std::vector<unsigned char>> Base64Url(const std::string& text)
    {
        if (text.empty() || text.size() > 65536) return {};
        size_t end = text.size();
        while (end && text[end - 1] == '=') --end;
        if (text.size() - end > 2 || end % 4 == 1) return {};
        if (end != text.size() && text.size() % 4 != 0) return {};
        unsigned int bits = 0, value = 0;
        std::vector<unsigned char> result;
        result.reserve(end * 3 / 4);
        for (size_t pos = 0; pos < end; ++pos) {
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            const int digit = c >= 'A' && c <= 'Z' ? c - 'A' :
                c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                c >= '0' && c <= '9' ? c - '0' + 52 :
                c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : -1;
            if (digit < 0) return {};
            value = (value << 6) | static_cast<unsigned int>(digit);
            bits += 6;
            if (bits >= 8) { bits -= 8; result.push_back(static_cast<unsigned char>((value >> bits) & 255)); }
        }
        if (bits && (value & ((1u << bits) - 1u)) != 0) return {};
        return result;
    }

    inline std::optional<Envelope> ParseEnvelope(const std::string& text)
    {
        if (text.rfind("enc:v1:", 0) != 0 || text.size() > 65536) return {};
        const size_t first = text.find('.', 7);
        const size_t second = first == std::string::npos ? first : text.find('.', first + 1);
        if (first == std::string::npos || second == std::string::npos || text.find('.', second + 1) != std::string::npos) return {};
        auto iv = Base64Url(text.substr(7, first - 7));
        auto tag = Base64Url(text.substr(first + 1, second - first - 1));
        auto ciphertext = Base64Url(text.substr(second + 1));
        if (!iv || iv->size() != 12 || !tag || tag->size() != 16 || !ciphertext || ciphertext->empty()) return {};
        return Envelope{ std::move(*iv), std::move(*tag), std::move(*ciphertext) };
    }

    inline std::string Token(std::string text)
    {
        text = Protocol::Trim(std::move(text));
        if (text.size() > 7 && Protocol::Lower(text.substr(0, 6)) == "bearer" &&
            (text[6] == ' ' || text[6] == '\t')) text = Protocol::Trim(text.substr(7));
        if (text.size() < 16 || text.size() > 16384 || Protocol::Lower(text).rfind("enc:", 0) == 0) return {};
        for (unsigned char ch : text) if (ch < 33 || ch > 126) return {};
        return text;
    }

    inline std::string EncodeComponent(const std::string& text)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string result;
        for (unsigned char ch : text) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
                ch == '!' || ch == '*' || ch == '\'' || ch == '(' || ch == ')') result += static_cast<char>(ch);
            else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
        }
        return result;
    }

    struct Selection
    {
        std::string startPlanJwt;
        std::string oauthAccessToken;
        std::vector<std::string> codingApiKeys;
        std::string diagnostic;
        bool currentZaiAccount = false;
        bool personalCodingSelected = false;
    };

    inline bool Enabled(const Json& provider)
    {
        const auto it = provider.find("enabled");
        return it != provider.end() && it->is_boolean() && it->get<bool>();
    }

    inline Selection Select(const Json& store, const Json& config, const Decoder& decode, bool allowLegacyConfig = true)
    {
        Selection result;
        bool decryptFailed = false;
        auto read = [&](const std::string& key) -> std::string {
            if (!store.is_object()) return {};
            const auto it = store.find(key);
            if (it == store.end() || !it->is_string()) return {};
            auto value = decode(it->get<std::string>());
            if (!value) { decryptFailed = true; return {}; }
            return Protocol::Trim(std::move(*value));
        };
        const bool marker = store.is_object() && store.contains("oauth:active_provider");
        bool modernStore = marker;
        if (store.is_object()) {
            for (auto it = store.begin(); it != store.end(); ++it)
                modernStore = modernStore || it.key().rfind("oauth:", 0) == 0 || it.key().rfind("account-provider:", 0) == 0;
        }
        if (marker) {
            result.currentZaiAccount = read("oauth:active_provider") == "zai";
            if (!result.currentZaiAccount) {
                result.diagnostic = decryptFailed ? "Could not decrypt ZCode credentials. Run under the same Windows account and ZCODE_CREDENTIAL_SECRET as ZCode."
                    : "The active ZCode account is not Z.Ai. Sign in to Z.Ai in ZCode.";
                return result;
            }
        }
        else if (modernStore) {
            result.diagnostic = "ZCode has no active signed-in account. Sign in to Z.Ai in ZCode.";
            return result; // Never resurrect a signed-out account from cached API keys/config.
        }
        result.startPlanJwt = Token(read("zcodejwttoken"));
        if (!modernStore && result.startPlanJwt.empty()) result.startPlanJwt = Token(read("zcodeJwtToken"));
        if (result.currentZaiAccount) {
            result.oauthAccessToken = Token(read("oauth:zai:access_token"));
            const auto profileText = read("oauth:zai:user_info");
            const auto profile = Json::parse(profileText, nullptr, false);
            std::string identity;
            if (profile.is_object()) {
                identity = Protocol::String(profile, "id");
                const auto id = profile.find("id");
                if (identity.empty() && id != profile.end() && id->is_number_integer()) identity = id->dump();
            }
            if (!identity.empty()) {
                // Do not enumerate keys from other accounts, team projects, or providers.
                const auto key = "account-provider:coding-plan:account:zai-individual-coding-plan:account:" +
                    EncodeComponent(identity) + ":api-key";
                const auto token = Token(read(key));
                if (!token.empty()) result.codingApiKeys.push_back(token);
            }
        }
        const auto providers = config.find("provider");
        if (config.is_object() && providers != config.end() && providers->is_object()) {
            size_t selectedCount = 0;
            bool personal = false;
            for (auto it = providers->begin(); it != providers->end(); ++it) {
                if (!it->is_object() || !Enabled(*it)) continue;
                ++selectedCount;
                const bool coding = it.key() == "account:zai-individual-coding-plan" || it.key() == "builtin:zai-coding-plan";
                const bool start = it.key() == "account:zai-start-plan" || it.key() == "builtin:zai-start-plan";
                personal = personal || coding;
                // Legacy, explicitly enabled Z.Ai providers only. Never scan arbitrary
                // JSON tokens, refresh tokens, transcripts, or other vendors' API keys.
                if (modernStore || !allowLegacyConfig || (!coding && !start)) continue;
                const auto options = it->find("options");
                const Json& values = options != it->end() && options->is_object() ? *options : *it;
                std::string raw = Protocol::String(values, "apiKey");
                if (raw.empty()) raw = Protocol::String(values, "api_key");
                const auto decoded = raw.empty() ? std::optional<std::string>{} : decode(raw);
                const auto token = decoded ? Token(*decoded) : std::string{};
                if (token.empty()) continue;
                if (start && result.startPlanJwt.empty()) result.startPlanJwt = token;
                if (coding && std::find(result.codingApiKeys.begin(), result.codingApiKeys.end(), token) == result.codingApiKeys.end())
                    result.codingApiKeys.push_back(token);
            }
            result.personalCodingSelected = result.currentZaiAccount && selectedCount == 1 && personal;
        }
        if (decryptFailed) result.diagnostic = "One or more ZCode credentials could not be decrypted. Check the Windows account and ZCODE_CREDENTIAL_SECRET used by ZCode.";
        return result;
    }
}
