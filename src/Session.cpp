#include "Session.hpp"

#include "Import.hpp"
#include "LevelMenu.hpp"
#include "Overlay.hpp"

#include <Geode/ui/Notification.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

namespace rp {

void pickImportFile(Session& session);

namespace {
Session* g_session = nullptr;
uint64_t g_generation = 0;
std::optional<std::chrono::steady_clock::time_point> g_menuWaitStart;

constexpr double kMenuWaitLimitSeconds = 6.0;

bool isPlatformer(PlayLayer* layer) {
    return layer->m_isPlatformer || (layer->m_levelSettings && layer->m_levelSettings->m_platformerMode);
}

bool sessionAlive(Session* owner, uint64_t generation) {
    return owner && g_session == owner && g_generation == generation && owner->layer;
}

bool sceneReady(PlayLayer* layer) {
    auto scene = CCDirector::get()->getRunningScene();
    if (!scene || !layer || !layer->getParent()) return false;
    if (typeinfo_cast<CCTransitionScene*>(scene)) return false;
    CCNode* node = layer;
    while (node->getParent()) node = node->getParent();
    return node == scene;
}

void notify(std::string const& text, NotificationIcon icon) {
    float time = icon == NotificationIcon::Success || icon == NotificationIcon::Info ? NOTIFICATION_DEFAULT_TIME * 1.5f
                                                                                    : NOTIFICATION_LONG_TIME;
    Notification::create(text, icon, time)->show();
}

float cleanPercent(double value, float fallback) {
    if (!std::isfinite(value)) return fallback;
    return static_cast<float>(std::clamp(value, 0.0, 100.0));
}

std::string formatSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    if (seconds < 60.0) return fmt::format("{:.1f}s", seconds);
    auto total = static_cast<long long>(seconds);
    if (total < 3600) return fmt::format("{}m {:02}s", total / 60, total % 60);
    return fmt::format("{}h {:02}m", total / 3600, (total / 60) % 60);
}

std::string sourceWord(std::string const& format) {
    std::string out;
    for (char c : format) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!out.empty() && out.back() != '-') out += '-';
        }
        else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "file" : out;
}

void refreshMenu(Session& s) {
    if (auto menu = s.getMenu()) menu->refresh();
}

void chartChanged(Session& s) {
    if (auto overlay = s.getOverlay()) overlay->onChartChanged();
}

void setOverlayVisible(Session& s, bool visible) {
    if (s.overlay) s.overlay->setVisible(visible);
}

void holdAudio(Session& s, bool hold) {
    auto layer = s.layer;
    if (!layer) return;
    if (hold) {
        auto engine = FMODAudioEngine::sharedEngine();
        if (s.audioHeld && (!engine || engine->m_allAudioPaused)) return;
        layer->pauseAudio();
        s.audioHeld = true;
    }
    else if (s.audioHeld) {
        s.audioHeld = false;
        layer->resumeAudio();
    }
}

void applyGameplayCursor() {
#ifdef GEODE_IS_DESKTOP
    auto gm = GameManager::sharedState();
    if (!gm) return;
    if (gm->getGameVariable("0128")) PlatformToolbox::toggleLockCursor(true);
    else if (!gm->getGameVariable("0024")) PlatformToolbox::hideCursor();
#endif
}

void holdCursor(Session& s, bool hold) {
#ifdef GEODE_IS_DESKTOP
    auto layer = s.layer;
    if (!layer) return;
    bool paused = layer->m_isPaused;
    if (hold) {
        if (!s.cursorHeld || (s.cursorPaused && !paused)) {
            PlatformToolbox::toggleLockCursor(false);
            PlatformToolbox::showCursor();
            s.cursorHeld = true;
        }
    }
    else if (s.cursorHeld) {
        s.cursorHeld = false;
        if (!paused && !layer->m_hasCompletedLevel) applyGameplayCursor();
    }
    s.cursorPaused = paused;
#endif
}

bool currentFlip(Session const& s) {
    if (!s.ref.twoPlayer) return false;
    auto gm = GameManager::sharedState();
    return gm && gm->getGameVariable("0010");
}

