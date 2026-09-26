#include "Import.hpp"

#include "Chart.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>

using namespace geode::prelude;

namespace rp {

namespace {

constexpr int64_t kMaxTicks = 50'000'000;
constexpr uintmax_t kMaxImportBytes = 64ull * 1024 * 1024;
constexpr int kMaxDepth = 64;
constexpr size_t kMaxValues = 12'000'000;
constexpr uint64_t kMaxMapKeys = 64;
constexpr char const* kUnsupported =
    "Unsupported file format. Import a Rhythm Path chart (.rpchart) or a GDR macro (.gdr, .gdr.json).";

class MsgPackReader {
public:
    explicit MsgPackReader(std::string_view data) : m_data(data) {}

    std::optional<matjson::Value> readRoot(std::initializer_list<std::string_view> keep) {
        m_keep = keep;
        uint8_t tag = 0;
        if (!peek(tag)) return std::nullopt;
        if (!((tag >= 0x80 && tag <= 0x8f) || tag == 0xde || tag == 0xdf)) return std::nullopt;
        matjson::Value out;
        if (!readValue(out, 0)) return std::nullopt;
        return out;
    }

private:
    enum class Kind { Nil, Bool, Int, UInt, Float, Str, Bin, Ext, Array, Map };

    struct Head {
        Kind kind = Kind::Nil;
        uint64_t len = 0;
        bool b = false;
        int64_t i = 0;
        uint64_t u = 0;
        double f = 0.0;
    };

    std::string_view m_data;
    size_t m_pos = 0;
    size_t m_values = 0;
    std::initializer_list<std::string_view> m_keep;

    size_t remaining() const {
        return m_data.size() - m_pos;
    }

    bool peek(uint8_t& out) const {
        if (m_pos >= m_data.size()) return false;
        out = static_cast<uint8_t>(m_data[m_pos]);
        return true;
    }

    bool readBE(size_t n, uint64_t& out) {
        if (remaining() < n) return false;
        uint64_t v = 0;
        for (size_t k = 0; k < n; k++) v = (v << 8) | static_cast<uint8_t>(m_data[m_pos + k]);
        m_pos += n;
        out = v;
        return true;
    }

    bool take(uint64_t n, std::string_view& out) {
        if (n > remaining()) return false;
        out = m_data.substr(m_pos, static_cast<size_t>(n));
        m_pos += static_cast<size_t>(n);
        return true;
    }

    bool skipBytes(uint64_t n) {
        std::string_view unused;
        return take(n, unused);
    }

