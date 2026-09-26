#include "Solver.hpp"

#include <Geode/fmod/fmod.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace rp {

namespace {

constexpr int kSelfTestCheckpointTick = 20;
constexpr int kRefineHorizon = 240;
constexpr int kVerifyGraceTicks = 480;
constexpr size_t kMaxDeadStates = 6'000'000;
constexpr int kMaxVerifyFailures = 12;
constexpr int kRepairRewindTicks = 120;
constexpr int kFastTestTicks = 480;

uint8_t seqAt(std::vector<uint8_t> const& seq, int tick) {
    if (tick < 0 || tick >= static_cast<int>(seq.size())) return 0;
    return seq[tick];
}

}

Solver::Solver(PlayLayer* layer) : m_layer(layer) {}

Solver::~Solver() {
    releaseAllCheckpoints();
    setMuted(false);
}

bool Solver::running() const {
    switch (m_phase) {
        case Phase::SelfTest:
        case Phase::Search:
        case Phase::Verify:
        case Phase::Refine:
        case Phase::FinalVerify:
            return true;
        default:
            return false;
    }
}

void Solver::readSettings() {
    auto mod = Mod::get();
    m_stepsPerUpdate = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("steps-per-update"), 1, 8));
    m_inputResolution = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("input-resolution"), 1, 8));
    m_frameBudgetMs = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("frame-budget"), 1, 1000));
    m_timeLimitSec = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("time-limit"), 5, 86400));
    m_refineEnabled = mod->getSettingValue<bool>("refine-timing");
    m_refineWindow = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("refine-window"), 1, 240));
    m_verifyCpInterval = m_refineEnabled ? 24 : 60;
}

double Solver::elapsedSeconds() const {
    return std::chrono::duration<double>(Clock::now() - m_startTime).count();
}

bool Solver::timeUp() const {
    return elapsedSeconds() > m_timeLimitSec;
}

void Solver::setMuted(bool muted) {
    if (m_muted == muted) return;
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;
    FMOD::ChannelGroup* master = nullptr;
    if (engine->m_system->getMasterChannelGroup(&master) == FMOD_OK && master) {
        master->setMute(muted);
        m_muted = muted;
    }
}

void Solver::inject(int lane, bool down) {
    m_injecting = true;
    m_layer->handleButton(down, 1, lane == 0);
    m_injecting = false;
}

void Solver::applyHeld(uint8_t held) {
    uint8_t diff = static_cast<uint8_t>(held ^ m_curHeld);
    if (!diff) return;
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        if (diff & mask) inject(lane, (held & mask) != 0);
    }
    m_curHeld = held;
}

void Solver::releaseAll() {
    applyHeld(0);
}

uint64_t Solver::stateHash(int tick) const {
    uint64_t h = mixHash(0x5eed5eedull, static_cast<uint64_t>(tick));
    h = mixHash(h, m_curHeld);
    h = mixHash(h, hashPlayer(m_layer->m_player1));
    if (m_layer->m_gameState.m_isDualMode) {
        h = mixHash(h, hashPlayer(m_layer->m_player2));
    }
    else {
        h = mixHash(h, 7);
    }
    h = mixHash(h, static_cast<uint64_t>(m_layer->m_gameState.m_activatedObjectIDs.size()));
    return h;
}

bool Solver::fastModeNow() const {
    if (m_phase == Phase::SelfTest) return m_selfTestStage == 2;
    if (m_phase == Phase::FinalVerify) return false;
    return m_fastAllowed;
}

void Solver::setLayerHidden(bool hidden) {
    if (!m_layer) return;
    if (hidden) {
        if (m_layerHidden) return;
        m_layerHidden = true;
        m_hiddenChildren.clear();
        auto children = m_layer->getChildren();
        if (!children) return;
        for (auto child : CCArrayExt<CCNode*>(children)) {
            if (!child || child == m_layer->m_uiLayer) continue;
            m_hiddenChildren.emplace_back(child, child->isVisible());
            child->setVisible(false);
        }
    }
    else if (m_layerHidden) {
        m_layerHidden = false;
        for (auto& [child, visible] : m_hiddenChildren) {
            if (child) child->setVisible(visible);
        }
        m_hiddenChildren.clear();
    }
}

void Solver::onPause() {
    setLayerHidden(false);
}

void Solver::updateCheckpointInterval() {
    double ratio = m_cpCostUs / std::max(1.0, m_stepCostUs);
    m_searchCpInterval = static_cast<int>(std::clamp<long>(std::lround(3.0 * ratio), 4, 32));
}

void Solver::clearStepFlags() {
    m_diedThisStep = false;
    m_completedThisStep = false;
    m_stopDecisions = false;
    m_pendingDeath = false;
    m_success = false;
    m_deathTick = -1;
    m_prunedAt = -1;
    m_successTick = -1;
}

uint8_t Solver::optionCountNow() const {
    if (m_inputResolution > 1 && (m_tick % m_inputResolution) != 0) return 1;
    return (m_twoPlayer && m_layer->m_gameState.m_isDualMode) ? 4 : 2;
}

int Solver::nextOption(int tick) const {
    auto const& node = m_path[tick];
    uint8_t prev = tick > 0 ? m_path[tick - 1].held : 0;
    uint8_t order[4] = {
        prev,
        static_cast<uint8_t>(prev ^ 1),
        static_cast<uint8_t>(prev ^ 2),
        static_cast<uint8_t>(prev ^ 3),
    };
    int count = node.optionCount;
    for (int i = 0; i < count; i++) {
        if (!(node.tried & (1u << order[i]))) return order[i];
    }
    return -1;
}

