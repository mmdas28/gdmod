#include "Storage.hpp"

#include <Geode/utils/file.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace geode::prelude;

namespace rp {

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uintmax_t kMaxChartFileSize = 256ull * 1024 * 1024;
constexpr uintmax_t kMaxProgressFileSize = 512ull * 1024 * 1024;
constexpr uintmax_t kMaxPrefsFileSize = 1024 * 1024;

uint64_t fnv1a(std::string_view data, uint64_t h = kFnvOffset) {
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string_view gdView(gd::string const& str) {
    return std::string_view(str.c_str(), str.size());
}

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::filesystem::path chartFile(LevelRef const& ref) {
    return levelFolder(ref) / (ref.variant + ".rpchart");
}

std::filesystem::path progressFile(LevelRef const& ref) {
    return levelFolder(ref) / (ref.variant + ".rpsolve");
}

std::filesystem::path prefsFile(LevelRef const& ref) {
    return levelFolder(ref) / "prefs.json";
}

std::filesystem::path legacyChartFile(LevelRef const& ref) {
    return Mod::get()->getSaveDir() / "charts" / (ref.legacyKey + ".txt");
}

bool fileSizeOk(std::filesystem::path const& path, uintmax_t limit) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    auto size = std::filesystem::file_size(path, ec);
    return !ec && size <= limit;
}

std::optional<std::string> readFileCapped(std::filesystem::path const& path, uintmax_t limit) {
    if (!fileSizeOk(path, limit)) return std::nullopt;
    auto res = file::readString(path);
    if (!res) return std::nullopt;
    auto data = std::move(res).unwrap();
    if (data.size() > limit) return std::nullopt;
    return data;
}

Result<> writeAtomic(std::filesystem::path const& path, std::string_view data) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    auto res = file::writeString(tmp, data);
    if (!res) {
        std::filesystem::remove(tmp, ec);
        return Err(res.unwrapErr());
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(path, ec2);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec2);
            return Err(fmt::format("Unable to replace {}: {}", utils::string::pathToString(path.filename()), ec.message()));
        }
    }
    return Ok();
}

struct ProgressState {
    std::mutex ioMutex;
    std::mutex stateMutex;
    uint64_t counter = 0;
    struct Pending {
        uint64_t seq = 0;
        std::shared_ptr<std::string const> blob;
    };
    std::unordered_map<std::string, Pending> pending;
};

ProgressState& progressState() {
    static auto* state = new ProgressState();
    return *state;
}

std::shared_ptr<std::string const> pendingBlob(std::string const& key) {
    auto& st = progressState();
    std::lock_guard lock(st.stateMutex);
    auto it = st.pending.find(key);
    if (it == st.pending.end()) return nullptr;
    return it->second.blob;
}

float clampPercent(double v, float lo, float fallback) {
    if (!std::isfinite(v)) return fallback;
    return static_cast<float>(std::clamp(v, static_cast<double>(lo), 100.0));
}

}

std::string LevelRef::key() const {
    return folder + "/" + variant;
}

std::string LevelRef::describe() const {
    std::string out = fromStartPos ? "From a start position" : "From the start";
    if (twoPlayer) out += " (2-player)";
    return out;
}

LevelRef makeLevelRef(PlayLayer* layer) {
    LevelRef ref;
    ref.folder = "unknown";
    ref.variant = "start";
    if (!layer) return ref;

    auto level = layer->m_level;
    std::string_view levelString;
    std::string_view levelName;
    if (level) {
        ref.levelID = level->m_levelID.value();
        levelString = gdView(level->m_levelString);
        levelName = gdView(level->m_levelName);
        ref.levelName = std::string(levelName);
    }

    uint64_t contentHash = kFnvOffset;
    if (level) {
        contentHash = fnv1a(levelString, contentHash);
        if (levelString.empty()) contentHash = fnv1a(levelName, contentHash);
    }
    ref.levelHash = contentHash;

    if (ref.levelID > 0) {
        ref.folder = std::to_string(ref.levelID);
    }
    else {
        uint64_t local = fnv1a(levelName);
        local = fnv1a("\n", local);
        local = fnv1a(levelString, local);
        ref.folder = fmt::format("local-{:016x}", local);
    }

    ref.twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    bool flip = false;
    if (ref.twoPlayer) {
        auto gm = GameManager::sharedState();
        flip = gm && gm->getGameVariable("0010");
    }

    uint64_t legacy = contentHash;
    if (ref.twoPlayer) legacy = fnv1a(flip ? "flip:1" : "flip:0", legacy);

    if (auto sp = layer->m_startPosObject) {
        auto pos = sp->getPosition();
        ref.fromStartPos = true;
        ref.startPosX = pos.x;
        ref.variant = fmt::format("sp-{:.1f}-{:.1f}", pos.x, pos.y);
        legacy = fnv1a(fmt::format("sp:{:.3f},{:.3f}", pos.x, pos.y), legacy);
    }
    if (ref.twoPlayer) ref.variant += flip ? "-flip" : "-noflip";

    ref.legacyKey = fmt::format("{}_{:016x}", ref.levelID, legacy);
    return ref;
}