    bool readHead(Head& h) {
        uint64_t tag = 0;
        if (!readBE(1, tag)) return false;
        uint64_t v = 0;
        if (tag <= 0x7f) {
            h.kind = Kind::Int;
            h.i = static_cast<int64_t>(tag);
            return true;
        }
        if (tag >= 0xe0) {
            h.kind = Kind::Int;
            h.i = static_cast<int64_t>(static_cast<int8_t>(static_cast<uint8_t>(tag)));
            return true;
        }
        if (tag >= 0x80 && tag <= 0x8f) {
            h.kind = Kind::Map;
            h.len = tag & 0x0f;
            return true;
        }
        if (tag >= 0x90 && tag <= 0x9f) {
            h.kind = Kind::Array;
            h.len = tag & 0x0f;
            return true;
        }
        if (tag >= 0xa0 && tag <= 0xbf) {
            h.kind = Kind::Str;
            h.len = tag & 0x1f;
            return true;
        }
        switch (tag) {
            case 0xc0: h.kind = Kind::Nil; return true;
            case 0xc2: h.kind = Kind::Bool; h.b = false; return true;
            case 0xc3: h.kind = Kind::Bool; h.b = true; return true;
            case 0xc4: h.kind = Kind::Bin; return readBE(1, h.len);
            case 0xc5: h.kind = Kind::Bin; return readBE(2, h.len);
            case 0xc6: h.kind = Kind::Bin; return readBE(4, h.len);
            case 0xc7: h.kind = Kind::Ext; return readBE(1, h.len) && skipBytes(1);
            case 0xc8: h.kind = Kind::Ext; return readBE(2, h.len) && skipBytes(1);
            case 0xc9: h.kind = Kind::Ext; return readBE(4, h.len) && skipBytes(1);
            case 0xca: {
                if (!readBE(4, v)) return false;
                uint32_t bits = static_cast<uint32_t>(v);
                float fv = 0.f;
                std::memcpy(&fv, &bits, sizeof(fv));
                h.kind = Kind::Float;
                h.f = fv;
                return true;
            }
            case 0xcb: {
                if (!readBE(8, v)) return false;
                double dv = 0.0;
                std::memcpy(&dv, &v, sizeof(dv));
                h.kind = Kind::Float;
                h.f = dv;
                return true;
            }
            case 0xcc:
            case 0xcd:
            case 0xce:
            case 0xcf: {
                size_t n = size_t(1) << (tag - 0xcc);
                if (!readBE(n, v)) return false;
                h.kind = Kind::UInt;
                h.u = v;
                return true;
            }
            case 0xd0: if (!readBE(1, v)) return false; h.kind = Kind::Int; h.i = static_cast<int8_t>(v); return true;
            case 0xd1: if (!readBE(2, v)) return false; h.kind = Kind::Int; h.i = static_cast<int16_t>(v); return true;
            case 0xd2: if (!readBE(4, v)) return false; h.kind = Kind::Int; h.i = static_cast<int32_t>(v); return true;
            case 0xd3: if (!readBE(8, v)) return false; h.kind = Kind::Int; h.i = static_cast<int64_t>(v); return true;
            case 0xd4: h.kind = Kind::Ext; h.len = 1; return skipBytes(1);
            case 0xd5: h.kind = Kind::Ext; h.len = 2; return skipBytes(1);
            case 0xd6: h.kind = Kind::Ext; h.len = 4; return skipBytes(1);
            case 0xd7: h.kind = Kind::Ext; h.len = 8; return skipBytes(1);
            case 0xd8: h.kind = Kind::Ext; h.len = 16; return skipBytes(1);
            case 0xd9: h.kind = Kind::Str; return readBE(1, h.len);
            case 0xda: h.kind = Kind::Str; return readBE(2, h.len);
            case 0xdb: h.kind = Kind::Str; return readBE(4, h.len);
            case 0xdc: h.kind = Kind::Array; return readBE(2, h.len);
            case 0xdd: h.kind = Kind::Array; return readBE(4, h.len);
            case 0xde: h.kind = Kind::Map; return readBE(2, h.len);
            case 0xdf: h.kind = Kind::Map; return readBE(4, h.len);
            default: return false;
        }
    }

    bool skipAfterHead(Head const& h, int depth) {
        if (depth > kMaxDepth) return false;
        switch (h.kind) {
            case Kind::Str:
            case Kind::Bin:
            case Kind::Ext:
                return skipBytes(h.len);
            case Kind::Array:
                if (h.len > remaining()) return false;
                for (uint64_t k = 0; k < h.len; k++) {
                    if (!skipValue(depth + 1)) return false;
                }
                return true;
            case Kind::Map:
                if (h.len > remaining() / 2) return false;
                for (uint64_t k = 0; k < h.len; k++) {
                    if (!skipValue(depth + 1) || !skipValue(depth + 1)) return false;
                }
                return true;
            default:
                return true;
        }
    }

    bool skipValue(int depth) {
        Head h;
        return readHead(h) && skipAfterHead(h, depth);
    }

    bool wanted(std::string_view key, int depth) const {
        if (depth != 0 || m_keep.size() == 0) return true;
        return std::find(m_keep.begin(), m_keep.end(), key) != m_keep.end();
    }

