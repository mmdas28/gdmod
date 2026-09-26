#include "Chart.hpp"

#include <Geode/utils/file.hpp>
#include <algorithm>
#include <sstream>

using namespace geode::prelude;

namespace rp {

void Chart::rebuildNotes() {
    for (auto& lane : lanes) lane.clear();
    if (held.empty()) return;

    auto timeAt = [&](int tick) -> double {
        if (tickTimes.empty()) return tick / 240.0;
        if (tick < static_cast<int>(tickTimes.size())) return tickTimes[tick];
        double last = tickTimes.back();
        double step = tickTimes.size() >= 2 ? tickTimes.back() - tickTimes[tickTimes.size() - 2] : 1.0 / 240.0;
        return last + step * (tick - static_cast<int>(tickTimes.size()) + 1);
    };

    int total = static_cast<int>(held.size());
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        int start = -1;
        for (int t = 0; t <= total; t++) {
            bool down = t < total && (held[t] & mask);
            if (down && start < 0) {
                start = t;
            }
            else if (!down && start >= 0) {
                Note note;
                note.lane = lane;
                note.startTick = start;
                note.endTick = t;
                note.startTime = timeAt(start);
                note.endTime = timeAt(t);
                lanes[lane].push_back(note);
                start = -1;
            }
        }
    }
}

std::string Chart::serialize() const {
    std::ostringstream out;
    out.precision(17);
    out << "RHYTHMPATH 1\n";
    out << "key " << key << "\n";
    out << "flags " << twoPlayer << " " << complete << " " << verified << " " << refined << " " << reachedPercent << "\n";
    for (auto const& lane : lanes) {
        for (auto const& n : lane) {
            out << "note " << n.lane << " " << n.startTick << " " << n.endTick << " " << n.startTime << " " << n.endTime << "\n";
        }
    }
    out << "end\n";
    return out.str();
}

std::optional<Chart> Chart::deserialize(std::string const& text) {
    std::istringstream in(text);
    std::string header;
    int version = 0;
    if (!(in >> header >> version) || header != "RHYTHMPATH" || version != 1) return std::nullopt;

    Chart chart;
    bool sawEnd = false;
    std::string word;
    while (in >> word) {
        if (word == "key") {
            in >> chart.key;
        }
        else if (word == "flags") {
            in >> chart.twoPlayer >> chart.complete >> chart.verified >> chart.refined >> chart.reachedPercent;
        }
        else if (word == "note") {
            Note n;
            in >> n.lane >> n.startTick >> n.endTick >> n.startTime >> n.endTime;
            if (!in || n.lane < 0 || n.lane >= kLaneCount) return std::nullopt;
            chart.lanes[n.lane].push_back(n);
        }
        else if (word == "end") {
            sawEnd = true;
            break;
        }
        else {
            return std::nullopt;
        }
        if (!in) return std::nullopt;
    }
    if (!sawEnd) return std::nullopt;
    for (auto& lane : chart.lanes) {
        std::sort(lane.begin(), lane.end(), [](Note const& a, Note const& b) { return a.startTick < b.startTick; });
    }
    return chart;
}

static uint64_t fnv1a(std::string_view data, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string levelKey(PlayLayer* layer) {
    auto level = layer->m_level;
    int id = level ? level->m_levelID.value() : 0;
    uint64_t h = 1469598103934665603ull;
    if (level) {
        std::string_view str(level->m_levelString.c_str(), level->m_levelString.size());
        h = fnv1a(str, h);
        if (str.empty()) {
            std::string_view name(level->m_levelName.c_str(), level->m_levelName.size());
            h = fnv1a(name, h);
        }
    }
    if (layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode) {
        h = fnv1a(GameManager::sharedState()->getGameVariable("0010") ? "flip:1" : "flip:0", h);
    }
    if (auto sp = layer->m_startPosObject) {
        auto pos = sp->getPosition();
        h = fnv1a(fmt::format("sp:{:.3f},{:.3f}", pos.x, pos.y), h);
    }
    return fmt::format("{}_{:016x}", id, h);
}

std::filesystem::path chartPath(std::string const& key) {
    return Mod::get()->getSaveDir() / "charts" / (key + ".txt");
}

std::optional<Chart> loadChart(std::string const& key) {
    auto path = chartPath(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    auto res = file::readString(path);
    if (!res) return std::nullopt;
    auto chart = Chart::deserialize(res.unwrap());
    if (!chart || chart->key != key) return std::nullopt;
    return chart;
}

void saveChart(Chart const& chart) {
    if (chart.key.empty()) return;
    auto path = chartPath(chart.key);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto res = file::writeString(path, chart.serialize());
    if (!res) {
        log::warn("Failed to save chart {}: {}", chart.key, res.unwrapErr());
    }
}

void deleteChart(std::string const& key) {
    std::error_code ec;
    std::filesystem::remove(chartPath(key), ec);
}

}