void Solver::start() {
    if (running()) return;
    readSettings();

    m_key = levelKey(m_layer);
    m_twoPlayer = m_layer->m_levelSettings && m_layer->m_levelSettings->m_twoPlayerMode;
    m_startTime = Clock::now();
    m_lastLog = m_startTime;
    m_savedClickBetweenSteps = m_layer->m_clickBetweenSteps;
    m_layer->m_clickBetweenSteps = false;
    captureStats();
    setMuted(true);

    m_restoreMode = RestoreMode::Checkpoint;
    m_path.clear();
    m_dead.clear();
    m_best.clear();
    m_seq.clear();
    m_verifiedSeq.clear();
    m_tickTimes.clear();
    m_verifiedTimes.clear();
    m_maxTick = 0;
    m_maxPercent = 0.f;
    m_verifyFailures = 0;
    m_furthestVerifyFail = -1;
    m_totalSteps = 0;
    m_backtracks = 0;
    m_prunes = 0;
    m_stallFrames = 0;
    m_hasResult = false;
    m_failReason.clear();
    m_sawHalfTick = false;
    m_fastAllowed = false;
    m_finalIsRefined = false;
    m_replayFallbackUsed = false;
    m_replayFromRepair = false;
    setLayerHidden(true);

    log::info(
        "Solver starting for {} (two player: {}, steps/update: {}, input resolution: {}, time limit: {}s)",
        m_key, m_twoPlayer, m_stepsPerUpdate, m_inputResolution, m_timeLimitSec
    );

    m_phase = Phase::SelfTest;
    m_selfTestStage = 0;
    m_selfTestHashes.clear();
    m_selfTestMismatch = -1;
    m_selfTestCpTick = -1;
    m_selfTestDied = false;
    m_fastMismatch = -1;
    releaseAllCheckpoints();
    resetToStart();
}

void Solver::cancel(bool resetLevel) {
    if (!running()) return;
    log::info("Solver cancelled");
    cleanup();
    m_failReason = "cancelled";
    m_phase = Phase::Failed;
    if (resetLevel) {
        resetToStart();
        m_layer->m_resumeTimer = 1;
    }
}

void Solver::runFrame() {
    if (!running()) return;
    setLayerHidden(true);
    auto frameStart = Clock::now();
    auto budget = std::chrono::milliseconds(m_frameBudgetMs);
    while (running()) {
        bool keepGoing = false;
        switch (m_phase) {
            case Phase::SelfTest: keepGoing = advanceSelfTest(); break;
            case Phase::Search: keepGoing = advanceSearch(); break;
            case Phase::Verify:
            case Phase::FinalVerify: keepGoing = advanceVerify(); break;
            case Phase::Refine: keepGoing = advanceRefine(); break;
            default: break;
        }
        if (!keepGoing) break;
        if (Clock::now() - frameStart >= budget) break;
    }
    if (running() && Clock::now() - m_lastLog >= std::chrono::seconds(10)) {
        m_lastLog = Clock::now();
        log::info("Solver: {} | {} | tick {} / max {}", statusLine(), detailLine(), m_tick, m_maxTick);
    }
}

bool Solver::simulate() {
    int before = m_tick;
    int steps = m_stepsPerUpdate - (((m_tick % m_stepsPerUpdate) + m_stepsPerUpdate) % m_stepsPerUpdate);
    auto stepStart = Clock::now();
    m_inStep = true;
    m_layer->update(static_cast<float>(steps / 240.0));
    m_inStep = false;
    if (m_tick > before) {
        double us = std::chrono::duration<double, std::micro>(Clock::now() - stepStart).count() / (m_tick - before);
        m_stepCostUs = m_stepCostUs * 0.95 + us * 0.05;
    }
    if (m_diedThisStep || m_completedThisStep) {
        afterStep(false);
        m_tick--;
    }
    int advanced = m_tick - before;
    if (advanced <= 0) {
        if (++m_stallFrames > 600) {
            m_failReason = "the game stopped advancing";
            log::error("Solver: game is not advancing (tick {})", m_tick);
            std::vector<uint8_t> best = m_best.size() > m_path.size() ? m_best : heldOf(m_path);
            finish(false, best, false, false, false);
        }
        return false;
    }
    m_stallFrames = 0;
    m_totalSteps += static_cast<uint64_t>(advanced);
    return true;
}

void Solver::captureStats() {
    auto gsm = GameStatsManager::sharedState();
    auto level = m_layer->m_level;
    m_stats.statJumps = gsm->getStat("1");
    m_stats.statAttempts = gsm->getStat("2");
    if (level) {
        m_stats.levelAttempts = level->m_attempts.value();
        m_stats.levelJumps = level->m_jumps.value();
        m_stats.levelClicks = level->m_clicks.value();
        m_stats.levelAttemptTime = level->m_attemptTime.value();
    }
    m_stats.layerAttempts = m_layer->m_attempts;
    m_stats.layerJumps = m_layer->m_jumps;
    m_stats.layerUncommittedJumps = m_layer->m_uncommittedJumps;
    m_stats.layerClicks = m_layer->m_clicks;
    m_stats.valid = true;
}

