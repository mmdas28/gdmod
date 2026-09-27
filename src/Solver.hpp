#pragma once

#include "Chart.hpp"
#include "PlayerState.hpp"
#include "SolverProgress.hpp"
#include "Storage.hpp"

#include <Geode/Geode.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace rp {

enum class Phase {
    Idle,
    SelfTest,
    Search,
    Verify,
    Optimize,
    Refine,
    FinalVerify,
    Done,
    Failed,
};

enum class RestoreMode {
    Checkpoint,
    Replay,
};

enum class SolveKind {
    Fresh,
    Resume,
    CheckImport,
};

struct SolveRequest {
    SolveKind kind = SolveKind::Fresh;
    std::vector<uint8_t> importSeq;
    bool repairImport = true;
    std::optional<SolveProgress> progress;
    float targetPercent = 100.f;
    ChartInfo info;
};

struct SolverView {
    Phase phase = Phase::Idle;
    std::string title;
    std::string detail;
    std::string note;
    float progress = 0.f;
    float percent = 0.f;
    double elapsed = 0.0;
    double gameSpeed = 0.0;
    int stepIndex = 0;
    std::vector<std::string> steps;
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
    uint8_t def = 0;
    uint8_t flips = 0;
    bool seeded = false;
    bool recompute = false;
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

struct RefineNote {
    int lane = 0;
    int start = 0;
    int end = 0;
};

class Solver {
public:
    Solver(PlayLayer* layer, LevelRef ref);
    ~Solver();

    Solver(Solver const&) = delete;
    Solver& operator=(Solver const&) = delete;

    void start(SolveRequest request);
    void cancel(bool resetLevel);
    bool saveProgress();

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

    SolverView view() const;
    std::string statusLine() const;
    std::string detailLine() const;
    float progress() const;
    double elapsed() const { return elapsedSeconds(); }
    std::string const& failReason() const { return m_failReason; }

    Chart const& result() const { return m_result; }
    bool hasResult() const { return m_hasResult; }

private:
    using Clock = std::chrono::steady_clock;

    enum class RefineStage { Baseline, Test };
    enum class OptStep { Remove, Join, Shorten, Finished };
    enum class EditKind { Remove, Join, Shorten };

    struct LadderStep {
        int tier = 2;
        bool constraints = false;
    };

    PlayLayer* m_layer = nullptr;
    LevelRef m_ref;
    SolveRequest m_request;
    Phase m_phase = Phase::Idle;
    Phase m_stoppedPhase = Phase::Idle;
    RestoreMode m_restoreMode = RestoreMode::Checkpoint;

    bool m_inStep = false;
    bool m_injecting = false;
    bool m_internalReset = false;

    int m_tick = 0;
    uint8_t m_curHeld = 0;
    bool m_twoPlayer = false;
    bool m_savedClickBetweenSteps = false;
    bool m_muted = false;
    float m_targetPercent = 100.f;

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
    std::vector<std::pair<geode::Ref<cocos2d::CCNode>, bool>> m_hiddenChildren;
    double m_stepCostUs = 30.0;
    double m_cpCostUs = 250.0;
    bool m_finalIsEdited = false;
    bool m_replayFallbackUsed = false;
    bool m_replayFromRepair = false;
    bool m_importChecking = false;
    bool m_fromImport = false;
    bool m_bestFromImport = false;
    bool m_resumed = false;
    bool m_exhausted = false;

    int m_stepsPerUpdate = 4;
    int m_inputResolution = 1;
    double m_frameBudgetMs = 14.0;
    int m_timeLimitSec = 600;
    bool m_refineEnabled = true;
    int m_refineWindow = 20;
    bool m_optimizeEnabled = true;
    bool m_preferHolds = true;
    double m_mergeGapMs = 40.0;
    int m_minHold[3] = {1, 1, 1};
    int m_minRelease[3] = {1, 1, 1};
    bool m_relaxTiming = true;
    int m_startTier = 0;
    bool m_constraintsActive = false;
    int m_constraintCap = 1;

    std::vector<LadderStep> m_ladder;
    int m_level = 0;
    int m_escalatedAt = 0;
    int m_levelBase = 0;
    int m_finestTier = -1;
    int m_preloadEnd = 0;
    bool m_preloadTiming = false;
    std::string m_preloadSignature;
    int m_progressMark = 0;
    double m_progressTime = 0.0;
    void markProgress();

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
    std::vector<uint8_t> m_laneModes;
    std::vector<std::pair<uintptr_t, uint32_t>> m_objectIndex;

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
    bool m_refineRan = false;
    bool m_runConverged = false;
    std::vector<uint8_t> m_testSeq;

    std::vector<RefineNote> m_optNotes;
    OptStep m_optStep = OptStep::Finished;
    EditKind m_editKind = EditKind::Remove;
    int m_optCursorStart = -1;
    int m_optCursorLane = -1;
    int m_optLo = 0;
    int m_optHi = 0;
    int m_optTestEnd = 0;
    int m_optBestConverged = -1;
    bool m_optShortenReady = false;
    bool m_baseValid = false;
    int m_baseChangeEnd = 0;
    int m_testChangeFrom = 0;
    int m_optEdits = 0;
    size_t m_optPosition = 0;

