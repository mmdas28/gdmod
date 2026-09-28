#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rp {

enum class DeathKind : uint8_t {
    Unknown,
    Hazard,
    SolidCrash,
    SlopeCrash,
    Squeeze,
    Bounds,
};

struct DeathInfo {
    int tick = -1;
    DeathKind kind = DeathKind::Unknown;
    bool hasRect = false;
    cocos2d::CCRect rect;
    int objectType = -1;
    int objectID = -1;
    bool player2 = false;
};

struct PlannerLimits {
    bool active = false;
    int minHold[3] = {1, 1, 1};
    int minRelease[3] = {1, 1, 1};
    int inputResolution = 1;
};

struct PlannerAdvice {
    uint8_t held = 0;
    uint8_t lanes = 0;
};

struct PlannerHint {
    int tick = -1;
    uint8_t held = 0;
};

struct PlannerWindow {
    int lo = 0;
    int hi = -1;
    bool valid() const { return hi >= lo; }
};

struct PlannerStats {
    uint64_t mapObjects = 0;
    uint64_t advised = 0;
    uint64_t advisedPress = 0;
    uint64_t plans = 0;
    uint64_t planFailures = 0;
    uint64_t deviations = 0;
    uint64_t hints = 0;
    uint64_t observed = 0;
    uint64_t modelMismatches = 0;
    double planMicros = 0.0;
};

class PlannerImpl;

class Planner {
public:
    explicit Planner(PlayLayer* layer);
    ~Planner();

    Planner(Planner const&) = delete;
    Planner& operator=(Planner const&) = delete;

    void setEnabled(bool enabled);
    bool enabled() const;

    void buildMap();
    void setLimits(PlannerLimits const& limits);

    void onRestore(int tick);
    void observe(int tick, uint8_t held, bool died);
    void observe(int tick, uint8_t held, bool died, bool betweenFrames) {
        m_betweenFrames = betweenFrames;
        observe(tick, held, died);
        m_betweenFrames = false;
    }
    PlannerAdvice advise(int tick, uint8_t prevHeld);
    std::optional<PlannerHint> onDeath(DeathInfo const& death, std::vector<uint8_t> const& path);
    PlannerWindow pressWindow(int lane, int startTick, int endTick, std::vector<uint8_t> const& seq);

    PlannerStats const& stats() const;
    std::string summary() const;

private:
    std::unique_ptr<PlannerImpl> m_impl;
    bool m_betweenFrames = false;
};

}
