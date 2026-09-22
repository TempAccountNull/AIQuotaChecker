#pragma once

#include "ZAiCredentialFormat.hpp"
#include <filesystem>

namespace ZAi::Credentials
{
    // This path resolver is also used for local context/database telemetry.
    std::filesystem::path DataRoot();
    CredentialFormat::Selection Load();
    // Public for the offline Windows crypto known-answer regression test.
    std::optional<std::string> Decrypt(const std::string& value, const std::string& secret);
}
