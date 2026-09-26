#pragma once

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rp {

struct ImportedInputs {
    std::vector<uint8_t> held;
    bool usesP2 = false;
    std::string format;
    std::string warning;
    int macroLevelID = 0;
    std::string macroLevelName;
};

struct ImportContext {
    bool twoPlayerLevel = false;
    bool flipTwoPlayer = false;
};

geode::Result<ImportedInputs> importInputsFromFile(std::filesystem::path const& path, ImportContext const& context);
geode::Result<ImportedInputs> importInputsFromData(std::string_view data, std::string_view fileName, ImportContext const& context);
std::vector<geode::utils::file::FilePickOptions::Filter> importFilters();

}
