#include "Chart.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <string_view>

using namespace geode::prelude;

namespace rp {

namespace {

constexpr int64_t kMaxTicks = 50'000'000;
constexpr size_t kMaxNotes = 4'000'000;

class Tokens {
public:
    explicit Tokens(std::string_view text) : m_text(text) {}

    bool next(std::string_view& out) {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t')) m_pos++;
        if (m_pos >= m_text.size()) return false;
        size_t start = m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != ' ' && m_text[m_pos] != '\t') m_pos++;
        out = m_text.substr(start, m_pos - start);
        return true;
    }

    bool done() {
        std::string_view tok;
        return !next(tok);
    }

private:
    std::string_view m_text;
    size_t m_pos = 0;
};

bool parseInt(std::string_view tok, int64_t& out, int base = 10) {
    if (tok.empty()) return false;
    if (tok.front() == '+') tok.remove_prefix(1);
    auto res = std::from_chars(tok.data(), tok.data() + tok.size(), out, base);
    return res.ec == std::errc() && res.ptr == tok.data() + tok.size();
}

bool parseHex64(std::string_view tok, uint64_t& out) {
    if (tok.empty() || tok.size() > 16) return false;
    auto res = std::from_chars(tok.data(), tok.data() + tok.size(), out, 16);
    return res.ec == std::errc() && res.ptr == tok.data() + tok.size();
}

class DoubleParser {
public:
    DoubleParser() {
        m_stream.imbue(std::locale::classic());
    }

