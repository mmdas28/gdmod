#include "SolverProgress.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rp {

namespace {

constexpr char kMagic[8] = {'R', 'P', 'S', 'O', 'L', 'V', 'E', '1'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxTicks = 50'000'000;
constexpr uint32_t kMaxDead = 6'000'000;
constexpr uint32_t kMaxSignature = 4096;
constexpr size_t kChecksumSize = 8;

uint64_t checksumOf(std::string_view data) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

class Writer {
public:
    std::string out;

    void u8(uint8_t v) {
        out.push_back(static_cast<char>(v));
    }

    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) u8(static_cast<uint8_t>(v >> (8 * i)));
    }

    void u64(uint64_t v) {
        for (int i = 0; i < 8; i++) u8(static_cast<uint8_t>(v >> (8 * i)));
    }

    void i32(int32_t v) {
        u32(static_cast<uint32_t>(v));
    }

    void i64(int64_t v) {
        u64(static_cast<uint64_t>(v));
    }

    void f32(float v) {
        u32(std::bit_cast<uint32_t>(v));
    }

    void f64(double v) {
        u64(std::bit_cast<uint64_t>(v));
    }

    void bytes(std::vector<uint8_t> const& v) {
        u32(static_cast<uint32_t>(v.size()));
        out.append(reinterpret_cast<char const*>(v.data()), v.size());
    }

    void str(std::string const& s) {
        u32(static_cast<uint32_t>(s.size()));
        out.append(s);
    }

    void words(std::vector<uint64_t> const& v) {
        u32(static_cast<uint32_t>(v.size()));
        size_t at = out.size();
        out.resize(at + v.size() * 8);
        char* p = out.data() + at;
        for (auto w : v) {
            for (int i = 0; i < 8; i++) *p++ = static_cast<char>(static_cast<uint8_t>(w >> (8 * i)));
        }
    }
};

class Reader {
public:
    explicit Reader(std::string_view data) : m_data(data) {}

    bool ok() const {
        return m_ok;
    }

    size_t remaining() const {
        return m_data.size() - m_pos;
    }

    uint8_t u8() {
        if (!need(1)) return 0;
        return static_cast<uint8_t>(m_data[m_pos++]);
    }

    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(m_data[m_pos + i])) << (8 * i);
        m_pos += 4;
        return v;
    }

    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(static_cast<uint8_t>(m_data[m_pos + i])) << (8 * i);
        m_pos += 8;
        return v;
    }

    int32_t i32() {
        return static_cast<int32_t>(u32());
    }

    int64_t i64() {
        return static_cast<int64_t>(u64());
    }

    float f32() {
        return std::bit_cast<float>(u32());
    }

    double f64() {
        return std::bit_cast<double>(u64());
    }

    std::vector<uint8_t> bytes(uint32_t maxCount) {
        uint32_t count = u32();
        if (!m_ok || count > maxCount || count > remaining()) {
            m_ok = false;
            return {};
        }
        auto begin = reinterpret_cast<uint8_t const*>(m_data.data() + m_pos);
        std::vector<uint8_t> out(begin, begin + count);
        m_pos += count;
        return out;
    }

    std::string str(uint32_t maxLength) {
        uint32_t length = u32();
        if (!m_ok || length > maxLength || length > remaining()) {
            m_ok = false;
            return {};
        }
        std::string out(m_data.substr(m_pos, length));
        m_pos += length;
        return out;
    }

    std::vector<uint64_t> words(uint32_t maxCount) {
        uint32_t count = u32();
        if (!m_ok || count > maxCount || count > remaining() / 8) {
            m_ok = false;
            return {};
        }
        std::vector<uint64_t> out(count);
        auto p = reinterpret_cast<uint8_t const*>(m_data.data() + m_pos);
        for (uint32_t i = 0; i < count; i++) {
            uint64_t v = 0;
            for (int b = 0; b < 8; b++) v |= static_cast<uint64_t>(p[b]) << (8 * b);
            out[i] = v;
            p += 8;
        }
        m_pos += static_cast<size_t>(count) * 8;
        return out;
    }

private:
    std::string_view m_data;
    size_t m_pos = 0;
    bool m_ok = true;

    bool need(size_t n) {
        if (!m_ok || remaining() < n) {
            m_ok = false;
            return false;
        }
        return true;
    }
};

