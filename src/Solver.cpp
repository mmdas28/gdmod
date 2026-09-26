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
constexpr size_t kMaxSavedDeadStates = 1'500'000;
constexpr int kMaxVerifyFailures = 12;
constexpr int kRepairRewindTicks = 120;
constexpr int kFastTestTicks = 480;
constexpr int kEscalateTicks = 240 * 5;
constexpr int kDeescalateTicks = 240 * 10;
constexpr int kJoinGapTicks = 150;
constexpr int kExactTier = 2;
constexpr auto kAutoSaveInterval = std::chrono::seconds(45);
constexpr auto kLogInterval = std::chrono::seconds(10);
constexpr double kMaxFrameGapSeconds = 0.1;
constexpr double kMaxGameFrameMs = 100.0;

constexpr uint8_t kFamShip = 1;
constexpr uint8_t kFamWave = 2;
constexpr uint8_t kFamOther = 4;
constexpr uint8_t kFamAll = 7;
constexpr int kLaneShift = 3;
constexpr uint8_t kLane1Active = 0x40;
constexpr uint8_t kModesUnknown = 0x80;

uint8_t seqAt(std::vector<uint8_t> const& seq, int tick) {
    if (tick < 0 || tick >= static_cast<int>(seq.size())) return 0;
    return seq[tick];
}

int msToTicks(double ms) {
    if (!std::isfinite(ms) || ms <= 0.0) return 1;
    double ticks = std::ceil(ms * kTicksPerSecond / 1000.0 - 1e-9);
    return static_cast<int>(std::clamp(ticks, 1.0, 2400.0));
}

uint8_t familyOf(PlayerObject* player) {
    if (!player) return kFamOther;
    if (player->m_isShip) return kFamShip;
    if (player->m_isDart) return kFamWave;
    return kFamOther;
}

uint8_t laneFamily(uint8_t modes, int lane) {
    return static_cast<uint8_t>((modes >> (kLaneShift * lane)) & kFamAll);
}

bool laneActive(uint8_t modes, int lane) {
    return lane == 0 || (modes & kLane1Active) != 0;
}

bool isContinuous(uint8_t family) {
    return (family & (kFamShip | kFamWave)) != 0;
}

char const* tierName(int tier) {
    switch (tier) {
        case 0: return "fast";
        case 1: return "balanced";
        default: return "exact";
    }
}

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool noteBefore(RefineNote const& a, int start, int lane) {
    return a.start != start ? a.start < start : a.lane < lane;
}

std::vector<RefineNote> notesOf(std::vector<uint8_t> const& seq) {
    std::vector<RefineNote> notes;
    int total = static_cast<int>(seq.size());
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        int start = -1;
        for (int t = 0; t <= total; t++) {
            bool down = t < total && (seq[t] & mask);
            if (down && start < 0) start = t;
            else if (!down && start >= 0) {
                notes.push_back({lane, start, t});
                start = -1;
            }
        }
    }
    std::sort(notes.begin(), notes.end(), [](RefineNote const& a, RefineNote const& b) {
        return noteBefore(a, b.start, b.lane);
    });
    return notes;
}

void setLane(std::vector<uint8_t>& seq, int lane, int from, int to, bool down) {
    uint8_t mask = static_cast<uint8_t>(1u << lane);
    int end = std::min(to, static_cast<int>(seq.size()));
    for (int t = std::max(from, 0); t < end; t++) {
        seq[t] = down ? static_cast<uint8_t>(seq[t] | mask) : static_cast<uint8_t>(seq[t] & ~mask);
    }
}

float cleanPercent(float value, float fallback) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 100.f) : fallback;
}

}

Solver::Solver(PlayLayer* layer, LevelRef ref) : m_layer(layer), m_ref(std::move(ref)) {}

Solver::~Solver() {
    releaseAllCheckpoints();
    setMuted(false);
}

bool Solver::running() const {
    switch (m_phase) {
        case Phase::SelfTest:
        case Phase::Search:
        case Phase::Verify:
        case Phase::Optimize:
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
    auto fps = std::clamp<int64_t>(mod->getSettingValue<int64_t>("solver-fps"), 10, 240);
    m_frameBudgetMs = std::max(1.0, 0.85 * 1000.0 / static_cast<double>(fps));
    m_timeLimitSec = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("time-limit"), 5, 86400));
    m_refineEnabled = mod->getSettingValue<bool>("refine-timing");
    m_refineWindow = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("refine-window"), 1, 240));
    m_optimizeEnabled = mod->getSettingValue<bool>("optimize-inputs");
    m_preferHolds = mod->getSettingValue<bool>("prefer-holds");
    double gap = mod->getSettingValue<double>("merge-gap-ms");
    m_mergeGapMs = std::isfinite(gap) ? std::clamp(gap, 0.0, 1000.0) : 0.0;
    m_minHold[0] = msToTicks(mod->getSettingValue<double>("ship-min-hold-ms"));
    m_minRelease[0] = msToTicks(mod->getSettingValue<double>("ship-min-release-ms"));
    m_minHold[1] = msToTicks(mod->getSettingValue<double>("wave-min-hold-ms"));
    m_minRelease[1] = msToTicks(mod->getSettingValue<double>("wave-min-release-ms"));
    m_minHold[2] = msToTicks(mod->getSettingValue<double>("other-min-hold-ms"));
    m_minRelease[2] = msToTicks(mod->getSettingValue<double>("other-min-release-ms"));
    m_relaxTiming = mod->getSettingValue<bool>("relax-timing-if-stuck");
    auto precision = mod->getSettingValue<std::string>("ship-wave-precision");
    m_startTier = precision == "Exact" ? 2 : (precision == "Balanced" ? 1 : 0);
    m_constraintCap = 1;
    for (int i = 0; i < 3; i++) m_constraintCap = std::max({m_constraintCap, m_minHold[i], m_minRelease[i]});
    m_constraintsActive = m_constraintCap > 1;
    m_verifyCpInterval = (m_refineEnabled || m_optimizeEnabled) ? 24 : 60;
}

void Solver::buildLadder() {
    m_ladder.clear();
    auto add = [&](int tier, bool constraints) {
        if (tier < m_startTier) return;
        if (!constraints && !(m_relaxTiming && m_constraintsActive)) return;
        bool on = constraints && m_constraintsActive;
        for (auto const& step : m_ladder) {
            if (step.tier == tier && step.constraints == on) return;
        }
        m_ladder.push_back({tier, on});
    };
    add(m_startTier, true);
    add(1, true);
    add(2, true);
    add(m_startTier, false);
    add(1, false);
    add(2, false);
    m_level = 0;
}

void Solver::buildObjectIndex() {
    m_objectIndex.clear();
    auto objects = m_layer ? m_layer->m_objects : nullptr;
    if (!objects) return;
    m_objectIndex.reserve(objects->count());
    uint32_t index = 0;
    for (auto object : CCArrayExt<GameObject*>(objects)) {
        index++;
        if (object) m_objectIndex.emplace_back(reinterpret_cast<uintptr_t>(object), index);
    }
    std::sort(m_objectIndex.begin(), m_objectIndex.end());
}

std::string Solver::settingsSignature() const {
    return fmt::format(
        "spu={};res={};hold={},{},{};release={},{},{};relax={};precision={};2p={};target={:.3f}",
        m_stepsPerUpdate, m_inputResolution, m_minHold[0], m_minHold[1], m_minHold[2],
        m_minRelease[0], m_minRelease[1], m_minRelease[2], m_relaxTiming ? 1 : 0, m_startTier,
        m_twoPlayer ? 1 : 0, m_targetPercent
    );
}

double Solver::runSeconds() const {
    double seconds = m_activeSeconds;
    if (m_inFrame) seconds += std::chrono::duration<double>(Clock::now() - m_frameStart).count();
    return seconds;
}

double Solver::elapsedSeconds() const {
    if (m_phase == Phase::Idle) return 0.0;
    if (m_frozenElapsed >= 0.0) return m_frozenElapsed;
    return m_elapsedOffset + runSeconds();
}

bool Solver::timeUp() const {
    return runSeconds() > m_timeLimitSec;
}

