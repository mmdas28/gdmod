#include "Planner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <utility>

using namespace geode::prelude;

namespace rp {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint8_t MCube = 0;
constexpr uint8_t MShip = 1;
constexpr uint8_t MUfo = 2;
constexpr uint8_t MWave = 3;
constexpr uint8_t MBall = 4;
constexpr uint8_t MRobot = 5;
constexpr uint8_t MSpider = 6;
constexpr uint8_t MSwing = 7;
constexpr uint8_t MNone = 8;

constexpr uint8_t KSolid = 0;
constexpr uint8_t KHazard = 1;
constexpr uint8_t KSlope = 2;
constexpr uint8_t KOrb = 3;
constexpr uint8_t KPad = 4;
constexpr uint8_t KPortal = 5;
constexpr uint8_t KSpeed = 6;

constexpr uint8_t RAlive = 0;
constexpr uint8_t RDead = 1;
constexpr uint8_t RStop = 2;
constexpr uint8_t RMiss = 4;

constexpr uint8_t AKDead = 0;
constexpr uint8_t AKLanded = 1;
constexpr uint8_t AKOpen = 2;

constexpr int kMaxTrace = 1 << 19;
constexpr int kSinceCap = 30000;
constexpr int kSafeFlipTicks = 24;
constexpr int kCubeHorizon = 600;
constexpr int kCubeLook = 800;
constexpr int kCubeDepth = 10;
constexpr int kClearWidth = 4;
constexpr int kCubeReplan = 120;
constexpr int kHintSegs = 6;
constexpr int64_t kCubeHintWork = 200000;
constexpr int kHintPast = 30;
constexpr int kJoinSlack = 8;
constexpr double kFallPrior = 15.0;
constexpr int kFlyHorizon = 360;
constexpr int kFlyReplan = 60;
constexpr int kBeamWidth = 40;
constexpr int kCrossAhead = 200;
constexpr int kLateTicks = 8;
constexpr int kYBins = 8;
constexpr int kVBins = 4;
constexpr double kClearCap = 30.0;
constexpr size_t kMaxGates = 12;
constexpr double kSafeClear = 6.0;
constexpr double kTierStep = 2.0;
constexpr float kChangeCost = 0.4f;
constexpr int kMaxExtras = 64;
constexpr int kMaxCand = 96;
constexpr int kMaxColCand = 1024;
constexpr double kYPad = 20.0;
constexpr int kTypeSlots = 48;
constexpr int kCubeBudget = 90000;
constexpr int64_t kAdviseWork = 20000;
constexpr int kCubeAdviseBudget = 24000;
constexpr int kMinTrunc = 120;
constexpr int kWindowBudget = 40000;
constexpr int kWideBuckets = 64;
constexpr int kMaxBuckets = 1 << 20;
constexpr double kBucket = 64.0;
constexpr int kMaxTracks = 8192;
constexpr int kMaxKeys = 32;
constexpr int kMergeGap = 240;
constexpr int kMaxDyn = 256;
constexpr double kNearBehind = 64.0;
constexpr double kNearAhead = 320.0;
constexpr int kMoveReplanGap = 8;
constexpr double kEps = 1e-3;
constexpr double kTouch = 0.02;
constexpr int kTrustN = 3;
constexpr int kMaxHintBack = 560;
constexpr int kHintTriesPerTick = 8;
constexpr int kHintHorizon = 720;
constexpr int64_t kHintWork = 250000;
constexpr int64_t kHintHard = 300000;
constexpr size_t kFunnelCap = size_t{1} << 18;
constexpr size_t kFunnelWorkDiv = 16;
constexpr float kWideSlideZone = 256.f;

double quant3(double v) {
    double t = std::trunc(v);
    return t + std::round((v - t) * 1000.0) / 1000.0;
}

template <int N>
struct Ring {
    std::array<double, N> a{};
    std::array<double, N> b{};
    int n = 0;
    int head = 0;

    void push(double x, double y) {
        a[head] = x;
        b[head] = y;
        head = (head + 1) % N;
        if (n < N) n++;
    }
    void clear() {
        n = 0;
        head = 0;
    }
    int at(int i) const {
        return (head - n + i + 2 * N) % N;
    }
    void keepRecent(int k) {
        if (k >= n) return;
        if (k < 0) k = 0;
        std::array<double, N> na{};
        std::array<double, N> nb{};
        for (int i = 0; i < k; i++) {
            int j = at(n - k + i);
            na[i] = a[j];
            nb[i] = b[j];
        }
        a = na;
        b = nb;
        n = k;
        head = k % N;
    }
};

double medianOf(double* v, int n) {
    if (n <= 0) return 0.0;
    std::nth_element(v, v + n / 2, v + n);
    double m = v[n / 2];
    if (n % 2 == 0) {
        double lo = *std::max_element(v, v + n / 2);
        m = 0.5 * (m + lo);
    }
    return m;
}

template <int N>
double ringMedianB(Ring<N> const& r) {
    double tmp[N];
    for (int i = 0; i < r.n; i++) tmp[i] = r.b[r.at(i)];
    return medianOf(tmp, r.n);
}

template <int N>
double ringMedianA(Ring<N> const& r) {
    double tmp[N];
    for (int i = 0; i < r.n; i++) tmp[i] = r.a[r.at(i)];
    return medianOf(tmp, r.n);
}

struct Branch {
    int hits = 0;
    int streak = 0;
    int samples = 0;

    void hit() {
        hits++;
        streak = 0;
    }
    bool miss() {
        streak++;
        if (streak >= 3) {
            hits = 0;
            return true;
        }
        return false;
    }
};

struct MapObj {
    float x0 = 0.f;
    float y0 = 0.f;
    float x1 = 0.f;
    float y1 = 0.f;
    float cx = 0.f;
    float cy = 0.f;
    float r = 0.f;
    float margin = 0.f;
    float speed = 0.f;
    GameObject* obj = nullptr;
    int bx0 = 0;
    int bx1 = 0;
    int track = -1;
    int16_t type = 0;
    uint8_t kind = 0;
    bool passable = false;
    bool dyn = false;
    bool off = false;
};

struct MoveKey {
    int a = 0;
    int b = 0;
    float x0 = 0.f;
    float y0 = 0.f;
    float x1 = 0.f;
    float y1 = 0.f;
    bool off = false;
};

struct MoveTrack {
    int obj = -1;
    float ux0 = 0.f;
    float ux1 = 0.f;
    std::vector<MoveKey> keys;
};

struct PS {
    double x = 0.0;
    double y = 0.0;
    double v = 0.0;
    float hw = 15.f;
    float hh = 15.f;
    float gh = 15.f;
    float lo = 0.f;
    float hi = 0.f;
    float maxY = 0.f;
    float speed = 0.f;
    float yStart = 0.f;
    float grav = 0.f;
    int flipAge = kSinceCap;
    GameObject* portal = nullptr;
    uint8_t mode = MNone;
    bool grounded = false;
    bool upside = false;
    bool mini = false;
    bool held = false;
    bool fresh = false;
    bool special = false;
    bool dead = false;
    bool valid = false;
};

struct TraceEntry {
    PS s;
    int lastPad = -1;
    int usedOrb = -1;
    uint8_t held = 0;
    bool valid = false;
    bool heldValid = false;
    bool died = false;
};

struct SimState {
    double x = 0.0;
    double y = 0.0;
    double v = 0.0;
    double dx = 0.0;
    double js = 1.0;
    double gs = 1.0;
    float speed = 0.f;
    GameObject* portal = nullptr;
    int lastPad = -1;
    int usedOrb = -1;
    int since = kSinceCap;
    int flipAge = kSinceCap;
    uint8_t mode = MNone;
    uint8_t pi = 0;
    bool held = false;
    bool grounded = false;
    bool upside = false;
    bool fresh = false;
};

struct StepInfo {
    int killer = -1;
    int surface = -1;
    bool orbTouch = false;
    bool event = false;
};

struct Phys {
    uint8_t mode = MNone;
    uint8_t mk = 0;
    bool mini = false;
    bool lenient = false;
    bool touchOk = false;
    bool capUpEst = false;
    bool capDownEst = false;
    bool quant = false;
    bool bounded = false;
    bool hasFloor = false;
    bool floorKills = false;
    double dx = 0.0;
    double jRef = 0.0;
    double gRef = 0.0;
    double maxY = 1e9;
    double a = 0.0;
    double b = 0.225;
    double J = 0.0;
    double g = 0.0;
    double vmax = 1e9;
    double walkV = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double d1 = 0.0;
    double d2 = 0.0;
    double thr[2] = {0.0, 0.0};
    double capUp = 1e9;
    double capDown = 1e9;
    double imp = 0.0;
    double flapHi = 0.0;
    double slope = 1.0;
    double tolCube = 0.0;
    double tolFly = 0.0;
    double bHalfLo = 15.0;
    double bHalfHi = 15.0;
    double floorFeet = -1e9;
    double lo = -1e9;
    double hi = 1e9;
    double hw = 15.0;
    double hh = 15.0;
    double iw = 4.5;
    double ih = 4.5;
};

struct CubeModel {
    Ring<32> jump;
    Ring<32> air;
    Ring<16> walk;
    double capV = 0.0;
    double maxFall = 0.0;
    bool capKnown = false;
    Branch bJump;
    Branch bAir;
    Branch bCap;
};

struct FlyModel {
    Ring<32> up[4];
    Ring<32> down[4];
    Ring<16> flap;
    Ring<16> flapHiR;
    double a1 = 0.0;
    double a2 = 0.0;
    double d1 = 0.0;
    double d2 = 0.0;
    bool hasA1 = false;
    bool hasA2 = false;
    bool hasD1 = false;
    bool hasD2 = false;
    double thr[2] = {0.0, 0.0};
    bool thrKnown[2] = {false, false};
    double capUp = 0.0;
    double capDown = 0.0;
    bool capUpKnown = false;
    bool capDownKnown = false;
    double maxUp = 0.0;
    double maxDown = 0.0;
    bool splitH = false;
    bool splitR = false;
    double maxFall[2] = {-1e18, -1e18};
    double minRise[2] = {1e18, 1e18};
    double exC[4] = {0.0, 0.0, 0.0, 0.0};
    double imp = 0.0;
    bool impKnown = false;
    double flapHi = 0.0;
    bool flapHiKnown = false;
    Branch b[4];
    Branch bFlap;
    Branch bCap;
    int sinceFit = 0;
};

struct WaveModel {
    Ring<32> s;
    double slope = 0.0;
    bool known = false;
    Branch b[2];
};

struct DyModel {
    std::array<double, 64> vo{};
    std::array<double, 64> vn{};
    std::array<double, 64> dy{};
    int n = 0;
    int head = 0;
    double a = 0.0;
    double b = 0.225;
    bool known = false;
    int sinceFit = 0;
};

struct PortalFx {
    uint8_t kind = 0;
    float lo = 0.f;
    float hi = 0.f;
    float hw = 15.f;
    float hh = 15.f;
    float gh = 15.f;
    double vFactor = 1.0;
    double vAbs = 0.0;
    uint8_t mode = MNone;
    bool mini = false;
    bool upside = false;
    bool vKnown = false;
    bool known = false;
};

struct TypeFx {
    PortalFx fx;
    int n = 0;
    bool bad = false;
};

struct Effect {
    double v = 0.0;
    double jRef = 0.0;
    bool flips = false;
    bool known = false;
    int hits = 0;
};

struct SpeedDx {
    float speed = 0.f;
    float yStart = 0.f;
    float grav = 0.f;
    Ring<16> r;
    double dx = 0.0;
    double missDx = 0.0;
    int miss = 0;
};

struct Plan {
    bool valid = false;
    bool failed = false;
    bool cross = false;
    uint8_t mode = MNone;
    int t0 = 0;
    int replanAt = 0;
    std::vector<uint8_t> held;
    std::vector<SimState> pred;

    int end() const {
        return t0 + static_cast<int>(held.size());
    }
};

struct Arc {
    uint8_t kind = AKDead;
    int end = 0;
    SimState land;
};

struct CubeVal {
    int value = 0;
    int walkEnd = 0;
    int depth = 0;
};

struct CubeScratch {
    std::vector<int> ticks;
    std::vector<Arc> arcs;
    std::vector<uint8_t> done;
    std::vector<int> vals;
    std::vector<int> order;
};

struct Explored {
    double y = 0.0;
    int land = 0;
    int walkEnd = 0;
    bool upside = false;
    bool held = false;
};

struct RollBuf {
    std::vector<SimState> st;
    std::vector<uint8_t> orb;
    uint8_t result = RAlive;
    int endTick = 0;
};

struct BNode {
    double y = 0.0;
    double v = 0.0;
    float minLate = static_cast<float>(kClearCap);
    float sumClear = 0.f;
    float score = 0.f;
    int parent = -1;
    int since = 0;
    int flipAge = kSinceCap;
    int lastPad = -1;
    int usedOrb = -1;
    int16_t changes = 0;
    uint8_t held = 0;
    bool upside = false;
    bool fresh = false;
    bool grounded = false;
    bool onOld = false;
    bool fromOld = false;
    bool miss = false;
};

struct Gate {
    int col = 0;
    double lo = 0.0;
    double hi = 0.0;
};

struct ColSwitch {
    GameObject* portal = nullptr;
    double vFactor = 1.0;
    int to = 0;
    int8_t upRule = 0;
    bool modeSwitch = false;
};

struct Pending {
    bool active = false;
    int death = -1;
    int from = 0;
    int key = 0;
    std::vector<uint8_t> tail;
};

int modeOf(PlayerObject* p) {
    if (p->m_isShip) return MShip;
    if (p->m_isBird) return MUfo;
    if (p->m_isDart) return MWave;
    if (p->m_isBall) return MBall;
    if (p->m_isRobot) return MRobot;
    if (p->m_isSpider) return MSpider;
    if (p->m_isSwing) return MSwing;
    return MCube;
}

int familyOfMode(uint8_t mode) {
    if (mode == MShip) return 0;
    if (mode == MWave) return 1;
    return 2;
}

float speedOfId(int id) {
    switch (id) {
        case 200: return 0.7f;
        case 201: return 0.9f;
        case 202: return 1.1f;
        case 203: return 1.3f;
        case 1334: return 1.6f;
        default: return 0.f;
    }
}

int classify(int type, uint8_t& kind) {
    switch (type) {
        case 0:
        case 21:
            kind = KSolid;
            return 1;
        case 2:
        case 47:
            kind = KHazard;
            return 1;
        case 25:
            kind = KSlope;
            return 1;
        case 11: case 12: case 13: case 29: case 32: case 35: case 36: case 37: case 38: case 43: case 46:
            kind = KOrb;
            return 1;
        case 8: case 9: case 34: case 44: case 10:
            kind = KPad;
            return 1;
        case 3: case 4: case 42: case 5: case 6: case 16: case 19: case 26: case 27: case 33: case 41:
        case 17: case 18: case 14: case 15: case 23: case 24: case 28:
            kind = KPortal;
            return 1;
        case 20:
            kind = KSpeed;
            return 1;
        default:
            return 0;
    }
}

bool finiteRect(CCRect const& r) {
    return std::isfinite(r.origin.x) && std::isfinite(r.origin.y) && std::isfinite(r.size.width) &&
        std::isfinite(r.size.height) && r.size.width >= 0.f && r.size.height >= 0.f && r.size.width < 1e6f &&
        r.size.height < 1e6f && std::fabs(r.origin.x) < 1e8f && std::fabs(r.origin.y) < 1e8f;
}

}

class PlannerImpl {
public:
    explicit PlannerImpl(PlayLayer* layer) : m_layer(layer) {
        m_roll.resize(kCubeDepth + 2);
    }

    PlayLayer* m_layer = nullptr;
    bool m_enabled = true;
    PlannerLimits m_limits;
    PlannerStats m_stats;

    std::vector<MapObj> m_objs;
    std::vector<int> m_bStart;
    std::vector<int> m_bIdx;
    std::vector<int> m_wide;
    std::vector<int> m_dyn;
    std::vector<MoveTrack> m_tracks;
    std::vector<MoveKey> m_changed;
    bool m_rebucketDue = false;
    int m_buildTick = 0;
    int m_curTick = 0;
    int m_lastMoveReplan = -1000000;
    int m_moveGap = kMoveReplanGap;
    uint64_t m_moveEvents = 0;
    uint64_t m_moveReplans = 0;
    double m_xBase = 0.0;
    double m_bw = kBucket;
    int m_nb = 0;
    int m_extras = 0;
    unsigned m_sourceCount = 0;
    bool m_built = false;