    bool readValue(matjson::Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        if (++m_values > kMaxValues) return false;
        Head h;
        if (!readHead(h)) return false;
        switch (h.kind) {
            case Kind::Nil:
                out = matjson::Value(nullptr);
                return true;
            case Kind::Bool:
                out = matjson::Value(h.b);
                return true;
            case Kind::Int:
                out = matjson::Value(static_cast<std::intmax_t>(h.i));
                return true;
            case Kind::UInt:
                out = matjson::Value(static_cast<std::uintmax_t>(h.u));
                return true;
            case Kind::Float:
                out = matjson::Value(h.f);
                return true;
            case Kind::Str: {
                std::string_view sv;
                if (!take(h.len, sv)) return false;
                out = matjson::Value(std::string(sv));
                return true;
            }
            case Kind::Bin:
            case Kind::Ext:
                if (!skipBytes(h.len)) return false;
                out = matjson::Value(nullptr);
                return true;
            case Kind::Array: {
                if (h.len > remaining()) return false;
                std::vector<matjson::Value> arr;
                arr.reserve(static_cast<size_t>(std::min<uint64_t>(h.len, 4096)));
                for (uint64_t k = 0; k < h.len; k++) {
                    matjson::Value item;
                    if (!readValue(item, depth + 1)) return false;
                    arr.push_back(std::move(item));
                }
                out = matjson::Value(std::move(arr));
                return true;
            }
            case Kind::Map: {
                if (h.len > remaining() / 2) return false;
                if (depth > 0 && h.len > kMaxMapKeys) {
                    if (!skipAfterHead(h, depth)) return false;
                    out = matjson::Value(nullptr);
                    return true;
                }
                matjson::Value obj = matjson::Value::object();
                for (uint64_t k = 0; k < h.len; k++) {
                    Head kh;
                    if (!readHead(kh)) return false;
                    if (kh.kind != Kind::Str) {
                        if (!skipAfterHead(kh, depth + 1) || !skipValue(depth + 1)) return false;
                        continue;
                    }
                    std::string_view key;
                    if (!take(kh.len, key)) return false;
                    if (!wanted(key, depth)) {
                        if (!skipValue(depth + 1)) return false;
                        continue;
                    }
                    matjson::Value val;
                    if (!readValue(val, depth + 1)) return false;
                    obj.set(key, std::move(val));
                }
                out = std::move(obj);
                return true;
            }
        }
        return false;
    }
};

std::optional<std::string> checkJsonShape(std::string_view text) {
    int depth = 0;
    size_t tokens = 0;
    bool inString = false;
    bool escape = false;
    for (char c : text) {
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        switch (c) {
            case '"':
                inString = true;
                break;
            case '{':
            case '[':
                if (++depth > kMaxDepth) return "The macro file is nested too deeply.";
                tokens++;
                break;
            case '}':
            case ']':
                depth--;
                break;
            case ',':
                tokens++;
                break;
            default:
                break;
        }
        if (tokens > kMaxValues) return "The macro file is too large.";
    }
    return std::nullopt;
}

std::optional<double> numberOf(matjson::Value const& v) {
    if (!v.isNumber()) return std::nullopt;
    auto d = v.asDouble();
    if (!d) return std::nullopt;
    double x = d.unwrap();
    if (!std::isfinite(x)) return std::nullopt;
    return x;
}

std::optional<bool> boolOf(matjson::Value const& v) {
    if (v.isBool()) {
        auto b = v.asBool();
        if (b) return b.unwrap();
        return std::nullopt;
    }
    if (auto n = numberOf(v)) return *n != 0.0;
    return std::nullopt;
}

std::string stringOf(matjson::Value const& v) {
    if (!v.isString()) return {};
    return v.asString().unwrapOr(std::string());
}

bool parseVersionPart(std::string_view part, int& out) {
    if (part.empty()) return false;
    auto res = std::from_chars(part.data(), part.data() + part.size(), out);
    return res.ec == std::errc() && res.ptr == part.data() + part.size();
}

int xdBotFrameOffset(std::string const& name, std::string const& version) {
    if (name != "xdBot") return 0;
    std::string_view ver(version);
    if (!ver.empty() && ver.front() == 'v') ver.remove_prefix(1);
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        size_t dot = ver.find('.', start);
        parts.push_back(ver.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
        if (parts.size() > 3) break;
    }
    if (parts.size() > 3) return 1;
    int nums[3] = {0, 0, 0};
    for (size_t k = 0; k < parts.size(); k++) {
        if (!parseVersionPart(parts[k], nums[k])) return 0;
    }
    int const check[3] = {2, 3, 6};
    for (int k = 0; k < 3; k++) {
        if (nums[k] != check[k]) return nums[k] < check[k] ? 1 : 0;
    }
    return 0;
}

struct InputEvent {
    int64_t tick = 0;
    int lane = 0;
    bool down = false;
};

Result<ImportedInputs> importGdr(matjson::Value const& root) {
    if (!root.isObject()) return Err(kUnsupported);
    auto const& inputs = root["inputs"];
    if (!inputs.isArray()) return Err(kUnsupported);

    double framerate = 240.0;
    auto const& fr = root["framerate"];
    if (!fr.isNull()) {
        auto n = numberOf(fr);
        if (!n || *n < 1.0 || *n > 100000.0) return Err("The macro has an invalid frame rate.");
        framerate = *n;
    }

    ImportedInputs out;
    out.format = "GDR macro";

    auto const& bot = root["bot"];
    std::string botName;
    std::string botVersion;
    if (bot.isObject()) {
        botName = stringOf(bot["name"]);
        botVersion = stringOf(bot["version"]);
    }
    auto const& level = root["level"];
    if (level.isObject()) {
        if (auto id = numberOf(level["id"])) {
            if (*id >= 0.0 && *id <= 2147483647.0) out.macroLevelID = static_cast<int>(*id);
        }
        out.macroLevelName = stringOf(level["name"]);
    }

    int offset = xdBotFrameOffset(botName, botVersion);
    double scale = kTicksPerSecond / framerate;

    std::vector<InputEvent> events;
    bool otherButtons = false;
    for (auto const& input : inputs) {
        if (!input.isObject()) continue;
        auto frame = numberOf(input["frame"]);
        if (!frame) continue;
        auto down = boolOf(input["down"]);
        if (!down) continue;
        auto button = numberOf(input["btn"]);
        if (button && *button != 1.0) {
            otherButtons = true;
            continue;
        }
        bool p2 = boolOf(input["2p"]).value_or(false);
        double f = *frame + offset;
        if (f < 0.0) continue;
        double t = f * scale;
        if (t >= static_cast<double>(kMaxTicks)) return Err("The macro is too long to import.");
        InputEvent ev;
        ev.tick = std::llround(t);
        ev.lane = p2 ? 1 : 0;
        ev.down = *down;
        events.push_back(ev);
    }

    bool anyPress = std::any_of(events.begin(), events.end(), [](InputEvent const& e) { return e.down; });
    if (!anyPress) return Err("The macro has no jump inputs.");

    std::stable_sort(events.begin(), events.end(), [](InputEvent const& a, InputEvent const& b) { return a.tick < b.tick; });

    int64_t length = events.back().tick + 1;
    if (length <= 0 || length > kMaxTicks) return Err("The macro is too long to import.");

    out.held.assign(static_cast<size_t>(length), 0);
    uint8_t cur = 0;
    size_t idx = 0;
    while (idx < events.size()) {
        int64_t t = events[idx].tick;
        uint8_t pressed = 0;
        while (idx < events.size() && events[idx].tick == t) {
            uint8_t bit = static_cast<uint8_t>(1u << events[idx].lane);
            if (events[idx].down) {
                cur |= bit;
                pressed |= bit;
            }
            else {
                cur &= static_cast<uint8_t>(~bit);
            }
            idx++;
        }
        out.held[static_cast<size_t>(t)] = static_cast<uint8_t>(cur | pressed);
        int64_t next = idx < events.size() ? events[idx].tick : length;
        if (cur != 0) {
            std::fill(out.held.begin() + static_cast<ptrdiff_t>(t + 1), out.held.begin() + static_cast<ptrdiff_t>(next), cur);
        }
    }
    if (cur != 0) out.held.insert(out.held.end(), static_cast<size_t>(kTicksPerSecond), cur);

    for (auto v : out.held) {
        if (v & 2) {
            out.usesP2 = true;
            break;
        }
    }
    if (otherButtons) out.warning = "Platformer left/right inputs were ignored";
    return Ok(std::move(out));
}

Result<ImportedInputs> importChartText(std::string_view text) {
    auto chart = Chart::deserialize(std::string(text));
    if (!chart) return Err("This Rhythm Path chart is damaged or from an unsupported version.");
    if (chart->held.empty()) chart->rebuildHeldFromNotes();
    ImportedInputs out;
    out.format = "Rhythm Path chart";
    out.macroLevelID = chart->info.levelID;
    out.macroLevelName = chart->info.levelName;
    bool any = false;
    for (auto v : chart->held) {
        if (v & 1) any = true;
        if (v & 2) {
            any = true;
            out.usesP2 = true;
        }
    }
    if (!chart->lanes[1].empty()) out.usesP2 = true;
    if (!any) return Err("The chart has no inputs.");
    out.held = std::move(chart->held);
    return Ok(std::move(out));
}

Result<ImportedInputs> importDataImpl(std::string_view data) {
    std::string_view body = data;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.remove_prefix(3);
    }
    size_t first = 0;
    while (first < body.size() && (body[first] == ' ' || body[first] == '\t' || body[first] == '\r' || body[first] == '\n')) {
        first++;
    }
    std::string_view text = body.substr(first);
    if (text.empty()) return Err(kUnsupported);