void Solver::updateSpeed(Clock::time_point now) {
    double dt = std::chrono::duration<double>(now - m_speedTime).count();
    if (dt < 0.5) return;
    double speed = static_cast<double>(m_totalSteps - m_speedSteps) / kTicksPerSecond / dt;
    m_recentSpeed = m_recentSpeed <= 0.0 ? speed : m_recentSpeed * 0.6 + speed * 0.4;
    m_speedTime = now;
    m_speedSteps = m_totalSteps;
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

Solver::LadderStep const& Solver::currentStep() const {
    static LadderStep const exact{kExactTier, false};
    if (m_ladder.empty()) return exact;
    return m_ladder[std::clamp(m_level, 0, static_cast<int>(m_ladder.size()) - 1)];
}

bool Solver::canEscalate() const {
    return m_level + 1 < static_cast<int>(m_ladder.size());
}

void Solver::escalate() {
    m_level = std::min(m_level + 1, static_cast<int>(m_ladder.size()) - 1);
    m_escalatedAt = m_maxTick;
    m_levelBase = static_cast<int>(m_path.size());
    m_preloadEnd = 0;
    m_dead.clear();
    for (int t = 0; t < static_cast<int>(m_path.size()); t++) {
        auto& node = m_path[t];
        node.hash = 0;
        node.tried = static_cast<uint8_t>(1u << (node.held & 3));
        uint8_t modes = laneModesAt(t);
        if (modes & kModesUnknown) continue;
        if (node.recompute) {
            settleNode(t, modes);
            continue;
        }
        uint8_t def = 0;
        uint8_t flips = 0;
        policyAt(t, modes, def, flips);
        node.flips = static_cast<uint8_t>(node.flips | flips);
    }
    auto const& step = currentStep();
    m_finestTier = std::max(m_finestTier, step.tier);
    log::info(
        "Solver: stuck before tick {}, retrying from tick {} with {} ship/wave precision{} (level {}/{})",
        m_maxTick, m_path.size(), tierName(step.tier),
        step.constraints || !m_constraintsActive ? "" : " and relaxed timing limits", m_level + 1, m_ladder.size()
    );
    restoreTo(static_cast<int>(m_path.size()));
}

void Solver::deescalate() {
    m_level = 0;
    m_levelBase = 0;
    m_preloadEnd = 0;
    m_dead.clear();
    for (auto& node : m_path) node.hash = 0;
    log::info("Solver: passed the hard section (tick {}), back to {} precision", m_maxTick, tierName(currentStep().tier));
}

uint64_t Solver::portalKey(PlayerObject* player) const {
    auto portal = player ? player->m_lastActivatedPortal : nullptr;
    if (!portal) return 0;
    auto key = reinterpret_cast<uintptr_t>(portal);
    auto it = std::lower_bound(
        m_objectIndex.begin(), m_objectIndex.end(), key,
        [](std::pair<uintptr_t, uint32_t> const& entry, uintptr_t value) { return entry.first < value; }
    );
    if (it != m_objectIndex.end() && it->first == key) return it->second;
    return 0x8000000000000000ull ^ static_cast<uint64_t>(key);
}

uint64_t Solver::hashState(int tick, int tier, bool constraints) const {
    uint64_t h = mixHash(0x5eed5eedull, static_cast<uint64_t>(tick));
    h = mixHash(h, m_curHeld);
    auto p1 = m_layer->m_player1;
    h = mixHash(h, hashPlayer(p1, tier, portalKey(p1)));
    if (m_layer->m_gameState.m_isDualMode) {
        auto p2 = m_layer->m_player2;
        h = mixHash(h, hashPlayer(p2, tier, portalKey(p2)));
    }
    else {
        h = mixHash(h, 7);
    }
    h = mixHash(h, static_cast<uint64_t>(m_layer->m_gameState.m_activatedObjectIDs.size()));
    if (constraints) {
        for (int lane = 0; lane < kLaneCount; lane++) {
            h = mixHash(h, static_cast<uint64_t>(sinceChange(tick, lane)));
        }
    }
    return h;
}

uint64_t Solver::stateHash(int tick) const {
    auto const& step = currentStep();
    return hashState(tick, step.tier, step.constraints);
}

uint64_t Solver::exactHash(int tick) const {
    return hashState(tick, kExactTier, false);
}

int Solver::sinceChange(int tick, int lane) const {
    uint8_t mask = static_cast<uint8_t>(1u << lane);
    int size = static_cast<int>(m_path.size());
    auto down = [&](int k) { return k >= 0 && k < size && (m_path[k].held & mask) != 0; };
    for (int s = 1; s < m_constraintCap; s++) {
        int at = tick - s;
        if (at < 0) return m_constraintCap;
        if (down(at) != down(at - 1)) return s;
    }
    return m_constraintCap;
}

int Solver::requiredTicks(uint8_t family, bool held) const {
    if (family == 0) family = kFamAll;
    int const* table = held ? m_minHold : m_minRelease;
    int required = 1;
    if (family & kFamShip) required = std::max(required, table[0]);
    if (family & kFamWave) required = std::max(required, table[1]);
    if (family & kFamOther) required = std::max(required, table[2]);
    return required;
}

uint8_t Solver::laneModesNow() const {
    uint8_t lane0 = familyOf(m_layer->m_player1);
    uint8_t lane1 = 0;
    uint8_t flags = 0;
    if (m_layer->m_gameState.m_isDualMode) {
        if (m_twoPlayer) {
            lane1 = familyOf(m_layer->m_player2);
            flags = kLane1Active;
        }
        else {
            lane0 = static_cast<uint8_t>(lane0 | familyOf(m_layer->m_player2));
        }
    }
    return static_cast<uint8_t>(lane0 | (lane1 << kLaneShift) | flags);
}

uint8_t Solver::laneModesAt(int tick) const {
    if (tick < 0 || tick >= static_cast<int>(m_laneModes.size())) return kModesUnknown;
    return m_laneModes[tick];
}

void Solver::recordLaneModes(int tick, uint8_t modes) {
    if (tick < 0) return;
    if (static_cast<int>(m_laneModes.size()) <= tick) m_laneModes.resize(static_cast<size_t>(tick) + 1, kModesUnknown);
    m_laneModes[tick] = modes;
}

void Solver::policyAt(int tick, uint8_t modes, uint8_t& def, uint8_t& flips) const {
    uint8_t prev = tick > 0 && tick - 1 < static_cast<int>(m_path.size()) ? m_path[tick - 1].held : 0;
    bool decisionTick = m_inputResolution <= 1 || tick % m_inputResolution == 0;
    bool constraints = currentStep().constraints;
    def = 0;
    flips = 0;
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        if (!laneActive(modes, lane)) continue;
        bool prevDown = (prev & mask) != 0;
        uint8_t family = laneFamily(modes, lane);
        bool canFlip = decisionTick;
        if (canFlip && constraints) {
            int required = requiredTicks(family, prevDown);
            if (required > 1 && sinceChange(tick, lane) < required) canFlip = false;
        }
        if (canFlip) {
            flips = static_cast<uint8_t>(flips | mask);
            if (prevDown && isContinuous(family)) def = static_cast<uint8_t>(def | mask);
        }
        else if (prevDown) {
            def = static_cast<uint8_t>(def | mask);
        }
    }
}

void Solver::settleNode(int tick, uint8_t modes) {
    auto& node = m_path[tick];
    policyAt(tick, modes, node.def, node.flips);
    node.seeded = ((node.held ^ node.def) & ~node.flips & 3) != 0;
    node.recompute = false;
}

bool Solver::changeBreaksTiming(int tick, uint8_t modes, uint8_t held) const {
    if (!m_constraintsActive || (modes & kModesUnknown)) return false;
    uint8_t prev = tick > 0 && tick - 1 < static_cast<int>(m_path.size()) ? m_path[tick - 1].held : 0;
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        if (!laneActive(modes, lane) || !((held ^ prev) & mask)) continue;
        int required = requiredTicks(laneFamily(modes, lane), (prev & mask) != 0);
        if (required > 1 && sinceChange(tick, lane) < required) return true;
    }
    return false;
}

void Solver::seedPath(std::vector<uint8_t> const& seq, int length) {
    m_path.clear();
    int count = std::min(length, static_cast<int>(seq.size()));
    if (count <= 0) return;
    m_path.reserve(static_cast<size_t>(count));
    for (int t = 0; t < count; t++) {
        SearchNode node;
        node.held = static_cast<uint8_t>(seq[t] & 3);
        node.def = node.held;
        node.tried = static_cast<uint8_t>(1u << node.held);
        node.seeded = true;
        node.recompute = true;
        m_path.push_back(node);
        uint8_t modes = laneModesAt(t);
        if (!(modes & kModesUnknown)) settleNode(t, modes);
    }
}