    CubeModel m_cube[2];
    FlyModel m_ship[2];
    FlyModel m_ufo[2];
    WaveModel m_wave[2];
    DyModel m_dy;
    Effect m_padFx[kTypeSlots][8];
    std::unordered_map<GameObject*, PortalFx> m_portalFx;
    std::unordered_map<int, TypeFx> m_typeFx;
    std::unordered_map<GameObject*, int> m_portalType;
    Effect m_orbFx[kTypeSlots][8];
    int m_orbPreVotes = 0;
    int m_orbPostVotes = 0;
    std::vector<std::array<double, 4>> m_slideZones;
    std::vector<std::array<double, 4>> m_wideSlideZones;
    double m_slideMaxW = 0.0;
    std::vector<SpeedDx> m_speeds;
    double m_tolCube = 0.0;
    double m_tolFly = 0.0;
    double m_floorFeet = 0.0;
    bool m_floorKnown = false;
    double m_bHalfLo[8] = {};
    double m_bHalfHi[8] = {};
    bool m_bHalfLoKnown[8] = {};
    bool m_bHalfHiKnown[8] = {};
    double m_bHalfLoCand[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    double m_bHalfHiCand[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int m_qYes = 0;
    int m_qNo = 0;
    double m_vq = 0.0;
    int m_vqHits = 0;
    int m_vqMiss = 0;
    int m_dyReject = 0;

    std::vector<TraceEntry> m_trace;
    int m_traceEnd = -1;

    Plan m_plan;
    int m_adviceTick = -1;
    uint8_t m_adviceVal = 0;
    int m_lastMismatchReplan = -1000000;

    std::vector<RollBuf> m_roll;
    std::unordered_map<uint64_t, CubeVal> m_cmemo;
    std::unordered_map<uint64_t, Arc> m_arcMemo;
    std::array<CubeScratch, kCubeDepth + 2> m_cubeScratch;
    int m_budget = 0;
    int64_t m_work = 0;
    int64_t m_workCap = 0;
    int64_t m_hardCap = 0;
    int m_horizonEnd = 0;
    int m_maxArc = 200;

    std::vector<int> m_colStart;
    std::vector<int> m_colIdx;
    std::vector<int> m_gbuf = std::vector<int>(kMaxColCand);
    std::vector<int> m_funStart;
    std::vector<std::pair<double, double>> m_funIv;
    std::vector<std::pair<double, double>> m_tmpIv;
    std::vector<std::pair<double, double>> m_tmpIv2;
    bool m_funOn = false;
    bool m_stopCol = false;
    int m_stopColIdx = -1;
    std::vector<BNode> m_nodes;
    std::vector<BNode> m_children;
    std::vector<Phys> m_bPhys;
    std::vector<PS> m_bPs;
    std::vector<uint8_t> m_colPhys;
    std::vector<int> m_colSwStart;
    std::vector<ColSwitch> m_switches;
    std::vector<double> m_colX;
    std::vector<double> m_colDx;
    std::vector<float> m_colSpeed;
    std::vector<Gate> m_gates;
    int m_bNeed = 1;
    bool m_bOptimistic = false;
    std::vector<uint8_t> m_guide;
    int m_guideLen = 0;
    std::vector<BNode> m_sel;
    std::vector<int> m_restIdx;
    std::vector<uint8_t> m_pick;
    std::vector<int> m_rStart;
    std::vector<int> m_tmpEnds;
    std::vector<std::pair<double, double>> m_rIv;
    std::vector<std::pair<double, double>> m_dilIv;
    std::vector<int> m_leaves;
    std::array<uint32_t, 2048> m_dedupStamp{};
    std::array<int, 2048> m_dedupSlot{};
    std::array<uint64_t, 2048> m_dedupKey{};
    uint32_t m_dedupGen = 0;

    std::array<std::vector<int>, 64> m_offsets;
    Pending m_pending;
    struct LastHint {
        bool active = false;
        int tick = -1;
        int death = -1;
        uint8_t held = 0;
    } m_lastHint;
    uint64_t m_hintWins = 0;
    uint64_t m_hintFails = 0;
    uint64_t m_lenientPlans = 0;
    uint64_t m_crossPlans = 0;
    uint64_t m_crossHints = 0;
    std::array<uint64_t, 64> m_recentFallbacks{};
    std::unordered_map<int, int> m_deathTries;
    bool m_strictOnly = false;
    int m_passedTick = -1;
    size_t m_recentFallbackPos = 0;
    uint64_t m_provisional = 0;

    int m_adviseLevel = 1;
    int m_hintLevel = 1;
    int m_strictDeaths = 0;
    int m_softSurvivals = 0;
    int m_touchSafe = 0;
    int m_touchDeadly = 0;
    bool m_bTouch = false;
    mutable bool m_touchHit = false;
    mutable bool m_capHit = false;
    mutable bool m_solidHit = false;
    struct TryMemo {
        uint64_t key = 0;
        int dt = -1;
        Plan plan;
    };
    std::array<TryMemo, 16> m_tryMemo;
    uint64_t m_mapEpoch = 0;

    int bucketOf(double x) const {
        if (m_nb <= 0) return 0;
        double f = std::floor((x - m_xBase) / m_bw);
        if (!(f > 0.0)) return 0;
        if (f >= static_cast<double>(m_nb - 1)) return m_nb - 1;
        return static_cast<int>(f);
    }

    template <class F>
    void query(double qx0, double qx1, F&& f) const {
        if (!m_built) return;
        if (m_nb > 0 && !m_bStart.empty()) {
            int b0 = bucketOf(qx0);
            int b1 = bucketOf(qx1);
            for (int b = b0; b <= b1; b++) {
                int s = m_bStart[static_cast<size_t>(b)];
                int e = m_bStart[static_cast<size_t>(b) + 1];
                for (int k = s; k < e; k++) {
                    int i = m_bIdx[static_cast<size_t>(k)];
                    MapObj const& o = m_objs[static_cast<size_t>(i)];
                    if (b != std::max(b0, o.bx0) || o.dyn || o.off) continue;
                    f(i, o);
                }
            }
        }
        for (int i : m_wide) {
            MapObj const& o = m_objs[static_cast<size_t>(i)];
            if (!o.off) f(i, o);
        }
        for (int i : m_dyn) {
            MapObj const& o = m_objs[static_cast<size_t>(i)];
            if (o.off || o.x1 + o.margin < qx0 || o.x0 - o.margin > qx1) continue;
            f(i, o);
        }
    }

    template <class F>
    void forEachIn(double qx0, double qx1, F&& f) {
        if (!m_built || m_nb <= 0 || m_bStart.empty()) return;
        int b0 = bucketOf(qx0);
        int b1 = bucketOf(qx1);
        for (int b = b0; b <= b1; b++) {
            int s = m_bStart[static_cast<size_t>(b)];
            int e = m_bStart[static_cast<size_t>(b) + 1];
            for (int k = s; k < e; k++) {
                int i = m_bIdx[static_cast<size_t>(k)];
                MapObj const& o = m_objs[static_cast<size_t>(i)];
                if (b != std::max(b0, o.bx0) || o.dyn) continue;
                f(i);
            }
        }
        for (int i : m_wide) {
            MapObj const& o = m_objs[static_cast<size_t>(i)];
            if (o.obj && o.x1 >= qx0 && o.x0 <= qx1) f(i);
        }
        size_t nd = m_dyn.size();
        for (size_t k = 0; k < nd && k < m_dyn.size(); k++) {
            int i = m_dyn[k];
            MapObj const& o = m_objs[static_cast<size_t>(i)];
            if (o.x1 >= qx0 && o.x0 <= qx1) f(i);
        }
    }

    int gather(double x0, double x1, int* out, int cap, bool* overflow = nullptr, double y0 = -1e18, double y1 = 1e18) const {
        int n = 0;
        query(x0, x1, [&](int i, MapObj const& o) {
            if (o.x1 + o.margin < x0 || o.x0 - o.margin > x1) return;
            if (o.y1 + o.margin < y0 || o.y0 - o.margin > y1) return;
            if (n < cap) out[n++] = i;
            else if (overflow) *overflow = true;
        });
        return n;
    }

    void buildMap() {
        std::vector<MoveTrack> oldTracks;
        oldTracks.swap(m_tracks);
        std::vector<GameObject*> oldTrackObj;
        oldTrackObj.reserve(oldTracks.size());
        for (auto const& tr : oldTracks) {
            GameObject* g = tr.obj >= 0 && tr.obj < static_cast<int>(m_objs.size()) ? m_objs[static_cast<size_t>(tr.obj)].obj : nullptr;
            oldTrackObj.push_back(g);
        }
        m_objs.clear();
        m_bStart.clear();
        m_bIdx.clear();
        m_wide.clear();
        m_dyn.clear();
        m_nb = 0;
        m_extras = 0;
        m_built = false;
        m_sourceCount = 0;
        m_plan.valid = false;
        m_slideZones.clear();
        m_wideSlideZones.clear();
        m_slideMaxW = 0.0;
        m_buildTick = m_curTick;
        if (!m_layer || !m_layer->m_objects) return;
        auto arr = m_layer->m_objects;
        m_sourceCount = arr->count();
        m_objs.reserve(m_sourceCount);
        for (auto obj : CCArrayExt<GameObject*>(arr)) {
            if (!obj) continue;
            if (obj == m_layer->m_anticheatSpike) continue;
            int type = static_cast<int>(obj->m_objectType);
            if (type == 7 || type == 39) continue;
            if (obj->m_isNoTouch) continue;
            if (type == 40) {
                if (obj->m_objectID == 1755 && m_slideZones.size() + m_wideSlideZones.size() < 65536) {
                    CCRect z = obj->getObjectRect();
                    if (finiteRect(z)) {
                        std::array<double, 4> zone{z.origin.x, z.origin.y, z.origin.x + z.size.width, z.origin.y + z.size.height};
                        if (z.size.width > kWideSlideZone && m_wideSlideZones.size() < 256) m_wideSlideZones.push_back(zone);
                        else {
                            m_slideZones.push_back(zone);
                            m_slideMaxW = std::max(m_slideMaxW, static_cast<double>(z.size.width));
                        }
                    }
                }
                continue;
            }
            uint8_t kind = 0;
            if (!classify(type, kind)) continue;
            float speed = 0.f;
            if (kind == KSpeed) {
                speed = speedOfId(obj->m_objectID);
                if (speed <= 0.f) continue;
            }
            CCRect r = obj->getObjectRect();
            if (!finiteRect(r)) continue;
            MapObj m;
            m.x0 = r.origin.x;
            m.y0 = r.origin.y;
            m.x1 = r.origin.x + r.size.width;
            m.y1 = r.origin.y + r.size.height;
            if (kind == KSlope) {
                float w = m.x1 - m.x0;
                float h = m.y1 - m.y0;
                m.x0 -= 0.5f * w;
                m.x1 += 0.5f * w;
                m.y0 -= 0.5f * h;
                m.y1 += 0.5f * h;
            }
            m.cx = 0.5f * (m.x0 + m.x1);
            m.cy = 0.5f * (m.y0 + m.y1);
            if (kind == KHazard) {
                float rad = obj->getObjectRadius();
                if (std::isfinite(rad) && rad > 0.f && rad < 1e5f) m.r = rad;
            }
            m.speed = speed;
            m.obj = obj;
            m.type = static_cast<int16_t>(type);
            m.kind = kind;
            m.passable = kind == KSolid && (obj->m_isPassable || type == 21);
            m.off = obj->m_isGroupDisabled || obj->m_isDisabled;
            if (kind == KPortal && m_portalType.size() < 8192) m_portalType[obj] = type;
            m_objs.push_back(m);
        }
        std::stable_sort(m_objs.begin(), m_objs.end(), [](MapObj const& a, MapObj const& b) { return a.x0 < b.x0; });
        std::sort(m_slideZones.begin(), m_slideZones.end());
        if (!oldTracks.empty()) {
            std::unordered_map<GameObject*, int> where;
            where.reserve(oldTracks.size() * 2);
            for (auto g : oldTrackObj) {
                if (g) where[g] = -1;
            }
            for (size_t i = 0; i < m_objs.size(); i++) {
                auto it = where.find(m_objs[i].obj);
                if (it != where.end()) it->second = static_cast<int>(i);
            }
            for (size_t k = 0; k < oldTracks.size(); k++) {
                GameObject* g = oldTrackObj[k];
                if (!g) continue;
                auto it = where.find(g);
                if (it == where.end() || it->second < 0) continue;
                MoveTrack tr = std::move(oldTracks[k]);
                tr.obj = it->second;
                m_objs[static_cast<size_t>(it->second)].track = static_cast<int>(m_tracks.size());
                m_tracks.push_back(std::move(tr));
            }
        }
        finalizeMap();
        m_stats.mapObjects = m_objs.size();
    }

    bool slideZone(double x0, double x1, double y0, double y1) const {
        for (auto const& z : m_wideSlideZones) {
            if (z[0] > x1 || z[2] < x0 || z[1] > y1 || z[3] < y0) continue;
            return true;
        }
        if (m_slideZones.empty()) return false;
        double from = x0 - m_slideMaxW;
        auto it = std::lower_bound(m_slideZones.begin(), m_slideZones.end(), from, [](std::array<double, 4> const& z, double v) { return z[0] < v; });
        for (; it != m_slideZones.end() && (*it)[0] <= x1; ++it) {
            auto const& z = *it;
            if (z[2] < x0 || z[1] > y1 || z[3] < y0) continue;
            return true;
        }
        return false;
    }

    void reserveBuffers(double xEnd) {
        double ticks = std::isfinite(xEnd) ? std::clamp(xEnd, 0.0, static_cast<double>(kMaxTrace)) : 0.0;
        size_t want = std::min(static_cast<size_t>(kMaxTrace), static_cast<size_t>(ticks) + 2048);
        if (m_trace.capacity() < want) m_trace.reserve(want);
        if (m_nodes.capacity() < 65536) m_nodes.reserve(65536);
        if (m_colIdx.capacity() < 65536) m_colIdx.reserve(65536);
        if (m_children.capacity() < 1024) m_children.reserve(1024);
        if (m_sel.capacity() < 512) m_sel.reserve(512);
        if (m_restIdx.capacity() < 1024) m_restIdx.reserve(1024);
        m_cmemo.reserve(8192);
        m_arcMemo.reserve(8192);
    }

    void finalizeMap() {
        m_built = true;
        if (m_objs.empty()) return;
        double lo = m_objs.front().x0;
        double hi = lo;
        for (auto const& o : m_objs) {
            lo = std::min(lo, static_cast<double>(o.x0));
            hi = std::max(hi, static_cast<double>(o.x1));
        }
        m_xBase = lo;
        m_bw = kBucket;
        double span = hi - lo;
        reserveBuffers(hi);
        if (span / m_bw > kMaxBuckets - 2) m_bw = span / (kMaxBuckets - 2) + 1.0;
        m_nb = std::clamp(static_cast<int>(span / m_bw) + 2, 1, kMaxBuckets);
        rebucket();
    }

    void regRange(MapObj const& o, float& x0, float& x1) const {
        x0 = o.x0;
        x1 = o.x1;
        if (o.track >= 0 && o.track < static_cast<int>(m_tracks.size())) {
            auto const& tr = m_tracks[static_cast<size_t>(o.track)];
            x0 = std::min(x0, tr.ux0);
            x1 = std::max(x1, tr.ux1);
        }
    }

    void rebucket() {
        m_wide.clear();
        m_dyn.clear();
        m_bStart.clear();
        m_bIdx.clear();
        if (m_nb <= 0) return;
        std::vector<int> counts(static_cast<size_t>(m_nb) + 1, 0);
        for (size_t i = 0; i < m_objs.size(); i++) {
            auto& o = m_objs[i];
            o.dyn = false;
            float rx0 = 0.f;
            float rx1 = 0.f;
            regRange(o, rx0, rx1);
            int b0 = bucketOf(rx0);
            int b1 = bucketOf(rx1);
            o.bx0 = b0;
            o.bx1 = b1;
            if (b1 - b0 > kWideBuckets) {
                m_wide.push_back(static_cast<int>(i));
                o.bx0 = -1;
                o.bx1 = -1;
                continue;
            }
            for (int b = b0; b <= b1; b++) counts[static_cast<size_t>(b)]++;
        }
        m_bStart.assign(static_cast<size_t>(m_nb) + 1, 0);
        for (int b = 0; b < m_nb; b++) m_bStart[static_cast<size_t>(b) + 1] = m_bStart[static_cast<size_t>(b)] + counts[static_cast<size_t>(b)];
        m_bIdx.assign(static_cast<size_t>(m_bStart[static_cast<size_t>(m_nb)]), 0);
        std::vector<int> fill(m_bStart.begin(), m_bStart.end() - 1);
        for (size_t i = 0; i < m_objs.size(); i++) {
            auto const& o = m_objs[i];
            if (o.bx0 < 0) continue;
            for (int b = o.bx0; b <= o.bx1; b++) m_bIdx[static_cast<size_t>(fill[static_cast<size_t>(b)]++)] = static_cast<int>(i);
        }
    }

    void addExtra(MapObj m) {
        if (m_extras >= kMaxExtras) return;
        m.bx0 = -1;
        m.bx1 = -1;
        m.track = -1;
        m.dyn = false;
        m.off = false;
        m_objs.push_back(m);
        m_wide.push_back(static_cast<int>(m_objs.size()) - 1);
        m_extras++;
    }

    bool liveKey(MapObj const& o, MoveKey& k) const {
        GameObject* g = o.obj;
        if (!g) return false;
        CCRect r = g->getObjectRect();
        if (!finiteRect(r)) return false;
        k.x0 = r.origin.x;
        k.y0 = r.origin.y;
        k.x1 = r.origin.x + r.size.width;
        k.y1 = r.origin.y + r.size.height;
        if (o.kind == KSlope) {
            float w = k.x1 - k.x0;
            float h = k.y1 - k.y0;
            k.x0 -= 0.5f * w;
            k.x1 += 0.5f * w;
            k.y0 -= 0.5f * h;
            k.y1 += 0.5f * h;
        }
        k.off = g->m_isGroupDisabled || g->m_isDisabled || g->m_isNoTouch || static_cast<int>(g->m_objectType) == 7;
        return true;
    }

    static bool sameKey(MoveKey const& a, MoveKey const& b, float tol = 0.01f) {
        return a.off == b.off && std::fabs(a.x0 - b.x0) < tol && std::fabs(a.y0 - b.y0) < tol && std::fabs(a.x1 - b.x1) < tol &&
            std::fabs(a.y1 - b.y1) < tol;
    }

    static MoveKey keyOf(MapObj const& o) {
        MoveKey k;
        k.x0 = o.x0;
        k.y0 = o.y0;
        k.x1 = o.x1;
        k.y1 = o.y1;
        k.off = o.off;
        return k;
    }

    void setRect(int i, MoveKey const& k) {
        MapObj& o = m_objs[static_cast<size_t>(i)];
        o.x0 = k.x0;
        o.y0 = k.y0;
        o.x1 = k.x1;
        o.y1 = k.y1;
        o.cx = 0.5f * (k.x0 + k.x1);
        o.cy = 0.5f * (k.y0 + k.y1);
        o.off = k.off;
        if (o.dyn || o.bx0 < 0) return;
        if (bucketOf(o.x0) < o.bx0 || bucketOf(o.x1) > o.bx1) {
            o.dyn = true;
            m_dyn.push_back(i);
            m_rebucketDue = m_rebucketDue || static_cast<int>(m_dyn.size()) > kMaxDyn;
        }
    }

    MoveKey const* lookupKey(MoveTrack const& tr, int tau) const {
        auto const& ks = tr.keys;
        if (ks.empty()) return nullptr;
        size_t idx = static_cast<size_t>(std::upper_bound(ks.begin(), ks.end(), tau, [](int t, MoveKey const& k) { return t < k.a; }) - ks.begin());
        if (idx == 0) return &ks.front();
        MoveKey const& p = ks[idx - 1];
        if (p.b >= tau || idx >= ks.size()) return &p;
        MoveKey const& n = ks[idx];
        return (tau - p.b) <= (n.a - tau) ? &p : &n;
    }

    bool sampleTrack(MoveTrack& tr, int t, MoveKey k) {
        MoveKey const* pred = lookupKey(tr, t);
        bool news = !pred || !sameKey(*pred, k, 0.5f);
        auto& ks = tr.keys;
        k.a = t;
        k.b = t;
        size_t idx = static_cast<size_t>(std::upper_bound(ks.begin(), ks.end(), t, [](int v, MoveKey const& q) { return v < q.a; }) - ks.begin());
        if (idx > 0 && ks[idx - 1].b >= t) {
            MoveKey old = ks[idx - 1];
            if (sameKey(old, k, 0.25f)) return news;
            std::array<MoveKey, 3> parts;
            int np = 0;
            if (old.a <= t - 1) {
                parts[static_cast<size_t>(np)] = old;
                parts[static_cast<size_t>(np)].b = t - 1;
                np++;
            }
            parts[static_cast<size_t>(np++)] = k;
            if (t + 1 <= old.b) {
                parts[static_cast<size_t>(np)] = old;
                parts[static_cast<size_t>(np)].a = t + 1;
                np++;
            }
            ks.erase(ks.begin() + static_cast<long>(idx - 1));
            ks.insert(ks.begin() + static_cast<long>(idx - 1), parts.begin(), parts.begin() + np);
        }
        else {
            bool mp = idx > 0 && t - ks[idx - 1].b <= kMergeGap && sameKey(ks[idx - 1], k, 0.25f);
            bool mn = idx < ks.size() && ks[idx].a - t <= kMergeGap && sameKey(ks[idx], k, 0.25f);
            if (mp && mn) {
                ks[idx - 1].b = ks[idx].b;
                ks.erase(ks.begin() + static_cast<long>(idx));
            }
            else if (mp) {
                ks[idx - 1].b = t;
            }
            else if (mn) {
                ks[idx].a = t;
            }
            else {
                ks.insert(ks.begin() + static_cast<long>(idx), k);
            }
        }
        while (ks.size() > static_cast<size_t>(kMaxKeys)) {
            long far0 = std::abs(static_cast<long>(t) - static_cast<long>(ks.front().b));
            long far1 = std::abs(static_cast<long>(ks.back().a) - static_cast<long>(t));
            if (far0 >= far1) ks.erase(ks.begin());
            else ks.pop_back();
        }
        tr.ux0 = std::min(tr.ux0, k.x0);
        tr.ux1 = std::max(tr.ux1, k.x1);
        return news;
    }

    bool refreshObj(int i, int tick) {
        MapObj& o = m_objs[static_cast<size_t>(i)];
        MoveKey k;
        if (!liveKey(o, k)) return false;
        if (o.track < 0) {
            MoveKey cur = keyOf(o);
            if (sameKey(cur, k, 0.05f)) return false;
            if (static_cast<int>(m_tracks.size()) >= kMaxTracks) {
                setRect(i, k);
                m_moveEvents++;
                return true;
            }
            MoveTrack tr;
            tr.obj = i;
            tr.ux0 = std::min(cur.x0, k.x0);
            tr.ux1 = std::max(cur.x1, k.x1);
            if (m_buildTick != tick) {
                cur.a = m_buildTick;
                cur.b = m_buildTick;
                tr.keys.push_back(cur);
            }
            o.track = static_cast<int>(m_tracks.size());
            m_tracks.push_back(std::move(tr));
        }
        MoveTrack& tr = m_tracks[static_cast<size_t>(o.track)];
        bool news = sampleTrack(tr, tick, k);
        if (news) m_moveEvents++;
        setRect(i, k);
        return news;
    }

    bool refreshRange(double x0, double x1, int tick, bool collect = false) {
        if (!m_built || m_objs.empty()) return false;
        bool any = false;
        if (collect) m_changed.clear();
        forEachIn(x0, x1, [&](int i) {
            MapObj const& o = m_objs[static_cast<size_t>(i)];
            MoveKey before = keyOf(o);
            if (refreshObj(i, tick)) {
                any = true;
                if (collect && m_changed.size() < 32) {
                    MapObj const& n = m_objs[static_cast<size_t>(i)];
                    MoveKey u = before;
                    u.x0 = std::min(before.x0, n.x0);
                    u.y0 = std::min(before.y0, n.y0);
                    u.x1 = std::max(before.x1, n.x1);
                    u.y1 = std::max(before.y1, n.y1);
                    m_changed.push_back(u);
                }
            }
        });
        return any;
    }

    void maybeRebucket() {
        if (!m_rebucketDue) return;
        m_rebucketDue = false;
        rebucket();
    }

    void applyReach(int t, double x, double dx, double hw, double xa, double xb) {
        if (m_tracks.empty() || !(dx > 1e-6)) return;
        for (auto& tr : m_tracks) {
            if (tr.obj < 0 || tr.obj >= static_cast<int>(m_objs.size())) continue;
            if (tr.ux1 < xa || tr.ux0 > xb) continue;
            MapObj const& o = m_objs[static_cast<size_t>(tr.obj)];
            double lead = static_cast<double>(o.x0) - (x + hw);
            int tau = t + static_cast<int>(std::clamp(lead / dx, 0.0, 1e6));
            MoveKey const* k = lookupKey(tr, tau);
            if (!k) continue;
            double lead2 = static_cast<double>(k->x0) - (x + hw);
            int tau2 = t + static_cast<int>(std::clamp(lead2 / dx, 0.0, 1e6));
            if (tau2 != tau) {
                MoveKey const* k2 = lookupKey(tr, tau2);
                if (k2) k = k2;
            }
            MoveKey c = *k;
            setRect(tr.obj, c);
        }
    }

    void prepareMap(int t, double x, double dx, double hw, int ticks, bool refresh = true) {
        if (!m_built) return;
        double span = std::max(0.0, dx) * std::max(0, ticks);
        double xa = x - kNearBehind;
        double xb = x + span + kNearBehind;
        if (refresh) refreshRange(xa, xb, m_curTick);
        applyReach(t, x, dx, hw, xa, xb);
        maybeRebucket();
    }

    bool readPlayer(PS& s, bool betweenFrames = false) const {
        s = PS{};
        if (!m_layer) return false;
        PlayerObject* p = m_layer->m_player1;
        if (!p) return false;
        float flipT = m_layer->m_gameState.m_levelFlipping;
        bool flipping = flipT > 0.f && flipT < 1.f;
        CCPoint pos = betweenFrames && flipping && !p->m_isLocked ? p->m_position : p->getPosition();
        s.x = pos.x;
        s.y = pos.y;
        s.v = p->m_yVelocity;
        CCRect r = p->getObjectRect();
        if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.v) || !finiteRect(r)) return false;
        s.hw = r.size.width * 0.5f;
        s.hh = r.size.height * 0.5f;
        if (s.hw < 0.5f || s.hh < 0.5f || s.hw > 200.f || s.hh > 200.f) return false;
        float groundSize = p->m_unkAngle1;
        float vehicle = p->m_vehicleSize;
        float gh = groundSize * 0.5f * vehicle;
        s.gh = std::isfinite(gh) && gh >= 0.5f && gh <= 200.f ? gh : s.hh;
        s.mode = static_cast<uint8_t>(modeOf(p));
        s.grounded = p->m_isOnGround;
        s.upside = p->m_isUpsideDown;
        s.mini = p->m_vehicleSize < 0.99f;
        s.held = p->m_jumpBuffered;
        s.fresh = p->m_stateRingJump;
        s.speed = p->m_playerSpeed;
        s.yStart = static_cast<float>(p->m_yStart);
        s.grav = static_cast<float>(p->m_gravity);
        s.portal = p->m_lastActivatedPortal;
        s.dead = p->m_isDead;
        bool plat = p->m_isPlatformer || (m_layer->m_levelSettings && m_layer->m_levelSettings->m_platformerMode);
        s.special = m_layer->m_gameState.m_isDualMode || plat || p->m_isSideways || p->m_isGoingLeft;
        float lo = m_layer->getMinPortalY();
        float hi = m_layer->getMaxPortalY();
        s.lo = std::isfinite(lo) ? lo : 0.f;
        s.hi = std::isfinite(hi) ? hi : 1e6f;
        float maxY = m_layer->m_maxGameplayY;
        if (std::isfinite(maxY) && maxY > 200.f && maxY < 1e6f) {
            bool sizeOk = std::isfinite(groundSize) && groundSize > 0.f && groundSize < 400.f && std::isfinite(vehicle) && vehicle > 0.f && vehicle < 1.f;
            s.maxY = maxY + (s.mini && sizeOk ? groundSize * (1.f - vehicle) * 0.5f : 0.f);
        }
        double lastFlip = p->m_lastFlipTime;
        double now = p->m_totalTime;
        if (std::isfinite(lastFlip) && std::isfinite(now) && lastFlip != 0.0 && now >= lastFlip) {
            s.flipAge = static_cast<int>(std::min<double>(kSinceCap, std::floor((now - lastFlip) * 240.0 + 0.5)));
        }
        s.valid = true;
        return true;
    }

    static int mkOf(uint8_t mode, bool mini) {
        return mode < 4 ? mode * 2 + (mini ? 1 : 0) : 0;
    }

    static bool validJ(float j) {
        return j > 0.5f && j < 200.f;
    }

    static bool validG(float g) {
        return g > 0.01f && g < 100.f;
    }

    double refJ(PS const& s) const {
        return validJ(s.yStart) ? static_cast<double>(s.yStart) : 1.0;
    }

    double refG(PS const& s) const {
        return validG(s.grav) ? static_cast<double>(s.grav) * 0.225 : 1.0;
    }

    double priorThr(PS const& s, int up) const {
        double g2 = (s.grav > 0.01f && s.grav < 100.f) ? 2.0 * s.grav : 1.916398;
        return up ? -g2 : g2;
    }

    SpeedDx* speedSlot(float speed, bool create) {
        for (auto& sd : m_speeds) {
            if (std::fabs(sd.speed - speed) < 0.005f) return &sd;
        }
        if (!create || m_speeds.size() >= 16) return nullptr;
        m_speeds.push_back(SpeedDx{});
        m_speeds.back().speed = speed;
        return &m_speeds.back();
    }

    bool jgFor(float speed, double& j, double& g) const {
        for (auto const& sd : m_speeds) {
            if (std::fabs(sd.speed - speed) < 0.005f && sd.r.n > 0 && validJ(sd.yStart) && validG(sd.grav)) {
                j = sd.yStart;
                g = sd.grav;
                return true;
            }
        }
        return false;
    }

    static double curJ(Phys const& P, SimState const& s) {
        return P.jRef > 0.0 ? P.jRef * s.js : 0.0;
    }

    static double orbV(Effect const& e, double j) {
        if (e.jRef > 0.0 && j > 0.0 && std::fabs(j - e.jRef) > 1e-9) return e.v * (j / e.jRef);
        return e.v;
    }

    int orbTiming() const {
        if (m_orbPreVotes >= 2 && m_orbPreVotes >= 3 * m_orbPostVotes) return 1;
        if (m_orbPostVotes >= 2 && m_orbPostVotes >= 3 * m_orbPreVotes) return 0;
        return -1;
    }

    bool dxFor(float speed, double& dx) const {
        for (auto const& sd : m_speeds) {
            if (std::fabs(sd.speed - speed) < 0.005f && sd.r.n > 0) {
                dx = sd.dx;
                return true;
            }
        }
        return false;
    }

    int level(uint8_t mode, bool mini) const {
        int m = mini ? 1 : 0;
        switch (mode) {
            case MCube: {
                auto const& c = m_cube[m];
                bool learned = c.jump.n > 0 && c.air.n > 0 && m_dy.known;
                bool valid = c.bJump.hits >= 1 && c.bAir.hits >= kTrustN;
                return learned ? (valid ? 3 : 2) : 1;
            }
            case MShip: {
                auto const& f = m_ship[m];
                bool learned = f.hasA1 && f.hasA2 && f.hasD1 && f.hasD2 && m_dy.known;
                bool valid = true;
                for (int i = 0; i < 4; i++) valid = valid && f.b[i].hits >= kTrustN;
                return learned ? (valid ? 3 : 2) : 1;
            }
            case MUfo: {
                auto const& f = m_ufo[m];
                bool learned = f.impKnown && f.hasD1 && f.hasD2 && m_dy.known;
                bool valid = f.bFlap.hits >= 1 && f.b[2].hits >= kTrustN && f.b[3].hits >= kTrustN;
                return learned ? (valid ? 3 : 2) : 1;
            }
            case MWave: {
                auto const& w = m_wave[m];
                bool valid = w.b[0].hits >= kTrustN && w.b[1].hits >= kTrustN;
                return w.known ? (valid ? 3 : 2) : 1;
            }
            default:
                return 0;
        }
    }

    bool resolve(PS const& s, Phys& P, bool lenient, int need, bool optimistic = false) const {
        P = Phys{};
        if (s.mode >= 4) return false;
        int lv = level(s.mode, s.mini);
        if (lv < need) return false;
        P.mode = s.mode;
        P.mini = s.mini;
        P.mk = static_cast<uint8_t>(mkOf(s.mode, s.mini));
        P.lenient = lenient;
        P.touchOk = m_bTouch || touchLearned();
        P.quant = m_qYes >= 16 && m_qNo * 20 < m_qYes;
        P.hw = s.hw;
        P.hh = s.hh;
        double vs = s.mini ? 0.6 : 1.0;
        P.iw = 0.3 * s.hw / vs;
        P.ih = 0.3 * s.hh / vs;
        if (m_dy.known) {
            P.a = m_dy.a;
            P.b = m_dy.b;
        }
        int m = s.mini ? 1 : 0;
        switch (s.mode) {
            case MCube: {
                auto const& c = m_cube[m];
                double jr = c.jump.n > 0 ? ringMedianA(c.jump) : (s.mini ? 0.8 : 1.0);
                double gr = c.air.n > 0 ? ringMedianB(c.air) : 1.0;
                P.J = jr * refJ(s);
                P.g = gr * refG(s);
                P.vmax = c.capKnown ? c.capV : (c.maxFall < kFallPrior - (0.004 + 1e-3 * kFallPrior) ? kFallPrior : 1e9);
                P.walkV = c.walk.n > 0 ? ringMedianA(c.walk) : 0.0;
                if (!(P.J > 0.0) || !(P.g > 0.0)) return false;
                break;
            }
            case MShip:
            case MUfo: {
                auto const& f = s.mode == MShip ? m_ship[m] : m_ufo[m];
                double k = s.mini ? 1.0 / 0.85 : 1.0;
                if (s.mode == MShip) {
                    P.a1 = f.hasA1 ? f.a1 : 0.107798 * k;
                    P.a2 = f.hasA2 ? f.a2 : 0.086238 * k;
                    P.d1 = f.hasD1 ? f.d1 : 0.068990 * k;
                    P.d2 = f.hasD2 ? f.d2 : 0.103486 * k;
                }
                else {
                    P.d1 = f.hasD1 ? f.d1 : 0.086238 * k;
                    P.d2 = f.hasD2 ? f.d2 : 0.129357 * k;
                    P.imp = f.impKnown ? f.imp : (s.mini ? 6.8 : 7.0) - P.d2;
                    P.flapHi = f.flapHiKnown ? f.flapHi : -P.d2;
                }
                for (int u = 0; u < 2; u++) P.thr[u] = sharedThr(f, s, u);
                double opt = optimistic ? 1.25 : 1.0;
                if (f.capUpKnown) P.capUp = f.capUp;
                else if (s.mode == MShip) P.capUp = optimistic ? std::max(f.maxUp, 8.0 * k) * opt : std::max(f.maxUp, 0.8 * 8.0 * k);
                else P.capUp = 1e9;
                P.capDown = f.capDownKnown ? f.capDown : std::max(f.maxDown, 6.4 * k) * opt;
                P.capUpEst = !f.capUpKnown && P.capUp < 1e8;
                P.capDownEst = !f.capDownKnown;
                break;
            }
            case MWave: {
                auto const& w = m_wave[m];
                P.slope = w.known ? w.slope : (s.mini ? 2.0 : 1.0);
                break;
            }
            default:
                return false;
        }
        if (!dxFor(s.speed, P.dx)) return false;
        P.jRef = validJ(s.yStart) ? static_cast<double>(s.yStart) : 0.0;
        P.gRef = validG(s.grav) ? static_cast<double>(s.grav) : 0.0;
        P.tolCube = lenient ? std::max(m_tolCube, 10.0) : m_tolCube;
        P.tolFly = lenient ? std::max(m_tolFly, 6.0) : m_tolFly;
        if (s.mode == MCube) {
            P.bounded = false;
            P.hasFloor = true;
            P.floorKills = s.maxY > 0.f;
            if (s.maxY > 0.f) P.maxY = s.maxY;
            if (m_floorKnown) P.floorFeet = m_floorFeet;
            else P.floorFeet = s.lo <= 90.5f ? static_cast<double>(s.lo) : 90.0;
        }
        else {
            P.bounded = true;
            P.lo = s.lo;
            P.hi = s.hi;
            P.bHalfLo = m_bHalfLoKnown[P.mk] ? m_bHalfLo[P.mk] : s.gh;
            P.bHalfHi = m_bHalfHiKnown[P.mk] ? m_bHalfHi[P.mk] : s.gh;
        }
        return true;
    }

    Effect const* padEffect(int type, int mk) const {
        if (type < 0 || type >= kTypeSlots || mk < 0 || mk >= 8) return nullptr;
        auto const& e = m_padFx[type][mk];
        return e.known ? &e : nullptr;
    }

    Effect const* orbEffect(int type, int mk) const {
        if (type < 0 || type >= kTypeSlots || mk < 0 || mk >= 8) return nullptr;
        auto const& e = m_orbFx[type][mk];
        return e.known ? &e : nullptr;
    }