float cleanFloat(float v, float fallback, float lo, float hi) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

std::optional<ProgressSummary> readHeader(Reader& in) {
    char magic[sizeof(kMagic)];
    for (auto& c : magic) c = static_cast<char>(in.u8());
    if (!in.ok() || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return std::nullopt;
    if (in.u32() != kVersion || !in.ok()) return std::nullopt;
    ProgressSummary summary;
    summary.percent = cleanFloat(in.f32(), 0.f, 0.f, 100.f);
    double elapsed = in.f64();
    summary.elapsedSeconds = std::isfinite(elapsed) ? std::clamp(elapsed, 0.0, 1e9) : 0.0;
    summary.savedAt = in.i64();
    summary.pathFound = in.u8() != 0;
    summary.targetPercent = cleanFloat(in.f32(), 100.f, 0.f, 100.f);
    summary.levelHash = in.u64();
    if (!in.ok()) return std::nullopt;
    return summary;
}

}

std::string SolveProgress::serialize() const {
    Writer out;
    out.out.reserve(128 + settingsSignature.size() + held.size() + tried.size() + def.size() + flips.size() + best.size() + dead.size() * 8);
    out.out.append(kMagic, sizeof(kMagic));
    out.u32(kVersion);
    out.f32(maxPercent);
    out.f64(elapsedSeconds);
    out.i64(savedAt);
    out.u8(pathFound ? 1 : 0);
    out.f32(targetPercent);
    out.u64(levelHash);

    out.str(settingsSignature);
    out.u64(totalSteps);
    out.u64(backtracks);
    out.u64(prunes);
    out.i32(escalation);
    out.u8(fastAllowed ? 1 : 0);
    out.u8(replayMode ? 1 : 0);
    out.i32(verifyFailures);
    out.bytes(held);
    out.bytes(tried);
    out.bytes(def);
    out.bytes(flips);
    out.bytes(best);
    out.i32(maxTick);
    out.words(dead);
    out.u64(checksumOf(out.out));
    return std::move(out.out);
}

std::optional<SolveProgress> SolveProgress::deserialize(std::string_view data) {
    if (data.size() < sizeof(kMagic) + kChecksumSize) return std::nullopt;
    auto body = data.substr(0, data.size() - kChecksumSize);
    Reader tail(data.substr(data.size() - kChecksumSize));
    if (tail.u64() != checksumOf(body)) return std::nullopt;

    Reader in(body);
    auto header = readHeader(in);
    if (!header) return std::nullopt;

    SolveProgress progress;
    progress.maxPercent = header->percent;
    progress.elapsedSeconds = header->elapsedSeconds;
    progress.savedAt = header->savedAt;
    progress.pathFound = header->pathFound;
    progress.targetPercent = header->targetPercent;
    progress.levelHash = header->levelHash;

    progress.settingsSignature = in.str(kMaxSignature);
    progress.totalSteps = in.u64();
    progress.backtracks = in.u64();
    progress.prunes = in.u64();
    progress.escalation = in.i32();
    progress.fastAllowed = in.u8() != 0;
    progress.replayMode = in.u8() != 0;
    progress.verifyFailures = std::max(0, in.i32());
    progress.held = in.bytes(kMaxTicks);
    progress.tried = in.bytes(kMaxTicks);
    progress.def = in.bytes(kMaxTicks);
    progress.flips = in.bytes(kMaxTicks);
    progress.best = in.bytes(kMaxTicks);
    progress.maxTick = std::max(0, in.i32());
    progress.dead = in.words(kMaxDead);
    if (!in.ok() || in.remaining() != 0) return std::nullopt;

    size_t count = progress.held.size();
    bool nodesMatch = progress.tried.size() == count && progress.def.size() == count && progress.flips.size() == count;
    bool nodesEmpty = progress.tried.empty() && progress.def.empty() && progress.flips.empty();
    if (!nodesMatch && !(progress.pathFound && nodesEmpty)) return std::nullopt;
    return progress;
}

std::optional<ProgressSummary> SolveProgress::peekSummary(std::string_view data) {
    Reader in(data);
    return readHeader(in);
}

}