void rememberRefInputs(Session& s) {
    auto layer = s.layer;
    s.refStartPos = layer ? layer->m_startPosObject : nullptr;
    s.refStartPosAt = s.refStartPos ? s.refStartPos->getPosition() : CCPoint{0.f, 0.f};
    s.refFlip = currentFlip(s);
}

bool refStale(Session const& s) {
    auto layer = s.layer;
    if (!layer || !s.refReady) return false;
    auto startPos = layer->m_startPosObject;
    if (startPos != s.refStartPos) return true;
    if (startPos && layer->m_gameState.m_levelTime <= 0.0 && !startPos->getPosition().equals(s.refStartPosAt)) return true;
    return currentFlip(s) != s.refFlip;
}

void loadRefData(Session& s) {
    s.ref = makeLevelRef(s.layer);
    s.refReady = true;
    rememberRefInputs(s);
    s.chart = loadChart(s.ref);
    s.chartOutdated = s.chart && chartOutdated(s.ref, *s.chart);
    s.progress = loadProgressSummary(s.ref);
    s.prefs = loadPrefs(s.ref);
    if (s.chart) {
        log::info(
            "Loaded saved chart {} ({} + {} notes{})", s.ref.key(), s.chart->lanes[0].size(), s.chart->lanes[1].size(),
            s.chartOutdated ? ", level changed since" : ""
        );
    }
}

float validTarget(float target) {
    if (!std::isfinite(target) || target < 1.f || target > 100.f) return 100.f;
    return target;
}

void useProgress(SolveRequest& request, SolveProgress progress) {
    request.kind = SolveKind::Resume;
    request.targetPercent = validTarget(progress.targetPercent);
    request.progress = std::move(progress);
}

std::optional<SolveProgress> loadResumable(Session const& s) {
    auto blob = loadProgressBlob(s.ref);
    if (!blob) return std::nullopt;
    auto progress = SolveProgress::deserialize(*blob);
    if (!progress || progress->levelHash != s.ref.levelHash) return std::nullopt;
    return progress;
}

void showMenu(Session& s) {
    auto layer = s.layer;
    if (!layer) return;
    auto menu = LevelMenu::create();
    if (!menu) {
        log::warn("Rhythm Path: could not create the level menu");
        s.menuOpen = false;
        s.menuAtLevelStart = false;
        return;
    }
    menu->m_scene = CCDirector::get()->getRunningScene();
    menu->show();
    s.menu = menu;
    s.menuShown = true;
    g_menuWaitStart.reset();
}

void notifyResult(Session& s, Solver const& solver) {
    if (solver.hasResult() && s.chart) {
        auto const& chart = *s.chart;
        size_t notes = chart.noteCount();
        bool imported = chart.info.source.starts_with("import:");
        if (chart.complete) {
            if (imported) notify(fmt::format("Imported chart ready: {} notes", notes), NotificationIcon::Success);
            else notify(fmt::format("Chart ready: {} notes, solved in {}", notes, formatSeconds(solver.elapsed())), NotificationIcon::Success);
        }
        else if (chart.targetReached()) {
            notify(
                fmt::format("{} ready up to {:.0f}%: {} notes", imported ? "Imported chart" : "Chart", chart.info.targetPercent, notes),
                NotificationIcon::Success
            );
        }
        else {
            std::string text = fmt::format("Partial chart: reaches {:.0f}%", std::floor(chart.reachedPercent));
            if (!solver.failReason().empty()) text += fmt::format(" ({})", solver.failReason());
            notify(text, NotificationIcon::Warning);
        }
        return;
    }
    if (!solver.finished() || solver.failReason() == "cancelled") return;
    std::string text = solver.statusLine();
    if (s.chart) text += " - kept the saved chart";
    notify(text, NotificationIcon::Error);
}

void collectSolver(Session& s, bool announce) {
    if (!s.solver) return;
    auto solver = std::move(s.solver);
    if (solver->hasResult()) {
        s.chart = solver->result();
        s.chartOutdated = s.refReady && chartOutdated(s.ref, *s.chart);
        log::info("Rhythm Path: chart ready ({} + {} notes)", s.chart->lanes[0].size(), s.chart->lanes[1].size());
    }
    if (announce) notifyResult(s, *solver);
    s.lastSolverStatus = solver->statusLine();
    solver.reset();
    if (s.refReady) s.progress = loadProgressSummary(s.ref);
    chartChanged(s);
    refreshMenu(s);
}