void Solver::restoreStats() {
    if (!m_stats.valid) return;
    m_stats.valid = false;
    auto gsm = GameStatsManager::sharedState();
    gsm->setStat("1", m_stats.statJumps);
    gsm->setStat("2", m_stats.statAttempts);
    if (auto level = m_layer->m_level) {
        level->m_attempts = m_stats.levelAttempts;
        level->m_jumps = m_stats.levelJumps;
        level->m_clicks = m_stats.levelClicks;
        level->m_attemptTime = m_stats.levelAttemptTime;
    }
    m_layer->m_attempts = m_stats.layerAttempts;
    m_layer->m_jumps = m_stats.layerJumps;
    m_layer->m_uncommittedJumps = m_stats.layerUncommittedJumps;
    m_layer->m_clicks = m_stats.layerClicks;
}

void Solver::resetToStart() {
    releaseAll();
    m_internalReset = true;
    auto practiceCheckpoints = m_layer->m_checkpointArray;
    if (m_layer->m_isPracticeMode && practiceCheckpoints && practiceCheckpoints->count() > 0) {
        auto empty = CCArray::create();
        empty->retain();
        m_layer->m_checkpointArray = empty;
        m_layer->resetLevel();
        m_layer->m_checkpointArray = practiceCheckpoints;
        empty->release();
    }
    else {
        m_layer->resetLevel();
    }
    m_internalReset = false;
    m_layer->m_queuedButtons.clear();
    m_layer->m_resumeTimer = 0;
    m_tick = 0;
    m_curHeld = 0;
    clearStepFlags();
}

bool Solver::hasCheckpointWithin(int interval) const {
    auto it = m_checkpoints.upper_bound(m_tick);
    if (it == m_checkpoints.begin()) return false;
    --it;
    return it->first > m_tick - interval;
}

void Solver::createCheckpointHere() {
    auto cpStart = Clock::now();
    auto object = m_layer->createCheckpoint();
    if (!object) {
        log::warn("Solver: createCheckpoint returned null at tick {}", m_tick);
        return;
    }
    object->retain();

    auto existing = m_checkpoints.find(m_tick);
    if (existing != m_checkpoints.end()) {
        if (existing->second.object) existing->second.object->release();
        m_checkpoints.erase(existing);
    }

    SavedCheckpoint cp;
    cp.tick = m_tick;
    cp.object = object;
    cp.p1.capture(m_layer->m_player1);
    cp.p2.capture(m_layer->m_player2);
    cp.held = m_curHeld;
    cp.jumping = m_layer->m_jumping;
    cp.extraDelta = m_layer->m_extraDelta;
    m_checkpoints.emplace(m_tick, std::move(cp));

    double us = std::chrono::duration<double, std::micro>(Clock::now() - cpStart).count();
    m_cpCostUs = m_cpCostUs * 0.8 + us * 0.2;
    if (m_phase == Phase::Search) {
        thinCheckpoints();
        updateCheckpointInterval();
    }
}

void Solver::thinCheckpoints() {
    int limit = m_tick - m_denseWindow;
    if (limit <= 0) return;
    int lastKept = -1000000;
    for (auto it = m_checkpoints.begin(); it != m_checkpoints.end() && it->first < limit;) {
        if (it->first == 0 || it->first - lastKept >= m_sparseInterval) {
            lastKept = it->first;
            ++it;
        }
        else {
            if (it->second.object) it->second.object->release();
            it = m_checkpoints.erase(it);
        }
    }
}

void Solver::releaseCheckpointsAfter(int tick) {
    for (auto it = m_checkpoints.upper_bound(tick); it != m_checkpoints.end();) {
        if (it->second.object) it->second.object->release();
        it = m_checkpoints.erase(it);
    }
}

void Solver::releaseAllCheckpoints() {
    for (auto& [tick, cp] : m_checkpoints) {
        if (cp.object) cp.object->release();
    }
    m_checkpoints.clear();
}

void Solver::loadCheckpoint(SavedCheckpoint const& cp) {
    m_layer->m_queuedButtons.clear();

    auto previousCurrent = m_layer->m_currentCheckpoint;
    auto retainBefore = cp.object->retainCount();
    m_layer->loadFromCheckpoint(cp.object);
    if (m_layer->m_currentCheckpoint != previousCurrent) {
        if (m_layer->m_currentCheckpoint == cp.object && cp.object->retainCount() > retainBefore) {
            cp.object->release();
            if (previousCurrent) previousCurrent->retain();
        }
        m_layer->m_currentCheckpoint = previousCurrent;
    }

    m_layer->m_queuedButtons.clear();
    cp.p1.apply(m_layer->m_player1);
    if (m_layer->m_gameState.m_isDualMode) cp.p2.apply(m_layer->m_player2);
    m_layer->m_jumping = cp.jumping;
    m_layer->m_extraDelta = cp.extraDelta;
    m_layer->m_player1->m_isDead = false;
    m_layer->m_player2->m_isDead = false;
    m_layer->m_playerDied = false;
    m_layer->m_levelEndAnimationStarted = false;
    m_layer->m_resumeTimer = 0;

    m_curHeld = cp.held;
    m_tick = cp.tick;
    clearStepFlags();
}