    void dynamics(Phys const& P, SimState const& s, double vo, bool held, bool edge, double& vn, double& dyf, bool& jumped) const {
        int up = s.upside ? 1 : 0;
        switch (P.mode) {
            case MCube:
                if (held && s.grounded) {
                    vn = P.J * s.js;
                    jumped = true;
                }
                else if (!s.grounded) {
                    vn = vo - P.g * s.gs;
                    if (P.quant) vn = quant3(vn);
                    if (vn < -P.vmax) vn = -P.vmax;
                }
                else {
                    vn = P.walkV;
                }
                dyf = P.a * vo + P.b * vn;
                break;
            case MShip: {
                bool falling = vo < P.thr[up];
                double acc = held ? (falling ? P.a1 : P.a2) : -(falling ? P.d1 : P.d2);
                vn = vo + acc;
                if (P.quant) vn = quant3(vn);
                if ((vn >= P.capUp && P.capUpEst) || (vn <= -P.capDown && P.capDownEst)) m_capHit = true;
                vn = std::clamp(vn, -P.capDown, P.capUp);
                dyf = P.a * vo + P.b * vn;
                break;
            }
            case MUfo: {
                if (edge) {
                    vn = vo < P.imp ? P.imp : vo + P.flapHi;
                }
                else {
                    bool falling = vo < P.thr[up];
                    vn = vo - (falling ? P.d1 : P.d2);
                    if (P.quant) vn = quant3(vn);
                    if (vn <= -P.capDown) {
                        if (P.capDownEst) m_capHit = true;
                        vn = -P.capDown;
                    }
                }
                if (vn >= P.capUp) {
                    if (P.capUpEst) m_capHit = true;
                    vn = P.capUp;
                }
                dyf = P.a * vo + P.b * vn;
                break;
            }
            case MWave:
                dyf = (held ? 1.0 : -1.0) * P.slope * s.dx;
                vn = dyf / 0.225;
                break;
            default:
                vn = vo;
                dyf = 0.0;
                break;
        }
    }

    static int portalKind(int type) {
        switch (type) {
            case 5: case 6: case 16: case 19: case 26: case 27: case 33: case 41:
                return 0;
            case 3: case 4: case 42:
                return 1;
            case 17: case 18:
                return 2;
            case 14: case 15:
                return 3;
            default:
                return -1;
        }
    }

    uint8_t contacts(SimState& s, double prevY, Phys const& P, int const* cand, int nc, bool held, bool jumped, StepInfo& info, bool colMode = false) const {
        double flip = s.upside ? -1.0 : 1.0;
        bool flying = P.mode != MCube;
        bool wave = P.mode == MWave;
        bool land = !wave || slideZone(s.x - P.hw - s.dx, s.x + P.hw, std::min(prevY, s.y) - P.hh, std::max(prevY, s.y) + P.hh);
        for (int pass = 0; pass < 2; pass++) {
            bool moved = false;
            double px0 = s.x - P.hw;
            double px1 = s.x + P.hw;
            double py0 = s.y - P.hh;
            double py1 = s.y + P.hh;
            for (int c = 0; c < nc; c++) {
                MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
                if (o.kind != KSolid) continue;
                double ox0 = o.x0 - o.margin;
                double ox1 = o.x1 + o.margin;
                if (ox1 <= px0 + kEps || ox0 >= px1 - kEps) continue;
                if (o.y1 <= py0 + kEps || o.y0 >= py1 - kEps) continue;
                double topF = flip > 0 ? o.y1 : -static_cast<double>(o.y0);
                double botF = flip > 0 ? o.y0 : -static_cast<double>(o.y1);
                double prevF = prevY * flip;
                double vF = s.v * flip;
                if (land) {
                    double tol = flying ? P.tolFly : P.tolCube;
                    if (vF <= 0.0 && prevF - P.hh + tol >= topF - kEps) {
                        s.y = (topF + P.hh) * flip;
                        s.v = 0.0;
                        info.surface = cand[c];
                        moved = true;
                        break;
                    }
                    if (flying && prevF + P.hh - tol <= botF + kEps) {
                        s.y = (botF - P.hh) * flip;
                        s.v = 0.0;
                        moved = true;
                        break;
                    }
                }
                if (o.passable) continue;
                if (P.lenient) {
                    double ix0 = s.x - P.iw;
                    double ix1 = s.x + P.iw;
                    double iy0 = s.y - P.ih;
                    double iy1 = s.y + P.ih;
                    if (ox1 <= ix0 || ox0 >= ix1 || o.y1 <= iy0 || o.y0 >= iy1) continue;
                }
                m_solidHit = true;
                info.killer = cand[c];
                return RDead;
            }
            if (!moved) break;
        }
        double px0 = s.x - P.hw;
        double px1 = s.x + P.hw;
        double py0 = s.y - P.hh;
        double py1 = s.y + P.hh;
        for (int c = 0; c < nc; c++) {
            MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
            if (o.kind != KHazard) continue;
            double m = o.margin;
            if (o.r > 0.f) {
                double qx = std::clamp(static_cast<double>(o.cx), px0, px1);
                double qy = std::clamp(static_cast<double>(o.cy), py0, py1);
                double ddx = qx - o.cx;
                double ddy = qy - o.cy;
                double rr = o.r + m;
                if (ddx * ddx + ddy * ddy <= rr * rr) {
                    info.killer = cand[c];
                    return RDead;
                }
            }
            else if (hazardHit(o, m, px0, px1, py0, py1, P.touchOk)) {
                if (!P.touchOk && !hazardHit(o, m, px0, px1, py0, py1, true)) m_touchHit = true;
                info.killer = cand[c];
                return RDead;
            }
        }
        for (int c = 0; c < nc; c++) {
            int idx = cand[c];
            MapObj const& o = m_objs[static_cast<size_t>(idx)];
            if (o.kind == KSolid || o.kind == KHazard) continue;
            if (o.x1 < px0 || o.x0 > px1 || o.y1 < py0 || o.y0 > py1) continue;
            switch (o.kind) {
                case KPad: {
                    if (idx == s.lastPad) break;
                    Effect const* e = padEffect(o.type, P.mk);
                    if (!e) return RStop;
                    if (e->flips) {
                        s.upside = !s.upside;
                        s.flipAge = 0;
                    }
                    s.v = e->v * (s.upside ? -1.0 : 1.0);
                    s.lastPad = idx;
                    s.grounded = false;
                    info.event = true;
                    break;
                }
                case KOrb: {
                    if (flying || jumped || idx == s.usedOrb) break;
                    Effect const* e = orbEffect(o.type, P.mk);
                    if (!e) break;
                    if (held && s.fresh) {
                        if (e->flips) {
                            s.upside = !s.upside;
                            s.flipAge = 0;
                        }
                        s.v = orbV(*e, curJ(P, s)) * (s.upside ? -1.0 : 1.0);
                        s.usedOrb = idx;
                        s.fresh = false;
                        s.grounded = false;
                        info.event = true;
                    }
                    else {
                        info.orbTouch = true;
                    }
                    break;
                }
                case KPortal:
                    if (colMode) break;
                    if (o.obj && o.obj == s.portal) break;
                    if (o.x0 <= s.x - s.dx + P.hw + 1e-6) break;
                    return RStop;
                case KSlope:
                    if (colMode) break;
                    return RStop;
                case KSpeed: {
                    if (colMode) break;
                    if (std::fabs(o.speed - s.speed) < 0.005f) break;
                    double ndx = 0.0;
                    if (!dxFor(o.speed, ndx)) return RStop;
                    if (P.mode == MCube && P.jRef > 0.0 && P.gRef > 0.0) {
                        double nj = 0.0;
                        double ng = 0.0;
                        if (!jgFor(o.speed, nj, ng)) return RStop;
                        s.js = nj / P.jRef;
                        s.gs = ng / P.gRef;
                    }
                    s.dx = ndx;
                    s.speed = o.speed;
                    break;
                }
                default:
                    break;
            }
        }
        flip = s.upside ? -1.0 : 1.0;
        bool sup = false;
        double vF = s.v * flip;
        if (vF <= kEps) {
            double bottomF = s.y * flip - P.hh;
            if (!flying && flip > 0 && P.hasFloor && bottomF <= P.floorFeet + kEps) sup = true;
            if (!sup && P.bounded) {
                if (flip > 0 && s.y <= P.lo + P.bHalfLo + kEps) sup = true;
                if (flip < 0 && s.y >= P.hi - P.bHalfHi - kEps) sup = true;
            }
            if (!sup) {
                for (int c = 0; c < nc; c++) {
                    MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
                    if (o.kind != KSolid) continue;
                    if (o.x1 + o.margin <= s.x - P.hw + kEps || o.x0 - o.margin >= s.x + P.hw - kEps) continue;
                    double topF = flip > 0 ? o.y1 : -static_cast<double>(o.y0);
                    if (std::fabs(bottomF - topF) <= kEps) {
                        sup = true;
                        info.surface = cand[c];
                        break;
                    }
                }
            }
        }
        s.grounded = sup;
        if (sup && !flying) s.v = 0.0;
        return RAlive;
    }

    bool applyBounds(SimState& s, Phys const& P) const {
        if (P.bounded) {
            double lo = P.lo + P.bHalfLo;
            double hi = P.hi - P.bHalfHi;
            if (s.y < lo) {
                s.y = lo;
                if (s.v < 0.0) s.v = 0.0;
            }
            if (s.y > hi && hi > lo) {
                s.y = hi;
                if (s.v > 0.0) s.v = 0.0;
            }
        }
        else if (P.hasFloor) {
            double lo = P.floorFeet + P.hh;
            if (s.y < lo) {
                if (P.floorKills && s.upside && s.flipAge >= kSafeFlipTicks) return false;
                s.y = lo;
                if (s.v < 0.0) s.v = 0.0;
            }
        }
        return !(s.y > P.maxY);
    }

    int firePreOrb(SimState& s, Phys const& P, int const* cand, int nc) const {
        double px0 = s.x - P.hw;
        double px1 = s.x + P.hw;
        double py0 = s.y - P.hh;
        double py1 = s.y + P.hh;
        for (int c = 0; c < nc; c++) {
            int idx = cand[c];
            MapObj const& o = m_objs[static_cast<size_t>(idx)];
            if (o.kind != KOrb || idx == s.usedOrb) continue;
            if (o.x1 < px0 || o.x0 > px1 || o.y1 < py0 || o.y0 > py1) continue;
            Effect const* e = orbEffect(o.type, P.mk);
            if (!e) return -1;
            if (e->flips) {
                s.upside = !s.upside;
                s.flipAge = 0;
            }
            s.v = orbV(*e, curJ(P, s)) * (s.upside ? -1.0 : 1.0);
            s.usedOrb = idx;
            s.fresh = false;
            s.grounded = false;
            return 1;
        }
        return 0;
    }

    bool orbOver(SimState const& s, Phys const& P) const {
        int cand[kMaxCand];
        bool over = false;
        int nc = gather(s.x - P.hw, s.x + P.hw, cand, kMaxCand, &over, s.y - P.hh - 1.0, s.y + P.hh + 1.0);
        if (over) return true;
        for (int c = 0; c < nc; c++) {
            int idx = cand[c];
            MapObj const& o = m_objs[static_cast<size_t>(idx)];
            if (o.kind != KOrb || idx == s.usedOrb || !orbEffect(o.type, P.mk)) continue;
            if (o.x1 < s.x - P.hw || o.x0 > s.x + P.hw || o.y1 < s.y - P.hh || o.y0 > s.y + P.hh) continue;
            return true;
        }
        return false;
    }

    uint8_t step(SimState& s, bool held, Phys const& P, StepInfo& info) const {
        return stepImpl(s, held, P, info);
    }

    uint8_t stepImpl(SimState& s, bool held, Phys const& P, StepInfo& info) const {
        info = StepInfo{};
        double flip = s.upside ? -1.0 : 1.0;
        bool flying = P.mode != MCube;
        bool edge = held && !s.held;
        if (edge) {
            s.fresh = true;
            s.usedOrb = -1;
        }
        if (!held) s.fresh = false;
        if (held != s.held) s.since = 1;
        else if (s.since < kSinceCap) s.since++;
        if (s.flipAge < kSinceCap) s.flipAge++;
        s.held = held;
        if (flying && edge) {
            int cand[kMaxCand];
            bool over = false;
            int nc = gather(s.x - P.hw, s.x + P.hw, cand, kMaxCand, &over, s.y - P.hh - 1.0, s.y + P.hh + 1.0);
            if (over) return RStop;
            for (int c = 0; c < nc; c++) {
                MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
                if (o.kind != KOrb) continue;
                if (o.x1 < s.x - P.hw || o.x0 > s.x + P.hw || o.y1 < s.y - P.hh || o.y0 > s.y + P.hh) continue;
                return RStop;
            }
        }
        if (!flying && edge && orbTiming() == 1) {
            int cand[kMaxCand];
            bool over = false;
            int nc = gather(s.x - P.hw, s.x + P.hw, cand, kMaxCand, &over, s.y - P.hh - 1.0, s.y + P.hh + 1.0);
            if (over) return RStop;
            int fired = firePreOrb(s, P, cand, nc);
            if (fired < 0) return RStop;
            if (fired > 0) {
                info.event = true;
                flip = s.upside ? -1.0 : 1.0;
            }
        }
        double vo = s.v * flip;
        double vn = vo;
        double dyf = 0.0;
        bool jumped = false;
        dynamics(P, s, vo, held, edge, vn, dyf, jumped);
        if (jumped) {
            s.grounded = false;
            s.fresh = false;
        }
        double prevY = s.y;
        s.x += s.dx;
        s.y += dyf * flip;
        s.v = vn * flip;
        if (!applyBounds(s, P)) return RDead;
        if (!(std::fabs(s.y) < 1e5)) return RDead;
        int cand[kMaxCand];
        bool over = false;
        double ylo = std::min(prevY, s.y) - P.hh - kYPad;
        double yhi = std::max(prevY, s.y) + P.hh + kYPad;
        int nc = gather(s.x - P.hw - 1.0, s.x + P.hw + 1.0, cand, kMaxCand, &over, ylo, yhi);
        if (over) return RStop;
        return contacts(s, prevY, P, cand, nc, held, jumped, info);
    }

    bool canFlip(int tick, int fam, bool prevDown, int since) const {
        int res = std::max(1, m_limits.inputResolution);
        if (res > 1 && tick % res != 0) return false;
        if (!m_limits.active) return true;
        int f = std::clamp(fam, 0, 2);
        int req = prevDown ? m_limits.minHold[f] : m_limits.minRelease[f];
        if (req > 1 && since < req) return false;
        return true;
    }

    int pressLength(int tick) const {
        int len = 1;
        if (m_limits.active) len = std::max(1, m_limits.minHold[2]);
        int res = std::max(1, m_limits.inputResolution);
        int guard = 0;
        while (res > 1 && (tick + len) % res != 0 && guard++ < 16) len++;
        return len;
    }

    int limitCap() const {
        int cap = 2;
        for (int i = 0; i < 3; i++) cap = std::max({cap, m_limits.minHold[i], m_limits.minRelease[i]});
        return std::min(cap + 1, 2402);
    }

    int sinceFromPath(std::vector<uint8_t> const& path, int tick) const {
        int cap = limitCap();
        auto down = [&](int k) {
            return k >= 0 && k < static_cast<int>(path.size()) && (path[static_cast<size_t>(k)] & 1) != 0;
        };
        for (int s = 1; s < cap; s++) {
            int at = tick - s;
            if (at < 0) return kSinceCap;
            if (down(at) != down(at - 1)) return s;
        }
        return kSinceCap;
    }

    bool traceHeld(int k, bool& v) const {
        if (k < 0) {
            v = false;
            return true;
        }
        if (k >= static_cast<int>(m_trace.size())) return false;
        auto const& e = m_trace[static_cast<size_t>(k)];
        if (!e.heldValid) return false;
        v = (e.held & 1) != 0;
        return true;
    }

    int sinceFromTrace(int tick) const {
        int cap = limitCap();
        for (int s = 1; s < cap; s++) {
            int at = tick - s;
            if (at < 0) return kSinceCap;
            bool a = false;
            bool b = false;
            if (!traceHeld(at, a) || !traceHeld(at - 1, b)) return kSinceCap;
            if (a != b) return s;
        }
        return kSinceCap;
    }

    SimState simFrom(PS const& ps, double dx, bool prevHeld, int since, int lastPad, int usedOrb) const {
        SimState s;
        s.x = ps.x;
        s.y = ps.y;
        s.v = ps.v;
        s.dx = dx;
        s.speed = ps.speed;
        s.portal = ps.portal;
        s.lastPad = lastPad;
        s.usedOrb = usedOrb;
        s.since = since;
        s.flipAge = ps.flipAge;
        s.held = prevHeld;
        s.grounded = ps.grounded;
        s.upside = ps.upside;
        s.fresh = ps.fresh;
        s.mode = ps.mode;
        return s;
    }

    void rollout(SimState const& s0, int t, int hend, Phys const& P, RollBuf& buf) {
        buf.st.clear();
        buf.orb.clear();
        buf.st.push_back(s0);
        SimState cur = s0;
        buf.result = RAlive;
        buf.endTick = hend;
        for (int tau = t; tau < hend; tau++) {
            m_work++;
            if (--m_budget < 0) {
                buf.result = RDead;
                buf.endTick = tau;
                buf.orb.push_back(0);
                return;
            }
            StepInfo info;
            uint8_t r = step(cur, false, P, info);
            buf.orb.push_back(info.orbTouch ? 1 : 0);
            if (r == RDead) {
                buf.result = RDead;
                buf.endTick = tau;
                return;
            }
            if (r == RStop) {
                buf.result = RStop;
                buf.endTick = tau;
                return;
            }
            buf.st.push_back(cur);
        }
    }

    Arc runArc(SimState const& s0, int k, int len, int hend, Phys const& P, std::vector<SimState>* trail) {
        Arc a;
        SimState cur = s0;
        bool wasGround = cur.grounded;
        for (int tau = k; tau < hend; tau++) {
            m_work++;
            if (--m_budget < 0) {
                a.kind = AKDead;
                a.end = tau;
                return a;
            }
            if (trail) trail->push_back(cur);
            StepInfo info;
            bool h = tau < k + len;
            uint8_t r = step(cur, h, P, info);
            if (r == RDead) {
                a.kind = AKDead;
                a.end = tau;
                return a;
            }
            if (r == RStop) {
                a.kind = AKOpen;
                a.end = tau + 1;
                a.land = cur;
                return a;
            }
            if (tau >= k + len - 1 && cur.grounded && !wasGround && !h) {
                a.kind = AKLanded;
                a.end = tau + 1;
                a.land = cur;
                return a;
            }
            if (tau >= k + len - 1 && cur.grounded && !wasGround && h) {
                a.kind = AKLanded;
                a.end = tau + 1;
                a.land = cur;
                return a;
            }
            wasGround = cur.grounded;
        }
        a.kind = AKOpen;
        a.end = hend;
        a.land = cur;
        return a;
    }

    bool pressable(RollBuf const& buf, int t, int tau, Phys const& P) const {
        int i = tau - t;
        if (i < 0 || i >= static_cast<int>(buf.st.size()) || i >= static_cast<int>(buf.orb.size())) return false;
        SimState const& s = buf.st[static_cast<size_t>(i)];
        if (s.held) return false;
        if (!s.grounded) {
            int timing = orbTiming();
            bool now = buf.orb[static_cast<size_t>(i)] != 0;
            if (timing == 0) {
                if (!now) return false;
            }
            else {
                bool before = i > 0 ? buf.orb[static_cast<size_t>(i) - 1] != 0 : orbOver(s, P);
                if (timing == 1 ? !(now || before) : !(now && !before)) return false;
            }
        }
        return canFlip(tau, 2, false, s.since);
    }

    static long long landKey(Arc const& a) {
        if (a.kind == AKDead) return std::numeric_limits<long long>::min();
        if (a.kind == AKOpen) return std::numeric_limits<long long>::max();
        return std::llround(a.land.y * 2.0);
    }

    static uint64_t simKey(SimState const& s, int t) {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(t)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(std::llround(s.y * 4.0)) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<uint64_t>(std::llround(s.v * 64.0)) * 0x165667B19E3779F9ull;
        uint64_t f = (s.grounded ? 1u : 0u) | (s.upside ? 2u : 0u) | (s.fresh ? 4u : 0u) | (s.held ? 8u : 0u);
        f |= static_cast<uint64_t>(static_cast<uint32_t>(s.lastPad + 1) & 0xFFFFFu) << 4;
        f |= static_cast<uint64_t>(static_cast<uint32_t>(s.usedOrb + 1) & 0xFFFFFu) << 24;
        if (s.flipAge < kSafeFlipTicks) f |= static_cast<uint64_t>(s.flipAge + 1) << 44;
        h ^= (f + 0x9E3779B97F4A7C15ull) * 0xD6E8FEB86659FD93ull;
        return h ^ (h >> 31);
    }

    Arc arcAt(SimState const& s, int tau, int len, Phys const& P) {
        uint64_t key = simKey(s, tau) ^ (static_cast<uint64_t>(len) * 0xA24BAED4963EE407ull);
        auto it = m_arcMemo.find(key);
        if (it != m_arcMemo.end()) return it->second;
        Arc a = runArc(s, tau, len, m_horizonEnd, P, nullptr);
        if (m_budget >= 0 && m_arcMemo.size() < 65536) m_arcMemo.emplace(key, a);
        return a;
    }

    static int walkEndOf(RollBuf const& buf, int t) {
        if (buf.st.empty()) return t;
        SimState const& s0 = buf.st.front();
        int we = t;
        if (!s0.grounded) return we;
        for (size_t i = 1; i < buf.st.size(); i++) {
            SimState const& s = buf.st[i];
            if (!s.grounded || s.upside != s0.upside || std::fabs(s.y - s0.y) > 0.01) break;
            we = t + static_cast<int>(i);
        }
        return we;
    }

    void probeArcs(RollBuf const& buf, int t, CubeScratch& sc, Phys const& P) {
        size_t n = sc.ticks.size();
        sc.arcs.assign(n, Arc{});
        sc.done.assign(n, 0);
        sc.vals.assign(n, std::numeric_limits<int>::min());
        if (n == 0) return;
        int res = std::max(1, m_limits.inputResolution);
        auto evalAt = [&](size_t i) {
            if (sc.done[i]) return;
            int tau = sc.ticks[i];
            size_t si = static_cast<size_t>(tau - t);
            if (si >= buf.st.size()) return;
            sc.arcs[i] = arcAt(buf.st[si], tau, pressLength(tau), P);
            sc.done[i] = 1;
        };
        size_t stride = n > 24 ? 4 : (n > 12 ? 2 : 1);
        size_t prev = 0;
        evalAt(0);
        if (n == 1) return;
        for (size_t i = stride;; i += stride) {
            size_t cur = std::min(i, n - 1);
            evalAt(cur);
            bool refine = !sc.done[prev] || !sc.done[cur] || landKey(sc.arcs[prev]) != landKey(sc.arcs[cur]);
            for (size_t q = prev + 1; q <= cur && !refine; q++) {
                if (sc.ticks[q] - sc.ticks[q - 1] > res) refine = true;
            }
            if (refine) {
                for (size_t q = prev + 1; q < cur; q++) evalAt(q);
            }
            prev = cur;
            if (cur == n - 1) break;
        }
    }

    int survive(SimState const& s, int t, int depth, Phys const& P, int& walkEnd) {
        walkEnd = t;
        if (t >= m_horizonEnd) return m_horizonEnd;
        uint64_t key = simKey(s, t);
        auto it = m_cmemo.find(key);
        if (it != m_cmemo.end() && (it->second.value >= m_horizonEnd || it->second.depth <= depth)) {
            walkEnd = it->second.walkEnd;
            return it->second.value;
        }
        auto store = [&](int value, int we) {
            if (m_cmemo.size() >= 32768) return;
            CubeVal cv;
            cv.value = value;
            cv.walkEnd = we;
            cv.depth = depth;
            m_cmemo[key] = cv;
        };
        size_t bi = static_cast<size_t>(std::clamp(depth, 1, static_cast<int>(m_roll.size()) - 1));
        RollBuf& buf = m_roll[bi];
        rollout(s, t, m_horizonEnd, P, buf);
        int we = walkEndOf(buf, t);
        walkEnd = we;
        if (m_budget < 0) return buf.endTick;
        if (buf.result != RDead) {
            store(m_horizonEnd, we);
            return m_horizonEnd;
        }
        int d = buf.endTick;
        int best = d;
        if (depth >= kCubeDepth || m_budget <= 0 || bi >= m_cubeScratch.size()) {
            store(best, we);
            return best;
        }
        CubeScratch& sc = m_cubeScratch[bi];
        sc.ticks.clear();
        for (int tau = t; tau <= d; tau++) {
            if (pressable(buf, t, tau, P)) sc.ticks.push_back(tau);
        }
        if (sc.ticks.empty()) {
            store(best, we);
            return best;
        }
        Explored ex[12];
        int nex = 0;
        auto explore = [&](Arc const& a) -> int {
            for (int e = 0; e < nex; e++) {
                Explored const& q = ex[e];
                if (q.upside == a.land.upside && q.held == a.land.held && std::fabs(q.y - a.land.y) <= 0.01 && q.land <= a.end && a.end <= q.walkEnd) return -1;
            }
            if (nex >= 12 || m_budget <= 0) return -1;
            int w2 = a.end;
            int v = survive(a.land, a.end, depth + 1, P, w2);
            Explored& q = ex[nex++];
            q.y = a.land.y;
            q.land = a.end;
            q.walkEnd = std::max(w2, a.end);
            q.upside = a.land.upside;
            q.held = a.land.held;
            return v;
        };
        for (size_t r = sc.ticks.size(); r-- > 0;) {
            if (m_budget <= 0) break;
            int tau = sc.ticks[r];
            Arc a = arcAt(buf.st[static_cast<size_t>(tau - t)], tau, pressLength(tau), P);
            if (a.kind == AKOpen) {
                store(m_horizonEnd, we);
                return m_horizonEnd;
            }
            if (a.kind == AKDead) {
                best = std::max(best, a.end);
                if (sc.ticks.size() - r > 24) break;
                continue;
            }
            int v = explore(a);
            if (v >= m_horizonEnd) {
                store(m_horizonEnd, we);
                return m_horizonEnd;
            }
            best = std::max(best, v);
            break;
        }
        if (m_budget <= 0) return best;
        probeArcs(buf, t, sc, P);
        sc.order.clear();
        for (size_t i = 0; i < sc.ticks.size(); i++) {
            if (!sc.done[i]) continue;
            Arc const& a = sc.arcs[i];
            if (a.kind == AKOpen) {
                store(m_horizonEnd, we);
                return m_horizonEnd;
            }
            if (a.kind == AKDead) {
                best = std::max(best, a.end);
                continue;
            }
            sc.order.push_back(static_cast<int>(i));
        }
        std::stable_sort(sc.order.begin(), sc.order.end(), [&](int a, int b) { return sc.arcs[static_cast<size_t>(a)].end < sc.arcs[static_cast<size_t>(b)].end; });
        for (size_t oi = 0; oi < sc.order.size(); oi++) {
            if (m_budget <= 0) break;
            Arc const a = sc.arcs[static_cast<size_t>(sc.order[oi])];
            int v = explore(a);
            if (v < 0) continue;
            best = std::max(best, v);
            if (v >= m_horizonEnd) {
                store(m_horizonEnd, we);
                return m_horizonEnd;
            }
        }
        if (m_budget >= 0) store(best, we);
        return best;
    }