std::filesystem::path levelFolder(LevelRef const& ref) {
    return Mod::get()->getSaveDir() / "levels" / ref.folder;
}

static void fillInfoFromRef(Chart& chart, LevelRef const& ref) {
    chart.key = ref.key();
    chart.info.levelID = ref.levelID;
    chart.info.levelName = ref.levelName;
    chart.info.variant = ref.variant;
    if (chart.info.levelHash == 0) chart.info.levelHash = ref.levelHash;
    if (chart.info.source.empty()) chart.info.source = "solver";
    if (chart.info.savedAt == 0) chart.info.savedAt = nowSeconds();
}

std::optional<Chart> loadChart(LevelRef const& ref) {
    auto path = chartFile(ref);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        if (auto text = readFileCapped(path, kMaxChartFileSize)) {
            if (auto chart = Chart::deserialize(*text)) {
                chart->key = ref.key();
                if (chart->info.variant.empty()) chart->info.variant = ref.variant;
                if (chart->info.levelID == 0) chart->info.levelID = ref.levelID;
                if (chart->info.levelName.empty()) chart->info.levelName = ref.levelName;
                return chart;
            }
        }
        log::warn("Could not read saved chart {}", utils::string::pathToString(path));
    }

    if (ref.legacyKey.empty()) return std::nullopt;
    auto legacy = legacyChartFile(ref);
    ec.clear();
    if (!std::filesystem::exists(legacy, ec)) return std::nullopt;
    auto text = readFileCapped(legacy, kMaxChartFileSize);
    if (!text) return std::nullopt;
    auto chart = Chart::deserialize(*text);
    if (!chart) {
        log::warn("Could not read legacy chart {}", utils::string::pathToString(legacy));
        return std::nullopt;
    }
    fillInfoFromRef(*chart, ref);
    chart->info.levelHash = ref.levelHash;
    saveChart(ref, *chart);
    return chart;
}

bool saveChart(LevelRef const& ref, Chart const& chart) {
    std::string text;
    if (chart.key == ref.key()) {
        text = chart.serialize();
    }
    else {
        Chart copy = chart;
        copy.key = ref.key();
        text = copy.serialize();
    }
    auto res = writeAtomic(chartFile(ref), text);
    if (!res) {
        log::warn("Failed to save chart {}: {}", ref.key(), res.unwrapErr());
        return false;
    }
    return true;
}

void deleteChart(LevelRef const& ref) {
    std::error_code ec;
    std::filesystem::remove(chartFile(ref), ec);
    if (!ref.legacyKey.empty()) {
        ec.clear();
        std::filesystem::remove(legacyChartFile(ref), ec);
    }
}

bool chartOutdated(LevelRef const& ref, Chart const& chart) {
    return chart.info.levelHash != 0 && chart.info.levelHash != ref.levelHash;
}

std::optional<std::string> loadProgressBlob(LevelRef const& ref) {
    auto path = progressFile(ref);
    if (auto blob = pendingBlob(ref.key())) return *blob;
    return readFileCapped(path, kMaxProgressFileSize);
}