void switchRef(Session& s) {
    if (s.solver) {
        if (s.solver->running()) cancelSolve(s);
        else collectSolver(s, true);
    }
    auto oldKey = s.ref.key();
    loadRefData(s);
    log::info("Rhythm Path: start position changed ({} -> {})", oldKey, s.ref.key());
    if (s.pendingRequest && s.pendingRequest->kind == SolveKind::Resume) {
        auto& request = *s.pendingRequest;
        request.kind = SolveKind::Fresh;
        request.progress.reset();
        request.targetPercent = s.prefs.targetPercent;
        if (auto progress = loadResumable(s)) useProgress(request, std::move(*progress));
    }
    chartChanged(s);
    refreshMenu(s);
}

void syncRef(Session& s) {
    if (refStale(s)) switchRef(s);
}

void startPending(Session& s) {
    auto layer = s.layer;
    syncRef(s);
    if (!s.pendingRequest || s.solver) return;
    auto request = std::move(*s.pendingRequest);
    s.pendingRequest.reset();
    if (isPlatformer(layer)) {
        s.lastSolverStatus = "Platformer levels are not supported";
        notify(s.lastSolverStatus, NotificationIcon::Error);
        return;
    }
    request.info.levelID = s.ref.levelID;
    request.info.levelName = s.ref.levelName;
    request.info.levelHash = s.ref.levelHash;
    request.info.variant = s.ref.variant;
    if (request.info.source.empty()) request.info.source = "solver";
    request.targetPercent = validTarget(request.targetPercent);
    s.lastSolverStatus.clear();
    s.chartHidden = false;
    setOverlayVisible(s, true);
    s.solver = std::make_unique<Solver>(layer, s.ref);
    s.solver->start(std::move(request));
    chartChanged(s);
}

std::optional<std::filesystem::path> downloadsFolder() {
    std::error_code ec;
    for (auto var : {"USERPROFILE", "HOME"}) {
        auto value = std::getenv(var);
        if (!value || !*value) continue;
        auto dir = std::filesystem::path(value) / "Downloads";
        if (std::filesystem::is_directory(dir, ec)) return dir;
    }
    return std::nullopt;
}
}

Session* session() {
    return g_session;
}

void setSession(Session* s) {
    g_session = s;
    g_generation++;
    g_menuWaitStart.reset();
}

Session* sessionFor(GJBaseGameLayer* layer) {
    auto s = g_session;
    if (!s || !s->layer || static_cast<GJBaseGameLayer*>(s->layer) != layer) return nullptr;
    return s;
}

Session::~Session() {
    if (g_session == this) {
        g_session = nullptr;
        g_generation++;
    }
}

RhythmOverlay* Session::getOverlay() const {
    return static_cast<RhythmOverlay*>(overlay.data());
}

LevelMenu* Session::getMenu() const {
    return static_cast<LevelMenu*>(menu.data());
}

void Session::shutdown() {
    if (solver) {
        if (solver->running()) {
            solver->saveProgress();
            solver->cancel(false);
        }
        solver.reset();
    }
    pendingRequest.reset();
    menuOpen = false;
    menuAtLevelStart = false;
    holdAudio(*this, false);
    cursorHeld = false;
    if (menu) {
        Ref<CCNode> node = menu;
        menu = nullptr;
        if (node->getParent()) node->removeFromParentAndCleanup(true);
    }
    if (overlay) {
        overlay->removeFromParentAndCleanup(true);
        overlay = nullptr;
    }
}