    int arcEstimate(Phys const& P) const {
        if (!(P.g > 0.0)) return 200;
        double t = 2.0 * P.J / P.g + 30.0;
        return std::clamp(static_cast<int>(t), 40, 360);
    }

    struct CubeChoice {
        bool press = false;
        bool clears = false;
        int k = -1;
        int first = -1;
        int goodLast = -1;
        int len = 1;
        int value = -1;
        int width = 0;
        Arc arc;
    };

    bool betterChoice(CubeChoice const& a, CubeChoice const& b) const {
        if (!b.press) return a.press;
        if (a.value != b.value) return a.value > b.value;
        if (a.value >= m_horizonEnd && a.clears != b.clears) {
            CubeChoice const& c = a.clears ? a : b;
            CubeChoice const& o = a.clears ? b : a;
            bool clearWins = c.width >= std::min(kClearWidth, o.width);
            return clearWins ? a.clears : !a.clears;
        }
        return a.width > b.width;
    }

    bool cubeChoose(SimState const& s0, int t, Phys const& P, CubeChoice& out, int& deathTick, int budget) {
        m_cmemo.clear();
        m_arcMemo.clear();
        m_budget = budget;
        m_horizonEnd = t + kCubeLook;
        m_maxArc = arcEstimate(P);
        RollBuf& buf = m_roll[0];
        rollout(s0, t, m_horizonEnd, P, buf);
        deathTick = -1;
        if (buf.result != RDead) {
            out.press = false;
            out.value = m_horizonEnd;
            return true;
        }
        int d = buf.endTick;
        deathTick = d;
        CubeScratch& sc = m_cubeScratch[0];
        int res = std::max(1, m_limits.inputResolution);
        auto evalAt = [&](size_t i) {
            if (sc.done[i]) return;
            int tau = sc.ticks[i];
            sc.arcs[i] = arcAt(buf.st[static_cast<size_t>(tau - t)], tau, pressLength(tau), P);
            sc.done[i] = 1;
        };
        auto valueAt = [&](size_t i) -> int {
            evalAt(i);
            if (sc.vals[i] != std::numeric_limits<int>::min()) return sc.vals[i];
            Arc const& a = sc.arcs[i];
            int v = a.end;
            if (a.kind == AKOpen) v = m_horizonEnd;
            else if (a.kind == AKLanded) {
                int we = 0;
                v = survive(a.land, a.end, 1, P, we);
            }
            sc.vals[i] = v;
            return v;
        };
        auto clearsOf = [&](Arc const& a) {
            return a.kind == AKOpen || (a.kind == AKLanded && a.end > d);
        };
        CubeChoice best;
        for (int phase = 0; phase < 2; phase++) {
            int lo = std::max(t, d - m_maxArc);
            int from = phase == 0 ? lo : t;
            int to = phase == 0 ? d : lo - 1;
            sc.ticks.clear();
            for (int tau = from; tau <= to; tau++) {
                if (pressable(buf, t, tau, P)) sc.ticks.push_back(tau);
            }
            if (sc.ticks.empty()) continue;
            probeArcs(buf, t, sc, P);
            size_t n = sc.ticks.size();
            size_t i = 0;
            while (i < n) {
                if (!sc.done[i] || sc.arcs[i].kind == AKDead) {
                    i++;
                    continue;
                }
                long long key = landKey(sc.arcs[i]);
                bool clr = clearsOf(sc.arcs[i]);
                auto same = [&](size_t q) { return landKey(sc.arcs[q]) == key && clearsOf(sc.arcs[q]) == clr; };
                size_t j = i;
                while (j + 1 < n) {
                    size_t nx = j + 1;
                    if (sc.ticks[nx] - sc.ticks[j] > res) break;
                    if (sc.done[nx] && !same(nx)) break;
                    if (!sc.done[nx]) {
                        size_t q = nx;
                        while (q < n && !sc.done[q]) q++;
                        if (q >= n || !same(q)) break;
                    }
                    j = nx;
                }
                CubeChoice c;
                c.press = true;
                c.clears = clr;
                int vi = valueAt(i);
                size_t pick = i;
                if (vi >= m_horizonEnd) {
                    size_t good = i;
                    evalAt(j);
                    if (same(j) && valueAt(j) >= m_horizonEnd) good = j;
                    else {
                        size_t a = i;
                        size_t b = j;
                        while (b - a > 1 && m_budget > 0) {
                            size_t m = (a + b) / 2;
                            evalAt(m);
                            if (same(m) && valueAt(m) >= m_horizonEnd) a = m;
                            else b = m;
                        }
                        good = a;
                    }
                    pick = (i + good) / 2;
                    evalAt(pick);
                    if (!same(pick) || valueAt(pick) < m_horizonEnd) pick = i;
                    c.value = m_horizonEnd;
                    c.width = static_cast<int>(good - i + 1);
                    c.goodLast = sc.ticks[good];
                }
                else {
                    size_t mid = (i + j) / 2;
                    evalAt(mid);
                    int v = vi;
                    if (same(mid) && valueAt(mid) > v) {
                        v = valueAt(mid);
                        pick = mid;
                    }
                    evalAt(j);
                    if (same(j) && valueAt(j) > v) {
                        v = valueAt(j);
                        pick = j;
                    }
                    c.value = v;
                    c.width = static_cast<int>(j - i + 1);
                }
                c.k = sc.ticks[pick];
                c.first = sc.ticks[i];
                c.len = pressLength(c.k);
                c.arc = sc.arcs[pick];
                if (betterChoice(c, best)) best = c;
                i = j + 1;
            }
            if (best.press && best.value >= m_horizonEnd) break;
        }
        if (!best.press) {
            out.press = false;
            out.value = d;
            return false;
        }
        out = best;
        if (best.value >= m_horizonEnd) {
            deathTick = -1;
            return true;
        }
        deathTick = best.value;
        return best.value - t >= kCubeHorizon / 2;
    }

    int joinLength(Arc const& a, int k, Phys const& P) {
        if (a.kind != AKLanded || !a.land.grounded || a.land.held) return 0;
        int land = a.end;
        SimState ls = a.land;
        CubeChoice c2;
        int dt2 = -1;
        if (!cubeChoose(ls, land, P, c2, dt2, kCubeAdviseBudget / 2) || !c2.press || c2.value < m_horizonEnd) return 0;
        if (c2.first != land || c2.goodLast - land < kJoinSlack) return 0;
        long long key = landKey(c2.arc);
        for (int back = 1; back <= 2; back++) {
            SimState v = ls;
            v.x -= v.dx * back;
            Arc va = runArc(v, land - back, pressLength(land), m_horizonEnd, P, nullptr);
            if (m_budget < 0) return 0;
            if (va.kind == AKDead || (va.kind == AKLanded && landKey(va) != key)) return 0;
        }
        return land - k + pressLength(land);
    }

    bool cubePlan(int t, SimState const& s0, Phys const& P, Plan& plan, int& deathTick) {
        CubeChoice ch;
        bool ok = cubeChoose(s0, t, P, ch, deathTick, m_workCap > 0 ? kCubeAdviseBudget : kCubeBudget);
        plan = Plan{};
        plan.mode = MCube;
        plan.t0 = t;
        RollBuf const& buf = m_roll[0];
        if (!ch.press) {
            int end = t + 1;
            if (buf.result == RStop) end = buf.endTick + 1;
            else if (buf.result == RAlive) end = t + kCubeHorizon / 2;
            else end = std::max(t + 1, buf.endTick + 1);
            int n = std::max(1, end - t);
            for (int i = 0; i < n && i < static_cast<int>(buf.st.size()); i++) {
                plan.held.push_back(0);
                plan.pred.push_back(buf.st[static_cast<size_t>(i)]);
            }
            if (plan.held.empty()) {
                plan.held.push_back(0);
                plan.pred.push_back(s0);
            }
            plan.valid = true;
            plan.failed = !ok && deathTick >= 0;
            plan.replanAt = plan.end();
            return ok;
        }
        int k = ch.k;
        for (int tau = t; tau < k; tau++) {
            size_t i = static_cast<size_t>(tau - t);
            if (i >= buf.st.size()) break;
            plan.held.push_back(0);
            plan.pred.push_back(buf.st[i]);
        }
        if (static_cast<int>(plan.held.size()) != k - t) {
            plan.valid = false;
            return false;
        }
        SimState ks = buf.st[static_cast<size_t>(k - t)];
        std::vector<SimState> trail;
        int saved = m_budget;
        m_budget = 100000;
        Arc a = runArc(ks, k, ch.len, m_horizonEnd, P, &trail);
        m_budget = saved;
        int joinLen = ok && deathTick < 0 && m_workCap > 0 ? joinLength(a, k, P) : 0;
        if (joinLen > ch.len) {
            std::vector<SimState> trail2;
            saved = m_budget;
            m_budget = 100000;
            Arc a2 = runArc(ks, k, joinLen, m_horizonEnd, P, &trail2);
            m_budget = saved;
            if (a2.kind != AKDead) {
                a = a2;
                trail.swap(trail2);
                ch.len = joinLen;
            }
        }
        int end = a.kind == AKDead ? a.end + 1 : a.end;
        for (int tau = k; tau < end; tau++) {
            size_t i = static_cast<size_t>(tau - k);
            if (i >= trail.size()) break;
            plan.held.push_back(tau < k + ch.len ? 1 : 0);
            plan.pred.push_back(trail[i]);
        }
        plan.valid = !plan.held.empty();
        plan.failed = !ok;
        plan.replanAt = k - t > kCubeReplan + 30 ? t + kCubeReplan : plan.end();
        return ok;
    }

    PortalFx const* fxFor(GameObject* obj, int type, uint8_t mode) const {
        auto it = m_portalFx.find(obj);
        if (it != m_portalFx.end()) return it->second.known ? &it->second : nullptr;
        int kind = portalKind(type);
        if (kind < 0 || mode >= 16) return nullptr;
        auto usable = [&](TypeFx const& tf) {
            if (tf.bad || tf.n < 1 || !tf.fx.known) return false;
            return kind == 1 || kind == 3 || (kind == 0 && tf.fx.mode == MCube) || (kind == 2 && tf.fx.mode == mode);
        };
        auto jt = m_typeFx.find(type * 16 + mode);
        if (jt != m_typeFx.end()) return usable(jt->second) ? &jt->second.fx : nullptr;
        if (kind != 0) return nullptr;
        for (int m = 0; m < 8; m++) {
            auto kt = m_typeFx.find(type * 16 + m);
            if (kt != m_typeFx.end() && usable(kt->second)) return &kt->second.fx;
        }
        return nullptr;
    }

    void notePortalType(GameObject* portal, uint8_t preMode) {
        auto it = m_portalFx.find(portal);
        auto ti = m_portalType.find(portal);
        if (it == m_portalFx.end() || ti == m_portalType.end() || preMode >= 16) return;
        int key = ti->second * 16 + preMode;
        if (m_typeFx.size() >= 512 && !m_typeFx.count(key)) return;
        TypeFx& tf = m_typeFx[key];
        PortalFx const& f = it->second;
        if (tf.n == 0) {
            tf.fx = f;
            tf.n = 1;
            tf.bad = !f.known;
            return;
        }
        bool same = tf.fx.kind == f.kind && tf.fx.mode == f.mode && tf.fx.mini == f.mini && tf.fx.known == f.known && tf.fx.hw == f.hw && tf.fx.hh == f.hh;
        if (!same) tf.bad = true;
        if (f.vKnown) {
            if (tf.fx.vKnown && tf.fx.vAbs >= 2.0 && f.vAbs >= 2.0 && std::fabs(tf.fx.vFactor - f.vFactor) > 0.03) tf.bad = true;
            if (!tf.fx.vKnown || f.vAbs > tf.fx.vAbs) {
                tf.fx.vFactor = f.vFactor;
                tf.fx.vAbs = f.vAbs;
            }
            tf.fx.vKnown = true;
        }
        if (tf.n < 1000) tf.n++;
    }

    double vFactorFor(int type, uint8_t mode, PortalFx const& fx) const {
        if (mode >= 16) return fx.vFactor;
        auto jt = m_typeFx.find(type * 16 + mode);
        if (jt == m_typeFx.end() || jt->second.bad || !jt->second.fx.vKnown) return fx.vFactor;
        if (!fx.vKnown || jt->second.fx.vAbs > fx.vAbs) return jt->second.fx.vFactor;
        return fx.vFactor;
    }

    bool switchFor(GameObject* obj, int type, int from, ColSwitch& out) {
        if (from < 0 || from >= static_cast<int>(m_bPs.size())) return false;
        PS q = m_bPs[static_cast<size_t>(from)];
        PortalFx const* fp = fxFor(obj, type, q.mode);
        PortalFx prior;
        if (!fp) {
            if (type != 6 && type != 3 && type != 4) return false;
            prior.kind = type == 6 ? 0 : 1;
            prior.mode = type == 6 ? MCube : q.mode;
            prior.mini = q.mini;
            prior.upside = q.upside;
            prior.lo = q.lo;
            prior.hi = q.hi;
            prior.hw = q.hw;
            prior.hh = q.hh;
            prior.gh = q.gh;
            prior.vFactor = type == 6 ? 1.0 : 0.5;
            prior.known = true;
            fp = &prior;
        }
        PortalFx const& fx = *fp;
        double vf = vFactorFor(type, q.mode, fx);
        out = ColSwitch{};
        out.portal = obj;
        switch (fx.kind) {
            case 0: {
                double sc = (q.mini ? 0.6 : 1.0) / (fx.mini ? 0.6 : 1.0);
                q.mode = fx.mode;
                q.lo = fx.lo;
                q.hi = fx.hi;
                q.hw = static_cast<float>(fx.hw * sc);
                q.hh = static_cast<float>(fx.hh * sc);
                q.gh = static_cast<float>(fx.gh * sc);
                out.modeSwitch = true;
                out.vFactor = vf;
                break;
            }
            case 1:
                out.upRule = static_cast<int8_t>(type == 3 ? 1 : (type == 4 ? 2 : 3));
                out.vFactor = vf;
                break;
            case 2:
                if (fx.mode != q.mode) return false;
                q.mini = fx.mini;
                q.hw = fx.hw;
                q.hh = fx.hh;
                q.gh = fx.gh;
                break;
            case 3:
                break;
            default:
                return false;
        }
        for (size_t i = 0; i < m_bPs.size(); i++) {
            PS const& r = m_bPs[i];
            if (r.mode == q.mode && r.mini == q.mini && r.lo == q.lo && r.hi == q.hi && r.hw == q.hw && r.hh == q.hh && r.gh == q.gh && r.speed == q.speed &&
                r.yStart == q.yStart && r.grav == q.grav) {
                out.to = static_cast<int>(i);
                return true;
            }
        }
        if (m_bPhys.size() >= 8) return false;
        Phys P;
        if (!resolve(q, P, false, m_bNeed, m_bOptimistic)) return false;
        m_bPhys.push_back(P);
        m_bPs.push_back(q);
        out.to = static_cast<int>(m_bPhys.size()) - 1;
        return true;
    }

    void buildColumns(int t, int hmax, SimState const& s0, PS const& ps0, Phys const& P0, int& horizon) {
        (void)t;
        m_colStart.clear();
        m_colIdx.clear();
        m_colPhys.clear();
        m_colSwStart.clear();
        m_switches.clear();
        m_bPhys.clear();
        m_bPs.clear();
        m_colX.clear();
        m_colDx.clear();
        m_colSpeed.clear();
        m_gates.clear();
        m_stopCol = false;
        m_bPhys.push_back(P0);
        m_bPs.push_back(ps0);
        horizon = hmax;
        int cur = 0;
        double x = s0.x;
        double dxc = P0.dx;
        float spd = s0.speed;
        m_colX.push_back(x);
        for (int j = -1; j < hmax; j++) {
            Phys const P = m_bPhys[static_cast<size_t>(cur)];
            double xprev = x - P0.dx;
            if (j >= 0) {
                xprev = x;
                m_colDx.push_back(dxc);
                m_colSpeed.push_back(spd);
                x += dxc;
                m_colX.push_back(x);
            }
            double xp = x;
            m_colStart.push_back(static_cast<int>(m_colIdx.size()));
            if (j >= 0) {
                m_colPhys.push_back(static_cast<uint8_t>(cur));
                m_colSwStart.push_back(static_cast<int>(m_switches.size()));
            }
            bool over = false;
            double gy0 = P.bounded ? P.lo - kYPad : -1e18;
            double gy1 = P.bounded ? P.hi + kYPad : 1e18;
            int n = gather(xp - P.hw - 1.0, xp + P.hw + 1.0, m_gbuf.data(), kMaxColCand, &over, gy0, gy1);
            bool stop = over;
            bool hard = over;
            int portals[4];
            int np = 0;
            int gate = -1;
            int pgate = -1;
            double gateLo = 0.0;
            double gateHi = 0.0;
            for (int c = 0; c < n; c++) {
                int ci = m_gbuf[static_cast<size_t>(c)];
                MapObj const& o = m_objs[static_cast<size_t>(ci)];
                m_colIdx.push_back(ci);
                if (o.x1 < xp - P.hw || o.x0 > xp + P.hw) continue;
                if (o.kind == KPortal && !(o.obj && o.obj == s0.portal) && o.x0 > xprev + P.hw + 1e-6) {
                    if (np < 4) portals[np++] = ci;
                    else stop = true;
                }
                if (o.kind == KSlope) {
                    stop = true;
                    hard = true;
                }
                if (o.kind == KSpeed && std::fabs(o.speed - spd) >= 0.005f && o.x0 > s0.x + P0.hw + 1e-6) {
                    if (o.x0 > xprev + P.hw + 1e-6 && gate < 0) {
                        gate = ci;
                        gateLo = o.y0;
                        gateHi = o.y1;
                    }
                    else if (gate >= 0 && o.x0 > xprev + P.hw + 1e-6 && std::fabs(o.speed - m_objs[static_cast<size_t>(gate)].speed) < 0.005f) {
                        gateLo = std::min(gateLo, static_cast<double>(o.y0));
                        gateHi = std::max(gateHi, static_cast<double>(o.y1));
                    }
                    else {
                        stop = true;
                    }
                }
            }
            if (j >= 0 && !stop && np > 0) {
                std::sort(portals, portals + np, [&](int a, int b) { return m_objs[static_cast<size_t>(a)].x0 < m_objs[static_cast<size_t>(b)].x0; });
                for (int k = 0; k < np && !stop; k++) {
                    MapObj const& o = m_objs[static_cast<size_t>(portals[k])];
                    ColSwitch sw;
                    if (!switchFor(o.obj, o.type, cur, sw)) {
                        if (m_bPhys.size() < 8 && !fxFor(o.obj, o.type, m_bPs[static_cast<size_t>(cur)].mode)) pgate = portals[k];
                        stop = true;
                        break;
                    }
                    m_switches.push_back(sw);
                    cur = sw.to;
                    if (m_gates.size() < kMaxGates) {
                        Gate g;
                        g.col = j + 1;
                        g.lo = o.y0 - P.hh + 1.0;
                        g.hi = o.y1 + P.hh - 1.0;
                        m_gates.push_back(g);
                    }
                }
            }
            if (j >= 0 && !stop && gate >= 0) {
                MapObj const& o = m_objs[static_cast<size_t>(gate)];
                double ndx = 0.0;
                double nj = 0.0;
                double ng = 0.0;
                bool jg = jgFor(o.speed, nj, ng);
                Phys const& C = m_bPhys[static_cast<size_t>(cur)];
                bool needJg = C.mode == MCube && C.jRef > 0.0 && C.gRef > 0.0;
                if (!dxFor(o.speed, ndx) || (needJg && !jg) || m_bPhys.size() >= 8 || m_gates.size() >= kMaxGates) {
                    stop = true;
                }
                else {
                    Phys Q = m_bPhys[static_cast<size_t>(cur)];
                    PS q = m_bPs[static_cast<size_t>(cur)];
                    Q.dx = ndx;
                    q.speed = o.speed;
                    if (jg) {
                        if (needJg) {
                            Q.J *= nj / Q.jRef;
                            Q.g *= ng / Q.gRef;
                        }
                        if (Q.jRef > 0.0) Q.jRef = nj;
                        if (Q.gRef > 0.0) Q.gRef = ng;
                        q.yStart = static_cast<float>(nj);
                        q.grav = static_cast<float>(ng);
                    }
                    m_bPhys.push_back(Q);
                    m_bPs.push_back(q);
                    cur = static_cast<int>(m_bPhys.size()) - 1;
                    Gate g;
                    g.col = j + 1;
                    g.lo = gateLo - Q.hh + 1.0;
                    g.hi = gateHi + Q.hh - 1.0;
                    m_gates.push_back(g);
                    dxc = ndx;
                    spd = o.speed;
                }
            }
            if (stop && j >= 0) {
                m_switches.resize(static_cast<size_t>(m_colSwStart.back()));
                if ((gate >= 0 || pgate >= 0) && m_gates.size() < kMaxGates) {
                    if (gate < 0) {
                        MapObj const& o = m_objs[static_cast<size_t>(pgate)];
                        gateLo = o.y0;
                        gateHi = o.y1;
                    }
                    Gate g;
                    g.col = j + 1;
                    g.lo = gateLo - P.hh + 1.0;
                    g.hi = gateHi + P.hh - 1.0;
                    m_gates.push_back(g);
                    horizon = j + 1;
                }
                else if (!hard && j > 0) {
                    horizon = j;
                    m_stopCol = true;
                    m_stopColIdx = j;
                }
                else {
                    horizon = j;
                    m_colPhys.pop_back();
                    m_colSwStart.pop_back();
                }
                m_colStart.push_back(static_cast<int>(m_colIdx.size()));
                m_colSwStart.push_back(static_cast<int>(m_switches.size()));
                return;
            }
        }
        m_colStart.push_back(static_cast<int>(m_colIdx.size()));
        m_colSwStart.push_back(static_cast<int>(m_switches.size()));
    }

    double colX(int c) const {
        if (m_colX.empty()) return 0.0;
        return m_colX[static_cast<size_t>(std::clamp(c, 0, static_cast<int>(m_colX.size()) - 1))];
    }

    double colDx(int j) const {
        if (m_colDx.empty()) return m_bPhys.empty() ? 0.0 : m_bPhys.front().dx;
        return m_colDx[static_cast<size_t>(std::clamp(j, 0, static_cast<int>(m_colDx.size()) - 1))];
    }

    float colSpeed(int j) const {
        if (m_colSpeed.empty()) return m_bPs.empty() ? 0.f : m_bPs.front().speed;
        return m_colSpeed[static_cast<size_t>(std::clamp(j, 0, static_cast<int>(m_colSpeed.size()) - 1))];
    }

    bool gateAt(int c, double& lo, double& hi) const {
        bool any = false;
        for (auto const& g : m_gates) {
            if (c != g.col && c != g.col + 1) continue;
            if (!any) {
                lo = g.lo;
                hi = g.hi;
                any = true;
            }
            else {
                lo = std::max(lo, g.lo);
                hi = std::min(hi, g.hi);
            }
        }
        return any;
    }

    Phys const& colPhys(int j) const {
        size_t k = j >= 0 && j < static_cast<int>(m_colPhys.size()) ? m_colPhys[static_cast<size_t>(j)] : (m_colPhys.empty() ? 0 : m_colPhys.back());
        return m_bPhys[std::min(k, m_bPhys.size() - 1)];
    }

    void freeIntervals(int j, std::vector<std::pair<double, double>>& out) {
        out.clear();
        Phys const& P = colPhys(j);
        double lo = P.bounded ? P.lo + P.bHalfLo : (P.hasFloor ? P.floorFeet + P.hh : -1e6);
        double hi = P.bounded ? P.hi - P.bHalfHi : std::min(1e6, P.maxY);
        double glo = 0.0;
        double ghi = 0.0;
        if (gateAt(j + 1, glo, ghi)) {
            lo = std::max(lo, glo);
            hi = std::min(hi, ghi);
        }
        if (!(hi >= lo)) return;
        double xp = colX(j + 1);
        m_tmpIv2.clear();
        size_t cs = static_cast<size_t>(m_colStart[static_cast<size_t>(j) + 1]);
        size_t ce = static_cast<size_t>(m_colStart[static_cast<size_t>(j) + 2]);
        for (size_t c = cs; c < ce; c++) {
            MapObj const& o = m_objs[static_cast<size_t>(m_colIdx[c])];
            if (o.kind == KSolid) {
                if (o.passable) continue;
                if (o.x1 + o.margin <= xp - P.hw + kEps || o.x0 - o.margin >= xp + P.hw - kEps) continue;
                m_tmpIv2.push_back({o.y0 - P.hh + kEps, o.y1 + P.hh - kEps});
            }
            else if (o.kind == KHazard) {
                double x0 = o.r > 0.f ? o.cx - o.r : o.x0;
                double x1 = o.r > 0.f ? o.cx + o.r : o.x1;
                double y0 = o.r > 0.f ? o.cy - o.r : o.y0;
                double y1 = o.r > 0.f ? o.cy + o.r : o.y1;
                double tt = P.touchOk && o.r <= 0.f ? kTouch : -kEps;
                if (x1 + o.margin <= xp - P.hw + tt || x0 - o.margin >= xp + P.hw - tt) continue;
                m_tmpIv2.push_back({y0 - P.hh - o.margin + tt, y1 + P.hh + o.margin - tt});
            }
        }
        std::sort(m_tmpIv2.begin(), m_tmpIv2.end());
        double cur = lo;
        for (auto const& b : m_tmpIv2) {
            if (b.second < cur) continue;
            if (b.first > cur) out.push_back({cur, std::min(b.first, hi)});
            cur = std::max(cur, b.second);
            if (cur >= hi) break;
        }
        if (cur <= hi) out.push_back({cur, hi});
        size_t k = 0;
        for (auto const& iv : out) {
            if (iv.second >= iv.first) out[k++] = iv;
        }
        out.resize(k);
    }

    void speedBounds(Phys const& P, double& dyUp, double& dyDown) const {
        double k = std::fabs(P.a) + std::fabs(P.b);
        dyUp = 0.0;
        dyDown = 0.0;
        if (P.mode == MWave) {
            dyUp = dyDown = P.slope * P.dx;
        }
        else if (P.mode == MUfo) {
            double vu = std::max(P.imp, 0.0);
            if (P.capUp < 1e8) vu = std::max(vu, P.capUp);
            dyUp = k * vu;
            dyDown = k * std::min(P.capDown, 100.0);
        }
        else if (P.mode == MCube) {
            double vu = P.J;
            for (int ty = 0; ty < kTypeSlots; ty++) {
                if (m_padFx[ty][P.mk].known) vu = std::max(vu, std::fabs(m_padFx[ty][P.mk].v));
                if (m_orbFx[ty][P.mk].known) vu = std::max(vu, std::fabs(orbV(m_orbFx[ty][P.mk], P.jRef)));
            }
            dyUp = k * vu;
            dyDown = k * std::min(std::max(P.vmax, vu), 100.0);
        }
        else {
            dyUp = k * std::min(P.capUp, 100.0);
            dyDown = k * std::min(P.capDown, 100.0);
        }
        dyUp = dyUp * 1.02 + 0.05;
        dyDown = dyDown * 1.02 + 0.05;
    }