void Solver::cutPreload(int tick) {
    log::info("Solver: the saved path is faster than the current timing limits at tick {}, searching on from there", tick);
    m_path.resize(static_cast<size_t>(tick) + 1);
    auto& node = m_path[tick];
    node.held = node.def;
    node.tried = static_cast<uint8_t>(node.tried | (1u << node.def));
    node.seeded = false;
    m_preloadEnd = 0;
    m_best = heldOf(m_path);
    m_bestFromImport = m_fromImport;
    m_maxPercent = cleanPercent(m_layer->getCurrentPercent(), 0.f);
}

bool Solver::importRun() const {
    return m_request.kind == SolveKind::CheckImport;
}

bool Solver::fastModeNow() const {
    if (m_phase == Phase::SelfTest) return m_selfTestStage == 2;
    if (m_phase == Phase::FinalVerify || m_importChecking) return false;
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

int Solver::nextOption(int tick) const {
    auto const& node = m_path[tick];
    uint8_t flips = static_cast<uint8_t>(node.flips & 3);
    for (uint8_t k = 0; k < 4; k++) {
        if (k & ~flips) continue;
        uint8_t option = static_cast<uint8_t>((node.def ^ k) & 3);
        if (node.tried & (1u << option)) continue;
        return option;
    }
    return -1;
}

void Solver::start(SolveRequest request) {
    if (running()) return;
    m_request = std::move(request);
    for (auto& v : m_request.importSeq) v = static_cast<uint8_t>(v & 3);
    readSettings();

    float target = m_request.targetPercent;
    m_targetPercent = std::isfinite(target) && target > 0.f ? std::clamp(target, 1.f, 100.f) : 100.f;
    m_key = m_ref.key();
    m_twoPlayer = m_layer->m_levelSettings && m_layer->m_levelSettings->m_twoPlayerMode;
    buildLadder();
    buildObjectIndex();

    auto now = Clock::now();
    m_lastLog = now;
    m_lastSave = now;
    m_speedTime = now;
    m_inFrame = false;
    m_frameClockValid = false;
    m_activeSeconds = 0.0;
    m_elapsedOffset = 0.0;
    m_frozenElapsed = -1.0;
    m_recentSpeed = 0.0;
    m_speedSteps = 0;
    m_lastSaveValid = false;
    m_exhausted = false;
    m_resumed = false;
    if (m_request.kind == SolveKind::Resume && m_request.progress) {
        if (m_request.progress->levelHash == m_ref.levelHash) {
            m_resumed = true;
            double offset = m_request.progress->elapsedSeconds;
            m_elapsedOffset = std::isfinite(offset) ? std::clamp(offset, 0.0, 1e9) : 0.0;
        }
        else {
            log::warn("Solver: saved progress is for a different version of this level, starting fresh");
            m_request.progress.reset();
        }
    }

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
    m_laneModes.clear();
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
    m_finalIsEdited = false;
    m_replayFallbackUsed = false;
    m_replayFromRepair = false;
    m_importChecking = false;
    m_fromImport = false;
    m_bestFromImport = false;
    m_escalatedAt = 0;
    m_levelBase = 0;
    m_preloadEnd = 0;
    m_preloadTiming = false;
    m_finestTier = -1;
    m_optEdits = 0;
    m_refineMoved = 0;
    m_refineRan = false;
    m_stoppedPhase = Phase::Idle;
    setLayerHidden(true);

    log::info(
        "Solver starting for {} ({}, two player: {}, steps/update: {}, input resolution: {}, time limit: {}s, target: {:.0f}%, "
        "precision: {}, timing limits: {}, search levels: {})",
        m_key,
        m_request.kind == SolveKind::CheckImport ? "import check" : (m_resumed ? "resumed" : "fresh"),
        m_twoPlayer, m_stepsPerUpdate, m_inputResolution, m_timeLimitSec, m_targetPercent,
        tierName(m_startTier), m_constraintsActive ? (m_relaxTiming ? "on, relaxed if stuck" : "on") : "off", m_ladder.size()
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
    saveProgress();
    m_frozenElapsed = elapsedSeconds();
    m_stoppedPhase = m_phase;
    cleanup();
    m_failReason = "cancelled";
    m_phase = Phase::Failed;
    if (resetLevel) {
        resetToStart();
        m_layer->m_resumeTimer = 1;
    }
}

bool Solver::saveProgress() {
    if (m_inStep || importRun()) return false;
    bool pathFound = false;
    switch (m_phase) {
        case Phase::Search:
            break;
        case Phase::Verify:
        case Phase::Optimize:
        case Phase::Refine:
        case Phase::FinalVerify:
            pathFound = true;
            break;
        default:
            return false;
    }
    if (m_lastSaveValid && m_lastSaveSteps == m_totalSteps && m_lastSavePhase == m_phase) return true;

    SolveProgress progress;
    progress.levelHash = m_ref.levelHash;
    progress.settingsSignature = (m_preloadTiming && m_preloadEnd > 0) ? m_preloadSignature : settingsSignature();
    progress.targetPercent = m_targetPercent;
    progress.elapsedSeconds = elapsedSeconds();
    progress.totalSteps = m_totalSteps;
    progress.backtracks = m_backtracks;
    progress.prunes = m_prunes;
    progress.pathFound = pathFound;
    progress.escalation = m_level;
    progress.fastAllowed = m_fastAllowed;
    progress.replayMode = m_restoreMode == RestoreMode::Replay;
    progress.verifyFailures = m_verifyFailures;
    if (pathFound) {
        progress.held = m_phase == Phase::Verify ? m_seq : m_verifiedSeq;
        progress.best = m_best.size() > progress.held.size() ? m_best : progress.held;
    }
    else {
        size_t count = m_path.size();
        progress.held.reserve(count);
        progress.tried.reserve(count);
        progress.def.reserve(count);
        progress.flips.reserve(count);
        for (auto const& node : m_path) {
            progress.held.push_back(node.held);
            progress.tried.push_back(node.tried);
            progress.def.push_back(node.def);
            progress.flips.push_back(node.flips);
        }
        progress.best = m_best;
        if (m_dead.size() <= kMaxSavedDeadStates) progress.dead.assign(m_dead.begin(), m_dead.end());
    }
    progress.maxPercent = m_maxPercent;
    progress.maxTick = m_maxTick;
    progress.savedAt = unixNow();

    saveProgressBlob(m_ref, progress.serialize());
    m_lastSave = Clock::now();
    m_lastSaveValid = true;
    m_lastSaveSteps = m_totalSteps;
    m_lastSavePhase = m_phase;
    log::info(
        "Solver: progress saved ({}, {} ticks, {:.1f}%, {:.0f}s)",
        pathFound ? "path found" : "searching", progress.held.size(), m_maxPercent, progress.elapsedSeconds
    );
    return true;
}

void Solver::runFrame() {
    if (!running()) return;
    setLayerHidden(true);
    auto frameStart = Clock::now();
    if (m_frameClockValid) {
        double gap = std::chrono::duration<double>(frameStart - m_lastFrameEnd).count();
        m_activeSeconds += std::clamp(gap, 0.0, kMaxFrameGapSeconds);
        if (gap > kMaxFrameGapSeconds) {
            m_speedTime = frameStart;
            m_speedSteps = m_totalSteps;
        }
    }
    m_frameStart = frameStart;
    m_inFrame = true;
    double budgetMs = m_frameBudgetMs;
    if (auto director = CCDirector::get()) {
        double gameMs = director->getAnimationInterval() * 1000.0;
        if (std::isfinite(gameMs) && gameMs > 0.0) budgetMs = std::max(budgetMs, 0.85 * std::min(gameMs, kMaxGameFrameMs));
    }
    auto budget = std::chrono::duration<double, std::milli>(budgetMs);
    while (running()) {
        bool keepGoing = false;
        switch (m_phase) {
            case Phase::SelfTest: keepGoing = advanceSelfTest(); break;
            case Phase::Search: keepGoing = advanceSearch(); break;
            case Phase::Verify:
            case Phase::FinalVerify: keepGoing = advanceVerify(); break;
            case Phase::Optimize: keepGoing = advanceOptimize(); break;
            case Phase::Refine: keepGoing = advanceRefine(); break;
            default: break;
        }
        if (!keepGoing) break;
        if (Clock::now() - frameStart >= budget) break;
    }
    auto now = Clock::now();
    m_activeSeconds += std::chrono::duration<double>(now - frameStart).count();
    m_inFrame = false;
    m_lastFrameEnd = now;
    m_frameClockValid = true;
    updateSpeed(now);
    if (running() && now - m_lastSave >= kAutoSaveInterval) {
        m_lastSave = now;
        saveProgress();
    }
    if (running() && now - m_lastLog >= kLogInterval) {
        m_lastLog = now;
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
            stopWithBest();
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

void Solver::releaseCheckpointsBetween(int after, int before) {
    for (auto it = m_checkpoints.upper_bound(after); it != m_checkpoints.end() && it->first < before;) {
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
            uint64_t h = exactHash(t);
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
                uint8_t modes = laneModesNow();
                recordLaneModes(t, modes);
                if (m_path[t].hash == 0) {
                    m_path[t].hash = stateHash(t);
                    if (t + 1 > m_maxTick) m_maxTick = t + 1;
                    if (t < m_preloadEnd) {
                        m_escalatedAt = std::max(m_escalatedAt, t + 1);
                        m_levelBase = std::max(m_levelBase, t + 1);
                    }
                    if (m_path[t].recompute) {
                        settleNode(t, modes);
                        if (m_preloadTiming && t < m_preloadEnd && changeBreaksTiming(t, modes, m_path[t].held)) cutPreload(t);
                    }
                    else {
                        uint8_t def = 0;
                        uint8_t flips = 0;
                        policyAt(t, modes, def, flips);
                        m_path[t].flips = static_cast<uint8_t>(m_path[t].flips | flips);
                    }
                }
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
            uint8_t modes = laneModesNow();
            recordLaneModes(t, modes);
            SearchNode node;
            node.hash = h;
            policyAt(t, modes, node.def, node.flips);
            node.held = node.def;
            node.tried = static_cast<uint8_t>(1u << node.def);
            m_path.push_back(node);
            applyHeld(node.held);
            if (t + 1 > m_maxTick) m_maxTick = t + 1;
            m_finestTier = std::max(m_finestTier, currentStep().tier);
            break;
        }
        case Phase::Verify:
        case Phase::FinalVerify: {
            recordLaneModes(t, laneModesNow());
            applyHeld(seqAt(m_seq, t));
            break;
        }
        case Phase::Optimize:
        case Phase::Refine: {
            if (m_refineStage == RefineStage::Baseline) {
                if (t >= m_baseFrom && t <= m_baseUntil) {
                    m_baseHashes[t - m_baseFrom] = exactHash(t);
                }
                applyHeld(seqAt(m_seq, t));
            }
            else {
                if (t >= m_convergeFrom && t >= m_baseFrom && t <= m_baseUntil) {
                    uint64_t base = m_baseHashes[t - m_baseFrom];
                    if (base != 0 && exactHash(t) == base) {
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
    if (!m_stopDecisions && m_targetPercent < 100.f && m_phase != Phase::SelfTest &&
        m_layer->getCurrentPercent() >= m_targetPercent) {
        m_success = true;
        m_successTick = m_tick;
        m_stopDecisions = true;
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
    beginAfterSelfTest();
    return true;
}

void Solver::beginAfterSelfTest() {
    if (m_request.kind == SolveKind::CheckImport) {
        auto seq = std::move(m_request.importSeq);
        m_request.importSeq.clear();
        log::info("Solver: checking {} ticks of imported inputs", seq.size());
        m_importChecking = true;
        m_fromImport = true;
        m_maxPercent = 0.f;
        beginVerify(std::move(seq), Phase::Verify);
        return;
    }
    if (m_resumed && m_request.progress) {
        SolveProgress progress = std::move(*m_request.progress);
        m_request.progress.reset();
        resumeFrom(progress);
        return;
    }
    beginSearchFresh();
}

void Solver::resumeFrom(SolveProgress const& progress) {
    m_totalSteps += progress.totalSteps;
    m_speedSteps += progress.totalSteps;
    m_backtracks += progress.backtracks;
    m_prunes += progress.prunes;
    m_verifyFailures = std::clamp(progress.verifyFailures, 0, 3);
    bool sameSettings = progress.settingsSignature == settingsSignature();
    bool validLevel = progress.escalation >= 0 && progress.escalation < static_cast<int>(m_ladder.size());
    if (!progress.fastAllowed) m_fastAllowed = false;
    bool adoptedReplay = false;
    if (progress.replayMode && m_restoreMode == RestoreMode::Checkpoint) {
        m_restoreMode = RestoreMode::Replay;
        adoptedReplay = true;
    }

    std::vector<uint8_t> best = progress.best;
    for (auto& v : best) v = static_cast<uint8_t>(v & 3);
    float maxPercent = cleanPercent(progress.maxPercent, 0.f);
    bool checkTiming = !sameSettings && m_constraintsActive && !m_relaxTiming;

    if (progress.pathFound && !checkTiming) {
        std::vector<uint8_t> seq = progress.held;
        for (auto& v : seq) v = static_cast<uint8_t>(v & 3);
        if (sameSettings && validLevel) m_level = progress.escalation;
        m_finestTier = currentStep().tier;
        m_best = std::move(best);
        m_maxPercent = maxPercent;
        m_replayFromRepair = adoptedReplay;
        log::info("Solver: resuming with a found path ({} ticks), checking it again", seq.size());
        beginVerify(std::move(seq), Phase::Verify);
        return;
    }

    beginSearchFresh();
    if (adoptedReplay) m_replayFromRepair = true;
    if (sameSettings && validLevel) {
        m_level = progress.escalation;
        m_finestTier = std::max(m_finestTier, currentStep().tier);
    }
    bool keepClaims = !progress.pathFound && sameSettings && progress.escalation == m_level &&
        progress.fastAllowed == m_fastAllowed && progress.replayMode == (m_restoreMode == RestoreMode::Replay);

    size_t count = progress.held.size();
    bool nodeData = progress.tried.size() == count && progress.def.size() == count && progress.flips.size() == count;
    m_path.reserve(count);
    for (size_t i = 0; i < count; i++) {
        SearchNode node;
        node.held = static_cast<uint8_t>(progress.held[i] & 3);
        node.def = node.held;
        node.recompute = true;
        node.tried = static_cast<uint8_t>(1u << node.held);
        if (keepClaims && nodeData) node.tried = static_cast<uint8_t>(node.tried | (progress.tried[i] & 15));
        m_path.push_back(node);
    }
    m_laneModes.assign(count, kModesUnknown);
    m_best = best.size() > count ? std::move(best) : heldOf(m_path);
    m_maxPercent = maxPercent;
    m_preloadEnd = static_cast<int>(count);
    m_preloadTiming = checkTiming;
    m_preloadSignature = progress.settingsSignature;
    size_t deadLoaded = 0;
    if (keepClaims && progress.dead.size() <= kMaxDeadStates) {
        m_dead.reserve(progress.dead.size());
        for (auto h : progress.dead) {
            if (h != 0) m_dead.insert(h);
        }
        deadLoaded = m_dead.size();
    }
    if (progress.pathFound) {
        log::info("Solver: resuming with a found path ({} ticks) made with other settings, replaying it with the current timing limits", count);
        return;
    }
    log::info(
        "Solver: resuming the search at {:.1f}% ({} ticks, level {}/{}, {} known dead ends{})",
        m_maxPercent, count, m_level + 1, m_ladder.size(), deadLoaded,
        keepClaims ? "" : ", settings changed so explored branches are checked again"
    );
}

void Solver::beginSearchFresh() {
    releaseAllCheckpoints();
    resetToStart();
    m_path.clear();
    m_dead.clear();
    m_best.clear();
    m_laneModes.clear();
    m_maxTick = 0;
    m_maxPercent = 0.f;
    m_replayFromRepair = false;
    m_fromImport = false;
    m_bestFromImport = false;
    m_level = 0;
    m_escalatedAt = 0;
    m_levelBase = 0;
    m_preloadEnd = 0;
    m_preloadTiming = false;
    m_finestTier = currentStep().tier;
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
        stopWithBest();
        return false;
    }
    if (m_level > 0 && m_maxTick > m_escalatedAt + kDeescalateTicks) deescalate();
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

    m_preloadEnd = 0;
    if (m_path.size() > m_best.size()) {
        m_best = heldOf(m_path);
        m_bestFromImport = m_fromImport;
    }
    failNode = std::min(failNode, static_cast<int>(m_path.size()) - 1);
    if (failNode < 0) {
        if (canEscalate()) {
            escalate();
            return;
        }
        if (trySearchFallback()) return;
        m_failReason = "the level kills the player before any input can help";
        m_exhausted = true;
        stopWithBest();
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
            if (t == 0) m_fromImport = false;
            m_backtracks++;
            restoreTo(t);
            return;
        }
        auto const& popped = m_path.back();
        if (popped.hash != 0 && !popped.seeded) m_dead.insert(popped.hash);
        m_path.pop_back();
        int base = m_level == 0 ? m_maxTick : std::min(m_maxTick, m_levelBase);
        if (base - t > kEscalateTicks && canEscalate()) {
            escalate();
            return;
        }
        if (m_maxTick - t > m_maxBacktrackTicks) {
            if (canEscalate()) {
                escalate();
                return;
            }
            if (trySearchFallback()) return;
            m_failReason = "stuck: no working input found for this section";
            stopWithBest();
            return;
        }
    }
    m_fromImport = false;
    if (canEscalate()) {
        escalate();
        return;
    }
    if (trySearchFallback()) return;
    m_failReason = "no possible path was found";
    m_exhausted = true;
    stopWithBest();
}

void Solver::stopWithBest() {
    if (m_path.size() > m_best.size()) {
        m_best = heldOf(m_path);
        m_bestFromImport = m_fromImport;
    }
    m_fromImport = m_bestFromImport;
    finish(false, m_best, false, false, false);
}

bool Solver::trySearchFallback() {
    if (timeUp()) return false;
    std::vector<uint8_t> best = m_best;
    bool bestFromImport = m_bestFromImport;
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
    m_bestFromImport = bestFromImport;
    m_maxPercent = percent;
    return true;
}

void Solver::onSearchSuccess() {
    auto seq = heldOf(m_path);
    if (m_successTick >= 0 && m_successTick + 1 < static_cast<int>(seq.size())) {
        seq.resize(static_cast<size_t>(m_successTick) + 1);
    }
    m_success = false;
    m_maxPercent = std::max(m_maxPercent, m_targetPercent);
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
    if (m_successTick >= 0 && m_successTick + 1 < static_cast<int>(m_seq.size())) {
        m_seq.resize(static_cast<size_t>(m_successTick) + 1);
    }
    if (m_phase == Phase::Verify) {
        log::info("Solver: path verified from a clean restart{}", m_importChecking ? " (imported inputs)" : "");
        m_importChecking = false;
        m_verifiedSeq = m_seq;
        m_verifiedTimes = m_tickTimes;
        m_maxPercent = std::max(m_maxPercent, m_targetPercent);
        m_optEdits = 0;
        m_refineMoved = 0;
        m_refineRan = false;
        m_finalIsEdited = false;
        m_polishDeadline = runSeconds() + std::max(30.0, 0.75 * m_timeLimitSec);
        if (m_restoreMode == RestoreMode::Checkpoint && m_optimizeEnabled) {
            beginOptimize();
            return;
        }
        if (m_restoreMode == RestoreMode::Checkpoint && m_refineEnabled) {
            beginRefine();
            return;
        }
        startFinalCheck();
        return;
    }
    log::info("Solver: final full-game check passed ({})", m_finalIsEdited ? "cleaned up chart" : "unedited chart");
    finishSuccess(m_seq);
}

void Solver::onVerifyFailed(int tick) {
    m_pendingDeath = false;
    m_stopDecisions = false;
    float failPercent = m_layer->getCurrentPercent();
    m_maxPercent = std::min(m_maxPercent, failPercent);

    if (m_phase == Phase::FinalVerify) {
        if (m_finalIsEdited) {
            log::warn("Solver: cleaned up chart failed the full-game check at tick {}, checking the unedited chart", tick);
            m_finalIsEdited = false;
            beginVerify(m_verifiedSeq, Phase::FinalVerify);
            return;
        }
        if (importRun() && m_fromImport) {
            log::warn("Solver: the imported chart failed the full-game check at tick {}", tick);
            if (m_fastAllowed) {
                m_fastAllowed = false;
                m_dead.clear();
                m_path.clear();
            }
            m_phase = Phase::Verify;
            m_seq = m_verifiedSeq;
            if (!m_request.repairImport) {
                int failTick = std::clamp(tick, 0, static_cast<int>(m_seq.size()));
                m_maxPercent = cleanPercent(failPercent, 0.f);
                m_failReason = fmt::format("the imported inputs fail at {:.0f}%", m_maxPercent);
                std::vector<uint8_t> prefix(m_seq.begin(), m_seq.begin() + failTick);
                finish(false, prefix, true, false, false);
                return;
            }
        }
        else if (m_fastAllowed) {
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
        else {
            log::warn("Solver: chart failed the clean full-game check at tick {}", tick);
            m_phase = Phase::Verify;
            m_seq = m_verifiedSeq;
        }
    }

    int failTick = std::clamp(tick, 0, static_cast<int>(m_seq.size()));
    if (m_importChecking) {
        m_importChecking = false;
        m_maxPercent = cleanPercent(failPercent, 0.f);
        if (!m_request.repairImport) {
            log::info("Solver: imported inputs fail at tick {} ({:.1f}%)", tick, failPercent);
            m_failReason = fmt::format("the imported inputs fail at {:.0f}%", m_maxPercent);
            std::vector<uint8_t> prefix(m_seq.begin(), m_seq.begin() + failTick);
            finish(false, prefix, true, false, false);
            return;
        }
        log::info("Solver: imported inputs fail at tick {} ({:.1f}%), repairing from there", tick, failPercent);
    }
    bool seeded = m_path.empty() && failTick > 0;
    if (seeded) {
        seedPath(m_seq, failTick);
        if (m_best.size() < m_path.size()) {
            m_best = heldOf(m_path);
            m_bestFromImport = m_fromImport;
        }
    }

    m_verifyFailures++;
    m_furthestVerifyFail = std::max(m_furthestVerifyFail, tick);
    log::warn("Solver: verification failed at tick {} (attempt {})", tick, m_verifyFailures);

    if (m_verifyFailures > kMaxVerifyFailures || timeUp()) {
        m_failReason = "the found path did not replay reliably";
        std::vector<uint8_t> prefix(m_seq.begin(), m_seq.begin() + failTick);
        finish(false, prefix, true, false, false);
        return;
    }
    if (m_verifyFailures >= 3 && m_restoreMode == RestoreMode::Checkpoint) {
        log::warn("Solver: switching to exact replay mode");
        m_restoreMode = RestoreMode::Replay;
        m_replayFromRepair = true;
        for (auto& node : m_path) {
            node.tried = static_cast<uint8_t>(1u << (node.held & 3));
            node.hash = 0;
        }
    }
    if (m_verifyFailures >= 2) m_dead.clear();

    int rewind = std::clamp(tick - kRepairRewindTicks, 0, static_cast<int>(m_path.size()));
    m_path.resize(static_cast<size_t>(rewind));
    if (m_path.empty()) m_fromImport = false;
    m_maxTick = rewind;
    m_preloadEnd = 0;
    if (seeded) {
        m_escalatedAt = rewind;
        m_levelBase = rewind;
    }
    m_phase = Phase::Search;
    m_finestTier = std::max(m_finestTier, currentStep().tier);
    restoreTo(rewind);
}

bool Solver::continuousAt(int lane, int tick) const {
    uint8_t modes = laneModesAt(tick);
    if (modes & kModesUnknown) return true;
    if (!laneActive(modes, lane)) return false;
    return isContinuous(laneFamily(modes, lane));
}

int Solver::editRequirement(int lane, int tick, bool held) const {
    if (!m_constraintsActive) return 1;
    uint8_t modes = laneModesAt(tick);
    if (modes & kModesUnknown) return requiredTicks(kFamAll, held);
    if (!laneActive(modes, lane)) return 1;
    return requiredTicks(laneFamily(modes, lane), held);
}

int Solver::laneViolations(std::vector<uint8_t> const& seq, int lane, int from, int to) const {
    if (!m_constraintsActive) return 0;
    uint8_t mask = static_cast<uint8_t>(1u << lane);
    int total = static_cast<int>(seq.size());
    from = std::clamp(from, 0, total);
    to = std::clamp(to, from, total);
    auto down = [&](int k) { return k >= 0 && k < total && (seq[k] & mask) != 0; };
    int lastChange = -1;
    for (int k = from - 1; k >= 0 && k >= from - m_constraintCap; k--) {
        if (down(k) != down(k - 1)) {
            lastChange = k;
            break;
        }
    }
    bool prev = down(from - 1);
    int count = 0;
    for (int t = from; t < to; t++) {
        bool now = down(t);
        if (now == prev) continue;
        uint8_t modes = laneModesAt(t);
        if (lastChange >= 0 && !(modes & kModesUnknown) && laneActive(modes, lane)) {
            if (t - lastChange < requiredTicks(laneFamily(modes, lane), prev)) count++;
        }
        lastChange = t;
        prev = now;
    }
    return count;
}

bool Solver::editBreaksTiming(std::vector<uint8_t> const& edited, int lane, int from, int to) const {
    if (!m_constraintsActive) return false;
    int until = to + m_constraintCap + 1;
    return laneViolations(edited, lane, from, until) > laneViolations(m_seq, lane, from, until);
}

int Solver::findOptNote(int start, int lane) const {
    auto it = std::lower_bound(m_optNotes.begin(), m_optNotes.end(), std::pair<int, int>(start, lane),
        [](RefineNote const& note, std::pair<int, int> const& key) { return noteBefore(note, key.first, key.second); });
    if (it == m_optNotes.end() || it->start != start || it->lane != lane) return -1;
    return static_cast<int>(it - m_optNotes.begin());
}

int Solver::firstOptNoteAfter(int start, int lane) const {
    auto it = std::upper_bound(m_optNotes.begin(), m_optNotes.end(), std::pair<int, int>(start, lane),
        [](std::pair<int, int> const& key, RefineNote const& note) {
            return key.first != note.start ? key.first < note.start : key.second < note.lane;
        });
    if (it == m_optNotes.end()) return -1;
    return static_cast<int>(it - m_optNotes.begin());
}

int Solver::nextOptNoteInLane(int index) const {
    if (index < 0 || index >= static_cast<int>(m_optNotes.size())) return -1;
    int lane = m_optNotes[index].lane;
    for (int j = index + 1; j < static_cast<int>(m_optNotes.size()); j++) {
        if (m_optNotes[j].lane == lane) return j;
    }
    return -1;
}

bool Solver::joinAllowed(RefineNote const& note, RefineNote const& next) const {
    int gap = next.start - note.end;
    if (gap <= 0) return false;
    if (continuousAt(note.lane, note.start)) {
        return m_mergeGapMs > 0.0 && static_cast<double>(gap) * 1000.0 / kTicksPerSecond < m_mergeGapMs;
    }
    return m_preferHolds && gap <= kJoinGapTicks;
}

void Solver::beginOptimize() {
    m_phase = Phase::Optimize;
    m_optDeadline = std::min(runSeconds() + std::max(10.0, m_timeLimitSec / 4.0), m_polishDeadline);
    m_optEdits = 0;
    m_optNotes = notesOf(m_seq);
    m_optCursorStart = -1;
    m_optCursorLane = -1;
    m_optPosition = 0;
    m_optStep = OptStep::Finished;
    m_baseValid = false;
    log::info("Solver: cleaning up {} notes", m_optNotes.size());
    nextOptNote();
}

void Solver::nextOptNote() {
    while (true) {
        int index = firstOptNoteAfter(m_optCursorStart, m_optCursorLane);
        if (index < 0) break;
        if (runSeconds() > m_optDeadline) {
            log::warn("Solver: input cleanup ran out of time at note {}/{}", index, m_optNotes.size());
            break;
        }
        auto const& note = m_optNotes[index];
        m_optCursorStart = note.start;
        m_optCursorLane = note.lane;
        m_optPosition = static_cast<size_t>(index);
        m_optStep = OptStep::Remove;
        m_optShortenReady = false;
        m_baseValid = false;
        if (nextOptTest()) return;
    }
    finishOptimize();
}

bool Solver::nextOptTest() {
    while (true) {
        int index = findOptNote(m_optCursorStart, m_optCursorLane);
        if (index < 0) return false;
        RefineNote const note = m_optNotes[index];
        if ((m_optStep == OptStep::Remove || m_optStep == OptStep::Join) && runSeconds() > m_optDeadline) {
            m_optStep = OptStep::Finished;
            return false;
        }
        switch (m_optStep) {
            case OptStep::Remove: {
                auto seq = m_seq;
                setLane(seq, note.lane, note.start, note.end, false);
                if (editBreaksTiming(seq, note.lane, note.start, note.end)) {
                    m_optStep = OptStep::Join;
                    continue;
                }
                return launchOptTest(EditKind::Remove, std::move(seq), note.start, note.end);
            }
            case OptStep::Join: {
                int next = nextOptNoteInLane(index);
                if (next < 0 || !joinAllowed(note, m_optNotes[next])) {
                    m_optStep = OptStep::Shorten;
                    continue;
                }
                int nextStart = m_optNotes[next].start;
                auto seq = m_seq;
                setLane(seq, note.lane, note.end, nextStart, true);
                if (editBreaksTiming(seq, note.lane, note.end, nextStart)) {
                    m_optStep = OptStep::Shorten;
                    continue;
                }
                return launchOptTest(EditKind::Join, std::move(seq), note.end, nextStart);
            }
            case OptStep::Shorten: {
                if (!m_optShortenReady) {
                    m_optShortenReady = true;
                    m_optBestConverged = -1;
                    m_optHi = note.end;
                    m_optLo = note.start + editRequirement(note.lane, note.start, true);
                    if (continuousAt(note.lane, note.start) || note.end - note.start <= 1) m_optLo = m_optHi;
                }
                if (m_optLo >= m_optHi) {
                    if (m_optHi < note.end && m_optHi > note.start) {
                        auto seq = m_seq;
                        setLane(seq, note.lane, m_optHi, note.end, false);
                        applyOptEdit(std::move(seq), m_optHi, m_optBestConverged);
                    }
                    m_optStep = OptStep::Finished;
                    continue;
                }
                m_optTestEnd = (m_optLo + m_optHi) / 2;
                auto seq = m_seq;
                setLane(seq, note.lane, m_optTestEnd, note.end, false);
                if (editBreaksTiming(seq, note.lane, m_optTestEnd, note.end)) {
                    m_optLo = m_optTestEnd + 1;
                    continue;
                }
                return launchOptTest(EditKind::Shorten, std::move(seq), m_optTestEnd, note.end);
            }
            case OptStep::Finished:
                return false;
        }
        return false;
    }
}

bool Solver::launchOptTest(EditKind kind, std::vector<uint8_t> seq, int changeFrom, int changeTo) {
    m_editKind = kind;
    m_testSeq = std::move(seq);
    m_testChangeFrom = changeFrom;
    m_convergeFrom = changeTo + 1;
    if (!m_baseValid) return startOptBaseline();
    if (startTestRun()) return true;
    m_optStep = OptStep::Finished;
    return false;
}

bool Solver::startOptBaseline() {
    int index = findOptNote(m_optCursorStart, m_optCursorLane);
    if (index < 0) {
        m_optStep = OptStep::Finished;
        return false;
    }
    auto const& note = m_optNotes[index];
    int changeEnd = note.end;
    int next = nextOptNoteInLane(index);
    if (next >= 0 && joinAllowed(note, m_optNotes[next])) changeEnd = std::max(changeEnd, m_optNotes[next].start);
    int wantedStart = std::max(0, note.start - 1);
    auto it = m_checkpoints.upper_bound(wantedStart);
    if (it == m_checkpoints.begin()) {
        m_optStep = OptStep::Finished;
        return false;
    }
    --it;
    m_baseFrom = it->first;
    m_baseUntil = changeEnd + kRefineHorizon;
    m_baseChangeEnd = changeEnd;
    m_baseHashes.assign(static_cast<size_t>(m_baseUntil - m_baseFrom + 1), 0);
    m_baseCompleted = false;
    m_refineStage = RefineStage::Baseline;
    loadCheckpoint(it->second);
    return true;
}

bool Solver::startTestRun() {
    auto it = m_checkpoints.find(m_baseFrom);
    if (it == m_checkpoints.end()) return false;
    m_runConverged = false;
    m_convergedAt = -1;
    m_refineStage = RefineStage::Test;
    loadCheckpoint(it->second);
    return true;
}

void Solver::applyOptEdit(std::vector<uint8_t> seq, int changedFrom, int convergedAt) {
    m_seq = std::move(seq);
    m_optEdits++;
    int changedUntil = convergedAt >= 0 ? convergedAt : m_baseUntil;
    releaseCheckpointsBetween(changedFrom, changedUntil);
    m_baseValid = false;
    m_optNotes = notesOf(m_seq);
}

void Solver::onOptTestResult(bool good) {
    switch (m_editKind) {
        case EditKind::Remove:
            if (good) {
                applyOptEdit(std::move(m_testSeq), m_testChangeFrom, m_convergedAt);
                m_optStep = OptStep::Finished;
            }
            else {
                m_optStep = OptStep::Join;
            }
            break;
        case EditKind::Join:
            if (good) applyOptEdit(std::move(m_testSeq), m_testChangeFrom, m_convergedAt);
            else m_optStep = OptStep::Shorten;
            break;
        case EditKind::Shorten:
            if (good) {
                m_optHi = m_optTestEnd;
                m_optBestConverged = m_convergedAt;
            }
            else {
                m_optLo = m_optTestEnd + 1;
            }
            break;
    }
    m_testSeq.clear();
    if (!nextOptTest()) nextOptNote();
}

bool Solver::advanceOptimize() {
    if (m_refineStage == RefineStage::Baseline) {
        bool ended = m_pendingDeath || m_success || m_tick > m_baseUntil;
        if (!ended) return simulate();

        bool died = m_pendingDeath;
        int deathTick = m_deathTick;
        m_baseCompleted = m_success;
        if (m_success) m_baseUntil = std::min(m_baseUntil, m_successTick);
        m_pendingDeath = false;
        m_success = false;
        m_stopDecisions = false;

        if (died) {
            if (deathTick <= m_baseChangeEnd + 1) {
                m_optStep = OptStep::Finished;
                nextOptNote();
                return true;
            }
            m_baseUntil = std::min(m_baseUntil, deathTick - 1);
        }
        m_baseValid = true;
        if (!startTestRun()) {
            m_optStep = OptStep::Finished;
            nextOptNote();
        }
        return true;
    }

    bool ended = m_runConverged || m_pendingDeath || m_success || m_tick > m_baseUntil;
    if (!ended) return simulate();

    bool good = m_runConverged || (m_success && m_baseCompleted);
    m_pendingDeath = false;
    m_success = false;
    m_stopDecisions = false;
    m_runConverged = false;
    onOptTestResult(good);
    return true;
}

void Solver::finishOptimize() {
    log::info("Solver: input cleanup made {} changes ({} notes left)", m_optEdits, m_optNotes.size());
    m_optStep = OptStep::Finished;
    m_testSeq.clear();
    if (m_restoreMode == RestoreMode::Checkpoint && m_refineEnabled) {
        beginRefine();
        return;
    }
    startFinalCheck();
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
    m_refineRan = true;
    m_refineDeadline = std::min(runSeconds() + std::max(20.0, m_timeLimitSec / 2.0), m_polishDeadline);
    m_refineNotes = notesOf(m_seq);
    m_refineIndex = 0;
    m_refineMoved = 0;
    log::info("Solver: centering {} notes in their timing windows", m_refineNotes.size());
    startRefineNote();
}

void Solver::startRefineNote() {
    int total = static_cast<int>(m_seq.size());
    while (m_refineIndex < m_refineNotes.size()) {
        if (runSeconds() > m_refineDeadline) {
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

        int minStart = prevEnd >= 0 ? prevEnd + editRequirement(note.lane, note.start, false) : 0;
        int maxEnd = nextStart >= 0 ? nextStart - editRequirement(note.lane, nextStart, false) : total;
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

    if (m_refineMoved > 0) log::info("Solver: moved {} notes", m_refineMoved);
    startFinalCheck();
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

bool Solver::shiftBreaksTiming(RefineNote const& note, int shift) const {
    if (!m_constraintsActive || shift == 0) return false;
    int from = std::min(note.start, note.start + shift);
    int to = std::max(note.end, note.end + shift);
    return editBreaksTiming(shiftedSeq(note, shift), note.lane, from, to);
}

void Solver::nextRefineTest() {
    auto const& note = m_refineNotes[m_refineIndex];
    while (true) {
        if (m_posLimit > 0 && m_posBad - m_posGood > 1) {
            int shift = m_posBad == m_posLimit + 1 ? m_posLimit : (m_posGood + m_posBad) / 2;
            if (shiftBreaksTiming(note, shift)) {
                m_posBad = shift;
                continue;
            }
            beginRefineTest(shift);
            return;
        }
        if (m_negLimit < 0 && m_negGood - m_negBad > 1) {
            int shift = m_negBad == m_negLimit - 1 ? m_negLimit : (m_negGood + m_negBad) / 2;
            if (shiftBreaksTiming(note, shift)) {
                m_negBad = shift;
                continue;
            }
            beginRefineTest(shift);
            return;
        }
        break;
    }
    int center = (m_negGood + m_posGood) / 2;
    if (center == 0 || shiftBreaksTiming(note, center)) {
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
    releaseCheckpointsBetween(changedFrom, changedUntil);
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

void Solver::startFinalCheck() {
    m_testSeq.clear();
    m_baseHashes.clear();
    if (m_optEdits > 0 || m_refineMoved > 0) {
        log::info("Solver: verifying the cleaned up chart ({} cleanups, {} moved notes)", m_optEdits, m_refineMoved);
        m_finalIsEdited = true;
        beginVerify(m_seq, Phase::FinalVerify);
        return;
    }
    m_finalIsEdited = false;
    if (m_fastAllowed) {
        beginVerify(m_verifiedSeq, Phase::FinalVerify);
        return;
    }
    finishSuccess(m_verifiedSeq);
}

void Solver::finishSuccess(std::vector<uint8_t> seq) {
    bool refined = m_refineRan && (m_finalIsEdited || (m_refineMoved == 0 && m_optEdits == 0));
    finish(true, seq, true, m_targetPercent >= 100.f, refined);
}

bool Solver::sequenceViolatesTiming(std::vector<uint8_t> const& seq) const {
    if (!m_constraintsActive) return false;
    int total = static_cast<int>(seq.size());
    for (int lane = 0; lane < kLaneCount; lane++) {
        uint8_t mask = static_cast<uint8_t>(1u << lane);
        int lastChange = -1;
        bool prev = false;
        for (int t = 0; t < total; t++) {
            bool down = (seq[t] & mask) != 0;
            if (down == prev) continue;
            uint8_t modes = laneModesAt(t);
            if (lastChange >= 0 && !(modes & kModesUnknown) && laneActive(modes, lane)) {
                int required = requiredTicks(laneFamily(modes, lane), prev);
                if (t - lastChange < required) return true;
            }
            lastChange = t;
            prev = down;
        }
    }
    return false;
}

bool Solver::existingChartIsBetter(Chart const& chart) const {
    auto existing = loadChart(m_ref);
    if (!existing || existing->empty() || chartOutdated(m_ref, *existing)) return false;
    if (existing->complete) return true;
    return existing->reachedPercent >= chart.reachedPercent;
}

std::vector<uint8_t> Solver::heldOf(std::vector<SearchNode> const& path) const {
    std::vector<uint8_t> out;
    out.reserve(path.size());
    for (auto const& node : path) out.push_back(node.held);
    return out;
}

void Solver::finish(bool success, std::vector<uint8_t> const& seq, bool verified, bool complete, bool refined) {
    double solveSeconds = elapsedSeconds();

    Chart chart;
    chart.key = m_ref.key();
    chart.held = seq;
    chart.tickTimes = m_tickTimes;
    chart.twoPlayer = m_twoPlayer;
    chart.complete = complete;
    chart.verified = verified;
    chart.refined = refined;
    chart.reachedPercent = complete ? 100.f : cleanPercent(success ? std::max(m_targetPercent, 0.f) : m_maxPercent, 0.f);
    chart.info = m_request.info;
    if (importRun() && !m_fromImport) {
        chart.info.source = "solver";
        chart.info.sourceFile.clear();
    }
    chart.info.levelID = m_ref.levelID;
    chart.info.levelName = m_ref.levelName;
    chart.info.levelHash = m_ref.levelHash;
    chart.info.variant = m_ref.variant;
    chart.info.savedAt = unixNow();
    chart.info.solveSeconds = solveSeconds;
    chart.info.targetPercent = m_targetPercent;
    chart.info.optimized = success && m_finalIsEdited && m_optEdits > 0;
    chart.info.timingRelaxed = sequenceViolatesTiming(seq);
    chart.info.precision = m_finestTier >= 0 ? std::clamp(m_finestTier, 0, kExactTier) : kExactTier;
    chart.rebuildNotes();

    if (!success && existingChartIsBetter(chart)) {
        log::info("Solver: keeping the saved chart, it reaches further than this attempt ({:.1f}%)", chart.reachedPercent);
        m_hasResult = false;
    }
    else {
        m_result = std::move(chart);
        m_hasResult = true;
        if (!saveChart(m_ref, m_result)) log::warn("Solver: failed to save the chart for {}", m_key);
    }
    if (!importRun()) {
        if (success || m_exhausted) deleteProgress(m_ref);
        else saveProgress();
    }

    log::info(
        "Solver finished: success={} complete={} verified={} refined={} optimized={} relaxed={} precision={} notes={}+{} time={:.1f}s steps={} reason='{}'",
        success, complete, verified, refined, m_hasResult && m_result.info.optimized, m_hasResult && m_result.info.timingRelaxed,
        m_hasResult ? m_result.info.precision : -1,
        m_hasResult ? m_result.lanes[0].size() : 0, m_hasResult ? m_result.lanes[1].size() : 0,
        solveSeconds, m_totalSteps, m_failReason
    );

    m_frozenElapsed = solveSeconds;
    m_stoppedPhase = m_phase;
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
    m_optNotes.clear();
    m_objectIndex.clear();
    m_objectIndex.shrink_to_fit();
    m_request.importSeq.clear();
    m_request.progress.reset();
}

std::string Solver::phaseNote() const {
    std::string note;
    switch (m_phase) {
        case Phase::SelfTest:
            note = "Checking that the game replays exactly";
            break;
        case Phase::Search: {
            auto const& step = currentStep();
            if (m_level == 0) {
                note = fmt::format("Ship/wave precision: {}", tierName(step.tier));
            }
            else if (!step.constraints && m_constraintsActive) {
                note = step.tier > m_startTier
                    ? fmt::format("Timing limits relaxed, {} precision for this section", tierName(step.tier))
                    : std::string("Timing limits relaxed for this section");
            }
            else {
                note = fmt::format("Ship/wave precision: {} for this section", tierName(step.tier));
            }
            if (importRun()) {
                note = fmt::format("{}  |  {}", m_fromImport ? "Repairing the imported inputs" : "Solving without the imported inputs", note);
            }
            break;
        }
        case Phase::Verify:
            note = m_importChecking ? "Replaying the imported inputs" : "Replaying the path from a clean restart";
            break;
        case Phase::Optimize:
            note = fmt::format(
                "Cleaning up inputs {}/{}  |  {} changed so far",
                std::min(m_optPosition + 1, m_optNotes.size()), m_optNotes.size(), m_optEdits
            );
            break;
        case Phase::Refine:
            note = fmt::format("Centering note timings {}/{}", std::min(m_refineIndex + 1, m_refineNotes.size()), m_refineNotes.size());
            break;
        case Phase::FinalVerify:
            note = "Checking the finished chart in the full game";
            break;
        default:
            break;
    }
    if (m_resumed && running()) {
        note += note.empty() ? "Resumed from saved progress" : "  |  Resumed from saved progress";
    }
    return note;
}

SolverView Solver::view() const {
    SolverView view;
    view.phase = m_phase;
    switch (m_phase) {
        case Phase::Idle:
        case Phase::SelfTest: view.title = "Getting ready"; break;
        case Phase::Search: view.title = "Finding a path"; break;
        case Phase::Verify: view.title = "Checking the path"; break;
        case Phase::Optimize: view.title = "Cleaning up inputs"; break;
        case Phase::Refine: view.title = "Centering timings"; break;
        case Phase::FinalVerify: view.title = "Final check"; break;
        case Phase::Done: view.title = "Done"; break;
        case Phase::Failed: view.title = "Stopped"; break;
    }
    if (importRun() && running()) view.title = "Checking imported inputs";

    bool checkpointMode = m_restoreMode == RestoreMode::Checkpoint;
    view.steps = {"Test", "Search", "Check"};
    int cleanIndex = -1;
    int centerIndex = -1;
    if (m_optimizeEnabled && checkpointMode) {
        cleanIndex = static_cast<int>(view.steps.size());
        view.steps.push_back("Clean up");
    }
    if (m_refineEnabled && checkpointMode) {
        centerIndex = static_cast<int>(view.steps.size());
        view.steps.push_back("Center");
    }
    view.steps.push_back("Final check");
    int last = static_cast<int>(view.steps.size()) - 1;

    Phase shown = m_phase == Phase::Failed ? m_stoppedPhase : m_phase;
    switch (shown) {
        case Phase::Search: view.stepIndex = 1; break;
        case Phase::Verify: view.stepIndex = 2; break;
        case Phase::Optimize: view.stepIndex = cleanIndex >= 0 ? cleanIndex : 2; break;
        case Phase::Refine: view.stepIndex = centerIndex >= 0 ? centerIndex : (cleanIndex >= 0 ? cleanIndex : 2); break;
        case Phase::FinalVerify:
        case Phase::Done: view.stepIndex = last; break;
        default: view.stepIndex = 0; break;
    }

    view.progress = progress();
    view.percent = m_maxPercent;
    if (m_phase == Phase::Verify && m_importChecking && m_layer) {
        view.percent = std::max(view.percent, cleanPercent(m_layer->getCurrentPercent(), 0.f));
    }
    view.elapsed = elapsedSeconds();
    double elapsed = std::max(0.001, view.elapsed);
    view.gameSpeed = m_recentSpeed > 0.0 ? m_recentSpeed : static_cast<double>(m_totalSteps) / kTicksPerSecond / elapsed;
    view.detail = fmt::format(
        "{:.1f}%  |  {:.0f}x speed  |  {} backtracks  |  {} pruned",
        view.percent, view.gameSpeed, m_backtracks, m_prunes
    );
    view.note = phaseNote();
    return view;
}

std::string Solver::statusLine() const {
    switch (m_phase) {
        case Phase::SelfTest: return "Preparing solver...";
        case Phase::Search: return fmt::format("Finding a path... {:.1f}%", m_maxPercent);
        case Phase::Verify: return m_importChecking ? "Checking the imported inputs..." : "Checking the path from a clean restart...";
        case Phase::Optimize: return fmt::format("Cleaning up inputs {}/{}", std::min(m_optPosition + 1, m_optNotes.size()), m_optNotes.size());
        case Phase::Refine: return fmt::format("Centering note timings {}/{}", m_refineIndex, m_refineNotes.size());
        case Phase::FinalVerify: return "Final check...";
        case Phase::Done: return "Solved!";
        case Phase::Failed: return fmt::format("Solver stopped: {}", m_failReason.empty() ? "unknown reason" : m_failReason);
        default: return "";
    }
}

std::string Solver::detailLine() const {
    double elapsed = std::max(0.001, elapsedSeconds());
    double speed = m_recentSpeed > 0.0 ? m_recentSpeed : (static_cast<double>(m_totalSteps) / kTicksPerSecond) / elapsed;
    std::string out = fmt::format(
        "{:.1f}s  |  {:.0f}x game speed  |  {} backtracks  |  {} pruned",
        elapsed, speed, m_backtracks, m_prunes
    );
    if (m_verifyFailures > 0) out += fmt::format("  |  {} re-checks", m_verifyFailures);
    if (m_restoreMode == RestoreMode::Replay) out += "  |  exact replay mode";
    if (m_phase != Phase::SelfTest && !m_fastAllowed) out += "  |  full simulation";
    if (m_phase == Phase::Search && m_ladder.size() > 1) {
        auto const& step = currentStep();
        out += fmt::format("  |  {} precision{}", tierName(step.tier), step.constraints || !m_constraintsActive ? "" : ", relaxed timing");
    }
    return out;
}

float Solver::progress() const {
    switch (m_phase) {
        case Phase::SelfTest: {
            float length = static_cast<float>(m_selfTestStage == 0 ? kFastTestTicks : std::max(1, m_selfTestLength));
            float part = std::clamp(static_cast<float>(m_tick) / length, 0.f, 1.f);
            return std::clamp((static_cast<float>(m_selfTestStage) + part) / 3.f, 0.f, 1.f);
        }
        case Phase::Search: return std::clamp(m_maxPercent / std::max(1.f, m_targetPercent), 0.f, 1.f);
        case Phase::Verify:
        case Phase::FinalVerify:
            return m_seq.empty() ? 0.f : std::clamp(static_cast<float>(m_tick) / m_seq.size(), 0.f, 1.f);
        case Phase::Optimize:
            return m_optNotes.empty() ? 1.f : std::clamp(static_cast<float>(m_optPosition) / m_optNotes.size(), 0.f, 1.f);
        case Phase::Refine:
            return m_refineNotes.empty() ? 1.f : static_cast<float>(m_refineIndex) / m_refineNotes.size();
        case Phase::Done: return 1.f;
        default: return 0.f;
    }
}

}