void Solver::restoreTo(int tick) {
    if (m_restoreMode == RestoreMode::Replay || m_checkpoints.empty()) {
        releaseAllCheckpoints();
        resetToStart();
        if (m_restoreMode == RestoreMode::Checkpoint) createCheckpointHere();
        return;
    }
    releaseCheckpointsAfter(tick);
    auto it = m_checkpoints.upper_bound(tick);
    if (it == m_checkpoints.begin()) {
        resetToStart();
        createCheckpointHere();
        return;
    }
    --it;
    loadCheckpoint(it->second);
}

void Solver::beforeStep(bool halfTick) {
    if (halfTick && !m_sawHalfTick) {
        m_sawHalfTick = true;
        log::warn("Solver: saw a half tick while solving (tick {})", m_tick);
    }

    int t = m_tick;
    if (t >= 0) {
        if (static_cast<int>(m_tickTimes.size()) <= t) m_tickTimes.resize(t + 1, 0.0);
        m_tickTimes[t] = m_layer->m_gameState.m_levelTime;
    }
    if (m_stopDecisions) return;

    switch (m_phase) {
        case Phase::SelfTest: {
            uint64_t h = stateHash(t);
            if (m_selfTestStage == 0) {
                if (static_cast<int>(m_selfTestHashes.size()) <= t) m_selfTestHashes.resize(t + 1, 0);
                m_selfTestHashes[t] = h;
            }
            else if (t < static_cast<int>(m_selfTestHashes.size()) && m_selfTestHashes[t] != h) {
                int& mismatch = m_selfTestStage == 2 ? m_fastMismatch : m_selfTestMismatch;
                if (mismatch < 0) mismatch = t;
            }
            applyHeld(0);
            break;
        }
        case Phase::Search: {
            if (t < static_cast<int>(m_path.size())) {
                if (m_path[t].hash == 0) m_path[t].hash = stateHash(t);
                applyHeld(m_path[t].held);
                break;
            }
            uint64_t h = stateHash(t);
            if (m_dead.contains(h)) {
                m_pendingDeath = true;
                m_prunedAt = t;
                m_stopDecisions = true;
                break;
            }
            SearchNode node;
            node.hash = h;
            node.held = m_path.empty() ? 0 : m_path.back().held;
            node.tried = static_cast<uint8_t>(1u << node.held);
            node.optionCount = optionCountNow();
            m_path.push_back(node);
            applyHeld(node.held);
            if (t + 1 > m_maxTick) m_maxTick = t + 1;
            break;
        }
        case Phase::Verify:
        case Phase::FinalVerify: {
            applyHeld(seqAt(m_seq, t));
            break;
        }
        case Phase::Refine: {
            if (m_refineStage == RefineStage::Baseline) {
                if (t >= m_baseFrom && t <= m_baseUntil) {
                    m_baseHashes[t - m_baseFrom] = stateHash(t);
                }
                applyHeld(seqAt(m_seq, t));
            }
            else {
                if (t >= m_convergeFrom && t >= m_baseFrom && t <= m_baseUntil) {
                    uint64_t base = m_baseHashes[t - m_baseFrom];
                    if (base != 0 && stateHash(t) == base) {
                        m_runConverged = true;
                        m_convergedAt = t;
                        m_stopDecisions = true;
                        break;
                    }
                }
                applyHeld(seqAt(m_testSeq, t));
            }
            break;
        }
        default:
            break;
    }
}

void Solver::afterStep(bool) {
    if (m_diedThisStep) {
        m_diedThisStep = false;
        if (!m_stopDecisions) {
            m_pendingDeath = true;
            m_deathTick = m_tick;
            m_stopDecisions = true;
        }
    }
    if (m_completedThisStep || m_layer->m_levelEndAnimationStarted) {
        m_completedThisStep = false;
        if (!m_stopDecisions) {
            m_success = true;
            m_successTick = m_tick;
            m_stopDecisions = true;
        }
    }
    m_tick++;
}

void Solver::onPlayerDestroyed() {
    if (running() && m_inStep) m_diedThisStep = true;
}

void Solver::onLevelComplete() {
    if (running() && m_inStep) m_completedThisStep = true;
}