void tickSession(Session& s) {
    auto layer = s.layer;
    if (!layer) return;

    if (!s.refReady) {
        loadRefData(s);
        if (!s.menuShown && !s.solver && !s.pendingRequest && Mod::get()->getSettingValue<bool>("show-level-menu")) {
            s.menuOpen = true;
            s.menuShown = true;
            s.autoChecked = true;
            s.menuAtLevelStart = true;
            g_menuWaitStart.reset();
        }
    }
    else {
        syncRef(s);
    }

    if (!s.overlay && layer->getParent()) {
        auto overlay = RhythmOverlay::create();
        layer->getParent()->addChild(overlay, 5);
        s.overlay = overlay;
    }

    if (s.menu && !s.menu->getParent()) {
        s.menu = nullptr;
        if (s.menuOpen) closeLevelMenu(s, true);
    }

    if (s.menuOpen && !s.menu) {
        if (s.solving()) {
            s.menuOpen = false;
            s.menuAtLevelStart = false;
        }
        else if (sceneReady(layer)) {
            showMenu(s);
        }
        else {
            auto now = std::chrono::steady_clock::now();
            if (!g_menuWaitStart) g_menuWaitStart = now;
            if (std::chrono::duration<double>(now - *g_menuWaitStart).count() > kMenuWaitLimitSeconds) {
                log::warn("Rhythm Path: the level scene never became active, not showing the menu");
                g_menuWaitStart.reset();
                closeLevelMenu(s, true);
            }
        }
    }

    if (s.solver && !s.solver->running()) collectSolver(s, true);

    if (!s.autoChecked && layer->m_started) {
        s.autoChecked = true;
        if (!s.chart && !s.solver && !s.pendingRequest && !isPlatformer(layer) && Mod::get()->getSettingValue<bool>("auto-solve")) {
            SolveRequest request;
            request.kind = SolveKind::Fresh;
            request.targetPercent = s.prefs.targetPercent;
            if (auto progress = loadResumable(s)) useProgress(request, std::move(*progress));
            s.pendingRequest = std::move(request);
        }
    }

    if (s.pendingRequest && !s.solver && !s.frozen() && layer->m_started && !layer->m_isPaused) startPending(s);

    holdAudio(s, s.frozen());
    holdCursor(s, s.frozen() || s.solving());
}

void openLevelMenu(Session& s) {
    if (!s.layer || s.solving()) return;
    s.pendingRequest.reset();
    s.chartHidden = false;
    setOverlayVisible(s, true);
    s.menuOpen = true;
    s.menuShown = true;
    s.autoChecked = true;
    s.menuAtLevelStart = false;
    g_menuWaitStart.reset();
    holdAudio(s, true);
    if (!s.refReady) return;
    syncRef(s);
    s.progress = loadProgressSummary(s.ref);
    if (s.menu && s.menu->getParent()) {
        refreshMenu(s);
        return;
    }
    s.menu = nullptr;
    if (sceneReady(s.layer)) showMenu(s);
}

void closeLevelMenu(Session& s, bool restartLevel) {
    s.menuOpen = false;
    bool unplayed = s.menuAtLevelStart;
    s.menuAtLevelStart = false;
    if (s.menu) {
        Ref<CCNode> node = s.menu;
        s.menu = nullptr;
        if (node->getParent()) node->removeFromParentAndCleanup(true);
    }
    holdAudio(s, false);
    auto layer = s.layer;
    if (restartLevel && layer && !s.solving()) {
        if (!layer->m_queuedButtons.empty()) layer->m_queuedButtons.clear();
        auto gsm = GameStatsManager::sharedState();
        auto level = layer->m_level;
        int layerAttempts = layer->m_attempts;
        int statAttempts = gsm ? gsm->getStat("2") : 0;
        int levelAttempts = level ? level->m_attempts.value() : 0;
        layer->resetLevel();
        if (unplayed) {
            layer->m_attempts = layerAttempts;
            if (gsm) gsm->setStat("2", statAttempts);
            if (level) level->m_attempts = levelAttempts;
            if (layer->m_attemptLabel) layer->m_attemptLabel->setString(fmt::format("Attempt {}", layerAttempts).c_str());
        }
    }
}

void requestSolve(Session& s, SolveKind kind) {
    if (!s.layer || s.solving() || !s.refReady) return;
    syncRef(s);
    SolveRequest request;
    request.kind = SolveKind::Fresh;
    request.targetPercent = s.prefs.targetPercent;
    if (kind == SolveKind::Resume) {
        std::optional<SolveProgress> progress;
        if (auto blob = loadProgressBlob(s.ref)) progress = SolveProgress::deserialize(*blob);
        if (!progress) {
            notify("The saved progress could not be read, starting over", NotificationIcon::Warning);
        }
        else if (progress->levelHash != s.ref.levelHash) {
            notify("Saved progress is for an older version of this level, starting over", NotificationIcon::Warning);
            deleteProgress(s.ref);
            s.progress.reset();
        }
        else {
            useProgress(request, std::move(*progress));
        }
    }
    closeLevelMenu(s, true);
    s.lastSolverStatus.clear();
    s.pendingRequest = std::move(request);
}