void saveProgressBlob(LevelRef const& ref, std::string blob) {
    auto path = progressFile(ref);
    auto key = ref.key();
    auto shared = std::make_shared<std::string const>(std::move(blob));
    auto& st = progressState();
    uint64_t seq = 0;
    {
        std::lock_guard lock(st.stateMutex);
        seq = ++st.counter;
        st.pending[key] = ProgressState::Pending{seq, shared};
    }
    auto work = [path, key, seq]() {
        auto& st = progressState();
        std::lock_guard io(st.ioMutex);
        std::shared_ptr<std::string const> data;
        {
            std::lock_guard lock(st.stateMutex);
            auto it = st.pending.find(key);
            if (it == st.pending.end() || it->second.seq != seq) return;
            data = it->second.blob;
        }
        auto res = writeAtomic(path, *data);
        if (!res) log::warn("Failed to save solver progress: {}", res.unwrapErr());
        {
            std::lock_guard lock(st.stateMutex);
            auto it = st.pending.find(key);
            if (it != st.pending.end() && it->second.seq == seq) st.pending.erase(it);
        }
    };
    try {
        std::thread(work).detach();
    }
    catch (...) {
        work();
    }
}

void deleteProgress(LevelRef const& ref) {
    auto path = progressFile(ref);
    auto key = ref.key();
    auto& st = progressState();
    std::lock_guard io(st.ioMutex);
    {
        std::lock_guard lock(st.stateMutex);
        st.pending.erase(key);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::optional<ProgressSummary> loadProgressSummary(LevelRef const& ref) {
    auto path = progressFile(ref);
    if (auto blob = pendingBlob(ref.key())) return SolveProgress::peekSummary(*blob);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    auto data = readFileCapped(path, kMaxProgressFileSize);
    if (!data) return std::nullopt;
    return SolveProgress::peekSummary(*data);
}

LevelPrefs loadPrefs(LevelRef const& ref) {
    LevelPrefs prefs;
    auto path = prefsFile(ref);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return prefs;
    auto text = readFileCapped(path, kMaxPrefsFileSize);
    if (!text) return prefs;
    auto parsed = matjson::parse(*text);
    if (!parsed) return prefs;
    matjson::Value const json = std::move(parsed).unwrap();
    if (!json.isObject()) return prefs;
    if (auto v = json["customRange"].asBool()) prefs.customRange = v.unwrap();
    if (auto v = json["rangeFrom"].asDouble()) prefs.rangeFrom = clampPercent(v.unwrap(), 0.f, prefs.rangeFrom);
    if (auto v = json["rangeTo"].asDouble()) prefs.rangeTo = clampPercent(v.unwrap(), 0.f, prefs.rangeTo);
    if (auto v = json["targetPercent"].asDouble()) prefs.targetPercent = clampPercent(v.unwrap(), 1.f, prefs.targetPercent);
    return prefs;
}

void savePrefs(LevelRef const& ref, LevelPrefs const& prefs) {
    auto json = matjson::makeObject({
        {"customRange", prefs.customRange},
        {"rangeFrom", static_cast<double>(prefs.rangeFrom)},
        {"rangeTo", static_cast<double>(prefs.rangeTo)},
        {"targetPercent", static_cast<double>(prefs.targetPercent)},
    });
    auto res = writeAtomic(prefsFile(ref), json.dump());
    if (!res) log::warn("Failed to save level preferences {}: {}", ref.folder, res.unwrapErr());
}

Result<> exportChart(Chart const& chart, std::filesystem::path const& path) {
    if (path.empty()) return Err("No file selected");
    auto target = path;
    if (!target.has_extension()) target += ".rpchart";
    auto res = writeAtomic(target, chart.serialize());
    if (!res) return Err(fmt::format("Could not write the chart: {}", res.unwrapErr()));
    return Ok();
}

std::string suggestedExportName(LevelRef const& ref) {
    std::string name;
    for (unsigned char c : ref.levelName) {
        if (name.size() >= 48) break;
        if (std::isalnum(c) || c == '-' || c == '_') name += static_cast<char>(c);
        else if (c == ' ' && !name.empty() && name.back() != '_') name += '_';
    }
    while (!name.empty() && name.back() == '_') name.pop_back();
    if (name.empty()) name = ref.folder.empty() ? "chart" : ref.folder;
    return name + "-" + ref.variant + ".rpchart";
}

}
