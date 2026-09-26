#pragma once

#include "Chart.hpp"
#include "Solver.hpp"
#include "SolverProgress.hpp"
#include "Storage.hpp"

#include <Geode/Geode.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace rp {

class RhythmOverlay;
class LevelMenu;

struct Session {
    PlayLayer* layer = nullptr;
    LevelRef ref;
    bool refReady = false;
    std::optional<Chart> chart;
    bool chartOutdated = false;
    std::optional<ProgressSummary> progress;
    LevelPrefs prefs;
    std::unique_ptr<Solver> solver;
    std::optional<SolveRequest> pendingRequest;
    bool autoChecked = false;
    bool menuShown = false;
    bool menuOpen = false;
    bool importBusy = false;
    bool chartHidden = false;
    std::string lastSolverStatus;
    geode::Ref<cocos2d::CCNode> overlay;
    geode::Ref<cocos2d::CCNode> menu;

    Session() = default;
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    ~Session();

    RhythmOverlay* getOverlay() const;
    LevelMenu* getMenu() const;
    bool solving() const { return solver && solver->running(); }
    bool frozen() const { return menuOpen; }
    void shutdown();
};

Session* session();
void setSession(Session* session);
Session* sessionFor(GJBaseGameLayer* layer);

void tickSession(Session& session);

void openLevelMenu(Session& session);
void closeLevelMenu(Session& session, bool restartLevel);

void requestSolve(Session& session, SolveKind kind);
void requestImport(Session& session, std::filesystem::path const& path);
void playChart(Session& session);
void playWithoutChart(Session& session);
void cancelSolve(Session& session);

void deleteSavedChart(Session& session);
void deleteSavedProgress(Session& session);
void setLevelPrefs(Session& session, LevelPrefs const& prefs);
void exportSavedChart(Session& session);

float effectiveRangeFrom(Session const& session);
float effectiveRangeTo(Session const& session);
bool chartVisibleAt(Session const& session, float percent);

}