bool Solver::advanceSelfTest() {
    if (m_selfTestStage == 0) {
        bool ended = m_pendingDeath || m_success || m_tick >= kFastTestTicks;
        if (!ended) {
            if (m_selfTestCpTick == -1 && m_tick >= kSelfTestCheckpointTick) {
                createCheckpointHere();
                m_selfTestCpTick = m_checkpoints.contains(m_tick) ? m_tick : -2;
            }
            return simulate();
        }
        m_selfTestDied = m_pendingDeath;
        m_selfTestLength = m_pendingDeath ? m_deathTick : (m_success ? m_successTick : m_tick);
        clearStepFlags();
        auto cp = m_checkpoints.find(m_selfTestCpTick);
        if (cp != m_checkpoints.end() && m_selfTestLength > m_selfTestCpTick + 8) {
            m_selfTestStage = 1;
            m_selfTestMismatch = -1;
            loadCheckpoint(cp->second);
            return true;
        }
        log::info("Solver self-test: restore check skipped (player dies at tick {} without input)", m_selfTestLength);
        m_selfTestStage = 2;
        m_fastMismatch = -1;
        releaseAllCheckpoints();
        resetToStart();
        return true;
    }

    int limit = m_selfTestLength;
    bool expectDeath = m_selfTestDied;
    if (m_selfTestStage == 1 && m_selfTestCpTick + 160 < limit) {
        limit = m_selfTestCpTick + 160;
        expectDeath = false;
    }
    int stopAt = expectDeath ? limit + 1 : limit;
    bool ended = m_pendingDeath || m_success || m_tick >= stopAt;
    if (!ended) return simulate();

    int& mismatch = m_selfTestStage == 2 ? m_fastMismatch : m_selfTestMismatch;
    if (mismatch < 0) {
        if (m_pendingDeath) {
            if (!expectDeath || m_deathTick != limit) mismatch = m_deathTick;
        }
        else if (expectDeath) {
            mismatch = limit;
        }
    }
    clearStepFlags();

    if (m_selfTestStage == 1) {
        if (m_selfTestMismatch < 0) {
            log::info("Solver self-test: checkpoint restore reproduces the game exactly");
        }
        else if (m_selfTestMismatch <= m_selfTestCpTick + 2) {
            log::warn("Solver self-test: checkpoint restore diverges immediately (tick {}), using exact replay mode", m_selfTestMismatch);
            m_restoreMode = RestoreMode::Replay;
        }
        else {
            log::warn("Solver self-test: checkpoint restore drifts at tick {} (restored at {}); results will be re-verified", m_selfTestMismatch, m_selfTestCpTick);
        }
        m_selfTestStage = 2;
        m_fastMismatch = -1;
        releaseAllCheckpoints();
        resetToStart();
        return true;
    }

    m_fastAllowed = m_fastMismatch < 0;
    if (m_fastAllowed) log::info("Solver self-test: fast simulation matches the full game ({} ticks)", m_selfTestLength);
    else log::warn("Solver self-test: fast simulation differs at tick {}, using full simulation", m_fastMismatch);
    beginSearchFresh();
    return true;
}

void Solver::beginSearchFresh() {
    releaseAllCheckpoints();
    resetToStart();
    m_path.clear();
    m_dead.clear();
    m_best.clear();
    m_maxTick = 0;
    m_maxPercent = 0.f;
    m_replayFromRepair = false;
    m_phase = Phase::Search;
    if (m_restoreMode == RestoreMode::Checkpoint) createCheckpointHere();
}

bool Solver::advanceSearch() {
    if (m_pendingDeath) {
        handleSearchDeath();
        return running();
    }
    if (m_success) {
        onSearchSuccess();
        return running();
    }
    if (timeUp()) {
        m_failReason = "time limit reached";
        std::vector<uint8_t> best = m_best.size() > m_path.size() ? m_best : heldOf(m_path);
        finish(false, best, false, false, false);
        return false;
    }
    if (m_restoreMode == RestoreMode::Checkpoint && !hasCheckpointWithin(m_searchCpInterval)) {
        createCheckpointHere();
    }
    bool advanced = simulate();
    if (advanced && !m_stopDecisions) {
        m_maxPercent = std::max(m_maxPercent, m_layer->getCurrentPercent());
    }
    return advanced;
}

void Solver::handleSearchDeath() {
    bool pruned = m_prunedAt >= 0;
    int failNode = pruned ? m_prunedAt - 1 : m_deathTick;
    if (pruned) m_prunes++;
    clearStepFlags();

    if (m_path.size() > m_best.size()) m_best = heldOf(m_path);
    failNode = std::min(failNode, static_cast<int>(m_path.size()) - 1);
    if (failNode < 0) {
        if (trySearchFallback()) return;
        m_failReason = "the level kills the player before any input can help";
        finish(false, m_best, false, false, false);
        return;
    }
    dfsBacktrack(failNode);
}

void Solver::dfsBacktrack(int failNode) {
    m_path.resize(static_cast<size_t>(failNode) + 1);
    if (m_dead.size() > kMaxDeadStates) m_dead.clear();

    while (!m_path.empty()) {
        int t = static_cast<int>(m_path.size()) - 1;
        int next = nextOption(t);
        if (next >= 0) {
            auto& node = m_path.back();
            node.held = static_cast<uint8_t>(next);
            node.tried = static_cast<uint8_t>(node.tried | (1u << next));
            m_backtracks++;
            restoreTo(t);
            return;
        }
        if (m_path.back().hash != 0) m_dead.insert(m_path.back().hash);
        m_path.pop_back();
        if (m_maxTick - t > m_maxBacktrackTicks) {
            if (trySearchFallback()) return;
            m_failReason = "stuck: no working input found for this section";
            finish(false, m_best, false, false, false);
            return;
        }
    }
    if (trySearchFallback()) return;
    m_failReason = "no possible path was found";
    finish(false, m_best, false, false, false);
}

bool Solver::trySearchFallback() {
    if (timeUp()) return false;
    std::vector<uint8_t> best = m_best;
    float percent = m_maxPercent;
    if (m_fastAllowed) {
        log::warn("Solver: no path found with fast simulation, searching again in full mode");
        m_fastAllowed = false;
    }
    else if (!m_replayFallbackUsed && (m_restoreMode == RestoreMode::Checkpoint || m_replayFromRepair)) {
        log::warn("Solver: no path found, searching again from the start in exact replay mode");
        m_replayFallbackUsed = true;
        m_restoreMode = RestoreMode::Replay;
    }
    else {
        return false;
    }
    beginSearchFresh();
    m_best = std::move(best);
    m_maxPercent = percent;
    return true;
}

