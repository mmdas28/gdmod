#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rp {

constexpr int kLaneCount = 2;

struct Note {
    int lane = 0;
    int startTick = 0;
    int endTick = 0;
    double startTime = 0.0;
    double endTime = 0.0;
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

    bool empty() const {
        return lanes[0].empty() && lanes[1].empty() && held.empty();
    }

    void rebuildNotes();

    std::string serialize() const;
    static std::optional<Chart> deserialize(std::string const& text);
};

std::string levelKey(PlayLayer* layer);
std::filesystem::path chartPath(std::string const& key);
std::optional<Chart> loadChart(std::string const& key);
void saveChart(Chart const& chart);
void deleteChart(std::string const& key);

}