    Clock::time_point m_frameStart;
    Clock::time_point m_lastFrameEnd;
    Clock::time_point m_lastLog;
    Clock::time_point m_lastSave;
    Clock::time_point m_speedTime;
    bool m_inFrame = false;
    bool m_frameClockValid = false;
    double m_activeSeconds = 0.0;
    double m_optDeadline = 0.0;
    double m_refineDeadline = 0.0;
    double m_polishDeadline = 0.0;
    double m_elapsedOffset = 0.0;
    double m_frozenElapsed = -1.0;
    double m_recentSpeed = 0.0;
    uint64_t m_speedSteps = 0;
    uint64_t m_totalSteps = 0;
    uint64_t m_backtracks = 0;
    uint64_t m_prunes = 0;
    uint64_t m_lastSaveSteps = 0;
    Phase m_lastSavePhase = Phase::Idle;
    bool m_lastSaveValid = false;

    StatSnapshot m_stats;
    std::string m_key;
    Chart m_result;
    bool m_hasResult = false;
    std::string m_failReason;

    void readSettings();
    void buildLadder();
    void buildObjectIndex();
    std::string settingsSignature() const;
    void captureStats();
    void restoreStats();
    double runSeconds() const;
    double elapsedSeconds() const;
    bool timeUp() const;
    void updateSpeed(Clock::time_point now);

    void setMuted(bool muted);
    void inject(int lane, bool down);
    void applyHeld(uint8_t held);
    void releaseAll();

    LadderStep const& currentStep() const;
    bool canEscalate() const;
    void escalate();
    void deescalate();
    uint64_t portalKey(PlayerObject* player) const;
    uint64_t hashState(int tick, int tier, bool constraints) const;
    uint64_t stateHash(int tick) const;
    uint64_t exactHash(int tick) const;
    int sinceChange(int tick, int lane) const;
    int requiredTicks(uint8_t family, bool held) const;
    uint8_t laneModesNow() const;
    uint8_t laneModesAt(int tick) const;
    void recordLaneModes(int tick, uint8_t modes);
    void policyAt(int tick, uint8_t modes, uint8_t& def, uint8_t& flips) const;
    void settleNode(int tick, uint8_t modes);
    bool changeBreaksTiming(int tick, uint8_t modes, uint8_t held) const;
    void seedPath(std::vector<uint8_t> const& seq, int length);
    void cutPreload(int tick);
    bool importRun() const;
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
    void releaseCheckpointsBetween(int after, int before);
    void releaseAllCheckpoints();
    void loadCheckpoint(SavedCheckpoint const& cp);
    void restoreTo(int tick);
    bool hasCheckpointWithin(int interval) const;

    bool advanceSelfTest();
    bool advanceSearch();
    bool advanceVerify();
    bool advanceOptimize();
    bool advanceRefine();

    void beginAfterSelfTest();
    void resumeFrom(SolveProgress const& progress);
    void beginSearchFresh();
    void handleSearchDeath();
    void dfsBacktrack(int failNode);
    bool trySearchFallback();
    void stopWithBest();
    void onSearchSuccess();
    void beginVerify(std::vector<uint8_t> seq, Phase phase);
    void onVerifySuccess();
    void onVerifyFailed(int tick);

    void beginOptimize();
    void nextOptNote();
    bool nextOptTest();
    bool launchOptTest(EditKind kind, std::vector<uint8_t> seq, int changeFrom, int changeTo);
    bool startOptBaseline();
    bool startTestRun();
    void onOptTestResult(bool good);
    void applyOptEdit(std::vector<uint8_t> seq, int changedFrom, int convergedAt);
    void finishOptimize();
    int findOptNote(int start, int lane) const;
    int firstOptNoteAfter(int start, int lane) const;
    int nextOptNoteInLane(int index) const;
    bool joinAllowed(RefineNote const& note, RefineNote const& next) const;
    bool continuousAt(int lane, int tick) const;
    int editRequirement(int lane, int tick, bool held) const;
    int laneViolations(std::vector<uint8_t> const& seq, int lane, int from, int to) const;
    bool editBreaksTiming(std::vector<uint8_t> const& edited, int lane, int from, int to) const;

    void beginRefine();
    void startRefineNote();
    void beginRefineTest(int shift);
    void nextRefineTest();
    void applyRefineShift(int shift);
    bool shiftBreaksTiming(RefineNote const& note, int shift) const;
    std::vector<uint8_t> shiftedSeq(RefineNote const& note, int shift) const;

    void startFinalCheck();
    void finishSuccess(std::vector<uint8_t> seq);
    bool sequenceViolatesTiming(std::vector<uint8_t> const& seq) const;
    bool existingChartIsBetter(Chart const& chart) const;
    std::vector<uint8_t> heldOf(std::vector<SearchNode> const& path) const;
    void finish(bool success, std::vector<uint8_t> const& seq, bool verified, bool complete, bool refined);
    void cleanup();
    std::string phaseNote() const;
};

}