void Solver::onSearchSuccess() {
    auto seq = heldOf(m_path);
    if (m_successTick >= 0 && m_successTick + 1 < static_cast<int>(seq.size())) {
        seq.resize(static_cast<size_t>(m_successTick) + 1);
    }
    m_success = false;
    m_maxPercent = 100.f;
    log::info(
        "Solver found a path ({} ticks, {} backtracks, {} pruned, {:.1f}s), verifying",
        seq.size(), m_backtracks, m_prunes, elapsedSeconds()
    );
    beginVerify(std::move(seq), Phase::Verify);
}

void Solver::beginVerify(std::vector<uint8_t> seq, Phase phase) {
    m_seq = std::move(seq);
    m_phase = phase;
    releaseAllCheckpoints();
    resetToStart();
    if (phase == Phase::Verify && m_restoreMode == RestoreMode::Checkpoint) createCheckpointHere();
}

bool Solver::advanceVerify() {
    if (m_pendingDeath) {
        onVerifyFailed(m_deathTick);
        return running();
    }
    if (m_success) {
        onVerifySuccess();
        return running();
    }
    if (m_tick > static_cast<int>(m_seq.size()) + kVerifyGraceTicks) {
        onVerifyFailed(static_cast<int>(m_seq.size()));
        return running();
    }
    if (m_phase == Phase::Verify && m_restoreMode == RestoreMode::Checkpoint && !hasCheckpointWithin(m_verifyCpInterval)) {
        createCheckpointHere();
    }
    return simulate();
}

void Solver::onVerifySuccess() {
    m_success = false;
    if (m_phase == Phase::Verify) {
        log::info("Solver: path verified from a clean restart");
        m_verifiedSeq = m_seq;
        m_verifiedTimes = m_tickTimes;
        if (m_refineEnabled && m_restoreMode == RestoreMode::Checkpoint) {
            beginRefine();
            return;
        }
        if (m_fastAllowed) {
            m_finalIsRefined = false;
            beginVerify(m_verifiedSeq, Phase::FinalVerify);
            return;
        }
        finish(true, m_seq, true, true, false);
        return;
    }
    log::info("Solver: final full-game check passed ({})", m_finalIsRefined ? "refined chart" : "unrefined chart");
    finish(true, m_seq, true, true, m_finalIsRefined);
}

void Solver::onVerifyFailed(int tick) {
    m_pendingDeath = false;
    m_stopDecisions = false;
    m_maxPercent = std::min(m_maxPercent, m_layer->getCurrentPercent());

    if (m_phase == Phase::FinalVerify) {
        if (m_finalIsRefined) {
            log::warn("Solver: refined chart failed the full-game check at tick {}, checking the unrefined chart", tick);
            m_finalIsRefined = false;
            beginVerify(m_verifiedSeq, Phase::FinalVerify);
            return;
        }
        if (m_fastAllowed) {
            log::warn("Solver: fast simulation disagreed with the full game at tick {}, solving again in full mode", tick);
            m_fastAllowed = false;
            m_verifyFailures++;
            size_t keep = std::min<size_t>(static_cast<size_t>(std::max(tick, 0)), m_verifiedSeq.size());
            std::vector<uint8_t> verifiedPrefix(m_verifiedSeq.begin(), m_verifiedSeq.begin() + keep);
            float verifiedPercent = m_maxPercent;
            if (m_verifyFailures > kMaxVerifyFailures || timeUp()) {
                m_failReason = "the found path did not replay reliably";
                finish(false, verifiedPrefix, true, false, false);
                return;
            }
            beginSearchFresh();
            m_best = std::move(verifiedPrefix);
            m_maxPercent = verifiedPercent;
            return;
        }
        log::warn("Solver: chart failed the clean full-game check at tick {}", tick);
        m_phase = Phase::Verify;
        m_seq = m_verifiedSeq;
    }

    m_verifyFailures++;
    m_furthestVerifyFail = std::max(m_furthestVerifyFail, tick);
    log::warn("Solver: verification failed at tick {} (attempt {})", tick, m_verifyFailures);

    if (m_verifyFailures > kMaxVerifyFailures || timeUp()) {
        m_failReason = "the found path did not replay reliably";
        std::vector<uint8_t> prefix(m_seq.begin(), m_seq.begin() + std::min<size_t>(static_cast<size_t>(std::max(tick, 0)), m_seq.size()));
        finish(false, prefix, true, false, false);
        return;
    }
    if (m_verifyFailures >= 3 && m_restoreMode == RestoreMode::Checkpoint) {
        log::warn("Solver: switching to exact replay mode");
        m_restoreMode = RestoreMode::Replay;
        m_replayFromRepair = true;
        for (auto& node : m_path) {
            node.tried = static_cast<uint8_t>(1u << node.held);
            node.hash = 0;
        }
    }
    if (m_verifyFailures >= 2) m_dead.clear();

    int rewind = std::clamp(tick - kRepairRewindTicks, 0, static_cast<int>(m_path.size()));
    m_path.resize(static_cast<size_t>(rewind));
    m_maxTick = rewind;
    m_phase = Phase::Search;
    restoreTo(rewind);
}

std::vector<uint8_t> Solver::shiftedSeq(RefineNote const& note, int shift) const {
    std::vector<uint8_t> out = m_seq;
    uint8_t mask = static_cast<uint8_t>(1u << note.lane);
    for (int t = note.start; t < note.end && t < static_cast<int>(out.size()); t++) {
        out[t] = static_cast<uint8_t>(out[t] & ~mask);
    }
    for (int t = note.start + shift; t < note.end + shift; t++) {
        if (t >= 0 && t < static_cast<int>(out.size())) out[t] = static_cast<uint8_t>(out[t] | mask);
    }
    return out;
}

