#pragma once

#include "Chart.hpp"
#include "SolverProgress.hpp"

#include <Geode/Geode.hpp>
#include <filesystem>
#include <optional>
#include <string>

namespace rp {

struct LevelRef {
    int levelID = 0;
    std::string folder;
    std::string variant;
    uint64_t levelHash = 0;
    std::string levelName;
    bool twoPlayer = false;
    bool fromStartPos = false;
    float startPosX = 0.f;
    std::string legacyKey;

    std::string key() const;
    std::string describe() const;
};

struct LevelPrefs {
    bool customRange = false;
    float rangeFrom = 0.f;
    float rangeTo = 100.f;
    float targetPercent = 100.f;
};

LevelRef makeLevelRef(PlayLayer* layer);

std::filesystem::path levelFolder(LevelRef const& ref);

std::optional<Chart> loadChart(LevelRef const& ref);
bool saveChart(LevelRef const& ref, Chart const& chart);
void deleteChart(LevelRef const& ref);
bool chartOutdated(LevelRef const& ref, Chart const& chart);

std::optional<std::string> loadProgressBlob(LevelRef const& ref);
void saveProgressBlob(LevelRef const& ref, std::string blob);
void deleteProgress(LevelRef const& ref);
std::optional<ProgressSummary> loadProgressSummary(LevelRef const& ref);

LevelPrefs loadPrefs(LevelRef const& ref);
void savePrefs(LevelRef const& ref, LevelPrefs const& prefs);

geode::Result<> exportChart(Chart const& chart, std::filesystem::path const& path);
std::string suggestedExportName(LevelRef const& ref);

}
