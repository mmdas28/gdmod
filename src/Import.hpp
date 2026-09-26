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

geode::Result<ImportedInputs> importInputsFromFile(std::filesystem::path const& path);
geode::Result<ImportedInputs> importInputsFromData(std::string_view data, std::string_view fileName);
std::vector<geode::utils::file::FilePickOptions::Filter> importFilters();

}