    if (text.starts_with("RHYTHMPATH")) return importChartText(text);

    if (text.front() == '{') {
        if (auto err = checkJsonShape(text)) return Err(*err);
        auto parsed = matjson::parse(text);
        if (!parsed) return Err("The macro file is not valid JSON.");
        return importGdr(parsed.unwrap());
    }

    MsgPackReader reader(data);
    auto root = reader.readRoot({"inputs", "framerate", "bot", "level"});
    if (!root) return Err(kUnsupported);
    return importGdr(*root);
}

}

std::vector<geode::utils::file::FilePickOptions::Filter> importFilters() {
    std::vector<file::FilePickOptions::Filter> filters;
    file::FilePickOptions::Filter chart;
    chart.description = "Rhythm Path chart";
    chart.files = {"*.rpchart", "*.txt"};
    filters.push_back(std::move(chart));
    file::FilePickOptions::Filter gdr;
    gdr.description = "GDR macro";
    gdr.files = {"*.gdr", "*.json"};
    filters.push_back(std::move(gdr));
    file::FilePickOptions::Filter all;
    all.description = "All files";
    all.files = {"*.*"};
    filters.push_back(std::move(all));
    return filters;
}

Result<ImportedInputs> importInputsFromData(std::string_view data, std::string_view fileName) {
    try {
        if (data.size() > kMaxImportBytes) return Err("The file is too large to import.");
        return importDataImpl(data);
    }
    catch (std::exception const& e) {
        log::warn("Import of {} failed: {}", fileName, e.what());
        return Err("The file could not be read.");
    }
    catch (...) {
        log::warn("Import of {} failed", fileName);
        return Err("The file could not be read.");
    }
}

Result<ImportedInputs> importInputsFromFile(std::filesystem::path const& path) {
    try {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) return Err("The file could not be opened.");
        auto size = std::filesystem::file_size(path, ec);
        if (ec) return Err("The file could not be opened.");
        if (size > kMaxImportBytes) return Err("The file is too large to import.");
        auto res = file::readBinary(path);
        if (!res) return Err(fmt::format("The file could not be read: {}", res.unwrapErr()));
        auto bytes = std::move(res).unwrap();
        std::string_view data(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        auto name = utils::string::pathToString(path.filename());
        return importInputsFromData(data, name);
    }
    catch (std::exception const& e) {
        log::warn("Import failed: {}", e.what());
        return Err("The file could not be read.");
    }
    catch (...) {
        return Err("The file could not be read.");
    }
}

}
