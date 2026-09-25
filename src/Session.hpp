#pragma once

#include "Chart.hpp"
#include "Solver.hpp"

#include <Geode/Geode.hpp>
#include <memory>
#include <optional>
#include <string>

namespace rp {

class RhythmOverlay;

struct Session {
    PlayLayer* layer = nullptr;
    std::string key;
    bool keyReady = false;
    std::optional<Chart> chart;
    std::unique_ptr<Solver> solver;
    bool solveRequested = false;
    bool autoSolveChecked = false;
    std::string lastSolverStatus;
    geode::Ref<cocos2d::CCNode> overlay;

    Session() = default;
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    ~Session();

    RhythmOverlay* getOverlay() const;
    bool solving() const { return solver && solver->running(); }
    void shutdown();
};

Session* session();
void setSession(Session* session);
Session* sessionFor(GJBaseGameLayer* layer);

void tickSession(Session& session);
void requestSolve(Session& session);

}
