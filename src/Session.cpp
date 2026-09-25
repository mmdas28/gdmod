#include "Session.hpp"

#include "Overlay.hpp"

using namespace geode::prelude;

namespace rp {

namespace {
Session* g_session = nullptr;

bool isPlatformer(PlayLayer* layer) {
    return layer->m_isPlatformer || (layer->m_levelSettings && layer->m_levelSettings->m_platformerMode);
}
}

Session* session() {
    return g_session;
}

void setSession(Session* s) {
    g_session = s;
}

Session* sessionFor(GJBaseGameLayer* layer) {
    auto s = g_session;
    if (!s || !s->layer || static_cast<GJBaseGameLayer*>(s->layer) != layer) return nullptr;
    return s;
}

Session::~Session() {
    if (g_session == this) g_session = nullptr;
}

RhythmOverlay* Session::getOverlay() const {
    return static_cast<RhythmOverlay*>(overlay.data());
}

void Session::shutdown() {
    if (solver) {
        if (solver->running()) solver->cancel(false);
        solver.reset();
    }
    if (overlay) {
        overlay->removeFromParentAndCleanup(true);
        overlay = nullptr;
    }
}

void requestSolve(Session& s) {
    if (s.solving()) return;
    s.solveRequested = true;
    s.lastSolverStatus.clear();
}

void tickSession(Session& s) {
    auto layer = s.layer;
    if (!layer) return;

    if (!s.keyReady) {
        s.key = levelKey(layer);
        s.keyReady = true;
        s.chart = loadChart(s.key);
        if (s.chart) {
            log::info("Loaded saved chart {} ({} + {} notes)", s.key, s.chart->lanes[0].size(), s.chart->lanes[1].size());
        }
    }

    if (!s.overlay && layer->getParent()) {
        auto overlay = RhythmOverlay::create();
        layer->getParent()->addChild(overlay, 5);
        s.overlay = overlay;
    }

    if (s.solver && s.solver->finished()) {
        if (s.solver->hasResult()) s.chart = s.solver->result();
        s.lastSolverStatus = s.solver->statusLine();
        s.solver.reset();
        if (auto overlay = s.getOverlay()) overlay->onChartChanged();
    }

    if (!s.autoSolveChecked && layer->m_started) {
        s.autoSolveChecked = true;
        if (!s.chart && !isPlatformer(layer) && Mod::get()->getSettingValue<bool>("auto-solve")) {
            s.solveRequested = true;
        }
    }

    if (s.solveRequested && !s.solver && layer->m_started && !layer->m_isPaused) {
        s.solveRequested = false;
        if (isPlatformer(layer)) {
            s.lastSolverStatus = "Platformer levels are not supported";
            return;
        }
        s.solver = std::make_unique<Solver>(layer);
        s.solver->start();
        if (auto overlay = s.getOverlay()) overlay->onChartChanged();
    }
}

}