void Solver::beginRefine() {
    m_phase = Phase::Refine;
    m_refineDeadline = Clock::now() + std::chrono::seconds(std::max(30, m_timeLimitSec / 2));
    m_refineNotes.clear();
    int total = static_cast<int>(m_seq.size());
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        int start = -1;
        for (int t = 0; t <= total; t++) {
            bool down = t < total && (m_seq[t] & mask);
            if (down && start < 0) start = t;
            else if (!down && start >= 0) {
                m_refineNotes.push_back({lane, start, t});
                start = -1;
            }
        }
    }
    std::sort(m_refineNotes.begin(), m_refineNotes.end(), [](RefineNote const& a, RefineNote const& b) {
        return a.start < b.start;
    });
    m_refineIndex = 0;
    m_refineMoved = 0;
    log::info("Solver: centering {} notes in their timing windows", m_refineNotes.size());
    startRefineNote();
}

void Solver::startRefineNote() {
    int total = static_cast<int>(m_seq.size());
    while (m_refineIndex < m_refineNotes.size()) {
        if (Clock::now() > m_refineDeadline) {
            log::warn("Solver: timing refinement ran out of time at note {}/{}", m_refineIndex, m_refineNotes.size());
            break;
        }
        auto const& note = m_refineNotes[m_refineIndex];

        int prevEnd = -1;
        int nextStart = -1;
        for (size_t j = 0; j < m_refineNotes.size(); j++) {
            if (j == m_refineIndex || m_refineNotes[j].lane != note.lane) continue;
            auto const& other = m_refineNotes[j];
            if (other.end <= note.start) prevEnd = std::max(prevEnd, other.end);
            else if (other.start >= note.end && (nextStart < 0 || other.start < nextStart)) nextStart = other.start;
        }

        int minStart = prevEnd >= 0 ? prevEnd + 1 : 0;
        int maxEnd = nextStart >= 0 ? nextStart - 1 : total;
        m_negLimit = std::max(-m_refineWindow, minStart - note.start);
        m_posLimit = std::min(m_refineWindow, maxEnd - note.end);
        if (m_negLimit > 0) m_negLimit = 0;
        if (m_posLimit < 0) m_posLimit = 0;
        if (m_negLimit == 0 && m_posLimit == 0) {
            m_refineIndex++;
            continue;
        }

        int wantedStart = std::max(0, note.start + m_negLimit - 1);
        auto it = m_checkpoints.upper_bound(wantedStart);
        if (it == m_checkpoints.begin()) {
            m_refineIndex++;
            continue;
        }
        --it;
        m_baseFrom = it->first;
        m_baseUntil = note.end + m_posLimit + kRefineHorizon;
        m_baseHashes.assign(static_cast<size_t>(m_baseUntil - m_baseFrom + 1), 0);
        m_baseCompleted = false;
        m_refineStage = RefineStage::Baseline;
        loadCheckpoint(it->second);
        return;
    }

    if (m_refineMoved > 0) {
        log::info("Solver: moved {} notes, verifying refined chart", m_refineMoved);
        m_finalIsRefined = true;
        beginVerify(m_seq, Phase::FinalVerify);
    }
    else if (m_fastAllowed) {
        m_finalIsRefined = false;
        beginVerify(m_verifiedSeq, Phase::FinalVerify);
    }
    else {
        finish(true, m_verifiedSeq, true, true, true);
    }
}

void Solver::beginRefineTest(int shift) {
    auto const& note = m_refineNotes[m_refineIndex];
    m_testShift = shift;
    m_testSeq = shiftedSeq(note, shift);
    m_convergeFrom = std::max(note.end, note.end + shift) + 1;
    m_runConverged = false;
    m_convergedAt = -1;
    m_refineStage = RefineStage::Test;
    auto it = m_checkpoints.find(m_baseFrom);
    if (it == m_checkpoints.end()) {
        m_refineIndex++;
        startRefineNote();
        return;
    }
    loadCheckpoint(it->second);
}

void Solver::nextRefineTest() {
    if (m_posLimit > 0 && m_posBad - m_posGood > 1) {
        int shift = m_posBad == m_posLimit + 1 ? m_posLimit : (m_posGood + m_posBad) / 2;
        beginRefineTest(shift);
        return;
    }
    if (m_negLimit < 0 && m_negGood - m_negBad > 1) {
        int shift = m_negBad == m_negLimit - 1 ? m_negLimit : (m_negGood + m_negBad) / 2;
        beginRefineTest(shift);
        return;
    }
    int center = (m_negGood + m_posGood) / 2;
    if (center == 0) {
        m_refineIndex++;
        startRefineNote();
        return;
    }
    m_confirmingShift = true;
    beginRefineTest(center);
}

void Solver::applyRefineShift(int shift) {
    auto& note = m_refineNotes[m_refineIndex];
    int changedFrom = std::min(note.start, note.start + shift);
    m_seq = shiftedSeq(note, shift);
    note.start += shift;
    note.end += shift;
    m_refineMoved++;
    int changedUntil = m_convergedAt >= 0 ? m_convergedAt : m_baseUntil;
    for (auto it = m_checkpoints.upper_bound(changedFrom); it != m_checkpoints.end() && it->first < changedUntil;) {
        if (it->second.object) it->second.object->release();
        it = m_checkpoints.erase(it);
    }
}

