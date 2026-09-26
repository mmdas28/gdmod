#pragma once

#include "Chart.hpp"
#include "PlayerState.hpp"

#include <Geode/Geode.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace rp {

enum class Phase {
    Idle,
    SelfTest,
    Search,
    Verify,
    Refine,
    FinalVerify,
    Done,
    Failed,
};

enum class RestoreMode {
    Checkpoint,
    Replay,
};

struct SavedCheckpoint {
    int tick = 0;
    CheckpointObject* object = nullptr;
    PlayerSnapshot p1;
    PlayerSnapshot p2;
    uint8_t held = 0;
    bool jumping = false;
    double extraDelta = 0.0;
};

struct SearchNode {
    uint64_t hash = 0;
    uint8_t held = 0;
    uint8_t tried = 0;
    uint8_t optionCount = 2;
};

struct StatSnapshot {
    bool valid = false;
    int statJumps = 0;
    int statAttempts = 0;
    int levelAttempts = 0;
    int levelJumps = 0;
    int levelClicks = 0;
    int levelAttemptTime = 0;
    int layerAttempts = 0;
    int layerJumps = 0;
    int layerUncommittedJumps = 0;
    int layerClicks = 0;
};

struct ProbeState {
    bool active = false;
    bool running = false;
    int deathTick = 0;
    int failNode = 0;
    int target = 0;
    int floor = 0;
    int cand = 0;
    int maskIndex = 0;
    uint8_t held = 0;
    int bestCand = -1;
    uint8_t bestHeld = 0;
    int bestReach = 0;
};

struct RefineNote {
    int lane = 0;
    int start = 0;
    int end = 0;
};

class Solver {
public:
    explicit Solver(PlayLayer* layer);
    ~Solver();

    Solver(Solver const&) = delete;
    Solver& operator=(Solver const&) = delete;

    void start();
    void cancel(bool resetLevel);

    Phase phase() const { return m_phase; }
    bool running() const;
    bool finished() const { return m_phase == Phase::Done || m_phase == Phase::Failed; }

    void runFrame();

    bool isStepping() const { return m_inStep; }
    bool isInjecting() const { return m_injecting; }
    bool isInternalReset() const { return m_internalReset; }
    bool skipVisuals() const { return m_inStep && running() && fastModeNow(); }
    void onPause();

    void beforeStep(bool halfTick);
    void afterStep(bool halfTick);
    void onPlayerDestroyed();
    void onLevelComplete();

    std::string statusLine() const;
    std::string detailLine() const;
    float progress() const;

    Chart const& result() const { return m_result; }
    bool hasResult() const { return m_hasResult; }

private:
    using Clock = std::chrono::steady_clock;

    PlayLayer* m_layer = nullptr;
    Phase m_phase = Phase::Idle;
    RestoreMode m_restoreMode = RestoreMode::Checkpoint;

    bool m_inStep = false;
    bool m_injecting = false;
    bool m_internalReset = false;

    int m_tick = 0;
    uint8_t m_curHeld = 0;
    bool m_twoPlayer = false;
    bool m_savedClickBetweenSteps = false;
    bool m_muted = false;

    bool m_diedThisStep = false;
    bool m_completedThisStep = false;
    bool m_stopDecisions = false;
    bool m_pendingDeath = false;
    bool m_success = false;
    int m_deathTick = -1;
    int m_prunedAt = -1;
    int m_successTick = -1;
    int m_stallFrames = 0;
    bool m_sawHalfTick = false;

    bool m_fastAllowed = false;
    bool m_layerHidden = false;
    bool m_layerWasVisible = true;
    ProbeState m_probe;
    int m_dfsUntil = -1;
    double m_stepCostUs = 30.0;
    double m_cpCostUs = 250.0;
    bool m_finalIsRefined = false;

    int m_stepsPerUpdate = 2;
    int m_inputResolution = 1;
    int m_frameBudgetMs = 14;
    int m_timeLimitSec = 600;
    bool m_refineEnabled = true;
    int m_refineWindow = 20;

    int m_searchCpInterval = 8;
    int m_denseWindow = 1440;
    int m_sparseInterval = 240;
    int m_verifyCpInterval = 60;
    int m_maxBacktrackTicks = 240 * 20;