    void buildFunnel(int horizon, SimState const& s0) {
        m_funStart.assign(static_cast<size_t>(horizon) + 1, 0);
        m_funIv.clear();
        m_funOn = false;
        if (horizon <= 0) return;
        m_rStart.assign(static_cast<size_t>(horizon) + 1, 0);
        m_rIv.clear();
        std::vector<int>& ends = m_tmpEnds;
        ends.assign(static_cast<size_t>(horizon), 0);
        size_t pieces = 0;
        for (int j = horizon - 1; j >= 0; j--) {
            freeIntervals(j, m_tmpIv);
            size_t start = m_rIv.size();
            m_rStart[static_cast<size_t>(j)] = static_cast<int>(start);
            pieces += m_tmpIv.size() + 1;
            if (j == horizon - 1) {
                for (auto const& iv : m_tmpIv) m_rIv.push_back(iv);
                ends[static_cast<size_t>(j)] = static_cast<int>(m_rIv.size());
                continue;
            }
            double dyUp = 0.0;
            double dyDown = 0.0;
            speedBounds(colPhys(j + 1), dyUp, dyDown);
            bool reset = false;
            if (m_colSwStart[static_cast<size_t>(j)] != m_colSwStart[static_cast<size_t>(j) + 1]) {
                double u2 = 0.0;
                double d2 = 0.0;
                speedBounds(colPhys(j), u2, d2);
                dyUp = std::max(dyUp, u2);
                dyDown = std::max(dyDown, d2);
                reset = colPhys(j).mode != colPhys(j + 1).mode || colPhys(j).mini != colPhys(j + 1).mini;
            }
            if (reset) {
                for (auto const& iv : m_tmpIv) m_rIv.push_back(iv);
            }
            else {
                size_t na = static_cast<size_t>(m_rStart[static_cast<size_t>(j) + 1]);
                size_t nb = static_cast<size_t>(ends[static_cast<size_t>(j) + 1]);
                pieces += nb - na;
                m_dilIv.clear();
                for (size_t q = na; q < nb; q++) {
                    double a = m_rIv[q].first - dyUp;
                    double b = m_rIv[q].second + dyDown;
                    if (!m_dilIv.empty() && a <= m_dilIv.back().second) m_dilIv.back().second = std::max(m_dilIv.back().second, b);
                    else m_dilIv.push_back({a, b});
                }
                size_t p = 0;
                size_t q = 0;
                while (p < m_tmpIv.size() && q < m_dilIv.size()) {
                    auto const& f = m_tmpIv[p];
                    auto const& r = m_dilIv[q];
                    double a = std::max(f.first, r.first);
                    double b = std::min(f.second, r.second);
                    if (b >= a) {
                        if (m_rIv.size() > start && a <= m_rIv.back().second) m_rIv.back().second = std::max(m_rIv.back().second, b);
                        else m_rIv.push_back({a, b});
                    }
                    if (f.second < r.second) p++;
                    else q++;
                }
            }
            ends[static_cast<size_t>(j)] = static_cast<int>(m_rIv.size());
            if (m_rIv.size() > kFunnelCap) {
                if (m_hardCap > 0) m_work += static_cast<int64_t>(pieces / kFunnelWorkDiv);
                m_rIv.clear();
                if (m_rIv.capacity() > 2 * kFunnelCap) std::vector<std::pair<double, double>>().swap(m_rIv);
                return;
            }
        }
        pieces += m_rIv.size();
        if (m_hardCap > 0) m_work += static_cast<int64_t>(pieces / kFunnelWorkDiv);
        for (int j = 0; j < horizon; j++) {
            m_funStart[static_cast<size_t>(j)] = static_cast<int>(m_funIv.size());
            for (int q = m_rStart[static_cast<size_t>(j)]; q < ends[static_cast<size_t>(j)]; q++) m_funIv.push_back(m_rIv[static_cast<size_t>(q)]);
        }
        m_funStart[static_cast<size_t>(horizon)] = static_cast<int>(m_funIv.size());
        if (m_funStart[1] == m_funStart[0]) return;
        double dyUp = 0.0;
        double dyDown = 0.0;
        speedBounds(colPhys(0), dyUp, dyDown);
        bool reach = false;
        for (int q = m_funStart[0]; q < m_funStart[1]; q++) {
            auto const& iv = m_funIv[static_cast<size_t>(q)];
            if (s0.y >= iv.first - dyDown - 1e-6 && s0.y <= iv.second + dyUp + 1e-6) reach = true;
        }
        m_funOn = reach;
    }

    bool funnelClear(int j, double y, double& clear) const {
        size_t a = static_cast<size_t>(m_funStart[static_cast<size_t>(j)]);
        size_t b = static_cast<size_t>(m_funStart[static_cast<size_t>(j) + 1]);
        if (a >= b || b > m_funIv.size()) return false;
        auto first = m_funIv.begin() + static_cast<std::ptrdiff_t>(a);
        auto last = m_funIv.begin() + static_cast<std::ptrdiff_t>(b);
        auto it = std::lower_bound(first, last, y - 1e-6, [](std::pair<double, double> const& iv, double v) { return iv.second < v; });
        if (it == last || y < it->first - 1e-6) return false;
        clear = std::min(y - it->first, it->second - y);
        if (clear < 0.0) clear = 0.0;
        return true;
    }

    uint8_t expand(BNode const& nd, bool h, int j, SimState const& s0, BNode& out, SimState& ns, double bias) {
        m_work++;
        Phys const& P = colPhys(j);
        double flip = nd.upside ? -1.0 : 1.0;
        bool edge = h && !nd.held;
        bool flying = P.mode != MCube;
        SimState s;
        s.x = colX(j);
        s.y = nd.y;
        s.v = nd.v;
        s.dx = colDx(j);
        s.speed = colSpeed(j);
        s.portal = s0.portal;
        s.lastPad = nd.lastPad;
        s.usedOrb = nd.usedOrb;
        s.since = nd.since;
        s.flipAge = nd.flipAge;
        s.held = nd.held;
        s.upside = nd.upside;
        s.fresh = nd.fresh;
        s.grounded = nd.grounded;
        s.mode = P.mode;
        if (edge) {
            s.fresh = true;
            s.usedOrb = -1;
        }
        if (!h) s.fresh = false;
        if (h != s.held) s.since = 1;
        else if (s.since < kSinceCap) s.since++;
        if (s.flipAge < kSinceCap) s.flipAge++;
        s.held = h;
        if (edge && flying) {
            size_t cs = static_cast<size_t>(m_colStart[static_cast<size_t>(j)]);
            size_t ce = static_cast<size_t>(m_colStart[static_cast<size_t>(j) + 1]);
            for (size_t c = cs; c < ce; c++) {
                MapObj const& o = m_objs[static_cast<size_t>(m_colIdx[c])];
                if (o.kind != KOrb) continue;
                if (o.x1 < s.x - P.hw || o.x0 > s.x + P.hw || o.y1 < s.y - P.hh || o.y0 > s.y + P.hh) continue;
                return RStop + 10;
            }
        }
        if (edge && !flying && orbTiming() == 1) {
            size_t cs = static_cast<size_t>(m_colStart[static_cast<size_t>(j)]);
            size_t ce = static_cast<size_t>(m_colStart[static_cast<size_t>(j) + 1]);
            int fired = firePreOrb(s, P, m_colIdx.data() + cs, static_cast<int>(ce - cs));
            if (fired < 0) return RStop + 10;
            if (fired > 0) flip = s.upside ? -1.0 : 1.0;
        }
        double vo = s.v * flip;
        double vn = vo;
        double dyf = 0.0;
        bool jumped = false;
        dynamics(P, s, vo, h, edge, vn, dyf, jumped);
        if (jumped) {
            s.grounded = false;
            s.fresh = false;
        }
        double prevY = s.y;
        s.x = colX(j + 1);
        s.y += dyf * flip;
        s.v = vn * flip;
        if (!applyBounds(s, P)) return RDead;
        if (!(std::fabs(s.y) < 1e5)) return RDead;
        size_t cs = static_cast<size_t>(m_colStart[static_cast<size_t>(j) + 1]);
        size_t ce = static_cast<size_t>(m_colStart[static_cast<size_t>(j) + 2]);
        int nc = static_cast<int>(ce - cs);
        StepInfo info;
        uint8_t r = contacts(s, prevY, P, m_colIdx.data() + cs, nc, h, jumped, info, true);
        ns = s;
        if (r != RAlive) return r;
        for (int k = m_colSwStart[static_cast<size_t>(j)]; k < m_colSwStart[static_cast<size_t>(j) + 1]; k++) {
            ColSwitch const& sw = m_switches[static_cast<size_t>(k)];
            bool up = s.upside;
            if (sw.upRule == 1) up = true;
            else if (sw.upRule == 2) up = false;
            else if (sw.upRule == 3) up = !up;
            bool flipped = up != s.upside;
            if (sw.modeSwitch || flipped) s.v = snapGrid(s.v * sw.vFactor);
            s.upside = up;
            if (flipped) {
                s.grounded = false;
                s.flipAge = 0;
            }
            s.portal = sw.portal;
        }
        if (!m_gates.empty()) {
            double glo = 0.0;
            double ghi = 0.0;
            if (gateAt(j + 1, glo, ghi) && !(s.y > glo && s.y < ghi)) {
                ns = s;
                return RMiss;
            }
        }
        double clear = kClearCap;
        bool term = m_stopCol && j == m_stopColIdx;
        if (m_funOn && !term) {
            double fc = 0.0;
            if (!funnelClear(j, s.y, fc)) return 3;
            clear = std::min(clear, fc);
        }
        else if (!term) {
            double py0 = s.y - P.hh;
            double py1 = s.y + P.hh;
            for (size_t c = cs; c < ce; c++) {
                MapObj const& o = m_objs[static_cast<size_t>(m_colIdx[c])];
                if (o.kind != KHazard && !(o.kind == KSolid && flying)) continue;
                if (o.x1 + o.margin < s.x - P.hw || o.x0 - o.margin > s.x + P.hw) continue;
                double d = 0.0;
                if (o.y1 <= py0) d = py0 - o.y1;
                else if (o.y0 >= py1) d = o.y0 - py1;
                clear = std::min(clear, d);
            }
            if (P.bounded) {
                clear = std::min(clear, s.y - (P.lo + P.bHalfLo));
                clear = std::min(clear, (P.hi - P.bHalfHi) - s.y);
            }
        }
        clear = std::max(0.0, std::min(clear, kClearCap));
        out = BNode{};
        out.y = s.y;
        out.v = s.v;
        out.since = s.since;
        out.flipAge = s.flipAge;
        out.held = h ? 1 : 0;
        out.upside = s.upside;
        out.fresh = s.fresh;
        out.grounded = s.grounded;
        out.lastPad = s.lastPad;
        out.usedOrb = s.usedOrb;
        out.changes = static_cast<int16_t>(nd.changes + (h != (nd.held != 0) ? 1 : 0));
        out.minLate = j + 1 >= kLateTicks ? std::min(nd.minLate, static_cast<float>(clear)) : nd.minLate;
        out.sumClear = nd.sumClear + static_cast<float>(clear);
        out.score = std::min(out.minLate, static_cast<float>(kSafeClear)) + static_cast<float>(std::min(clear, kSafeClear)) * 0.5f - static_cast<float>(out.changes) * kChangeCost +
            static_cast<float>(bias * (s.y - s0.y));
        ns = s;
        return RAlive;
    }

    bool beamPlan(int t, SimState const& s0, PS const& ps0, Phys const& P0, int hmax, Plan& plan, int& deathTick, bool thorough) {
        int horizon = hmax;
        buildColumns(t, hmax, s0, ps0, P0, horizon);
        buildFunnel(horizon, s0);
        bool funnel = m_funOn;
        int width = thorough ? kBeamWidth * 3 / 2 : kBeamWidth;
        plan = Plan{};
        deathTick = -1;
        int tries = 2;
        for (int k = 0; k < tries; k++) {
            if (k > 0 && overBudget()) break;
            m_funOn = funnel && k != 1;
            if (k == 1 && !funnel) continue;
            double bias = k < 2 ? 0.0 : (k == 2 ? -0.3 : 0.3);
            Plan p;
            int dt = -1;
            bool ok = beamRun(t, s0, horizon, width, bias, p, dt);
            if (ok) {
                plan = std::move(p);
                deathTick = -1;
                return true;
            }
            if (p.valid && (!plan.valid || dt > deathTick)) {
                plan = std::move(p);
                deathTick = dt;
            }
        }
        return false;
    }

    void selectLayer(int width) {
        size_t n = m_children.size();
        size_t ext[4] = {0, 0, 0, 0};
        for (size_t i = 1; i < n; i++) {
            BNode const& c = m_children[i];
            if (c.y > m_children[ext[0]].y) ext[0] = i;
            if (c.y < m_children[ext[1]].y) ext[1] = i;
            if (c.v > m_children[ext[2]].v) ext[2] = i;
            if (c.v < m_children[ext[3]].v) ext[3] = i;
        }
        double ymin = m_children[ext[1]].y;
        double ymax = m_children[ext[0]].y;
        double vmin = m_children[ext[3]].v;
        double vmax = m_children[ext[2]].v;
        double yScale = kYBins / std::max(0.5, ymax - ymin + 1e-6);
        double vScale = kVBins / std::max(0.05, vmax - vmin + 1e-6);
        constexpr int kCells = kYBins * kVBins;
        int perBin = std::max(1, std::min(2, width / kCells));
        int best[kCells][2];
        for (int b = 0; b < kCells; b++) best[b][0] = best[b][1] = -1;
        m_pick.assign(n, 0);
        for (size_t e : ext) m_pick[e] = 1;
        for (size_t i = 0; i < n; i++) {
            BNode const& c = m_children[i];
            if (c.onOld || m_pick[i]) {
                m_pick[i] = 1;
                continue;
            }
            int by = std::clamp(static_cast<int>((c.y - ymin) * yScale), 0, kYBins - 1);
            int bv = std::clamp(static_cast<int>((c.v - vmin) * vScale), 0, kVBins - 1);
            int* slot = best[by * kVBins + bv];
            int ci = static_cast<int>(i);
            for (int k = 0; k < perBin; k++) {
                if (slot[k] < 0 || c.score > m_children[static_cast<size_t>(slot[k])].score) {
                    for (int q = perBin - 1; q > k; q--) slot[q] = slot[q - 1];
                    slot[k] = ci;
                    break;
                }
            }
        }
        for (int b = 0; b < kCells; b++) {
            for (int k = 0; k < perBin; k++) {
                if (best[b][k] >= 0) m_pick[static_cast<size_t>(best[b][k])] = 1;
            }
        }
        m_sel.clear();
        m_restIdx.clear();
        for (size_t i = 0; i < n; i++) {
            if (m_pick[i]) m_sel.push_back(m_children[i]);
            else m_restIdx.push_back(static_cast<int>(i));
        }
        int room = width - static_cast<int>(m_sel.size());
        if (room > 0 && !m_restIdx.empty()) {
            if (static_cast<int>(m_restIdx.size()) > room) {
                std::nth_element(m_restIdx.begin(), m_restIdx.begin() + room, m_restIdx.end(), [&](int a, int b) {
                    return m_children[static_cast<size_t>(a)].score > m_children[static_cast<size_t>(b)].score;
                });
                m_restIdx.resize(static_cast<size_t>(room));
            }
            for (int i : m_restIdx) m_sel.push_back(m_children[static_cast<size_t>(i)]);
        }
        m_children.swap(m_sel);
    }

    bool doomed(int ni, int t, SimState const& s0) {
        BNode nd = m_nodes[static_cast<size_t>(ni)];
        int j = m_stopColIdx;
        Phys const& P = colPhys(j);
        bool flipOk = canFlip(t + j, familyOfMode(P.mode), nd.held != 0, nd.since);
        for (int hv = 0; hv < 2; hv++) {
            bool h = hv != 0;
            if (h != (nd.held != 0) && !flipOk) continue;
            if (P.mode == MUfo && nd.held && h && flipOk) continue;
            BNode c;
            SimState ns;
            if (expand(nd, h, j, s0, c, ns, 0.0) != RDead) return false;
        }
        return true;
    }

    bool beamRun(int t, SimState const& s0, int horizon, int width, double bias, Plan& plan, int& deathTick) {
        m_nodes.clear();
        m_leaves.clear();
        BNode root;
        root.y = s0.y;
        root.v = s0.v;
        root.since = s0.since;
        root.flipAge = s0.flipAge;
        root.held = s0.held ? 1 : 0;
        root.upside = s0.upside;
        root.fresh = s0.fresh;
        root.grounded = s0.grounded;
        root.lastPad = s0.lastPad;
        root.usedOrb = s0.usedOrb;
        root.parent = -1;
        root.onOld = m_guideLen > 0;
        root.fromOld = m_guideLen > 0;
        m_nodes.push_back(root);
        int layerStart = 0;
        int layerEnd = 1;
        int bestDeadTick = -1;
        int bestDeadParent = -1;
        uint8_t bestDeadAct = 0;
        int reached = 0;
        bool truncated = false;
        for (int j = 0; j < horizon; j++) {
            if (m_hardCap > 0 && m_work >= m_hardCap) break;
            if (m_workCap > 0 && j >= kMinTrunc && m_work >= m_workCap) {
                truncated = true;
                break;
            }
            int tau = t + j;
            Phys const& P = colPhys(j);
            int fam = familyOfMode(P.mode);
            int cap = m_limits.active ? std::max(m_limits.minHold[fam], m_limits.minRelease[fam]) : 1;
            m_children.clear();
            for (int ni = layerStart; ni < layerEnd; ni++) {
                BNode nd = m_nodes[static_cast<size_t>(ni)];
                bool flipOk = canFlip(tau, fam, nd.held != 0, nd.since);
                for (int hv = 0; hv < 2; hv++) {
                    bool h = hv != 0;
                    if (h != (nd.held != 0) && !flipOk) continue;
                    if (P.mode == MUfo && nd.held && h && flipOk) continue;
                    BNode c;
                    SimState ns;
                    uint8_t r = expand(nd, h, j, s0, c, ns, bias);
                    if (r == RDead) {
                        if (tau > bestDeadTick) {
                            bestDeadTick = tau;
                            bestDeadParent = ni;
                            bestDeadAct = static_cast<uint8_t>(hv);
                        }
                        continue;
                    }
                    if (r == RStop || r == RMiss) {
                        BNode leaf = nd;
                        leaf.miss = r == RMiss;
                        leaf.parent = ni;
                        leaf.held = static_cast<uint8_t>(hv);
                        leaf.y = ns.y;
                        leaf.v = ns.v;
                        leaf.since = ns.since;
                        leaf.flipAge = ns.flipAge;
                        leaf.grounded = ns.grounded;
                        leaf.changes = static_cast<int16_t>(nd.changes + (h != (nd.held != 0) ? 1 : 0));
                        m_nodes.push_back(leaf);
                        m_leaves.push_back(static_cast<int>(m_nodes.size()) - 1);
                        continue;
                    }
                    if (r != RAlive) continue;
                    c.parent = ni;
                    c.onOld = nd.onOld && j < m_guideLen && m_guide[static_cast<size_t>(j)] == (h ? 1 : 0);
                    c.fromOld = nd.fromOld && (j >= m_guideLen || m_guide[static_cast<size_t>(j)] == (h ? 1 : 0));
                    m_children.push_back(c);
                }
            }
            if (m_children.empty()) break;
            m_dedupGen++;
            if (m_dedupGen == 0) {
                m_dedupStamp.fill(0);
                m_dedupGen = 1;
            }
            size_t kept = 0;
            for (size_t ci = 0; ci < m_children.size(); ci++) {
                BNode const& c = m_children[ci];
                int64_t qy = static_cast<int64_t>(std::floor(c.y * 4.0));
                int64_t qv = static_cast<int64_t>(std::floor(c.v * 50.0));
                int64_t qs = std::min(c.since, cap);
                uint64_t key = static_cast<uint64_t>(qy) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(qv) * 0xC2B2AE3D27D4EB4Full ^
                    static_cast<uint64_t>(qs * 16 + c.held + (c.upside ? 2 : 0) + (c.grounded ? 4 : 0) + (c.fresh ? 8 : 0)) * 0x165667B19E3779F9ull;
                if (c.flipAge < kSafeFlipTicks) key ^= static_cast<uint64_t>(c.flipAge + 1) * 0x2545F4914F6CDD1Dull;
                size_t slot = static_cast<size_t>((key ^ (key >> 29)) & 2047);
                bool placed = false;
                for (int probe = 0; probe < 16; probe++) {
                    size_t sl = (slot + static_cast<size_t>(probe)) & 2047;
                    if (m_dedupStamp[sl] != m_dedupGen) {
                        m_dedupStamp[sl] = m_dedupGen;
                        m_dedupKey[sl] = key;
                        m_dedupSlot[sl] = static_cast<int>(kept);
                        m_children[kept++] = c;
                        placed = true;
                        break;
                    }
                    if (m_dedupKey[sl] == key) {
                        int at = m_dedupSlot[sl];
                        BNode& cur = m_children[static_cast<size_t>(at)];
                        if (!cur.onOld && (c.onOld || (c.fromOld && !cur.fromOld) || (c.fromOld == cur.fromOld && c.score > cur.score))) cur = c;
                        placed = true;
                        break;
                    }
                }
                if (!placed) m_children[kept++] = c;
            }
            m_children.resize(kept);
            if (static_cast<int>(m_children.size()) > width) selectLayer(width);
            layerStart = static_cast<int>(m_nodes.size());
            for (auto const& c : m_children) m_nodes.push_back(c);
            layerEnd = static_cast<int>(m_nodes.size());
            reached = j + 1;
        }
        int bestLeaf = -1;
        auto better = [&](int a, int b) {
            BNode const& x = m_nodes[static_cast<size_t>(a)];
            BNode const& y = m_nodes[static_cast<size_t>(b)];
            if (x.miss != y.miss) return !x.miss;
            int tx = safetyTier(x.minLate);
            int ty = safetyTier(y.minLate);
            if (tx != ty) return tx > ty;
            if (x.fromOld != y.fromOld) return x.fromOld;
            if (x.changes != y.changes) return x.changes < y.changes;
            if (x.minLate != y.minLate) return x.minLate > y.minLate;
            return x.sumClear > y.sumClear;
        };
        bool survived = false;
        if (reached == horizon || truncated) {
            bool check = m_stopCol && reached == horizon && !truncated && m_stopColIdx == horizon;
            int bestDoomed = -1;
            for (int ni = layerStart; ni < layerEnd; ni++) {
                if (check && doomed(ni, t, s0)) {
                    if (bestDoomed < 0 || better(ni, bestDoomed)) bestDoomed = ni;
                    continue;
                }
                if (bestLeaf < 0 || better(ni, bestLeaf)) bestLeaf = ni;
            }
            survived = bestLeaf >= 0;
            if (!survived && bestDoomed >= 0 && t + horizon > bestDeadTick) {
                bestDeadTick = t + horizon;
                bestDeadParent = bestDoomed;
                bestDeadAct = m_nodes[static_cast<size_t>(bestDoomed)].held;
            }
        }
        int missTick = -1;
        for (int li : m_leaves) {
            if (bestLeaf < 0 || better(li, bestLeaf)) bestLeaf = li;
            if (!m_nodes[static_cast<size_t>(li)].miss) survived = true;
        }
        if (!survived && bestLeaf >= 0) {
            int depth = 0;
            for (int n = bestLeaf; n >= 0 && depth <= horizon; n = m_nodes[static_cast<size_t>(n)].parent) depth++;
            missTick = t + std::max(0, depth - 1);
        }
        deathTick = survived ? -1 : std::max(bestDeadTick, missTick);
        std::vector<int> chain;
        if (bestLeaf >= 0) {
            for (int n = bestLeaf; n >= 0; n = m_nodes[static_cast<size_t>(n)].parent) chain.push_back(n);
        }
        else if (bestDeadParent >= 0) {
            for (int n = bestDeadParent; n >= 0; n = m_nodes[static_cast<size_t>(n)].parent) chain.push_back(n);
        }
        else {
            plan = Plan{};
            return false;
        }
        std::reverse(chain.begin(), chain.end());
        plan = Plan{};
        plan.mode = colPhys(0).mode;
        plan.t0 = t;
        int lastSwitch = -1;
        for (size_t i = 0; i < chain.size(); i++) {
            BNode const& nd = m_nodes[static_cast<size_t>(chain[i])];
            int ji = static_cast<int>(i);
            SimState st;
            st.x = colX(ji);
            st.y = nd.y;
            st.v = nd.v;
            st.dx = colDx(ji);
            st.speed = colSpeed(ji);
            st.portal = s0.portal;
            st.lastPad = nd.lastPad;
            st.usedOrb = nd.usedOrb;
            st.since = nd.since;
            st.flipAge = nd.flipAge;
            st.held = nd.held != 0;
            st.upside = nd.upside;
            st.fresh = nd.fresh;
            st.grounded = nd.grounded;
            st.mode = colPhys(ji).mode;
            if (ji < static_cast<int>(m_colSwStart.size()) - 1 && m_colSwStart[i] != m_colSwStart[i + 1]) lastSwitch = t + ji + 1;
            if (i + 1 < chain.size()) {
                plan.pred.push_back(st);
                plan.held.push_back(m_nodes[static_cast<size_t>(chain[i + 1])].held);
            }
            else if (bestLeaf < 0) {
                plan.pred.push_back(st);
                plan.held.push_back(bestDeadAct);
            }
        }
        if (plan.held.empty()) {
            plan.pred.push_back(s0);
            plan.held.push_back(s0.held ? 1 : 0);
        }
        plan.valid = true;
        plan.failed = !survived;
        plan.cross = lastSwitch >= 0;
        plan.replanAt = plan.mode == MCube ? std::max(t + kFlyReplan, lastSwitch + 1) : t + kFlyReplan;
        return survived;
    }

    int knownPortalAhead(SimState const& s0, double dx, int ticks, double hw) const {
        if (!(dx > 0.0)) return -1;
        double x0 = s0.x;
        double x1 = s0.x + dx * ticks;
        int best = -1;
        double bestX = 1e18;
        query(x0, x1 + 40.0, [&](int i, MapObj const& o) {
            if (o.x0 <= x0 + hw || o.x0 > x1 + hw) return;
            if (o.kind == KSpeed) {
                if (std::fabs(o.speed - s0.speed) < 0.005f) return;
            }
            else if (o.kind != KPortal || !o.obj || o.obj == s0.portal || !fxFor(o.obj, o.type, s0.mode)) {
                return;
            }
            if (o.x0 < bestX) {
                bestX = o.x0;
                best = i;
            }
        });
        if (best < 0) return -1;
        return static_cast<int>(std::max(0.0, (bestX - x0) / dx));
    }