bool Solver::advanceRefine() {
    if (m_refineStage == RefineStage::Baseline) {
        bool ended = m_pendingDeath || m_success || m_tick > m_baseUntil;
        if (!ended) return simulate();

        auto const& note = m_refineNotes[m_refineIndex];
        bool died = m_pendingDeath;
        int deathTick = m_deathTick;
        m_baseCompleted = m_success;
        if (m_success) m_baseUntil = std::min(m_baseUntil, m_successTick);
        m_pendingDeath = false;
        m_success = false;
        m_stopDecisions = false;

        if (died) {
            if (deathTick <= note.end + m_posLimit + 1) {
                m_refineIndex++;
                startRefineNote();
                return true;
            }
            m_baseUntil = std::min(m_baseUntil, deathTick - 1);
        }

        m_posGood = 0;
        m_posBad = m_posLimit + 1;
        m_negGood = 0;
        m_negBad = m_negLimit - 1;
        m_confirmingShift = false;
        nextRefineTest();
        return true;
    }

    bool ended = m_runConverged || m_pendingDeath || m_success || m_tick > m_baseUntil;
    if (!ended) return simulate();

    bool good = m_runConverged || (m_success && m_baseCompleted);
    m_pendingDeath = false;
    m_success = false;
    m_stopDecisions = false;
    m_runConverged = false;

    if (m_confirmingShift) {
        m_confirmingShift = false;
        if (good) applyRefineShift(m_testShift);
        m_refineIndex++;
        startRefineNote();
        return true;
    }
    if (m_testShift > 0) {
        if (good) m_posGood = m_testShift;
        else m_posBad = m_testShift;
    }
    else {
        if (good) m_negGood = m_testShift;
        else m_negBad = m_testShift;
    }
    nextRefineTest();
    return true;
}

std::vector<uint8_t> Solver::heldOf(std::vector<SearchNode> const& path) const {
    std::vector<uint8_t> out;
    out.reserve(path.size());
    for (auto const& node : path) out.push_back(node.held);
    return out;
}

void Solver::finish(bool success, std::vector<uint8_t> const& seq, bool verified, bool complete, bool refined) {
    m_result = Chart{};
    m_result.key = m_key;
    m_result.held = seq;
    m_result.tickTimes = m_tickTimes;
    m_result.twoPlayer = m_twoPlayer;
    m_result.complete = complete;
    m_result.verified = verified;
    m_result.refined = refined;
    m_result.reachedPercent = complete ? 100.f : m_maxPercent;
    m_result.rebuildNotes();
    m_hasResult = true;
    saveChart(m_result);

    log::info(
        "Solver finished: success={} complete={} verified={} refined={} notes={}+{} time={:.1f}s steps={} reason='{}'",
        success, complete, verified, refined, m_result.lanes[0].size(), m_result.lanes[1].size(),
        elapsedSeconds(), m_totalSteps, m_failReason
    );

    cleanup();
    m_phase = success ? Phase::Done : Phase::Failed;
    resetToStart();
    m_layer->m_resumeTimer = 1;
}

void Solver::cleanup() {
    releaseAllCheckpoints();
    if (m_layer) {
        m_layer->m_clickBetweenSteps = m_savedClickBetweenSteps;
        restoreStats();
        setLayerHidden(false);
    }
    setMuted(false);
    m_path.clear();
    m_path.shrink_to_fit();
    m_dead.clear();
    m_baseHashes.clear();
    m_testSeq.clear();
}

std::string Solver::statusLine() const {
    switch (m_phase) {
        case Phase::SelfTest: return "Preparing solver...";
        case Phase::Search: return fmt::format("Finding a path... {:.1f}%", m_maxPercent);
        case Phase::Verify: return "Checking the path from a clean restart...";
        case Phase::Refine: return fmt::format("Centering note timings {}/{}", m_refineIndex, m_refineNotes.size());
        case Phase::FinalVerify: return "Final check...";
        case Phase::Done: return "Solved!";
        case Phase::Failed: return fmt::format("Solver stopped: {}", m_failReason.empty() ? "unknown reason" : m_failReason);
        default: return "";
    }
}

std::string Solver::detailLine() const {
    double elapsed = std::max(0.001, elapsedSeconds());
    double speed = (static_cast<double>(m_totalSteps) / 240.0) / elapsed;
    std::string out = fmt::format(
        "{:.1f}s  |  {:.0f}x game speed  |  {} backtracks  |  {} pruned",
        elapsed, speed, m_backtracks, m_prunes
    );
    if (m_verifyFailures > 0) out += fmt::format("  |  {} re-checks", m_verifyFailures);
    if (m_restoreMode == RestoreMode::Replay) out += "  |  exact replay mode";
    if (m_phase != Phase::SelfTest && !m_fastAllowed) out += "  |  full simulation";
    return out;
}

float Solver::progress() const {
    switch (m_phase) {
        case Phase::Search: return std::clamp(m_maxPercent / 100.f, 0.f, 1.f);
        case Phase::Verify:
        case Phase::FinalVerify:
            return m_seq.empty() ? 0.f : std::clamp(static_cast<float>(m_tick) / m_seq.size(), 0.f, 1.f);
        case Phase::Refine:
            return m_refineNotes.empty() ? 1.f : static_cast<float>(m_refineIndex) / m_refineNotes.size();
        case Phase::Done: return 1.f;
        default: return 0.f;
    }
}

}
