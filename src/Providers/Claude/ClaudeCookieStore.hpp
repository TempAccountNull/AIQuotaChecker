#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ClaudeCookieStore {

    struct RawCookie {
        std::string name;
        std::string hostKey;
        std::string value;
        std::vector<unsigned char> encryptedValue;
        long long expiresUtc = 0;
        long long lastAccessUtc = 0;
        long long creationUtc = 0;
    };

    struct ReadResult {
        int databaseVersion = 0;
        std::vector<RawCookie> cookies;
    };

    // Reads the live Chromium Cookies SQLite database and its WAL directly.
    // The implementation is strictly read-only: it does not copy, snapshot,
    // rename, create, modify, or persist any credential-bearing file.
    ReadResult ReadLive(const std::filesystem::path& cookiePath);

}
