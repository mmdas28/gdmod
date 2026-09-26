#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rp {

struct ProgressSummary {
    uint64_t levelHash = 0;
    float percent = 0.f;
    double elapsedSeconds = 0.0;
    int64_t savedAt = 0;
    bool pathFound = false;
    float targetPercent = 100.f;
};

struct SolveProgress {
    uint64_t levelHash = 0;
    std::string settingsSignature;
    float targetPercent = 100.f;
    double elapsedSeconds = 0.0;
    uint64_t totalSteps = 0;
    uint64_t backtracks = 0;
    uint64_t prunes = 0;
    bool pathFound = false;
    int escalation = 0;
    bool fastAllowed = false;
    bool replayMode = false;
    int verifyFailures = 0;
    std::vector<uint8_t> held;
    std::vector<uint8_t> tried;
    std::vector<uint8_t> def;
    std::vector<uint8_t> flips;
    std::vector<uint8_t> best;
    float maxPercent = 0.f;
    int maxTick = 0;
    std::vector<uint64_t> dead;
    int64_t savedAt = 0;

    std::string serialize() const;
    static std::optional<SolveProgress> deserialize(std::string_view data);
    static std::optional<ProgressSummary> peekSummary(std::string_view data);
};

}