static void finishImport(Session& s, std::filesystem::path const& path, Result<ImportedInputs> result) {
    s.importBusy = false;
    s.importLoading = false;
    if (!s.refReady || s.solving() || s.pendingRequest || !s.menuOpen) {
        if (!s.menuOpen && result.isOk()) notify("Import cancelled because the level was started", NotificationIcon::Info);
        refreshMenu(s);
        return;
    }
    if (result.isErr()) {
        auto error = result.unwrapErr();
        log::warn("Rhythm Path: import of {} failed: {}", utils::string::pathToString(path), error);
        notify(error, NotificationIcon::Error);
        refreshMenu(s);
        return;
    }
    auto imported = std::move(result).unwrap();
    if (imported.held.empty()) {
        notify("The file has no inputs to import", NotificationIcon::Error);
        refreshMenu(s);
        return;
    }

    if (!imported.warning.empty()) notify(imported.warning, NotificationIcon::Warning);
    if (imported.macroLevelID > 0 && s.ref.levelID > 0 && imported.macroLevelID != s.ref.levelID) {
        notify(
            fmt::format("This file was made for level ID {}, checking it anyway", imported.macroLevelID),
            NotificationIcon::Warning
        );
    }
    if (!s.ref.twoPlayer) {
        for (auto& value : imported.held) value = static_cast<uint8_t>((value | (value >> 1)) & 1);
        imported.usesP2 = false;
    }

    SolveRequest request;
    request.kind = SolveKind::CheckImport;
    request.importSeq = std::move(imported.held);
    request.repairImport = Mod::get()->getSettingValue<bool>("repair-imports");
    request.targetPercent = 100.f;
    request.info.source = "import:" + sourceWord(imported.format);
    request.info.sourceFile = utils::string::pathToString(path.filename());
    log::info(
        "Rhythm Path: importing {} ({}, {} ticks, P2: {})", request.info.sourceFile, imported.format, request.importSeq.size(),
        imported.usesP2
    );
    closeLevelMenu(s, true);
    s.lastSolverStatus.clear();
    s.pendingRequest = std::move(request);
}

void requestImport(Session& s, std::filesystem::path const& path) {
    if (!s.layer || !s.refReady || s.importBusy || s.solving()) return;
    syncRef(s);
    ImportContext context;
    context.twoPlayerLevel = s.ref.twoPlayer;
    context.flipTwoPlayer = currentFlip(s);
    s.importBusy = true;
    s.importLoading = true;
    refreshMenu(s);
    Session* owner = &s;
    uint64_t generation = g_generation;
    (void)async::runtime().spawnBlocking<void>([owner, generation, path, context] {
        auto result = [&]() -> Result<ImportedInputs> {
            try {
                return importInputsFromFile(path, context);
            }
            catch (std::exception const& e) {
                return Err(fmt::format("The file could not be read: {}", e.what()));
            }
            catch (...) {
                return Err(std::string("The file could not be read"));
            }
        }();
        geode::queueInMainThread([owner, generation, path, result = std::move(result)]() mutable {
            if (!sessionAlive(owner, generation)) return;
            finishImport(*owner, path, std::move(result));
        });
    });
}

void pickImportFile(Session& s) {
    if (!s.layer || s.importBusy || s.solving()) return;
    s.importBusy = true;
    refreshMenu(s);
    Session* owner = &s;
    uint64_t generation = g_generation;
    file::FilePickOptions options;
    options.filters = importFilters();
    async::spawn(
        file::pick(file::PickMode::OpenFile, std::move(options)),
        [owner, generation](file::PickResult result) {
            if (!sessionAlive(owner, generation)) return;
            auto& s = *owner;
            s.importBusy = false;
            if (result.isErr()) {
                notify(fmt::format("Could not open the file picker: {}", result.unwrapErr()), NotificationIcon::Error);
                refreshMenu(s);
                return;
            }
            auto picked = std::move(result).unwrap();
            if (!picked || s.solving()) {
                refreshMenu(s);
                return;
            }
            requestImport(s, *picked);
        }
    );
}

