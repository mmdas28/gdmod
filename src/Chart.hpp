#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rp {

constexpr int kLaneCount = 2;
constexpr double kTicksPerSecond = 240.0;

struct Note {
    int lane = 0;
    int startTick = 0;
    int endTick = 0;
    double startTime = 0.0;
    double endTime = 0.0;
};

struct ChartInfo {
    int levelID = 0;
    std::string levelName;
    uint64_t levelHash = 0;
    std::string variant;
    int64_t savedAt = 0;
    double solveSeconds = 0.0;
    std::string source = "solver";
    std::string sourceFile;
    float targetPercent = 100.f;
    bool optimized = false;
    bool timingRelaxed = false;
    int precision = 2;
};

struct Chart {
    std::vector<Note> lanes[kLaneCount];
    std::vector<uint8_t> held;
    std::vector<double> tickTimes;
    bool twoPlayer = false;
    bool complete = false;
    bool verified = false;
    bool refined = false;
    float reachedPercent = 0.f;
    std::string key;
    ChartInfo info;

    bool empty() const;
    size_t noteCount() const;
    bool targetReached() const;

    void rebuildNotes();
    void rebuildHeldFromNotes();

    std::string serialize() const;
    static std::optional<Chart> deserialize(std::string const& text);
};

}