    bool planWith(int t, SimState const& s0, PS const& ps, Phys const& P, Plan& plan, int& deathTick, int minEnd) {
        bool thorough = minEnd >= 0;
        bool ok = false;
        plan = Plan{};
        deathTick = -1;
        if (P.mode == MCube) {
            int ahead = knownPortalAhead(s0, P.dx, kCrossAhead, P.hw);
            int want = ahead >= 0 ? ahead + 180 : -1;
            if (minEnd > t && (ahead >= 0 || thorough)) want = std::max(want, minEnd - t);
            if (want > 0) {
                ok = beamPlan(t, s0, ps, P, std::min(want, thorough ? kHintHorizon : 450), plan, deathTick, thorough);
                if (ok) m_crossPlans++;
            }
            if (!ok) {
                Plan cp;
                int dt = -1;
                bool ok2 = cubePlan(t, s0, P, cp, dt);
                if (want > 0) {
                    int portalTick = ahead >= 0 ? t + ahead : t;
                    bool beamBetter = plan.valid && deathTick > portalTick;
                    if (cp.valid && !beamBetter) {
                        plan = std::move(cp);
                        deathTick = dt;
                    }
                    ok = false;
                }
                else if (cp.valid && (ok2 || !plan.valid || cp.end() > plan.end())) {
                    plan = std::move(cp);
                    deathTick = dt;
                    ok = ok2;
                }
            }
        }
        else {
            int h = kFlyHorizon;
            if (minEnd > t) h = std::min(std::max(h, minEnd - t), kHintHorizon);
            ok = beamPlan(t, s0, ps, P, h, plan, deathTick, thorough);
        }
        return ok;
    }

    bool makePlan(int t, SimState const& s0, PS const& ps, Plan& plan, int& deathTick, int need, bool* survived = nullptr, int minEnd = -1) {
        m_bOptimistic = false;
        m_bTouch = false;
        Phys P;
        if (survived) *survived = false;
        if (!resolve(ps, P, false, need)) return false;
        if (level(ps.mode, ps.mini) < 3) m_provisional++;
        m_bNeed = need;
        m_stats.plans++;
        m_touchHit = false;
        m_capHit = false;
        m_solidHit = false;
        bool ok = planWith(t, s0, ps, P, plan, deathTick, minEnd);
        if (!ok && m_capHit && m_guideLen == 0 && capsUnknown(ps) && !overBudget()) {
            Phys O;
            if (resolve(ps, O, false, need, true)) {
                Plan alt;
                int dt2 = -1;
                m_bOptimistic = true;
                bool ok2 = planWith(t, s0, ps, O, alt, dt2, minEnd);
                m_bOptimistic = false;
                if (ok2 && alt.valid) {
                    plan = std::move(alt);
                    deathTick = dt2;
                    ok = true;
                    m_lenientPlans++;
                }
            }
        }
        if (!ok && m_solidHit && m_guideLen == 0 && !m_strictOnly && !lenientBad() && !overBudget()) {
            Phys L;
            if (resolve(ps, L, true, need)) {
                Plan alt;
                int dt2 = -1;
                bool ok2 = planWith(t, s0, ps, L, alt, dt2, minEnd);
                if (alt.valid && (ok2 || !plan.valid || (dt2 > deathTick && deathTick >= 0))) {
                    plan = std::move(alt);
                    deathTick = dt2;
                    ok = ok2;
                    m_lenientPlans++;
                }
            }
        }
        if (!ok && m_touchHit && m_guideLen == 0 && !P.touchOk && m_touchDeadly == 0 && !overBudget()) {
            m_bTouch = true;
            Phys X;
            if (resolve(ps, X, false, need)) {
                Plan alt;
                int dt2 = -1;
                bool ok2 = planWith(t, s0, ps, X, alt, dt2, minEnd);
                if (ok2 && alt.valid) {
                    plan = std::move(alt);
                    deathTick = dt2;
                    ok = true;
                    m_lenientPlans++;
                }
            }
            m_bTouch = false;
        }
        if (!ok) m_stats.planFailures++;
        if (survived) *survived = ok;
        return plan.valid;
    }

    void noteGrid(double v, double dv) {
        if (m_vqHits > 1000000) {
            m_vqHits /= 2;
            m_vqMiss /= 2;
        }
        double av = std::fabs(v);
        double adv = std::fabs(dv);
        double cand = av > 1e-3 ? av : 0.0;
        if (adv > 1e-3 && (cand == 0.0 || adv < cand)) cand = adv;
        if (cand > 0.0 && (m_vq <= 0.0 || cand < m_vq - 1e-9)) {
            bool divides = false;
            if (m_vq > 0.0) {
                double r = m_vq / cand;
                divides = std::fabs(r - std::round(r)) <= 1e-4 * std::max(1.0, r);
            }
            if (!divides) m_vqHits = 0;
            m_vqMiss = 0;
            m_vq = cand;
        }
        if (m_vq <= 0.0) return;
        double k = v / m_vq;
        if (std::fabs(k - std::round(k)) <= 1e-4 * std::max(1.0, std::fabs(k))) m_vqHits++;
        else m_vqMiss++;
    }

    double snapGrid(double v) const {
        if (!(m_vq > 0.0) || m_vqHits < 64 || m_vqMiss * 50 >= m_vqHits) return v;
        double k = v / m_vq;
        double kr = std::round(k);
        if (std::fabs(k - kr) < 1e-6 * std::max(1.0, std::fabs(k))) return kr * m_vq;
        return std::trunc(k) * m_vq;
    }

    bool lenientBad() const {
        return m_strictDeaths >= 2 && m_strictDeaths > 2 * m_softSurvivals;
    }

    void learnCollision(int d, std::vector<uint8_t> const& path) {
        TraceEntry const* te = entryAt(d);
        if (!te || !te->s.valid || te->s.special || te->s.mode >= 4 || d >= static_cast<int>(path.size())) return;
        if (level(te->s.mode, te->s.mini) < 3 || m_strictDeaths >= 1000) return;
        double dx = 0.0;
        if (!dxFor(te->s.speed, dx)) return;
        bool prevDown = d > 0 && (path[static_cast<size_t>(d) - 1] & 1);
        SimState st = simFrom(te->s, dx, prevDown, sinceFromPath(path, d), te->lastPad, te->usedOrb);
        Phys S;
        Phys L;
        if (!resolve(te->s, S, false, 3) || !resolve(te->s, L, true, 3)) return;
        bool h = (path[static_cast<size_t>(d)] & 1) != 0;
        SimState a = st;
        SimState b = st;
        StepInfo ia;
        StepInfo ib;
        int saved = m_budget;
        m_budget = 16;
        uint8_t ra = step(a, h, S, ia);
        uint8_t rb = step(b, h, L, ib);
        m_budget = saved;
        if (ra != RDead || rb != RAlive || ia.killer < 0 || ia.killer >= static_cast<int>(m_objs.size())) return;
        if (m_objs[static_cast<size_t>(ia.killer)].kind != KSolid) return;
        m_strictDeaths++;
    }

    void noteSoftSurvival(PS const& post) {
        if (post.mode >= 4 || post.special || m_softSurvivals >= 1000) return;
        int cand[kMaxCand];
        int n = touching(post, 0.0, cand, kMaxCand);
        for (int c = 0; c < n; c++) {
            MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
            if (o.kind != KSolid || o.passable) continue;
            double ox = std::min(post.x + post.hw, static_cast<double>(o.x1)) - std::max(post.x - post.hw, static_cast<double>(o.x0));
            double oy = std::min(post.y + post.hh, static_cast<double>(o.y1)) - std::max(post.y - post.hh, static_cast<double>(o.y0));
            if (ox > 1.0 && oy > 1.0) {
                m_softSurvivals++;
                return;
            }
        }
    }

    static bool hazardHit(MapObj const& o, double m, double px0, double px1, double py0, double py1, bool touchOk) {
        if (touchOk) return o.x1 + m > px0 + kTouch && o.x0 - m < px1 - kTouch && o.y1 + m > py0 + kTouch && o.y0 - m < py1 - kTouch;
        return o.x1 + m >= px0 && o.x0 - m <= px1 && o.y1 + m >= py0 && o.y0 - m <= py1;
    }

    bool touchLearned() const {
        return m_touchSafe >= 2 && m_touchSafe > 4 * m_touchDeadly;
    }

    void noteTouch(PS const& p, bool died) {
        if (!p.valid || p.mode >= 4 || p.special || m_touchSafe + m_touchDeadly >= 100000) return;
        int cand[kMaxCand];
        int n = touching(p, kTouch, cand, kMaxCand);
        double px0 = p.x - p.hw;
        double px1 = p.x + p.hw;
        double py0 = p.y - p.hh;
        double py1 = p.y + p.hh;
        bool touch = false;
        for (int c = 0; c < n; c++) {
            MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
            if (o.kind == KSolid && !o.passable && died) {
                double ox = std::min(px1, static_cast<double>(o.x1)) - std::max(px0, static_cast<double>(o.x0));
                double oy = std::min(py1, static_cast<double>(o.y1)) - std::max(py0, static_cast<double>(o.y0));
                if (ox > kTouch && oy > kTouch) return;
                continue;
            }
            if (o.kind != KHazard) continue;
            if (o.r > 0.f) {
                if (died) return;
                continue;
            }
            if (hazardHit(o, 0.0, px0, px1, py0, py1, true)) return;
            if (hazardHit(o, 0.0, px0, px1, py0, py1, false)) touch = true;
        }
        if (!touch) return;
        if (died) m_touchDeadly++;
        else m_touchSafe++;
    }

    static uint64_t mixKey(uint64_t h, double v) {
        int64_t q = std::isfinite(v) ? static_cast<int64_t>(std::llround(v * 4096.0)) : 0;
        h ^= static_cast<uint64_t>(q) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }

    uint64_t modelSignature() const {
        uint64_t h = 1469598103934665603ull;
        for (int m = 0; m < 2; m++) {
            for (FlyModel const* f : {&m_ship[m], &m_ufo[m]}) {
                for (double v : {f->a1, f->a2, f->d1, f->d2, f->thr[0], f->thr[1], f->capUp, f->capDown, f->imp, f->flapHi}) h = mixKey(h, v);
                h = mixKey(h, (f->capUpKnown ? 1 : 0) + (f->capDownKnown ? 2 : 0) + (f->impKnown ? 4 : 0) + (f->thrKnown[0] ? 8 : 0) + (f->thrKnown[1] ? 16 : 0));
            }
            h = mixKey(h, m_wave[m].slope);
            auto const& c = m_cube[m];
            h = mixKey(h, c.jump.n > 0 ? ringMedianA(c.jump) : 0.0);
            h = mixKey(h, c.air.n > 0 ? ringMedianB(c.air) : 0.0);
            h = mixKey(h, c.walk.n > 0 ? ringMedianA(c.walk) : 0.0);
            h = mixKey(h, c.capKnown ? c.capV : c.maxFall);
        }
        for (int mode = 0; mode < 4; mode++) {
            h = mixKey(h, level(static_cast<uint8_t>(mode), false));
            h = mixKey(h, level(static_cast<uint8_t>(mode), true));
        }
        for (double v : {m_dy.a, m_dy.b, m_tolCube, m_tolFly, m_floorFeet, m_vq}) h = mixKey(h, v);
        h = mixKey(h, (m_qYes >= 16 && m_qNo * 20 < m_qYes ? 1 : 0) + (m_vqHits >= 64 && m_vqMiss * 50 < m_vqHits ? 2 : 0));
        h = mixKey(h, static_cast<double>(m_portalFx.size()) + 1000.0 * static_cast<double>(m_typeFx.size()));
        for (auto const& sd : m_speeds) h = mixKey(h, sd.dx);
        h = mixKey(h, static_cast<double>(m_mapEpoch));
        h = mixKey(h, static_cast<double>(m_moveEvents));
        h = mixKey(h, (touchLearned() ? 1 : 0) + (m_touchDeadly > 0 ? 2 : 0));
        h = mixKey(h, static_cast<double>(m_extras));
        if (orbTiming() >= 0) h = mixKey(h, 3.0 + orbTiming());
        return h;
    }

    static int safetyTier(float minLate) {
        return static_cast<int>(std::floor(std::min(static_cast<double>(minLate), kSafeClear) / kTierStep));
    }

    bool overBudget() const {
        return m_workCap > 0 && m_work >= m_workCap;
    }

    bool capsUnknown(PS const& ps) const {
        (void)ps;
        for (int m = 0; m < 2; m++) {
            if (!m_ship[m].capUpKnown || !m_ship[m].capDownKnown || !m_ufo[m].capDownKnown) return true;
        }
        return false;
    }

    bool matches(SimState const& p, PS const& live, uint8_t mode) const {
        if (p.mode != MNone) mode = p.mode;
        if (live.mode != mode) return false;
        if (live.upside != p.upside) return false;
        if (std::fabs(live.x - p.x) > 1.0) return false;
        if (std::fabs(live.y - p.y) > 0.5) return false;
        if (mode != MWave && std::fabs(live.v - p.v) > 0.05) return false;
        if (mode == MCube && live.grounded != p.grounded) return false;
        return true;
    }

    bool eligible(PS const& s) const {
        if (!s.valid || s.special || s.dead) return false;
        if (s.mode >= 4) return false;
        return true;
    }

    TraceEntry* entry(int t) {
        if (t < 0 || t >= kMaxTrace) return nullptr;
        if (static_cast<int>(m_trace.size()) <= t) m_trace.resize(static_cast<size_t>(t) + 1);
        return &m_trace[static_cast<size_t>(t)];
    }

    TraceEntry const* entryAt(int t) const {
        if (t < 0 || t >= static_cast<int>(m_trace.size())) return nullptr;
        auto const& e = m_trace[static_cast<size_t>(t)];
        return e.valid ? &e : nullptr;
    }

    void onRestore(int tick) {
        if (tick < 0) tick = 0;
        m_curTick = tick;
        m_passedTick = std::min(m_passedTick, tick);
        int hi = std::min(m_traceEnd, static_cast<int>(m_trace.size()) - 1);
        for (int k = tick + 1; k <= hi; k++) {
            m_trace[static_cast<size_t>(k)].valid = false;
            m_trace[static_cast<size_t>(k)].heldValid = false;
        }
        for (int k = tick; k <= hi && k < static_cast<int>(m_trace.size()); k++) m_trace[static_cast<size_t>(k)].heldValid = false;
        m_traceEnd = tick;
        PS live;
        if (!readPlayer(live)) return;
        TraceEntry* e = entry(tick);
        if (!e) return;
        bool keep = e->valid && std::fabs(e->s.x - live.x) < 1e-3 && std::fabs(e->s.y - live.y) < 1e-3;
        int lp = keep ? e->lastPad : -1;
        int uo = keep ? e->usedOrb : -1;
        if (!keep && tick > 0) {
            TraceEntry const* prev = entryAt(tick - 1);
            if (prev) {
                lp = prev->lastPad;
                uo = prev->usedOrb;
            }
        }
        e->s = live;
        e->valid = true;
        e->died = false;
        e->lastPad = lp;
        e->usedOrb = uo;
        e->heldValid = false;
    }

    void addDy(double vo, double vn, double dyf) {
        auto& d = m_dy;
        if (d.known) {
            double pred = d.a * vo + d.b * vn;
            if (std::fabs(pred - dyf) > 0.01 + 1e-3 * std::fabs(dyf)) {
                m_stats.modelMismatches++;
                d.sinceFit = 100;
            }
        }
        d.vo[static_cast<size_t>(d.head)] = vo;
        d.vn[static_cast<size_t>(d.head)] = vn;
        d.dy[static_cast<size_t>(d.head)] = dyf;
        d.head = (d.head + 1) % 64;
        if (d.n < 64) d.n++;
        d.sinceFit++;
        if (d.n >= 6 && (d.sinceFit >= 16 || !d.known)) {
            d.sinceFit = 0;
            double svv = 0, svn = 0, snn = 0, sdv = 0, sdn = 0;
            for (int i = 0; i < d.n; i++) {
                double a = d.vo[static_cast<size_t>(i)];
                double b = d.vn[static_cast<size_t>(i)];
                double y = d.dy[static_cast<size_t>(i)];
                svv += a * a;
                svn += a * b;
                snn += b * b;
                sdv += y * a;
                sdn += y * b;
            }
            double det = svv * snn - svn * svn;
            if (std::fabs(det) > 1e-9 * std::max(1.0, svv * snn) && snn > 1e-9) {
                double a = (sdv * snn - sdn * svn) / det;
                double b = (svv * sdn - svn * sdv) / det;
                if (std::isfinite(a) && std::isfinite(b)) {
                    d.a = a;
                    d.b = b;
                    d.known = true;
                }
            }
            else if (snn > 1e-9) {
                d.a = 0.0;
                d.b = sdn / snn;
                d.known = true;
            }
            if (d.known && std::fabs(d.a) < 1e-9) d.a = 0.0;
        }
    }

    static bool twoMeans(double const* v, int n, double tol, double& lo, double& hi) {
        if (n <= 0) return false;
        double tmp[128];
        int m = std::min(n, 128);
        std::copy(v, v + m, tmp);
        std::sort(tmp, tmp + m);
        lo = tmp[m / 4];
        hi = tmp[(3 * m) / 4];
        if (hi - lo < 3.0 * tol) {
            lo = hi = tmp[m / 2];
            if (tmp[m - 1] - tmp[0] < 3.0 * tol) return false;
            lo = tmp[0];
            hi = tmp[m - 1];
        }
        for (int it = 0; it < 8; it++) {
            double a[128];
            double c[128];
            int na = 0;
            int nc = 0;
            for (int i = 0; i < m; i++) {
                if (std::fabs(tmp[i] - lo) <= std::fabs(tmp[i] - hi)) a[na++] = tmp[i];
                else c[nc++] = tmp[i];
            }
            if (na == 0 || nc == 0) {
                lo = hi = tmp[m / 2];
                return false;
            }
            double nlo = medianOf(a, na);
            double nhi = medianOf(c, nc);
            if (nlo == lo && nhi == hi) break;
            lo = nlo;
            hi = nhi;
        }
        if (hi - lo < 3.0 * tol) {
            lo = hi = tmp[m / 2];
            return false;
        }
        return true;
    }

    void fitGroup(Ring<32> const* rings, double sign, double tol, double& cf, double& cr, bool& hasF, bool& hasR, bool& split, double* meanVo) {
        double dv[128];
        double vo[128];
        int n = 0;
        for (int u = 0; u < 4; u++) {
            for (int i = 0; i < rings[u].n && n < 128; i++) {
                int j = rings[u].at(i);
                dv[n] = rings[u].b[static_cast<size_t>(j)] * sign;
                vo[n] = rings[u].a[static_cast<size_t>(j)];
                n++;
            }
        }
        split = false;
        if (n == 0) return;
        double lo = 0.0;
        double hi = 0.0;
        if (!twoMeans(dv, n, tol, lo, hi)) {
            double tmp[128];
            std::copy(dv, dv + n, tmp);
            double med = medianOf(tmp, n);
            if (!hasF && !hasR) {
                cf = cr = med;
                hasF = hasR = true;
            }
            else {
                if (hasF) cf = med;
                if (hasR) cr = med;
                if (!hasF) {
                    cf = med;
                    hasF = true;
                }
                if (!hasR) {
                    cr = med;
                    hasR = true;
                }
            }
            return;
        }
        double sLo = 0.0;
        double sHi = 0.0;
        int nLo = 0;
        int nHi = 0;
        for (int i = 0; i < n; i++) {
            if (std::fabs(dv[i] - lo) <= std::fabs(dv[i] - hi)) {
                sLo += vo[i];
                nLo++;
            }
            else {
                sHi += vo[i];
                nHi++;
            }
        }
        double mLo = nLo ? sLo / nLo : 0.0;
        double mHi = nHi ? sHi / nHi : 0.0;
        if (mLo <= mHi) {
            cf = lo;
            cr = hi;
        }
        else {
            cf = hi;
            cr = lo;
        }
        hasF = hasR = true;
        split = true;
        if (meanVo) {
            meanVo[0] = std::min(mLo, mHi);
            meanVo[1] = std::max(mLo, mHi);
        }
    }

    void fitFly(FlyModel& f, PS const& ps, bool ufo) {
        double tol = 0.004;
        bool splitH = false;
        bool splitR = false;
        if (!ufo) fitGroup(f.up, 1.0, tol, f.a1, f.a2, f.hasA1, f.hasA2, splitH, nullptr);
        fitGroup(f.down, -1.0, tol, f.d1, f.d2, f.hasD1, f.hasD2, splitR, nullptr);
        f.splitH = splitH;
        f.splitR = splitR;
        for (int u = 0; u < 2; u++) {
            double vo[128];
            uint8_t fall[128];
            int n = 0;
            auto add = [&](Ring<32> const& r, double cf, double cr, double sign) {
                for (int i = 0; i < r.n && n < 128; i++) {
                    int j = r.at(i);
                    double dv = r.b[static_cast<size_t>(j)] * sign;
                    vo[n] = r.a[static_cast<size_t>(j)];
                    fall[n] = std::fabs(dv - cf) <= std::fabs(dv - cr) ? 1 : 0;
                    n++;
                }
            };
            for (int side = 0; side < 2; side++) {
                if (!ufo && splitH) add(f.up[u * 2 + side], f.a1, f.a2, 1.0);
                if (splitR) add(f.down[u * 2 + side], f.d1, f.d2, -1.0);
            }
            if (n == 0) continue;
            int idx[128];
            for (int i = 0; i < n; i++) idx[i] = i;
            std::sort(idx, idx + n, [&](int x, int y) { return vo[x] < vo[y]; });
            int riseBelow = 0;
            int fallAbove = 0;
            for (int i = 0; i < n; i++) fallAbove += fall[i];
            int bestErr = fallAbove;
            int bestPos = 0;
            for (int p = 0; p < n; p++) {
                int i = idx[p];
                if (fall[i]) fallAbove--;
                else riseBelow++;
                int err = riseBelow + fallAbove;
                if (err < bestErr) {
                    bestErr = err;
                    bestPos = p + 1;
                }
            }
            bool anyFall = false;
            bool anyRise = false;
            for (int i = 0; i < n; i++) {
                if (fall[i]) anyFall = true;
                else anyRise = true;
            }
            if (!anyFall || !anyRise) {
                if (anyFall && !f.thrKnown[u]) f.thr[u] = std::max(priorThr(ps, u), vo[idx[n - 1]] + 1e-6);
                if (anyRise && !f.thrKnown[u]) f.thr[u] = std::min(priorThr(ps, u), vo[idx[0]]);
                continue;
            }
            double t = 0.0;
            if (bestPos <= 0) t = vo[idx[0]];
            else if (bestPos >= n) t = vo[idx[n - 1]] + 1e-6;
            else t = 0.5 * (vo[idx[bestPos - 1]] + vo[idx[bestPos]]);
            f.thr[u] = t;
            f.thrKnown[u] = true;
        }
        mergeExtremes(f, ufo, tol);
        for (int u = 0; u < 2; u++) {
            bool hf = f.maxFall[u] > -1e17;
            bool hr = f.minRise[u] < 1e17;
            if (hf && hr && f.maxFall[u] < f.minRise[u]) {
                f.thr[u] = 0.5 * (f.maxFall[u] + f.minRise[u]);
                f.thrKnown[u] = true;
                continue;
            }
            if (hr && f.thr[u] > f.minRise[u]) f.thr[u] = f.minRise[u];
            if (hf && f.thr[u] <= f.maxFall[u]) f.thr[u] = f.maxFall[u] + 1e-6;
        }
        f.sinceFit = 0;
    }

    static void noteExtreme(FlyModel& f, int u, double vo, bool isFall) {
        if (isFall) f.maxFall[u] = std::max(f.maxFall[u], vo);
        else f.minRise[u] = std::min(f.minRise[u], vo);
        if (f.maxFall[u] >= f.minRise[u]) {
            f.maxFall[u] = isFall ? vo : -1e18;
            f.minRise[u] = isFall ? 1e18 : vo;
        }
    }

    void mergeExtremes(FlyModel& f, bool ufo, double tol) {
        bool useH = !ufo && f.splitH && std::fabs(f.a1 - f.a2) > 3.0 * tol;
        bool useR = f.splitR && std::fabs(f.d1 - f.d2) > 3.0 * tol;
        double c[4] = {useH ? f.a1 : 0.0, useH ? f.a2 : 0.0, useR ? f.d1 : 0.0, useR ? f.d2 : 0.0};
        bool changed = false;
        for (int i = 0; i < 4; i++) {
            if (f.exC[i] != 0.0 && c[i] != 0.0 && std::fabs(f.exC[i] - c[i]) > 3.0 * tol) changed = true;
        }
        if (changed) {
            for (int u = 0; u < 2; u++) {
                f.maxFall[u] = -1e18;
                f.minRise[u] = 1e18;
            }
        }
        for (int i = 0; i < 4; i++) {
            if (c[i] != 0.0) f.exC[i] = c[i];
        }
        for (int u = 0; u < 2; u++) {
            for (int side = 0; side < 2; side++) {
                auto scan = [&](Ring<32> const& r, double cf, double cr, double sign) {
                    for (int i = 0; i < r.n; i++) {
                        int j = r.at(i);
                        double vo = r.a[static_cast<size_t>(j)];
                        double dv = r.b[static_cast<size_t>(j)] * sign;
                        double tolV = 0.004 + 1e-3 * std::fabs(vo + dv);
                        bool isFall = std::fabs(dv - cf) < std::fabs(dv - cr);
                        if (std::fabs(dv - (isFall ? cf : cr)) <= tolV) noteExtreme(f, u, vo, isFall);
                    }
                };
                if (useH) scan(f.up[u * 2 + side], f.a1, f.a2, 1.0);
                if (useR) scan(f.down[u * 2 + side], -f.d1, -f.d2, 1.0);
            }
        }
    }

    double sharedThr(FlyModel const& f, PS const& ps, int u) const {
        double own = flyThr(f, ps, u);
        int m = ps.mini ? 1 : 0;
        FlyModel const* other = &f == &m_ship[m] ? &m_ufo[m] : (&f == &m_ufo[m] ? &m_ship[m] : nullptr);
        if (!other) return own;
        double lo = std::max(f.maxFall[u], other->maxFall[u]);
        double hi = std::min(f.minRise[u], other->minRise[u]);
        if (!(lo > -1e17) || !(hi < 1e17) || !(lo < hi)) return own;
        double ownWidth = std::min(f.minRise[u], 1e9) - std::max(f.maxFall[u], -1e9);
        if (ownWidth <= hi - lo + 1e-9) return own;
        bool inside = own > lo && own <= hi;
        if (inside && hi - lo >= 0.5 * ownWidth) return own;
        return 0.5 * (lo + hi);
    }

    double flyThr(FlyModel const& f, PS const& ps, int u) const {
        if (f.thrKnown[u]) return f.thr[u];
        if (f.thrKnown[1 - u]) return f.thr[1 - u];
        if (f.thr[u] != 0.0) return f.thr[u];
        return priorThr(ps, u);
    }

    bool flyPredict(FlyModel const& f, PS const& ps, double vo, bool held, bool edge, bool ufo, double& vn, int& br) const {
        int u = ps.upside ? 1 : 0;
        double thr = sharedThr(f, ps, u);
        bool falling = vo < thr;
        bool q = m_qYes >= 16 && m_qNo * 20 < m_qYes;
        if (ufo && edge) {
            br = -1;
            if (!f.impKnown) return false;
            vn = vo < f.imp ? f.imp : vo + (f.flapHiKnown ? f.flapHi : 0.0);
            return true;
        }
        if (!ufo && held) {
            br = falling ? 0 : 1;
            bool has = falling ? f.hasA1 : f.hasA2;
            if (!has) return false;
            vn = vo + (falling ? f.a1 : f.a2);
        }
        else {
            br = falling ? 2 : 3;
            bool has = falling ? f.hasD1 : f.hasD2;
            if (!has) return false;
            vn = vo - (falling ? f.d1 : f.d2);
        }
        if (q) vn = quant3(vn);
        if (f.capUpKnown) vn = std::min(vn, f.capUp);
        if (f.capDownKnown) vn = std::max(vn, -f.capDown);
        return true;
    }