void playChart(Session& s) {
    s.chartHidden = false;
    if (!Mod::get()->getSettingValue<bool>("show-overlay")) {
        Mod::get()->setSettingValue<bool>("show-overlay", true);
        if (auto overlay = s.getOverlay()) overlay->refreshStyleNow();
    }
    setOverlayVisible(s, true);
    closeLevelMenu(s, true);
}

void playWithoutChart(Session& s) {
    s.chartHidden = true;
    setOverlayVisible(s, true);
    closeLevelMenu(s, true);
}

void cancelSolve(Session& s) {
    if (!s.solver) return;
    bool saved = s.cancelSaved;
    s.cancelSaved = false;
    if (s.solver->running()) {
        saved = s.solver->saveProgress();
        s.solver->cancel(true);
    }
    collectSolver(s, false);
    s.lastSolverStatus = saved && s.progress ? "Solver cancelled - progress saved" : "Solver cancelled";
    chartChanged(s);
    refreshMenu(s);
}

void deleteSavedChart(Session& s) {
    if (!s.refReady) return;
    deleteChart(s.ref);
    s.chart.reset();
    s.chartOutdated = false;
    chartChanged(s);
    refreshMenu(s);
}

void deleteSavedProgress(Session& s) {
    if (!s.refReady) return;
    deleteProgress(s.ref);
    s.progress.reset();
    chartChanged(s);
    refreshMenu(s);
}

void setLevelPrefs(Session& s, LevelPrefs const& prefs) {
    LevelPrefs clean = prefs;
    clean.rangeFrom = cleanPercent(prefs.rangeFrom, 0.f);
    clean.rangeTo = cleanPercent(prefs.rangeTo, 100.f);
    clean.targetPercent = std::isfinite(prefs.targetPercent) ? std::clamp(prefs.targetPercent, 1.f, 100.f) : 100.f;
    s.prefs = clean;
    if (s.refReady) savePrefs(s.ref, clean);
}

void exportSavedChart(Session& s) {
    if (!s.chart || !s.refReady) return;
    auto chart = std::make_shared<Chart const>(*s.chart);
    auto name = suggestedExportName(s.ref);
    auto folder = downloadsFolder().value_or(Mod::get()->getSaveDir());
    file::FilePickOptions options;
    options.defaultPath = folder / name;
    options.filters = {file::FilePickOptions::Filter{"Rhythm Path chart", {"*.rpchart"}}};
    async::spawn(
        file::pick(file::PickMode::SaveFile, std::move(options)),
        [chart](file::PickResult result) {
            if (result.isErr()) {
                notify(fmt::format("Could not open the file picker: {}", result.unwrapErr()), NotificationIcon::Error);
                return;
            }
            auto picked = std::move(result).unwrap();
            if (!picked) return;
            auto saved = exportChart(*chart, *picked);
            if (saved.isErr()) {
                notify(fmt::format("Export failed: {}", saved.unwrapErr()), NotificationIcon::Error);
                return;
            }
            notify(fmt::format("Chart exported to {}", utils::string::pathToString(picked->filename())), NotificationIcon::Success);
        }
    );
}

float effectiveRangeFrom(Session const& s) {
    if (s.prefs.customRange) return cleanPercent(s.prefs.rangeFrom, 0.f);
    return cleanPercent(Mod::get()->getSettingValue<double>("range-from"), 0.f);
}

float effectiveRangeTo(Session const& s) {
    if (s.prefs.customRange) return cleanPercent(s.prefs.rangeTo, 100.f);
    return cleanPercent(Mod::get()->getSettingValue<double>("range-to"), 100.f);
}

bool chartVisibleAt(Session const& s, float percent) {
    float from = effectiveRangeFrom(s);
    float to = effectiveRangeTo(s);
    if (to < from) std::swap(from, to);
    if (!std::isfinite(percent)) return true;
    if (from > 0.f && percent < from) return false;
    if (to < 100.f && percent > to) return false;
    return true;
}

}
