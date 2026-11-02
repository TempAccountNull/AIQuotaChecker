#pragma once

#include <cstdint>
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

    // Reads a text file another process may be writing without ever getting
    // in its way: its own handle, shared read/write/delete, read-only. tail
    // keeps the last maxBytes, starting at a line boundary; otherwise a larger
    // file reads as empty. Empty on any failure.
    std::string ReadSharedText(const std::filesystem::path& path, std::uint64_t maxBytes, bool tail = false);

}