    bool parse(std::string_view tok, double& out) {
        if (tok.empty() || tok.size() > 64) return false;
        m_stream.clear();
        m_stream.str(std::string(tok));
        double v = 0.0;
        m_stream >> v;
        if (m_stream.fail()) return false;
        char extra = 0;
        if (m_stream >> extra) return false;
        if (!std::isfinite(v)) return false;
        out = v;
        return true;
    }

private:
    std::istringstream m_stream;
};

bool nextInt(Tokens& toks, int64_t& out) {
    std::string_view tok;
    return toks.next(tok) && parseInt(tok, out);
}

bool nextDouble(Tokens& toks, DoubleParser& dp, double& out) {
    std::string_view tok;
    return toks.next(tok) && dp.parse(tok, out);
}

bool nextBool(Tokens& toks, bool& out) {
    int64_t v = 0;
    if (!nextInt(toks, v)) return false;
    out = v != 0;
    return true;
}

std::string singleLine(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

std::string_view trimView(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

void normalizeLane(std::vector<Note>& lane) {
    std::stable_sort(lane.begin(), lane.end(), [](Note const& a, Note const& b) {
        if (a.startTick != b.startTick) return a.startTick < b.startTick;
        return a.endTick < b.endTick;
    });
    if (lane.size() < 2) return;
    size_t out = 0;
    for (size_t i = 1; i < lane.size(); i++) {
        Note& cur = lane[out];
        Note const& n = lane[i];
        if (n.startTick < cur.endTick) {
            if (n.endTick > cur.endTick) {
                cur.endTick = n.endTick;
                cur.endTime = n.endTime;
            }
        }
        else {
            lane[++out] = n;
        }
    }
    lane.resize(out + 1);
}

}

bool Chart::empty() const {
    return lanes[0].empty() && lanes[1].empty() && held.empty();
}

size_t Chart::noteCount() const {
    size_t count = 0;
    for (auto const& lane : lanes) count += lane.size();
    return count;
}

bool Chart::targetReached() const {
    if (complete) return true;
    return info.targetPercent < 100.f && reachedPercent + 0.01f >= info.targetPercent;
}

void Chart::rebuildNotes() {
    for (auto& lane : lanes) lane.clear();
    if (held.empty()) return;

    auto timeAt = [&](int tick) -> double {
        if (tickTimes.empty()) return tick / kTicksPerSecond;
        if (tick < static_cast<int>(tickTimes.size())) return tickTimes[tick];
        double last = tickTimes.back();
        double step = tickTimes.size() >= 2 ? tickTimes.back() - tickTimes[tickTimes.size() - 2] : 1.0 / kTicksPerSecond;
        return last + step * (tick - static_cast<int>(tickTimes.size()) + 1);
    };

    int total = static_cast<int>(std::min<size_t>(held.size(), static_cast<size_t>(kMaxTicks) * 2));
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

void Chart::rebuildHeldFromNotes() {
    int total = 0;
    for (auto const& lane : lanes) {
        for (auto const& n : lane) total = std::max(total, n.endTick);
    }
    total = static_cast<int>(std::min<int64_t>(total, kMaxTicks));
    held.assign(static_cast<size_t>(total), 0);
    for (int lane = 0; lane < kLaneCount; lane++) {
        std::vector<std::pair<int, int>> spans;
        spans.reserve(lanes[lane].size());
        for (auto const& n : lanes[lane]) {
            int s = std::clamp(n.startTick, 0, total);
            int e = std::clamp(n.endTick, 0, total);
            if (e > s) spans.emplace_back(s, e);
        }
        std::sort(spans.begin(), spans.end());
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        int filled = 0;
        for (auto const& [s, e] : spans) {
            for (int t = std::max(s, filled); t < e; t++) held[t] |= mask;
            filled = std::max(filled, e);
        }
    }
}

std::string Chart::serialize() const {
    std::string out;
    out.reserve(256 + noteCount() * 64);
    out += "RHYTHMPATH 2\n";
    out += fmt::format("key {}\n", singleLine(key));
    out += fmt::format(
        "level {} {:016x} {} {:.17g} {} {}\n",
        info.levelID, info.levelHash, info.savedAt, info.solveSeconds, info.targetPercent, info.precision
    );
    out += fmt::format("name {}\n", singleLine(info.levelName));
    out += fmt::format("source {}\n", singleLine(info.source));
    out += fmt::format("file {}\n", singleLine(info.sourceFile));
    out += fmt::format("variant {}\n", singleLine(info.variant));
    out += fmt::format(
        "flags {} {} {} {} {} {} {}\n",
        twoPlayer ? 1 : 0, complete ? 1 : 0, verified ? 1 : 0, refined ? 1 : 0,
        info.optimized ? 1 : 0, info.timingRelaxed ? 1 : 0, reachedPercent
    );
    for (auto const& lane : lanes) {
        for (auto const& n : lane) {
            out += fmt::format("note {} {} {} {:.17g} {:.17g}\n", n.lane, n.startTick, n.endTick, n.startTime, n.endTime);
        }
    }
    if (!held.empty()) {
        out += fmt::format("held {}", held.size());
        size_t i = 0;
        while (i < held.size()) {
            size_t j = i + 1;
            while (j < held.size() && held[j] == held[i]) j++;
            out += fmt::format(" {} {}", static_cast<int>(held[i]), j - i);
            i = j;
        }
        out += "\n";
    }
    out += "end\n";
    return out;
}

std::optional<Chart> Chart::deserialize(std::string const& text) {
    std::string_view src(text);
    if (src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF && static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        src.remove_prefix(3);
    }

    size_t pos = 0;
    auto nextLine = [&](std::string_view& line) -> bool {
        if (pos >= src.size()) return false;
        size_t nl = src.find('\n', pos);
        if (nl == std::string_view::npos) nl = src.size();
        line = src.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return true;
    };

    int version = 0;
    std::string_view line;
    while (nextLine(line)) {
        if (trimView(line).empty()) continue;
        Tokens toks(line);
        std::string_view magic;
        int64_t ver = 0;
        if (!toks.next(magic) || magic != "RHYTHMPATH" || !nextInt(toks, ver) || !toks.done()) return std::nullopt;
        if (ver != 1 && ver != 2) return std::nullopt;
        version = static_cast<int>(ver);
        break;
    }
    if (version == 0) return std::nullopt;

    Chart chart;
    DoubleParser dp;
    bool sawEnd = false;
    bool sawHeld = false;
    size_t notes = 0;

    while (nextLine(line)) {
        if (trimView(line).empty()) continue;
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        line.remove_prefix(lead);
        size_t sp = line.find_first_of(" \t");
        std::string_view word = line.substr(0, sp);
        std::string_view rest = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
        Tokens toks(rest);

        if (word == "end") {
            sawEnd = true;
            break;
        }
        else if (word == "key") {
            chart.key = std::string(trimView(rest));
        }
        else if (word == "flags") {
            double reached = 0.0;
            if (!nextBool(toks, chart.twoPlayer) || !nextBool(toks, chart.complete) || !nextBool(toks, chart.verified) ||
                !nextBool(toks, chart.refined)) {
                return std::nullopt;
            }
            if (version >= 2) {
                if (!nextBool(toks, chart.info.optimized) || !nextBool(toks, chart.info.timingRelaxed)) return std::nullopt;
            }
            if (!nextDouble(toks, dp, reached)) return std::nullopt;
            chart.reachedPercent = static_cast<float>(std::clamp(reached, 0.0, 100.0));
        }
        else if (word == "note") {
            int64_t lane = 0, start = 0, end = 0;
            double startTime = 0.0, endTime = 0.0;
            if (!nextInt(toks, lane) || !nextInt(toks, start) || !nextInt(toks, end) || !nextDouble(toks, dp, startTime) ||
                !nextDouble(toks, dp, endTime)) {
                return std::nullopt;
            }
            if (lane < 0 || lane >= kLaneCount) return std::nullopt;
            if (start < 0 || end <= start || end > kMaxTicks) return std::nullopt;
            if (++notes > kMaxNotes) return std::nullopt;
            Note n;
            n.lane = static_cast<int>(lane);
            n.startTick = static_cast<int>(start);
            n.endTick = static_cast<int>(end);
            n.startTime = startTime;
            n.endTime = std::max(endTime, startTime);
            chart.lanes[n.lane].push_back(n);
        }
        else if (version >= 2 && word == "level") {
            int64_t id = 0, savedAt = 0, precision = 0;
            uint64_t hash = 0;
            double solveSeconds = 0.0, target = 100.0;
            std::string_view hashTok;
            if (!nextInt(toks, id) || !toks.next(hashTok) || !parseHex64(hashTok, hash) || !nextInt(toks, savedAt) ||
                !nextDouble(toks, dp, solveSeconds) || !nextDouble(toks, dp, target) || !nextInt(toks, precision)) {
                return std::nullopt;
            }
            if (id < INT32_MIN || id > INT32_MAX) return std::nullopt;
            chart.info.levelID = static_cast<int>(id);
            chart.info.levelHash = hash;
            chart.info.savedAt = savedAt;
            chart.info.solveSeconds = std::max(0.0, solveSeconds);
            chart.info.targetPercent = static_cast<float>(std::clamp(target, 0.0, 100.0));
            chart.info.precision = static_cast<int>(std::clamp<int64_t>(precision, 0, 2));
        }
        else if (version >= 2 && word == "name") {
            chart.info.levelName = std::string(rest);
        }
        else if (version >= 2 && word == "source") {
            chart.info.source = std::string(trimView(rest));
        }
        else if (version >= 2 && word == "file") {
            chart.info.sourceFile = std::string(rest);
        }
        else if (version >= 2 && word == "variant") {
            chart.info.variant = std::string(trimView(rest));
        }
        else if (version >= 2 && word == "held") {
            if (sawHeld) return std::nullopt;
            sawHeld = true;
            int64_t total = 0;
            if (!nextInt(toks, total) || total < 0 || total > kMaxTicks) return std::nullopt;
            std::vector<std::pair<uint8_t, int64_t>> runs;
            int64_t sum = 0;
            std::string_view tok;
            while (toks.next(tok)) {
                int64_t value = 0, count = 0;
                if (!parseInt(tok, value) || !nextInt(toks, count)) return std::nullopt;
                if (value < 0 || value > 3 || count <= 0 || count > total - sum) return std::nullopt;
                sum += count;
                runs.emplace_back(static_cast<uint8_t>(value), count);
            }
            if (sum != total) return std::nullopt;
            chart.held.clear();
            chart.held.reserve(static_cast<size_t>(total));
            for (auto const& [value, count] : runs) {
                chart.held.insert(chart.held.end(), static_cast<size_t>(count), value);
            }
        }
        else if (version == 1) {
            return std::nullopt;
        }
    }
    if (!sawEnd) return std::nullopt;

    for (auto& lane : chart.lanes) normalizeLane(lane);

    if (version == 1 || chart.held.empty()) {
        chart.rebuildHeldFromNotes();
    }
    else if (chart.lanes[0].empty() && chart.lanes[1].empty()) {
        chart.rebuildNotes();
    }
    return chart;
}

}