    static void forgetFly(FlyModel& f, bool up) {
        for (int i = 0; i < 4; i++) {
            if (up) f.up[i].clear();
            else f.down[i].clear();
        }
        for (int u = 0; u < 2; u++) {
            f.maxFall[u] = -1e18;
            f.minRise[u] = 1e18;
            f.thrKnown[u] = false;
            f.thr[u] = 0.0;
        }
        for (double& c : f.exC) c = 0.0;
        for (auto& b : f.b) b.hits = 0;
    }

    void learnFly(FlyModel& f, PS const& pre, bool held, bool edge, double vo, double vn, bool ufo) {
        double tolV = 0.004 + 1e-3 * std::fabs(vn);
        int u = pre.upside ? 1 : 0;
        double pred = 0.0;
        int br = -1;
        bool had = flyPredict(f, pre, vo, held, edge, ufo, pred, br);
        if (ufo && edge) {
            if (had) {
                if (std::fabs(pred - vn) <= tolV) f.bFlap.hit();
                else {
                    m_stats.modelMismatches++;
                    if (f.bFlap.miss()) {
                        f.flap.keepRecent(2);
                        f.flapHiR.keepRecent(2);
                    }
                }
            }
            if (vn > vo + tolV) {
                f.flap.push(vn, vo);
                f.imp = ringMedianA(f.flap);
                f.impKnown = true;
            }
            else {
                f.flapHiR.push(vn - vo, 0.0);
                f.flapHi = ringMedianA(f.flapHiR);
                f.flapHiKnown = true;
            }
            f.bFlap.samples++;
            return;
        }
        double dv = vn - vo;
        bool up = !ufo && held;
        double prevMaxUp = f.maxUp;
        double prevMaxDown = f.maxDown;
        f.maxUp = std::max(f.maxUp, vn);
        f.maxDown = std::max(f.maxDown, -vn);
        if (std::fabs(dv) < 1e-6) {
            if (up && vn > 0.0) {
                if (f.capUpKnown && std::fabs(f.capUp - vn) <= tolV) f.bCap.hit();
                f.capUp = vn;
                f.capUpKnown = true;
            }
            else if (!up && vn < 0.0) {
                if (f.capDownKnown && std::fabs(f.capDown + vn) <= tolV) f.bCap.hit();
                f.capDown = -vn;
                f.capDownKnown = true;
            }
            return;
        }
        if (up && f.capUpKnown && std::fabs(vn - f.capUp) <= tolV) return;
        if (!up && f.capDownKnown && std::fabs(vn + f.capDown) <= tolV) return;
        if (had && br >= 0 && f.b[br].hits >= kTrustN) {
            bool upOk = f.b[0].hits >= kTrustN && f.b[1].hits >= kTrustN;
            bool downOk = f.b[2].hits >= kTrustN && f.b[3].hits >= kTrustN;
            if (up && upOk && f.splitH && !f.capUpKnown && vn >= prevMaxUp - tolV && vn > 0.0 && dv > tolV && dv < std::min(f.a1, f.a2) - tolV) return;
            if (!up && downOk && (f.splitR || ufo) && !f.capDownKnown && -vn >= prevMaxDown - tolV && vn < 0.0 && dv < -tolV && -dv < std::min(f.d1, f.d2) - tolV) return;
        }
        if (up && dv <= 0.0) return;
        if (!up && dv >= 0.0) return;
        bool refit = false;
        if (had && br >= 0) {
            if (std::fabs(pred - vn) <= tolV) f.b[br].hit();
            else {
                m_stats.modelMismatches++;
                if (f.b[br].miss()) forgetFly(f, up);
                refit = true;
            }
        }
        int side = vo >= flyThr(f, pre, u) ? 1 : 0;
        if (up) f.up[u * 2 + side].push(vo, dv);
        else f.down[u * 2 + side].push(vo, dv);
        {
            bool split = up ? f.splitH : f.splitR;
            double cf = up ? f.a1 : -f.d1;
            double cr = up ? f.a2 : -f.d2;
            if (split && std::fabs(cf - cr) > 3.0 * tolV) {
                bool isFall = std::fabs(dv - cf) < std::fabs(dv - cr);
                bool clean = std::fabs(dv - (isFall ? cf : cr)) <= tolV;
                if (clean) noteExtreme(f, u, vo, isFall);
            }
        }
        f.sinceFit++;
        bool known = up ? (f.hasA1 && f.hasA2) : (f.hasD1 && f.hasD2);
        if (refit || !known || f.sinceFit >= 8 || !f.thrKnown[u]) fitFly(f, pre, ufo);
    }

    void learnCube(CubeModel& c, PS const& pre, PS const& post, bool held, double vo, double vn, double dyf) {
        double tolV = 0.004 + 1e-3 * std::fabs(vn);
        double rj = refJ(post);
        double rg = refG(pre);
        if (pre.grounded && held) {
            if (c.jump.n > 0) {
                double pred = ringMedianA(c.jump) * rj;
                if (std::fabs(pred - vn) <= tolV) c.bJump.hit();
                else {
                    m_stats.modelMismatches++;
                    if (c.bJump.miss()) c.jump.keepRecent(1);
                }
            }
            if (vn > 0.0) c.jump.push(vn / rj, 0.0);
            c.bJump.samples++;
            addDy(vo, vn, dyf);
            return;
        }
        if (pre.grounded && !held) {
            if (!post.grounded && vn <= 0.0) c.walk.push(vn, 0.0);
            return;
        }
        if (post.grounded) return;
        if (std::fabs(vn - vo) < 1e-6 && vn < 0.0) {
            if (c.capKnown && std::fabs(c.capV + vn) <= tolV) c.bCap.hit();
            c.capV = -vn;
            c.capKnown = true;
            addDy(vo, vn, dyf);
            return;
        }
        if (c.capKnown && std::fabs(vn + c.capV) <= tolV) {
            addDy(vo, vn, dyf);
            return;
        }
        if (vo - vn <= 0.0) return;
        if (vn < 0.0) c.maxFall = std::max(c.maxFall, -vn);
        if (!c.capKnown && c.air.n > 0 && c.bAir.hits >= kTrustN && vn < 0.0) {
            double g0 = ringMedianB(c.air) * rg;
            if (vo - vn < g0 - tolV) {
                c.capV = -vn;
                c.capKnown = true;
                addDy(vo, vn, dyf);
                return;
            }
        }
        if (c.air.n > 0) {
            double g = ringMedianB(c.air) * rg;
            bool q = m_qYes >= 16 && m_qNo * 20 < m_qYes;
            double pred = vo - g;
            if (q) pred = quant3(pred);
            if (std::fabs(pred - vn) <= tolV) c.bAir.hit();
            else {
                m_stats.modelMismatches++;
                if (c.bAir.miss()) c.air.keepRecent(2);
            }
        }
        c.air.push(vo, (vo - vn) / rg);
        addDy(vo, vn, dyf);
    }

    void learnWave(WaveModel& w, PS const& pre, bool held, double dyf, double dx) {
        if (!(dx > 1e-6)) return;
        double s = std::fabs(dyf) / dx;
        bool dirOk = held ? dyf > 0.0 : dyf < 0.0;
        if (!dirOk) return;
        int br = held ? 0 : 1;
        if (w.known) {
            if (std::fabs(s - w.slope) <= 0.01 + 1e-3 * s) w.b[br].hit();
            else {
                m_stats.modelMismatches++;
                if (w.b[br].miss()) w.s.keepRecent(2);
            }
        }
        (void)pre;
        w.s.push(s, 0.0);
        w.slope = ringMedianA(w.s);
        w.known = true;
    }

    static bool overlaps(PS const& a, MapObj const& o) {
        return !(o.x1 < a.x - a.hw || o.x0 > a.x + a.hw || o.y1 < a.y - a.hh || o.y0 > a.y + a.hh);
    }

    int orbFiredAt(PS const& pre, PS const& post, double& g) const {
        auto const& c = m_cube[pre.mini ? 1 : 0];
        if (c.air.n == 0) return -1;
        g = ringMedianB(c.air) * refG(pre);
        if (!(g > 0.0)) return -1;
        double a = m_dy.known ? m_dy.a : 0.0;
        double b = m_dy.known ? m_dy.b : 0.225;
        double flip = pre.upside ? -1.0 : 1.0;
        double vo = pre.v * flip;
        double vp = post.v * flip;
        double dy = (post.y - pre.y) * flip;
        double dyPost = a * vo + b * (vo - g);
        double dyPre = a * (vp + g) + b * vp;
        double sep = std::fabs(dyPre - dyPost);
        if (!(sep >= 0.5)) return -1;
        double ePre = std::fabs(dy - dyPre);
        double ePost = std::fabs(dy - dyPost);
        if (ePre < 0.2 * sep && ePre < ePost) return 1;
        if (ePost < 0.2 * sep && ePost < ePre) return 0;
        return -1;
    }

    int touching(PS const& a, double pad, int* out, int cap) const {
        int n = 0;
        double x0 = a.x - a.hw - pad;
        double x1 = a.x + a.hw + pad;
        double y0 = a.y - a.hh - pad;
        double y1 = a.y + a.hh + pad;
        query(x0, x1, [&](int i, MapObj const& o) {
            if (o.x1 < x0 || o.x0 > x1 || o.y1 < y0 || o.y0 > y1) return;
            if (n < cap) out[n++] = i;
        });
        return n;
    }

    void learnPortal(PS const& pre, PS const& post, bool held, bool prevHeld, int since) {
        auto ti = m_portalType.find(post.portal);
        if (ti == m_portalType.end()) return;
        int kind = portalKind(ti->second);
        if (kind < 0) return;
        if (m_portalFx.size() >= 4096 && !m_portalFx.count(post.portal)) return;
        PortalFx& fx = m_portalFx[post.portal];
        fx.kind = static_cast<uint8_t>(kind);
        fx.mode = post.mode;
        fx.mini = post.mini;
        fx.upside = post.upside;
        fx.lo = post.lo;
        fx.hi = post.hi;
        fx.hw = post.hw;
        fx.hh = post.hh;
        fx.gh = post.gh;
        fx.known = post.mode < 4 && pre.mode < 4;
        if (kind == 2 && post.mode != pre.mode) fx.known = false;
        bool flyNew = post.mode == MShip || post.mode == MUfo || post.mode == MWave || post.mode == MSwing;
        bool flipG = post.upside != pre.upside;
        double prior = kind == 0 ? (flyNew && post.mode != pre.mode ? 0.5 : 1.0) : (kind == 1 ? 0.5 : 1.0);
        bool applies = kind == 0 || (kind == 1 && flipG);
        Phys P;
        double dx = 0.0;
        if (applies && pre.mode < 4 && level(pre.mode, pre.mini) >= 3 && dxFor(pre.speed, dx) && resolve(pre, P, false, 3)) {
            SimState st = simFrom(pre, dx, prevHeld, since, -1, -1);
            double flip = pre.upside ? -1.0 : 1.0;
            double vo = pre.v * flip;
            double vn = vo;
            double dyf = 0.0;
            bool jumped = false;
            dynamics(P, st, vo, held, held && !prevHeld, vn, dyf, jumped);
            double vDyn = vn * flip;
            if (std::fabs(vDyn) > 0.3) {
                double f = post.v / vDyn;
                double tolV = 0.004 + 1e-3 * std::fabs(post.v);
                if (std::fabs(f - prior) < 0.2) f = prior;
                for (double c : {0.5, 1.0}) {
                    double pv = snapGrid(vDyn * c);
                    if (std::fabs(pv - post.v) <= tolV && std::fabs(f - c) < 0.1) {
                        f = c;
                        break;
                    }
                }
                if (std::isfinite(f) && std::fabs(f) > 0.05 && std::fabs(f) < 2.0) {
                    if (!fx.vKnown || std::fabs(vDyn) >= fx.vAbs) {
                        fx.vFactor = f;
                        fx.vAbs = std::fabs(vDyn);
                    }
                    fx.vKnown = true;
                    return;
                }
            }
        }
        if (!fx.vKnown) fx.vFactor = prior;
    }

    void learn(int t, TraceEntry const& e, bool held, bool prevHeld, TraceEntry& n) {
        PS const& pre = e.s;
        PS const& post = n.s;
        if (!m_built || !pre.valid || !post.valid || pre.dead || post.dead) return;
        if (post.portal && post.portal != pre.portal && !pre.special && !post.special) {
            learnPortal(pre, post, held, prevHeld, sinceFromTrace(t));
            notePortalType(post.portal, pre.mode);
        }
        else if (post.portal && post.portal == pre.portal && pre.mode == post.mode && !post.special) {
            auto it = m_portalFx.find(post.portal);
            if (it != m_portalFx.end() && it->second.kind == 0 && it->second.mode == post.mode) {
                it->second.lo = post.lo;
                it->second.hi = post.hi;
            }
        }
        if (pre.special || post.special) return;
        if (pre.mode != post.mode || pre.mini != post.mini) return;
        if (pre.mode >= 4) return;
        double dx = post.x - pre.x;
        if (std::fabs(pre.speed - post.speed) < 0.005f && dx > 0.0 && dx < 100.0) {
            SpeedDx* sd = speedSlot(pre.speed, true);
            if (sd) {
                if (sd->r.n > 0 && std::fabs(sd->dx - dx) > 1e-3) {
                    m_stats.modelMismatches++;
                    if (sd->miss > 0 && std::fabs(sd->missDx - dx) > 1e-3) sd->miss = 0;
                    sd->missDx = dx;
                    if (++sd->miss >= 3) {
                        sd->r.keepRecent(2);
                        sd->miss = 0;
                    }
                }
                else {
                    sd->miss = 0;
                }
                sd->r.push(dx, 0.0);
                sd->dx = ringMedianA(sd->r);
                if (validJ(pre.yStart) && validG(pre.grav) && pre.yStart == post.yStart && pre.grav == post.grav) {
                    sd->yStart = pre.yStart;
                    sd->grav = pre.grav;
                }
            }
        }
        int mk = mkOf(pre.mode, pre.mini);
        bool edge = held && !prevHeld;
        bool flying = pre.mode != MCube;
        int cand[kMaxCand];
        int nc = touching(post, 0.0, cand, kMaxCand);
        bool eventFired = false;
        int usedBefore = edge ? -1 : e.usedOrb;
        for (int c = 0; c < nc; c++) {
            MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
            if (o.kind == KPad && cand[c] != e.lastPad && o.type >= 0 && o.type < kTypeSlots) {
                Effect& fx = m_padFx[o.type][mk];
                double vF = post.v * (post.upside ? -1.0 : 1.0);
                bool flips = post.upside != pre.upside;
                if (fx.known && std::fabs(fx.v - vF) <= 0.01 + 1e-3 * std::fabs(vF) && fx.flips == flips) fx.hits++;
                else if (fx.known) m_stats.modelMismatches++;
                fx.v = vF;
                fx.flips = flips;
                fx.known = true;
                n.lastPad = cand[c];
                eventFired = true;
            }
            if (o.kind == KOrb && !flying && cand[c] != usedBefore && held && (pre.fresh || edge) && !(pre.grounded && held) &&
                o.type >= 0 && o.type < kTypeSlots) {
                double vF = post.v * (post.upside ? -1.0 : 1.0);
                double voF = pre.v * (pre.upside ? -1.0 : 1.0);
                bool flips = post.upside != pre.upside;
                if (flips || vF > voF + 0.05) {
                    Effect& fx = m_orbFx[o.type][mk];
                    double ringV = vF;
                    bool use = true;
                    if (edge && overlaps(pre, o)) {
                        double g = 0.0;
                        int timing = flips ? -1 : orbFiredAt(pre, post, g);
                        if (timing == 1) {
                            m_orbPreVotes = std::min(m_orbPreVotes + 1, 1 << 20);
                            ringV = vF + g;
                        }
                        else if (timing == 0) {
                            m_orbPostVotes = std::min(m_orbPostVotes + 1, 1 << 20);
                        }
                        else {
                            use = false;
                        }
                    }
                    if (use) {
                        double jr = validJ(pre.yStart) ? static_cast<double>(pre.yStart) : 0.0;
                        double expect = orbV(fx, jr);
                        if (fx.known && std::fabs(expect - ringV) <= 0.01 + 1e-3 * std::fabs(ringV) && fx.flips == flips) fx.hits++;
                        else if (fx.known) m_stats.modelMismatches++;
                        fx.v = ringV;
                        fx.jRef = jr;
                        fx.flips = flips;
                        fx.known = true;
                    }
                    n.usedOrb = cand[c];
                    eventFired = true;
                }
            }
            if (o.kind == KSlope) eventFired = true;
        }
        if (post.portal != pre.portal || std::fabs(post.lo - pre.lo) > 1e-3 || std::fabs(post.hi - pre.hi) > 1e-3) eventFired = true;
        if (pre.upside != post.upside) return;
        if (eventFired) return;
        double flip = pre.upside ? -1.0 : 1.0;
        double vo = pre.v * flip;
        double vn = post.v * flip;
        double dyf = (post.y - pre.y) * flip;
        int nearIdx[kMaxCand];
        bool solidNear = false;
        for (int side = 0; side < 2 && !solidNear; side++) {
            int nn = touching(side == 0 ? pre : post, 0.05, nearIdx, kMaxCand);
            for (int c = 0; c < nn; c++) {
                MapObj const& o = m_objs[static_cast<size_t>(nearIdx[c])];
                if (o.kind == KPad || o.kind == KOrb || o.kind == KSlope) solidNear = true;
                if (side == 1 && o.kind == KSolid) solidNear = true;
            }
        }
        if (!flying) {
            if (post.grounded && flip > 0) {
                bool onSolid = false;
                for (int c = 0; c < nc; c++) {
                    MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
                    if (o.kind == KSolid && std::fabs((post.y - post.hh) - o.y1) < 0.05) onSolid = true;
                }
                if (!onSolid) {
                    double feet = post.y - post.hh;
                    if (!m_floorKnown || std::fabs(feet - m_floorFeet) > 1e-3) {
                        m_floorFeet = feet;
                        m_floorKnown = true;
                    }
                }
            }
            if (!pre.grounded && post.grounded) {
                for (int c = 0; c < nc; c++) {
                    MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
                    if (o.kind != KSolid) continue;
                    double topF = flip > 0 ? o.y1 : -static_cast<double>(o.y0);
                    double bottomF = post.y * flip - post.hh;
                    if (std::fabs(bottomF - topF) > 0.05) continue;
                    double prevBottomF = pre.y * flip - pre.hh;
                    double need = topF - prevBottomF;
                    if (need > 0.0 && need < 15.0) m_tolCube = std::max(m_tolCube, need + 0.01);
                }
            }
        }
        bool boundNear = false;
        if (flying) {
            double lo = post.lo;
            double hi = post.hi;
            double hl = m_bHalfLoKnown[mk] ? m_bHalfLo[mk] : post.gh;
            double hh = m_bHalfHiKnown[mk] ? m_bHalfHi[mk] : post.gh;
            if (std::fabs(post.y - (lo + hl)) < 0.05 || std::fabs((hi - hh) - post.y) < 0.05) boundNear = true;
            if (std::fabs(post.lo - pre.lo) > 1e-3 || std::fabs(post.hi - pre.hi) > 1e-3) boundNear = true;
            if (!solidNear && std::fabs(post.y - pre.y) < 1e-4 && (pre.mode == MWave || std::fabs(post.v) < 1e-6)) {
                double voF = pre.v * flip;
                bool pushDown = (pre.mode == MShip || pre.mode == MWave) ? !held : !edge;
                bool pushUp = (pre.mode == MShip || pre.mode == MWave) ? held : false;
                double dLo = post.y - lo;
                double dHi = hi - post.y;
                if (flip < 0) {
                    std::swap(pushDown, pushUp);
                    pushDown = pushDown && voF >= 0.0;
                    pushUp = pushUp && voF <= 0.0;
                }
                else {
                    pushDown = pushDown && voF <= 0.0;
                    pushUp = pushUp && voF >= 0.0;
                }
                if (pre.mode == MWave) {
                    pushDown = flip > 0 ? !held : held;
                    pushUp = flip > 0 ? held : !held;
                }
                if (pushDown && dLo > 0.0 && dLo < 3.0 * post.hh) {
                    if (std::fabs(m_bHalfLoCand[mk] - dLo) < 0.01) {
                        m_bHalfLo[mk] = dLo;
                        m_bHalfLoKnown[mk] = true;
                    }
                    m_bHalfLoCand[mk] = dLo;
                    boundNear = true;
                }
                if (pushUp && dHi > 0.0 && dHi < 3.0 * post.hh) {
                    if (std::fabs(m_bHalfHiCand[mk] - dHi) < 0.01) {
                        m_bHalfHi[mk] = dHi;
                        m_bHalfHiKnown[mk] = true;
                    }
                    m_bHalfHiCand[mk] = dHi;
                    boundNear = true;
                }
            }
        }
        if (!flying && pre.grounded && post.grounded) return;
        if (solidNear || boundNear) return;
        if (m_dy.known && pre.mode != MWave) {
            double pred = m_dy.a * vo + m_dy.b * vn;
            if (std::fabs(pred - dyf) > 0.02 + 2e-3 * std::fabs(dyf)) {
                if (++m_dyReject > 40) {
                    m_dy.known = false;
                    m_dy.n = 0;
                    m_dy.head = 0;
                    m_dyReject = 0;
                }
                return;
            }
            m_dyReject = 0;
        }
        double vq = post.v * 1000.0;
        if (pre.mode != MWave && !(pre.mode == MCube && pre.grounded && held)) {
            if (std::fabs(vq - std::round(vq)) < 1e-6) m_qYes++;
            else m_qNo++;
        }
        if (pre.mode != MWave) noteGrid(post.v, vn - vo);
        switch (pre.mode) {
            case MCube:
                learnCube(m_cube[pre.mini ? 1 : 0], pre, post, held, vo, vn, dyf);
                break;
            case MShip:
                learnFly(m_ship[pre.mini ? 1 : 0], pre, held, edge, vo, vn, false);
                if (!(std::fabs(vn - vo) < 1e-9)) addDy(vo, vn, dyf);
                break;
            case MUfo:
                learnFly(m_ufo[pre.mini ? 1 : 0], pre, held, edge, vo, vn, true);
                addDy(vo, vn, dyf);
                break;
            case MWave:
                learnWave(m_wave[pre.mini ? 1 : 0], pre, held, dyf, dx);
                break;
            default:
                break;
        }
        (void)t;
    }

    void observe(int tick, uint8_t held, bool died, bool betweenFrames) {
        m_stats.observed++;
        if (tick < 0) return;
        if (tick > m_passedTick + 64 && !m_deathTries.empty()) {
            m_passedTick = tick;
            for (auto it = m_deathTries.begin(); it != m_deathTries.end();) {
                if (it->first + 2 < tick) it = m_deathTries.erase(it);
                else ++it;
            }
        }
        if (tick + 1 >= kMaxTrace) return;
        if (!m_built && m_layer && m_layer->m_objects && m_layer->m_objects->count() > 0) buildMap();
        PS post;
        bool ok = readPlayer(post, betweenFrames);
        if (!entry(tick + 1)) return;
        m_curTick = tick + 1;
        if (ok && m_built) {
            refreshRange(post.x - post.hw - 8.0, post.x + post.hw + 8.0, m_curTick);
            maybeRebucket();
        }
        TraceEntry* e = &m_trace[static_cast<size_t>(tick)];
        TraceEntry* n = &m_trace[static_cast<size_t>(tick) + 1];
        e->held = held;
        e->heldValid = true;
        if (m_adviceTick == tick) {
            if ((held & 1) != (m_adviceVal & 1)) m_stats.deviations++;
            m_adviceTick = -1;
        }
        n->valid = ok;
        n->heldValid = false;
        n->died = died;
        if (ok) n->s = post;
        n->lastPad = e->valid ? e->lastPad : -1;
        n->usedOrb = e->valid ? e->usedOrb : -1;
        if (e->valid && (held & 1) && !(tick > 0 && m_trace[static_cast<size_t>(tick) - 1].heldValid && (m_trace[static_cast<size_t>(tick) - 1].held & 1))) {
            n->usedOrb = -1;
        }
        m_traceEnd = std::max(m_traceEnd, tick + 1);
        if (ok && e->valid && !died && m_built) {
            bool prevHeld = false;
            if (tick > 0) {
                auto const& p = m_trace[static_cast<size_t>(tick) - 1];
                prevHeld = p.heldValid && (p.held & 1);
            }
            learn(tick, *e, (held & 1) != 0, prevHeld, *n);
            noteSoftSurvival(n->s);
            noteTouch(n->s, false);
        }
        if (m_lastHint.active && tick > m_lastHint.death + 1) {
            bool v = false;
            if (traceHeld(m_lastHint.tick, v) && (v ? 1 : 0) == (m_lastHint.held & 1)) m_hintWins++;
            m_lastHint.active = false;
        }
        if (m_pending.active && tick >= m_pending.death + 30) {
            int found = -1;
            for (int k = m_pending.from; k <= m_pending.death; k++) {
                bool v = false;
                if (!traceHeld(k, v)) break;
                size_t idx = static_cast<size_t>(k - m_pending.from);
                if (idx >= m_pending.tail.size()) break;
                if ((v ? 1 : 0) != (m_pending.tail[idx] & 1)) {
                    found = k;
                    break;
                }
            }
            if (found >= 0 && m_pending.key >= 0 && m_pending.key < 64) {
                auto& list = m_offsets[static_cast<size_t>(m_pending.key)];
                list.push_back(m_pending.death - found);
                if (list.size() > 16) list.erase(list.begin());
            }
            m_pending.active = false;
        }
    }

    bool planTouches(int tick) const {
        if (!m_plan.valid || m_plan.pred.empty() || m_changed.empty()) return false;
        PS const* ps = nullptr;
        TraceEntry const* te = entryAt(tick);
        if (te) ps = &te->s;
        double hw = ps ? ps->hw + 12.0 : 27.0;
        double hh = ps ? ps->hh + 12.0 : 27.0;
        size_t from = tick > m_plan.t0 ? static_cast<size_t>(tick - m_plan.t0) : 0;
        for (auto const& c : m_changed) {
            for (size_t i = from; i < m_plan.pred.size(); i += 2) {
                SimState const& p = m_plan.pred[i];
                if (p.x + hw < c.x0 || p.x - hw > c.x1 || p.y + hh < c.y0 || p.y - hh > c.y1) continue;
                return true;
            }
        }
        return false;
    }

    PlannerAdvice emit(int tick, uint8_t bit, uint8_t prevHeld) {
        PlannerAdvice a;
        a.held = static_cast<uint8_t>((prevHeld & ~1) | (bit & 1));
        a.lanes = 1;
        m_stats.advised++;
        if ((bit & 1) && !(prevHeld & 1)) m_stats.advisedPress++;
        m_adviceTick = tick;
        m_adviceVal = bit;
        return a;
    }

