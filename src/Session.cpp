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

void showMenu(Session& s) {
    auto layer = s.layer;
    if (!layer) return;
    auto menu = LevelMenu::create();
    if (!menu) {
        log::warn("Rhythm Path: could not create the level menu");
        s.menuOpen = false;
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

void startPending(Session& s) {
    auto layer = s.layer;
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
    if (!std::isfinite(request.targetPercent) || request.targetPercent < 1.f || request.targetPercent > 100.f) {
        request.targetPercent = 100.f;
    }
    s.lastSolverStatus.clear();
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
        s.ref = makeLevelRef(layer);
        s.refReady = true;
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
        if (!s.menuShown && !s.solver && !s.pendingRequest && Mod::get()->getSettingValue<bool>("show-level-menu")) {
            s.menuOpen = true;
            s.menuShown = true;
            s.autoChecked = true;
            g_menuWaitStart.reset();
        }
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
        }
        else if (sceneReady(layer)) {
            showMenu(s);
        }
        else {
            auto now = std::chrono::steady_clock::now();
            if (!g_menuWaitStart) g_menuWaitStart = now;
            if (std::chrono::duration<double>(now - *g_menuWaitStart).count() > kMenuWaitLimitSeconds) {
                log::warn("Rhythm Path: the level scene never became active, not showing the menu");
                s.menuOpen = false;
                g_menuWaitStart.reset();
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
            s.pendingRequest = std::move(request);
        }
    }

    if (s.pendingRequest && !s.solver && !s.frozen() && layer->m_started && !layer->m_isPaused) startPending(s);
}

void openLevelMenu(Session& s) {
    if (!s.layer || s.solving()) return;
    s.pendingRequest.reset();
    setOverlayVisible(s, true);
    s.menuOpen = true;
    s.menuShown = true;
    s.autoChecked = true;
    g_menuWaitStart.reset();
    if (!s.refReady) return;
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
    if (s.menu) {
        Ref<CCNode> node = s.menu;
        s.menu = nullptr;
        if (node->getParent()) node->removeFromParentAndCleanup(true);
    }
    if (restartLevel && s.layer && !s.solving()) {
        if (!s.layer->m_queuedButtons.empty()) s.layer->m_queuedButtons.clear();
        s.layer->resetLevel();
    }
}

void requestSolve(Session& s, SolveKind kind) {
    if (!s.layer || s.solving() || !s.refReady) return;
    SolveRequest request;
    request.kind = SolveKind::Fresh;
    request.targetPercent = s.prefs.targetPercent;
    if (kind == SolveKind::Resume) {
        std::optional<SolveProgress> progress;
        if (auto blob = loadProgressBlob(s.ref)) progress = SolveProgress::deserialize(*blob);
        if (progress) {
            request.kind = SolveKind::Resume;
            request.progress = std::move(progress);
        }
        else {
            notify("The saved progress could not be read, starting over", NotificationIcon::Warning);
        }
    }
    closeLevelMenu(s, true);
    s.lastSolverStatus.clear();
    s.pendingRequest = std::move(request);
}

void requestImport(Session& s, std::filesystem::path const& path) {
    if (!s.layer || !s.refReady || s.importBusy || s.solving()) return;
    s.importBusy = true;
    auto result = importInputsFromFile(path);
    s.importBusy = false;
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
    if (imported.usesP2 && !s.ref.twoPlayer) {
        notify("The file has P2 inputs but this level is not 2-player", NotificationIcon::Warning);
    }

    SolveRequest request;
    request.kind = SolveKind::CheckImport;
    request.importSeq = std::move(imported.held);
    request.repairImport = Mod::get()->getSettingValue<bool>("repair-imports");
    request.targetPercent = s.prefs.targetPercent;
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
    setOverlayVisible(s, true);
    closeLevelMenu(s, true);
}

void playWithoutChart(Session& s) {
    setOverlayVisible(s, false);
    closeLevelMenu(s, true);
}

void cancelSolve(Session& s) {
    if (!s.solver) return;
    if (s.solver->running()) s.solver->cancel(true);
    collectSolver(s, false);
    s.lastSolverStatus = "Solver cancelled - progress saved";
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