    std::map<int, SavedCheckpoint> m_checkpoints;
    std::vector<SearchNode> m_path;
    std::unordered_set<uint64_t> m_dead;
    std::vector<uint8_t> m_best;
    int m_maxTick = 0;
    float m_maxPercent = 0.f;

    std::vector<uint8_t> m_seq;
    std::vector<uint8_t> m_verifiedSeq;
    std::vector<double> m_tickTimes;
    std::vector<double> m_verifiedTimes;
    int m_verifyFailures = 0;
    int m_furthestVerifyFail = -1;

    std::vector<uint64_t> m_selfTestHashes;
    int m_selfTestStage = 0;
    int m_selfTestLength = 0;
    int m_selfTestMismatch = -1;
    int m_selfTestCpTick = -1;
    bool m_selfTestDied = false;
    int m_fastMismatch = -1;

    enum class RefineStage { Baseline, Test };
    std::vector<RefineNote> m_refineNotes;
    size_t m_refineIndex = 0;
    RefineStage m_refineStage = RefineStage::Baseline;
    std::vector<uint64_t> m_baseHashes;
    int m_baseFrom = 0;
    int m_baseUntil = 0;
    bool m_baseCompleted = false;
    int m_testShift = 0;
    int m_convergeFrom = 0;
    int m_convergedAt = -1;
    int m_posLimit = 0;
    int m_negLimit = 0;
    int m_posGood = 0;
    int m_posBad = 0;
    int m_negGood = 0;
    int m_negBad = 0;
    bool m_confirmingShift = false;
    int m_refineMoved = 0;
    bool m_runConverged = false;
    std::vector<uint8_t> m_testSeq;

    Clock::time_point m_startTime;
    Clock::time_point m_refineDeadline;
    Clock::time_point m_lastLog;
    uint64_t m_totalSteps = 0;
    uint64_t m_backtracks = 0;
    uint64_t m_prunes = 0;
    uint64_t m_probeRuns = 0;
    uint64_t m_greedyCommits = 0;

    StatSnapshot m_stats;
    std::string m_key;
    Chart m_result;
    bool m_hasResult = false;
    std::string m_failReason;

    void readSettings();
    void captureStats();
    void restoreStats();
    double elapsedSeconds() const;
    bool timeUp() const;

    void setMuted(bool muted);
    void inject(int lane, bool down);
    void applyHeld(uint8_t held);
    void releaseAll();
    uint64_t stateHash(int tick) const;
    uint8_t optionCountNow() const;
    bool fastModeNow() const;
    void setLayerHidden(bool hidden);
    void updateCheckpointInterval();
    void clearStepFlags();
    int nextOption(int tick) const;

    void resetToStart();
    bool simulate();
    void createCheckpointHere();
    void thinCheckpoints();
    void releaseCheckpointsAfter(int tick);
    void releaseAllCheckpoints();
    void loadCheckpoint(SavedCheckpoint const& cp);
    void restoreTo(int tick);
    bool hasCheckpointWithin(int interval) const;

    bool advanceSelfTest();
    bool advanceSearch();
    bool advanceVerify();
    bool advanceRefine();

    void beginSearchFresh();
    void handleSearchDeath();
    void dfsBacktrack(int failNode);
    void startProbe(int deathTick, int failNode);
    void nextProbeCandidate();
    void commitProbe(int cand, uint8_t held);
    void finishProbeWithoutSuccess();
    bool advanceProbe();
    void onSearchSuccess();
    void beginVerify(std::vector<uint8_t> seq, Phase phase);
    void onVerifySuccess();
    void onVerifyFailed(int tick);

    void beginRefine();
    void startRefineNote();
    void beginRefineTest(int shift);
    void nextRefineTest();
    void applyRefineShift(int shift);
    std::vector<uint8_t> shiftedSeq(RefineNote const& note, int shift) const;

    std::vector<uint8_t> heldOf(std::vector<SearchNode> const& path) const;
    void finish(bool success, std::vector<uint8_t> const& seq, bool verified, bool complete, bool refined);
    void cleanup();
};

}