    PlannerAdvice advise(int tick, uint8_t prevHeld) {
        PlannerAdvice none;
        m_hardCap = 0;
        m_strictOnly = false;
        if (!m_built || !m_layer) return none;
        if (m_layer->m_objects && m_layer->m_objects->count() != m_sourceCount) buildMap();
        PS live;
        if (!readPlayer(live)) return none;
        m_curTick = tick;
        {
            bool news = refreshRange(live.x - kNearBehind, live.x + kNearAhead, tick, true);
            maybeRebucket();
            if (news && m_plan.valid && (tick - m_lastMoveReplan >= m_moveGap || tick < m_lastMoveReplan) && planTouches(tick)) {
                int since = tick - m_lastMoveReplan;
                m_moveGap = (since >= 0 && since < 120) ? std::min(m_moveGap * 2, 64) : kMoveReplanGap;
                m_plan.replanAt = std::min(m_plan.replanAt, tick);
                m_lastMoveReplan = tick;
                m_moveReplans++;
            }
        }
        TraceEntry* e = entry(tick);
        if (e && !e->valid) {
            e->s = live;
            e->valid = true;
            TraceEntry const* p = entryAt(tick - 1);
            e->lastPad = p ? p->lastPad : -1;
            e->usedOrb = p ? p->usedOrb : -1;
        }
        if (!eligible(live)) {
            m_plan.valid = false;
            return none;
        }
        bool prev = (prevHeld & 1) != 0;
        bool haveOld = false;
        size_t oldIdx = 0;
        if (m_plan.valid && tick >= m_plan.t0 && tick < m_plan.end()) {
            size_t i = static_cast<size_t>(tick - m_plan.t0);
            bool match = matches(m_plan.pred[i], live, m_plan.mode) && m_plan.pred[i].held == prev;
            haveOld = match && !m_plan.failed;
            oldIdx = i;
            bool due = tick >= m_plan.replanAt;
            if (match && !due) return emit(tick, m_plan.held[i], prevHeld);
            if (!match && tick - m_lastMismatchReplan < 4 && !due) return emit(tick, m_plan.held[i], prevHeld);
            if (!match) m_lastMismatchReplan = tick;
        }
        double dx = 0.0;
        if (!dxFor(live.speed, dx)) {
            m_plan.valid = false;
            return none;
        }
        int lp = e ? e->lastPad : -1;
        int uo = e ? e->usedOrb : -1;
        SimState s0 = simFrom(live, dx, prev, sinceFromTrace(tick), lp, uo);
        auto t0 = Clock::now();
        Plan plan;
        int dt = -1;
        m_guideLen = 0;
        if (haveOld) {
            m_guide.assign(m_plan.held.begin() + static_cast<long>(oldIdx), m_plan.held.end());
            m_guideLen = static_cast<int>(m_guide.size());
        }
        bool ok = false;
        prepareMap(tick, live.x, dx, live.hw, kCubeLook + 60);
        m_work = 0;
        m_workCap = kAdviseWork;
        bool made = makePlan(tick, s0, live, plan, dt, m_adviseLevel, &ok);
        m_workCap = 0;
        m_guideLen = 0;
        m_stats.planMicros += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        if (haveOld && !ok && (!made || !plan.valid || (dt >= 0 && dt < m_plan.end()))) {
            m_plan.replanAt = m_plan.mode == MCube ? m_plan.end() : tick + 10;
            return emit(tick, m_plan.held[oldIdx], prevHeld);
        }
        if (!made || !plan.valid || plan.held.empty()) {
            m_plan.valid = false;
            return none;
        }
        m_plan = std::move(plan);
        return emit(tick, m_plan.held[0], prevHeld);
    }

    std::optional<PlannerHint> genericHint(int d, std::vector<uint8_t> const& path, int key) {
        if (key < 0 || key >= 64) return std::nullopt;
        auto const& list = m_offsets[static_cast<size_t>(key)];
        if (list.empty()) return std::nullopt;
        std::vector<int> tmp(list.begin(), list.end());
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(tmp.size() / 2), tmp.end());
        int off = tmp[tmp.size() / 2];
        int k = d - off;
        if (k < 0 || k > d || k >= static_cast<int>(path.size())) return std::nullopt;
        TraceEntry const* te = entryAt(k);
        uint8_t mode = te ? te->s.mode : MCube;
        bool prevDown = k > 0 && (path[static_cast<size_t>(k) - 1] & 1);
        if (!canFlip(k, familyOfMode(mode), prevDown, sinceFromPath(path, k))) return std::nullopt;
        PlannerHint h;
        h.tick = k;
        h.held = static_cast<uint8_t>(path[static_cast<size_t>(k)] ^ 1);
        if (((h.held ^ (k > 0 ? path[static_cast<size_t>(k) - 1] : 0)) & 1) == 0 && !((path[static_cast<size_t>(k)] ^ h.held) & 1)) return std::nullopt;
        return h;
    }

    int attribute(DeathInfo const& info, int d) {
        TraceEntry const* te = entryAt(d + 1);
        if (!te) te = entryAt(d);
        if (te && m_built) {
            refreshRange(te->s.x - te->s.hw - kNearBehind, te->s.x + te->s.hw + kNearBehind, m_curTick);
            maybeRebucket();
        }
        if (info.hasRect && finiteRect(info.rect)) {
            double x0 = info.rect.origin.x;
            double y0 = info.rect.origin.y;
            double x1 = x0 + info.rect.size.width;
            double y1 = y0 + info.rect.size.height;
            int found = -1;
            query(x0 - 1.0, x1 + 1.0, [&](int i, MapObj const& o) {
                if (found >= 0) return;
                if (std::fabs(o.x0 - x0) < 0.5 && std::fabs(o.x1 - x1) < 0.5 && std::fabs(o.y0 - y0) < 0.5 && std::fabs(o.y1 - y1) < 0.5 &&
                    (o.kind == KSolid || o.kind == KHazard)) {
                    found = i;
                }
            });
            if (found >= 0) return found;
            if (info.kind == DeathKind::Hazard) {
                MapObj m;
                m.x0 = static_cast<float>(x0);
                m.y0 = static_cast<float>(y0);
                m.x1 = static_cast<float>(x1);
                m.y1 = static_cast<float>(y1);
                m.cx = static_cast<float>(0.5 * (x0 + x1));
                m.cy = static_cast<float>(0.5 * (y0 + y1));
                m.kind = KHazard;
                m.type = 2;
                m.margin = 1.f;
                addExtra(m);
                return static_cast<int>(m_objs.size()) - 1;
            }
        }
        if (!te) return -1;
        PS const& p = te->s;
        TraceEntry const* pe = entryAt(d);
        double prevY = pe && pe->s.valid ? pe->s.y : p.y;
        double vs = p.mini ? 0.6 : 1.0;
        double iw = 0.3 * p.hw / vs + 1.0;
        double ih = 0.3 * p.hh / vs + 1.0;
        double px0 = p.x - p.hw;
        double px1 = p.x + p.hw;
        double py0 = p.y - p.hh;
        double py1 = p.y + p.hh;
        int cand[kMaxCand];
        int n = touching(p, 2.0, cand, kMaxCand);
        int best = -1;
        int bestScore = 0;
        double bestPen = -1.0;
        for (int c = 0; c < n; c++) {
            MapObj const& o = m_objs[static_cast<size_t>(cand[c])];
            int score = 0;
            double pen = 0.0;
            if (o.kind == KHazard) {
                if (o.r > 0.f) {
                    double qx = std::clamp(static_cast<double>(o.cx), px0 - 1.0, px1 + 1.0);
                    double qy = std::clamp(static_cast<double>(o.cy), py0 - 1.0, py1 + 1.0);
                    double ddx = qx - o.cx;
                    double ddy = qy - o.cy;
                    score = ddx * ddx + ddy * ddy <= (o.r + 1.0) * (o.r + 1.0) ? 3 : 1;
                }
                else {
                    score = (o.x1 + 1.0 >= px0 && o.x0 - 1.0 <= px1 && o.y1 + 1.0 >= py0 && o.y0 - 1.0 <= py1) ? 3 : 1;
                }
                pen = 1.0;
            }
            else if (o.kind == KSolid && !o.passable) {
                double ox = std::min(static_cast<double>(o.x1), p.x + iw) - std::max(static_cast<double>(o.x0), p.x - iw);
                double oy = std::min(static_cast<double>(o.y1), p.y + ih) - std::max(static_cast<double>(o.y0), p.y - ih);
                bool top = std::fabs((p.y - p.hh) - o.y1) < 0.05 || std::fabs((p.y + p.hh) - o.y0) < 0.05;
                bool wasAbove = prevY - p.hh >= o.y1 - 0.05;
                bool wasBelow = prevY + p.hh <= o.y0 + 0.05;
                if (ox >= 0.0 && oy >= 0.0) score = 4;
                else if (!top && !wasAbove && !wasBelow) score = 2;
                else score = 1;
                pen = std::max(0.0, std::min(ox, oy));
            }
            else {
                continue;
            }
            if (score > bestScore || (score == bestScore && pen > bestPen)) {
                best = cand[c];
                bestScore = score;
                bestPen = pen;
            }
        }
        if (bestScore >= 2) return best;
        if (best >= 0 && info.kind != DeathKind::Unknown) return best;
        if (best >= 0 && m_objs[static_cast<size_t>(best)].kind == KHazard) return best;
        return -1;
    }

    bool predictedPathSurvives(int s, int d, std::vector<uint8_t> const& path, SimState st, Phys const& P) {
        m_budget = 20000;
        for (int k = s; k <= d; k++) {
            StepInfo info;
            bool h = k < static_cast<int>(path.size()) && (path[static_cast<size_t>(k)] & 1);
            uint8_t r = step(st, h, P, info);
            if (r == RDead) return false;
            if (r == RStop) return true;
        }
        return true;
    }

    std::optional<PlannerHint> planHint(int d, std::vector<uint8_t> const& path, int killer) {
        TraceEntry const* te = entryAt(d);
        if (!te) return std::nullopt;
        uint8_t mode = te->s.mode;
        if (mode >= 4 || te->s.special) return std::nullopt;
        if (level(mode, te->s.mini) < m_hintLevel) return std::nullopt;
        int s0 = d;
        while (s0 > 0 && d - s0 < kMaxHintBack) {
            TraceEntry const* p = entryAt(s0 - 1);
            if (!p || p->s.mode != mode || p->s.mini != te->s.mini || p->s.special) break;
            s0--;
        }
        if (m_built) {
            TraceEntry const* fe = entryAt(std::max(0, d - kMaxHintBack));
            double rdx = 0.0;
            if (!dxFor(te->s.speed, rdx) || !(rdx > 0.0)) rdx = 2.0;
            double xa = (fe && fe->valid ? std::min(fe->s.x, te->s.x) : te->s.x - rdx * kMaxHintBack) - kNearBehind;
            refreshRange(xa, te->s.x + rdx * (kCubeLook + 60) + kNearBehind, m_curTick);
            maybeRebucket();
        }
        std::vector<int> starts;
        if (mode == MCube) {
            int k = d;
            int segs = 0;
            while (k >= s0 && segs < kHintSegs) {
                while (k >= s0) {
                    TraceEntry const* p = entryAt(k);
                    if (p && p->s.grounded) break;
                    k--;
                }
                if (k < s0) break;
                int e = k;
                while (k - 1 >= s0) {
                    TraceEntry const* p = entryAt(k - 1);
                    if (!p || !p->s.grounded) break;
                    k--;
                }
                starts.push_back(k);
                segs++;
                (void)e;
                k--;
            }
            if (starts.empty()) starts.push_back(s0);
        }
        else {
            for (int back : {120, 240, 360, 480}) {
                int s = std::max(s0, d - back);
                if (starts.empty() || s < starts.back()) starts.push_back(s);
            }
        }
        bool inflated = false;
        std::optional<PlannerHint> fallback;
        bool fallbackCross = false;
        Plan fallbackPlan;
        int fallbackDeath = -1;
        bool validatedOnly = false;
        uint64_t sig = modelSignature();
        auto tryPlan = [&](int s, bool cross) -> std::optional<PlannerHint> {
            TraceEntry const* se = entryAt(s);
            if (!se || !se->s.valid || se->s.special || se->s.mode >= 4) return std::nullopt;
            double dx = 0.0;
            if (!dxFor(se->s.speed, dx)) return std::nullopt;
            bool prevDown = s > 0 && s - 1 < static_cast<int>(path.size()) && (path[static_cast<size_t>(s) - 1] & 1);
            SimState st = simFrom(se->s, dx, prevDown, sinceFromPath(path, s), se->lastPad, se->usedOrb);
            Phys P;
            if (!resolve(se->s, P, false, m_hintLevel)) return std::nullopt;
            prepareMap(s, se->s.x, dx, se->s.hw, std::max(std::max(0, d - s) + 520, kCubeLook + 60), false);
            if (!inflated && killer >= 0 && killer < static_cast<int>(m_objs.size()) && !cross && predictedPathSurvives(s, d, path, st, P)) {
                MapObj& o = m_objs[static_cast<size_t>(killer)];
                if (o.margin < 4.f) {
                    o.margin += 1.f;
                    m_mapEpoch++;
                }
                inflated = true;
            }
            Plan plan;
            int dt = -1;
            bool ok = false;
            int minEnd = cross ? std::min(s + kHintHorizon, std::max(d + 90, s0 + 150)) : (mode == MCube ? -1 : d + 60);
            uint64_t key = sig;
            for (double v : {static_cast<double>(d), static_cast<double>(s), static_cast<double>(minEnd), cross ? 1.0 : 0.0, m_strictOnly ? 1.0 : 0.0,
                     lenientBad() ? 1.0 : 0.0, st.y, st.v, st.dx, static_cast<double>(st.mode), st.upside ? 1.0 : 0.0, static_cast<double>(std::min(st.since, limitCap())),
                     st.held ? 1.0 : 0.0, st.grounded ? 1.0 : 0.0, st.fresh ? 1.0 : 0.0, static_cast<double>(st.lastPad), static_cast<double>(st.usedOrb)}) {
                key = mixKey(key, v);
            }
            if (st.flipAge < kSafeFlipTicks) key = mixKey(key, static_cast<double>(st.flipAge) + 0.5);
            if (key == 0) key = 1;
            TryMemo& memo = m_tryMemo[static_cast<size_t>(key % m_tryMemo.size())];
            if (memo.key == key) {
                plan = memo.plan;
                dt = memo.dt;
                ok = false;
            }
            else {
                if (!makePlan(s, st, se->s, plan, dt, m_hintLevel, &ok, minEnd)) return std::nullopt;
                if (!ok && plan.valid) {
                    memo.key = key;
                    memo.dt = dt;
                    memo.plan = plan;
                }
            }
            if (plan.end() <= d) ok = false;
            if (ok && mode == MCube && !cross && dt >= 0 && dt <= d + kHintPast) ok = false;
            int lim = std::min(plan.end() - 1, d);
            for (int k = s; k <= lim; k++) {
                if (k >= static_cast<int>(path.size())) break;
                uint8_t pv = plan.held[static_cast<size_t>(k - s)] & 1;
                if (pv == (path[static_cast<size_t>(k)] & 1)) continue;
                bool pd = k > 0 && (path[static_cast<size_t>(k) - 1] & 1);
                TraceEntry const* ke = entryAt(k);
                uint8_t km = ke ? ke->s.mode : mode;
                if (pv != (pd ? 1 : 0) && !canFlip(k, familyOfMode(km), pd, sinceFromPath(path, k))) break;
                PlannerHint h;
                h.tick = k;
                h.held = static_cast<uint8_t>((path[static_cast<size_t>(k)] & ~1) | pv);
                if (ok) {
                    m_plan = std::move(plan);
                    m_plan.replanAt = std::max(m_plan.replanAt, std::min(m_plan.end() - 1, d + kHintPast));
                    if (cross) m_crossHints++;
                    return h;
                }
                if (!validatedOnly && (!fallback || dt > fallbackDeath)) {
                    fallback = h;
                    fallbackCross = cross;
                    fallbackPlan = std::move(plan);
                    fallbackDeath = dt;
                }
                return std::nullopt;
            }
            return std::nullopt;
        };
        int64_t w0 = m_work;
        for (int s : starts) {
            if (m_work - w0 > kCubeHintWork) break;
            auto h = tryPlan(s, false);
            if (h) return h;
        }
        int lastCross = -1;
        static constexpr std::array<int, 10> kCrossBacks = {1, 10, 30, 60, 100, 150, 220, 300, 400, 500};
        for (int back : kCrossBacks) {
            if (m_work > kHintWork) break;
            validatedOnly = back != 150 && back != 300;
            int cs = std::max(0, s0 - back);
            if (cs >= s0 || cs == lastCross || d - cs > kMaxHintBack) continue;
            lastCross = cs;
            auto h = tryPlan(cs, true);
            if (h) return h;
        }
        if (fallback) {
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(d)) << 32) ^ (static_cast<uint64_t>(static_cast<uint32_t>(fallback->tick)) << 1) ^ (fallback->held & 1);
            for (uint64_t k : m_recentFallbacks) {
                if (k == key) return std::nullopt;
            }
            m_recentFallbacks[m_recentFallbackPos] = key;
            m_recentFallbackPos = (m_recentFallbackPos + 1) % m_recentFallbacks.size();
            m_plan = std::move(fallbackPlan);
            if (fallbackCross) m_plan.replanAt = std::max(m_plan.replanAt, std::min(m_plan.end() - 1, d + kHintPast));
            return fallback;
        }
        return std::nullopt;
    }

    std::optional<PlannerHint> onDeath(DeathInfo const& info, std::vector<uint8_t> const& path) {
        int d = info.tick;
        if (d < 0 || d >= static_cast<int>(path.size()) || info.player2) return std::nullopt;
        if (m_lastHint.active && d <= m_lastHint.death + 1) {
            m_hintFails++;
            m_lastHint.active = false;
        }
        TraceEntry const* te = entryAt(d);
        int key = -1;
        int killer = attribute(info, d);
        learnCollision(d, path);
        if (TraceEntry const* de = entryAt(d + 1); de && de->valid && de->died) noteTouch(de->s, true);
        int kindKey = 0;
        if (killer >= 0 && killer < static_cast<int>(m_objs.size())) kindKey = m_objs[static_cast<size_t>(killer)].kind == KSolid ? 1 : 2;
        if (te) key = std::min<int>(te->s.mode, 8) * 3 + kindKey;
        m_pending = Pending{};
        m_pending.active = true;
        m_pending.death = d;
        m_pending.from = std::max(0, d - 360);
        m_pending.key = key;
        m_pending.tail.assign(path.begin() + m_pending.from, path.begin() + d + 1);
        auto t0 = Clock::now();
        std::optional<PlannerHint> h;
        m_work = 0;
        m_workCap = 0;
        if (m_deathTries.size() >= 4096 && !m_deathTries.contains(d)) m_deathTries.clear();
        int& tries = m_deathTries[d];
        bool allow = tries < kHintTriesPerTick;
        m_strictOnly = tries >= 1;
        tries++;
        m_hardCap = kHintHard;
        if (allow && te && te->s.valid && !te->s.special && te->s.mode < 4) h = planHint(d, path, killer);
        m_hardCap = 0;
        m_strictOnly = false;
        if (!h && allow) h = genericHint(d, path, key);
        m_stats.planMicros += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        if (h) {
            m_stats.hints++;
            m_lastHint.active = true;
            m_lastHint.tick = h->tick;
            m_lastHint.death = d;
            m_lastHint.held = h->held;
        }
        return h;
    }

    PlannerWindow pressWindow(int lane, int start, int end, std::vector<uint8_t> const& seq) {
        PlannerWindow w;
        if (lane != 0 || start < 0 || end <= start || end - start > 60 || start >= static_cast<int>(seq.size())) return w;
        TraceEntry const* te = entryAt(start);
        if (!te || te->s.mode != MCube || te->s.special || !te->s.grounded) return w;
        if (level(MCube, te->s.mini) < 3) return w;
        int gs = start;
        while (gs > 0 && start - gs < 400) {
            TraceEntry const* p = entryAt(gs - 1);
            if (!p || !p->s.grounded || p->s.mode != MCube) break;
            if (gs - 1 < static_cast<int>(seq.size()) && (seq[static_cast<size_t>(gs) - 1] & 1)) break;
            gs--;
        }
        TraceEntry const* ge = entryAt(gs);
        if (!ge) return w;
        double dx = 0.0;
        if (!dxFor(ge->s.speed, dx)) return w;
        Phys P;
        if (!resolve(ge->s, P, false, 3)) return w;
        auto t0 = Clock::now();
        prepareMap(gs, ge->s.x, dx, ge->s.hw, (end - gs) + 460);
        bool prevDown = gs > 0 && (seq[static_cast<size_t>(gs) - 1] & 1);
        SimState s0 = simFrom(ge->s, dx, prevDown, sinceFromPath(seq, gs), ge->lastPad, ge->usedOrb);
        int len = end - start;
        int nextPress = static_cast<int>(seq.size());
        for (int q = end; q < static_cast<int>(seq.size()); q++) {
            if (seq[static_cast<size_t>(q)] & 1) {
                nextPress = q;
                break;
            }
        }
        m_budget = kWindowBudget;
        int const hend = end + 400;
        std::vector<SimState> walk;
        SimState cur = s0;
        for (int tau = gs; tau <= start + 200 && tau < hend; tau++) {
            walk.push_back(cur);
            StepInfo info;
            if (step(cur, false, P, info) != RAlive) break;
            if (--m_budget < 0) break;
        }
        auto outcome = [&](int k, long long& key) -> bool {
            int i = k - gs;
            if (i < 0 || i >= static_cast<int>(walk.size())) return false;
            SimState s = walk[static_cast<size_t>(i)];
            if (!s.grounded || s.held) return false;
            if (!canFlip(k, 2, false, s.since)) return false;
            if (!canFlip(k + len, 2, true, len)) return false;
            for (int tau = k; tau < hend; tau++) {
                if (--m_budget < 0) return false;
                if (tau >= nextPress) return false;
                StepInfo info;
                bool h = tau < k + len;
                bool wasGround = s.grounded;
                uint8_t r = step(s, h, P, info);
                if (r != RAlive) return false;
                if (tau >= k + len - 1 && s.grounded && !wasGround) {
                    key = std::llround(s.y * 2.0);
                    return true;
                }
            }
            return false;
        };
        long long key0 = 0;
        if (!outcome(start, key0)) {
            m_stats.planMicros += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            return w;
        }
        int lo = start;
        int hi = start;
        for (int k = start - 1; k >= gs; k--) {
            long long kk = 0;
            if (!outcome(k, kk) || kk != key0) break;
            lo = k;
        }
        for (int k = start + 1; k < gs + static_cast<int>(walk.size()); k++) {
            long long kk = 0;
            if (!outcome(k, kk) || kk != key0) break;
            hi = k;
        }
        m_stats.planMicros += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        w.lo = lo;
        w.hi = hi;
        return w;
    }

    std::string summary() const {
        auto trust = [&](uint8_t mode) {
            int a = level(mode, false);
            return a >= 3 ? 'T' : (a == 2 ? 'L' : '-');
        };
        char buf[640];
        std::snprintf(buf, sizeof(buf),
            "map %llu objects | models cube:%c ship:%c ufo:%c wave:%c | advised %llu (%llu presses, %llu provisional plans) | "
            "plans %llu (%llu failed, %llu lenient) | hints %llu (%llu won, %llu failed) | deviations %llu | mismatches %llu | moving %zu (%llu changes, %llu replans) | plan %.1f ms",
            static_cast<unsigned long long>(m_stats.mapObjects), trust(MCube), trust(MShip), trust(MUfo), trust(MWave),
            static_cast<unsigned long long>(m_stats.advised), static_cast<unsigned long long>(m_stats.advisedPress),
            static_cast<unsigned long long>(m_provisional), static_cast<unsigned long long>(m_stats.plans),
            static_cast<unsigned long long>(m_stats.planFailures), static_cast<unsigned long long>(m_lenientPlans),
            static_cast<unsigned long long>(m_stats.hints), static_cast<unsigned long long>(m_hintWins),
            static_cast<unsigned long long>(m_hintFails), static_cast<unsigned long long>(m_stats.deviations),
            static_cast<unsigned long long>(m_stats.modelMismatches), m_tracks.size(), static_cast<unsigned long long>(m_moveEvents),
            static_cast<unsigned long long>(m_moveReplans), m_stats.planMicros / 1000.0);
        return buf;
    }
};

Planner::Planner(PlayLayer* layer) {
    try {
        m_impl = std::make_unique<PlannerImpl>(layer);
    }
    catch (...) {
        m_impl.reset();
    }
}

Planner::~Planner() = default;

void Planner::setEnabled(bool enabled) {
    if (m_impl) m_impl->m_enabled = enabled;
}

bool Planner::enabled() const {
    return m_impl && m_impl->m_enabled;
}

void Planner::buildMap() {
    if (!m_impl || !m_impl->m_enabled) return;
    try {
        m_impl->buildMap();
    }
    catch (...) {
        m_impl->m_built = false;
    }
}

void Planner::setLimits(PlannerLimits const& limits) {
    if (!m_impl) return;
    auto& l = m_impl->m_limits;
    l = limits;
    l.inputResolution = std::clamp(l.inputResolution, 1, 64);
    for (int i = 0; i < 3; i++) {
        l.minHold[i] = std::clamp(l.minHold[i], 1, 2400);
        l.minRelease[i] = std::clamp(l.minRelease[i], 1, 2400);
    }
    m_impl->m_plan.valid = false;
}

void Planner::onRestore(int tick) {
    if (!m_impl || !m_impl->m_enabled) return;
    try {
        m_impl->onRestore(tick);
    }
    catch (...) {
    }
}

void Planner::observe(int tick, uint8_t held, bool died) {
    if (!m_impl || !m_impl->m_enabled) return;
    try {
        m_impl->observe(tick, held, died, m_betweenFrames);
    }
    catch (...) {
    }
}

PlannerAdvice Planner::advise(int tick, uint8_t prevHeld) {
    if (!m_impl || !m_impl->m_enabled) return {};
    try {
        return m_impl->advise(tick, prevHeld);
    }
    catch (...) {
        m_impl->m_plan.valid = false;
        return {};
    }
}

std::optional<PlannerHint> Planner::onDeath(DeathInfo const& death, std::vector<uint8_t> const& path) {
    if (!m_impl || !m_impl->m_enabled) return std::nullopt;
    try {
        return m_impl->onDeath(death, path);
    }
    catch (...) {
        return std::nullopt;
    }
}

PlannerWindow Planner::pressWindow(int lane, int startTick, int endTick, std::vector<uint8_t> const& seq) {
    if (!m_impl || !m_impl->m_enabled) return {};
    try {
        return m_impl->pressWindow(lane, startTick, endTick, seq);
    }
    catch (...) {
        return {};
    }
}

PlannerStats const& Planner::stats() const {
    static PlannerStats const empty;
    return m_impl ? m_impl->m_stats : empty;
}

std::string Planner::summary() const {
    if (!m_impl) return "planner unavailable";
    try {
        return m_impl->summary();
    }
    catch (...) {
        return "planner";
    }
}

}
