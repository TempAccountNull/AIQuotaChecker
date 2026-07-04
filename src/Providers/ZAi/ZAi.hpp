#pragma once

#include <string>
#include <vector>

namespace ZAi
{
    struct UsageBar
    {
        std::string label;
        std::string sublabel;
        std::string resetText;
        float usedPercent = 0.0f;
        long long resetAtUnixSeconds = 0;
        bool valid = false;
        bool red = false;
        bool white = false;
        bool green = false;
        bool thin = false;
    };

    struct DetailRow
    {
        std::string leftValue;
        std::string leftLabel;
        std::string rightValue;
        std::string rightLabel;
    };

    struct Snapshot
    {
        std::string plan = "Z.Ai";
        std::string statusText = "Waiting for Z.Ai auto refresh";
        std::string lastUpdated = "never";
        std::vector<UsageBar> bars;
        std::vector<DetailRow> details;
    };

    Snapshot FetchSnapshot();
}
