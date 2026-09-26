#include "Overlay.hpp"

#include "Session.hpp"
#include "Solver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

namespace rp {

namespace {

constexpr double kHoldSeconds = 0.1;
constexpr float kIdleFadeSeconds = 0.25f;
constexpr float kRangeFadeSeconds = 0.2f;
constexpr double kPopupSeconds = 0.62;

double clockSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

ccColor4F withAlpha(ccColor4F c, float a) {
    c.a = std::clamp(a, 0.f, 1.f);
    return c;
}

ccColor4F scaled(ccColor4F c, float factor) {
    c.a = std::clamp(c.a * factor, 0.f, 1.f);
    return c;
}

ccColor4F mixWhite(ccColor4F c, float t) {
    c.r += (1.f - c.r) * t;
    c.g += (1.f - c.g) * t;
    c.b += (1.f - c.b) * t;
    return c;
}

ccColor3B toColor3B(ccColor4F c) {
    auto channel = [](float v) {
        return static_cast<GLubyte>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
    };
    return {channel(c.r), channel(c.g), channel(c.b)};
}

float smooth(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

std::string formatElapsed(double seconds) {
    auto total = static_cast<long long>(std::max(0.0, seconds));
    long long h = total / 3600;
    long long m = (total / 60) % 60;
    long long s = total % 60;
    if (h > 0) return fmt::format("{}:{:02}:{:02}", h, m, s);
    return fmt::format("{}:{:02}", m, s);
}

std::string nodeID(std::string_view name) {
    return fmt::format("{}/{}", GEODE_MOD_ID, name);
}

char const* judgeWord(int judge) {
    static char const* const words[] = {"PERFECT", "GREAT", "GOOD", "EARLY", "LATE", "MISS", "DROP"};
    return words[std::clamp(judge, 0, 6)];
}

ccColor3B judgeColor(int judge) {
    static ccColor3B const colors[] = {
        {110, 225, 255},
        {120, 240, 150},
        {255, 220, 110},
        {255, 160, 90},
        {255, 160, 90},
        {255, 90, 110},
        {255, 90, 110},
    };
    return colors[std::clamp(judge, 0, 6)];
}

double judgeWeight(int judge) {
    static double const weights[] = {100.0, 80.0, 50.0, 20.0, 20.0, 0.0, 0.0};
    return weights[std::clamp(judge, 0, 6)];
}

}

CCPoint RhythmOverlay::Geometry::at(float u, float v) const {
    if (!vertical) return {reverse ? x1 - u : x0 + u, y1 - v};
    return {x0 + v, reverse ? y1 - u : y0 + u};
}

float RhythmOverlay::Geometry::laneCenter(int lane) const {
    int slot = std::clamp(lane, 0, std::max(0, laneCount - 1));
    if (swap) slot = laneCount - 1 - slot;
    return (static_cast<float>(slot) + 0.5f) * laneSize;
}

RhythmOverlay* RhythmOverlay::create() {
    auto ret = new RhythmOverlay();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

void RhythmOverlay::makeText(TextSlot& slot, char const* font, std::string const& id, int z) {
    slot.label = CCLabelBMFont::create("", font);
    slot.label->setID(id);
    slot.label->setVisible(false);
    slot.text.clear();
    this->addChild(slot.label, z);
}

bool RhythmOverlay::init() {
    if (!CCNode::init()) return false;
    this->setID("overlay"_spr);

    m_draw = CCDrawNode::create();
    m_draw->setID("draw"_spr);
    m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    this->addChild(m_draw, 0);

    for (int i = 0; i < kLaneCount; i++) {
        makeText(m_laneLabels[i], "bigFont.fnt", nodeID(fmt::format("lane-label-{}", i + 1)), 2);
        makeText(m_judgeWords[i], "bigFont.fnt", nodeID(fmt::format("judgement-{}", i + 1)), 4);
        makeText(m_judgeSubs[i], "chatFont.fnt", nodeID(fmt::format("judgement-offset-{}", i + 1)), 4);
    }
    makeText(m_comboText, "bigFont.fnt", "combo"_spr, 3);
    makeText(m_accuracyText, "bigFont.fnt", "accuracy"_spr, 3);
    makeText(m_hint, "chatFont.fnt", "hint"_spr, 3);
    makeText(m_title, "bigFont.fnt", "solver-title"_spr, 6);
    makeText(m_elapsed, "chatFont.fnt", "solver-elapsed"_spr, 6);
    makeText(m_percent, "bigFont.fnt", "solver-percent"_spr, 6);
    makeText(m_percentCaption, "chatFont.fnt", "solver-percent-caption"_spr, 6);
    makeText(m_detail, "chatFont.fnt", "solver-detail"_spr, 6);
    makeText(m_note, "chatFont.fnt", "solver-note"_spr, 6);
    for (int i = 0; i < kMaxSteps; i++) {
        makeText(m_steps[i], "chatFont.fnt", nodeID(fmt::format("solver-step-{}", i + 1)), 6);
    }

    auto cancelSprite = ButtonSprite::create("Cancel", "goldFont.fnt", "GJ_button_06.png", 0.8f);
    cancelSprite->setScale(0.62f);
    auto cancelButton = CCMenuItemSpriteExtra::create(cancelSprite, this, menu_selector(RhythmOverlay::onCancelSolve));
    cancelButton->setID("cancel-solve"_spr);
    m_cancelButton = cancelButton;
    m_panelMenu = CCMenu::create();
    m_panelMenu->setID("solver-menu"_spr);
    m_panelMenu->addChild(cancelButton);
    m_panelMenu->setPosition({0.f, 0.f});
    m_panelMenu->setVisible(false);
    this->addChild(m_panelMenu, 7);

    refreshStyle();
    return true;
}

void RhythmOverlay::refreshStyle() {
    m_style = loadOverlayStyle();
}

void RhythmOverlay::refreshStyleNow() {
    refreshStyle();
    m_styleAge = 0;
}

void RhythmOverlay::computeGeometry(int laneCount) {
    auto win = CCDirector::get()->getWinSize();
    auto& g = m_geo;
    g.laneCount = std::clamp(laneCount, 1, kLaneCount);
    g.vertical = m_style.orientation == Orientation::Vertical;
    g.reverse = m_style.reverse;
    g.swap = m_style.swapLanes;
    g.laneSize = m_style.laneSize;
    g.thickness = m_style.laneSize * static_cast<float>(g.laneCount);
    float along = g.vertical ? win.height : win.width;
    g.length = std::max(40.f, std::floor(along * m_style.lengthFraction));
    g.hit = std::clamp(m_style.hitOffset, 0.f, std::max(0.f, g.length - 12.f));

    float w = g.vertical ? g.thickness : g.length;
    float h = g.vertical ? g.length : g.thickness;
    float cx = win.width / 2.f;
    float cy = win.height / 2.f;
    switch (m_style.anchor) {
        case OverlayAnchor::Top: cy = win.height - 24.f - h / 2.f; break;
        case OverlayAnchor::Bottom: cy = 14.f + h / 2.f; break;
        case OverlayAnchor::Left: cx = 24.f + w / 2.f; break;
        case OverlayAnchor::Right: cx = win.width - 24.f - w / 2.f; break;
        case OverlayAnchor::Center: break;
        case OverlayAnchor::Custom:
            cx = win.width * m_style.customX;
            cy = win.height * m_style.customY;
            break;
    }
    cx = w + 4.f < win.width ? std::clamp(cx, w / 2.f + 2.f, win.width - w / 2.f - 2.f) : win.width / 2.f;
    cy = h + 4.f < win.height ? std::clamp(cy, h / 2.f + 2.f, win.height - h / 2.f - 2.f) : win.height / 2.f;
    g.x0 = std::floor(cx - w / 2.f);
    g.y0 = std::floor(cy - h / 2.f);
    g.x1 = g.x0 + w;
    g.y1 = g.y0 + h;
}

void RhythmOverlay::showText(
    TextSlot& slot, std::string const& text, CCPoint pos, float scale, CCPoint anchor, ccColor3B color, float opacity,
    float maxWidth
) {
    if (!slot.label) return;
    if (slot.text != text) {
        slot.label->setString(text.c_str());
        slot.text = text;
    }
    float width = slot.label->getContentSize().width;
    if (maxWidth > 0.f && width * scale > maxWidth && width > 0.f) scale = maxWidth / width;
    slot.label->setScale(scale);
    slot.label->setAnchorPoint(anchor);
    slot.label->setPosition(pos);
    slot.label->setColor(color);
    slot.label->setOpacity(static_cast<GLubyte>(std::clamp(opacity, 0.f, 1.f) * 255.f));
    slot.label->setVisible(opacity > 0.004f && !text.empty());
}

void RhythmOverlay::hideAllText() {
    for (int i = 0; i < kLaneCount; i++) {
        for (auto slot : {&m_laneLabels[i], &m_judgeWords[i], &m_judgeSubs[i]}) {
            if (slot->label) slot->label->setVisible(false);
        }
    }
    for (auto slot : {&m_comboText, &m_accuracyText, &m_hint, &m_title, &m_elapsed, &m_percent, &m_percentCaption, &m_detail, &m_note}) {
        if (slot->label) slot->label->setVisible(false);
    }
    for (auto& slot : m_steps) {
        if (slot.label) slot.label->setVisible(false);
    }
    if (m_panelMenu) m_panelMenu->setVisible(false);
}

void RhythmOverlay::onCancelSolve(CCObject*) {
    auto s = session();
    if (!s || !s->solving()) return;
    cancelSolve(*s);
}

void RhythmOverlay::fillRect(float x0, float y0, float x1, float y1, ccColor4F const& color) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (x1 - x0 < 0.01f || y1 - y0 < 0.01f || color.a <= 0.003f) return;
    CCPoint verts[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    m_draw->drawPolygon(verts, 4, color, 0.f, ccColor4F{0.f, 0.f, 0.f, 0.f});
}

void RhythmOverlay::fillRoundRect(float x0, float y0, float x1, float y1, float radius, ccColor4F const& color, ccColor4F const& border) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    float w = x1 - x0;
    float h = y1 - y0;
    if (w < 0.5f || h < 0.5f) return;
    if (color.a <= 0.003f && border.a <= 0.003f) return;
    radius = std::clamp(radius, 0.f, std::min(w, h) / 2.f - 0.25f);
    constexpr int kArc = 5;
    CCPoint verts[4 * kArc];
    int count = 0;
    if (radius < 0.75f) {
        verts[count++] = CCPoint(x0, y0);
        verts[count++] = CCPoint(x1, y0);
        verts[count++] = CCPoint(x1, y1);
        verts[count++] = CCPoint(x0, y1);
    }
    else {
        CCPoint centers[4] = {{x1 - radius, y0 + radius}, {x1 - radius, y1 - radius}, {x0 + radius, y1 - radius}, {x0 + radius, y0 + radius}};
        float starts[4] = {-90.f, 0.f, 90.f, 180.f};
        for (int c = 0; c < 4; c++) {
            for (int i = 0; i < kArc; i++) {
                float angle = (starts[c] + 90.f * static_cast<float>(i) / static_cast<float>(kArc - 1)) * 3.14159265f / 180.f;
                verts[count++] = CCPoint(centers[c].x + std::cos(angle) * radius, centers[c].y + std::sin(angle) * radius);
            }
        }
    }
    ccColor4F fill = color;
    if (fill.a <= 0.003f) fill.a = 0.f;
    if (border.a > 0.003f) {
        m_draw->drawPolygon(verts, static_cast<unsigned int>(count), fill, 0.6f, border, BorderAlignment::Inside);
    }
    else {
        m_draw->drawPolygon(verts, static_cast<unsigned int>(count), fill, 0.f, ccColor4F{0.f, 0.f, 0.f, 0.f});
    }
}

void RhythmOverlay::capsule(CCPoint a, CCPoint b, float radius, ccColor4F const& color) {
    if (color.a <= 0.003f || radius <= 0.f) return;
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) return;
    if (ccpDistance(a, b) < 0.01f) m_draw->drawDot(a, radius, color);
    else m_draw->drawSegment(a, b, radius, color);
}

void RhythmOverlay::laneRect(float u0, float u1, float v0, float v1, ccColor4F const& color) {
    auto a = m_geo.at(u0, v0);
    auto b = m_geo.at(u1, v1);
    fillRect(a.x, a.y, b.x, b.y, color);
}

void RhythmOverlay::drawHead(float u, float v, ccColor4F const& color, float grow, bool core) {
    float ls = m_geo.laneSize;
    ccColor4F light = mixWhite(color, 0.6f);
    switch (m_style.shape) {
        case NoteShape::Rounded: {
            float r = std::max(2.f, ls * 0.13f);
            float half = std::max(0.f, ls * 0.36f - r);
            capsule(m_geo.at(u, v - half), m_geo.at(u, v + half), r + grow, color);
            if (core) capsule(m_geo.at(u, v - half * 0.7f), m_geo.at(u, v + half * 0.7f), r * 0.45f, light);
            break;
        }
        case NoteShape::Square: {
            float hu = std::max(2.f, ls * 0.12f) + grow;
            float hv = ls * 0.38f + grow;
            laneRect(u - hu, u + hu, v - hv, v + hv, color);
            if (core) {
                float cu = std::max(0.75f, (hu - grow) * 0.4f);
                float cv = (hv - grow) * 0.7f;
                laneRect(u - cu, u + cu, v - cv, v + cv, light);
            }
            break;
        }
        case NoteShape::Circle: {
            float r = ls * 0.32f;
            capsule(m_geo.at(u, v), m_geo.at(u, v), r + grow, color);
            if (core) capsule(m_geo.at(u, v), m_geo.at(u, v), r * 0.45f, light);
            break;
        }
    }
}

void RhythmOverlay::drawBody(float u0, float u1, float v, ccColor4F const& color) {
    float r = m_geo.laneSize * (m_style.rounded ? 0.19f : 0.17f);
    if (m_style.shape == NoteShape::Square) {
        if (u1 - u0 < 0.5f) return;
        laneRect(u0, u1, v - r, v + r, color);
        return;
    }
    float a = std::max(u0, r);
    float b = std::min(u1, m_geo.length - r);
    if (b - a < 0.5f) return;
    capsule(m_geo.at(a, v), m_geo.at(b, v), r, color);
}

void RhythmOverlay::visit() {
    m_draw->clear();
    hideAllText();
    if (++m_styleAge > 30) {
        refreshStyle();
        m_styleAge = 0;
    }

    double clock = clockSeconds();
    float dt = m_lastClock < 0.0 ? 0.f : static_cast<float>(std::clamp(clock - m_lastClock, 0.0, 0.1));
    m_lastClock = clock;

    auto s = session();
    if (!s || !s->layer || !this->isVisible()) {
        m_judgeActive = false;
        CCNode::visit();
        return;
    }
    auto layer = s->layer;
    bool paused = layer->m_isPaused;

    if (s->solving()) {
        m_judgeActive = false;
        if (!paused) drawSolverPanel(s->solver->view());
        CCNode::visit();
        return;
    }

    bool shown = m_style.show && !paused && !s->menuOpen && !layer->m_hasCompletedLevel;

    if (!s->chart) {
        m_judgeActive = false;
        if (shown) {
            std::string message;
            if (!s->lastSolverStatus.empty()) message = s->lastSolverStatus;
            else if (s->pendingRequest) message = "Starting...";
            else message = "No chart - open the Rhythm Path menu from the pause menu";
            drawEmptyPill(message);
        }
        CCNode::visit();
        return;
    }

    auto const& chart = *s->chart;
    double gameTime = layer->m_gameState.m_levelTime;
    syncJudgeState(chart);
    if (m_lastTime < 0.0 || gameTime + 1e-9 < m_lastTime) rewindJudgeState(chart, gameTime);
    m_lastTime = gameTime;

    bool rangeVisible = chartVisibleAt(*s, layer->getCurrentPercent());
    m_judgeActive = shown && rangeVisible;
    bool dead = layer->m_player1 && layer->m_player1->m_isDead;
    if (!paused && !dead) detectMisses(chart, gameTime, m_judgeActive);

    if (!shown) {
        m_snapFade = true;
        CCNode::visit();
        return;
    }

    double now = gameTime + m_style.visualOffset;
    int laneCount = (chart.twoPlayer || m_style.alwaysShowP2) ? 2 : 1;
    bool idle = m_style.idleFade && idleAt(chart, now, laneCount);
    if (m_snapFade) {
        m_rangeMix = rangeVisible ? 1.f : 0.f;
        m_idleMix = idle ? 1.f : 0.f;
        m_snapFade = false;
    }
    else {
        m_rangeMix += (rangeVisible ? 1.f : -1.f) * dt / kRangeFadeSeconds;
        m_idleMix += (idle ? 1.f : -1.f) * dt / kIdleFadeSeconds;
        m_rangeMix = std::clamp(m_rangeMix, 0.f, 1.f);
        m_idleMix = std::clamp(m_idleMix, 0.f, 1.f);
    }

    float alpha = fadeAlpha();
    if (alpha > 0.004f) drawChart(*s, chart, now, alpha);
    CCNode::visit();
}

float RhythmOverlay::fadeAlpha() const {
    float idle = 1.f + (m_style.idleOpacity - 1.f) * smooth(m_idleMix);
    return std::clamp(smooth(m_rangeMix) * idle, 0.f, 1.f);
}

bool RhythmOverlay::idleAt(Chart const& chart, double now, int laneCount) const {
    double ahead = now + m_style.idleSeconds;
    for (int lane = 0; lane < laneCount && lane < kLaneCount; lane++) {
        auto const& notes = chart.lanes[lane];
        auto it = std::lower_bound(notes.begin(), notes.end(), now, [](Note const& n, double t) {
            return n.endTime < t;
        });
        if (it != notes.end() && it->startTime <= ahead) return false;
        if (m_activeHold[lane] >= 0) return false;
    }
    return true;
}

void RhythmOverlay::drawSolverPanel(SolverView const& view) {
    auto win = CCDirector::get()->getWinSize();
    fillRect(0.f, 0.f, win.width, win.height, {0.f, 0.f, 0.f, 0.78f});

    float cardW = std::min(win.width - 32.f, 400.f);
    float cardH = std::min(win.height - 24.f, 204.f);
    float cx = win.width / 2.f;
    float cy = win.height / 2.f;
    float left = std::floor(cx - cardW / 2.f);
    float right = left + cardW;
    float top = std::floor(cy + cardH / 2.f);
    float bottom = top - cardH;
    float pad = 16.f;
    float inner = cardW - pad * 2.f;

    ccColor4F accent = withAlpha(m_style.p1Color, 1.f);
    ccColor3B accent3 = toColor3B(mixWhite(accent, 0.25f));
    ccColor3B text = {240, 244, 255};
    ccColor3B dim = {160, 166, 184};

    fillRoundRect(left, bottom, right, top, 12.f, {18.f / 255.f, 20.f / 255.f, 30.f / 255.f, 0.97f}, {1.f, 1.f, 1.f, 0.09f});

    std::string title = view.title.empty() ? std::string("Solving") : view.title;
    capsule({left + pad + 3.f, top - 22.f}, {left + pad + 3.f, top - 22.f}, 3.f, accent);
    showText(m_title, title, {left + pad + 12.f, top - 22.f}, 0.5f, {0.f, 0.5f}, text, 1.f, inner * 0.72f);
    showText(m_elapsed, formatElapsed(view.elapsed), {right - pad, top - 22.f}, 0.55f, {1.f, 0.5f}, dim, 1.f, inner * 0.25f);

    int stepCount = std::min(static_cast<int>(view.steps.size()), kMaxSteps);
    if (stepCount > 0) {
        float slot = inner / static_cast<float>(stepCount);
        float dotY = top - 50.f;
        int current = view.phase == Phase::Done ? stepCount : std::clamp(view.stepIndex, 0, stepCount);
        float firstX = left + pad + slot / 2.f;
        float lastX = left + pad + slot * (static_cast<float>(stepCount) - 0.5f);
        if (stepCount > 1) {
            capsule({firstX, dotY}, {lastX, dotY}, 1.f, {1.f, 1.f, 1.f, 0.14f});
            float doneX = firstX + slot * static_cast<float>(std::min(current, stepCount - 1));
            if (doneX > firstX) capsule({firstX, dotY}, {doneX, dotY}, 1.f, withAlpha(accent, 0.75f));
        }
        for (int i = 0; i < stepCount; i++) {
            float x = firstX + slot * static_cast<float>(i);
            ccColor3B color = dim;
            if (i < current) {
                capsule({x, dotY}, {x, dotY}, 3.5f, accent);
                color = accent3;
            }
            else if (i == current) {
                capsule({x, dotY}, {x, dotY}, 9.f, withAlpha(accent, 0.2f));
                capsule({x, dotY}, {x, dotY}, 5.f, accent);
                capsule({x, dotY}, {x, dotY}, 2.f, {1.f, 1.f, 1.f, 0.9f});
                color = text;
            }
            else {
                capsule({x, dotY}, {x, dotY}, 3.5f, {0.1f, 0.11f, 0.16f, 1.f});
                capsule({x, dotY}, {x, dotY}, 2.5f, {1.f, 1.f, 1.f, 0.28f});
            }
            showText(m_steps[i], view.steps[static_cast<size_t>(i)], {x, dotY - 15.f}, 0.5f, {0.5f, 0.5f}, color, 1.f, slot * 0.92f);
        }
    }

    float percent = std::clamp(view.percent, 0.f, 100.f);
    showText(m_percent, fmt::format("{:.1f}%", percent), {left + pad, top - 94.f}, 0.62f, {0.f, 0.5f}, text, 1.f, inner * 0.5f);
    if (m_percent.label && m_percent.label->isVisible()) {
        float w = m_percent.label->getContentSize().width * m_percent.label->getScale();
        showText(m_percentCaption, "of the level reached", {left + pad + w + 8.f, top - 97.f}, 0.5f, {0.f, 0.5f}, dim, 1.f, inner - w - 8.f);
    }

    float barY = top - 118.f;
    float barR = 3.5f;
    float barL = left + pad + barR;
    float barRight = right - pad - barR;
    capsule({barL, barY}, {barRight, barY}, barR, {1.f, 1.f, 1.f, 0.1f});
    float progress = std::clamp(view.progress, 0.f, 1.f);
    if (std::isfinite(progress) && progress > 0.f) {
        capsule({barL, barY}, {barL + (barRight - barL) * progress, barY}, barR, accent);
    }
    float markX = barL + (barRight - barL) * percent / 100.f;
    capsule({markX, barY - 7.f}, {markX, barY + 7.f}, 0.9f, {1.f, 1.f, 1.f, 0.85f});

    showText(m_detail, view.detail, {left + pad, top - 138.f}, 0.55f, {0.f, 0.5f}, text, 0.92f, inner);
    std::string note = view.note.empty() ? std::string("Progress is saved if you cancel, so you can resume later.") : view.note;
    showText(m_note, note, {left + pad, top - 154.f}, 0.5f, {0.f, 0.5f}, dim, 1.f, inner);

    m_panelMenu->setVisible(true);
    if (m_cancelButton) m_cancelButton->setPosition({cx, bottom + 22.f});
}

void RhythmOverlay::drawEmptyPill(std::string const& message) {
    computeGeometry(1);
    auto win = CCDirector::get()->getWinSize();
    auto center = m_geo.vertical ? m_geo.at(m_geo.length - 12.f, m_geo.thickness / 2.f) : m_geo.at(m_geo.length / 2.f, m_geo.thickness / 2.f);
    float cx = center.x;
    float cy = center.y;
    showText(m_hint, message, {cx, cy}, 0.5f, {0.5f, 0.5f}, {225, 230, 242}, 0.9f, std::max(60.f, win.width - 48.f));
    if (!m_hint.label || !m_hint.label->isVisible()) return;
    auto size = m_hint.label->getContentSize();
    float w = size.width * m_hint.label->getScale() + 18.f;
    float h = size.height * m_hint.label->getScale() + 6.f;
    cx = std::clamp(cx, w / 2.f + 2.f, std::max(w / 2.f + 2.f, win.width - w / 2.f - 2.f));
    cy = std::clamp(cy, h / 2.f + 2.f, std::max(h / 2.f + 2.f, win.height - h / 2.f - 2.f));
    m_hint.label->setPosition({cx, cy});
    fillRoundRect(cx - w / 2.f, cy - h / 2.f, cx + w / 2.f, cy + h / 2.f, h / 2.f, {0.f, 0.f, 0.f, 0.5f}, {1.f, 1.f, 1.f, 0.08f});
}

void RhythmOverlay::drawChart(Session& session, Chart const& chart, double now, float alpha) {
    int laneCount = (chart.twoPlayer || m_style.alwaysShowP2) ? 2 : 1;
    computeGeometry(laneCount);
    auto const& g = m_geo;
    float ls = g.laneSize;
    float L = g.length;
    float T = g.thickness;
    float speed = m_style.scrollSpeed;

    float corner = m_style.rounded ? std::min(7.f, std::min(ls * 0.4f, T / 2.f)) : 0.f;
    fillRoundRect(
        g.x0, g.y0, g.x1, g.y1, corner,
        scaled(m_style.laneColor, m_style.laneOpacity * alpha),
        scaled(m_style.borderColor, m_style.laneOpacity * alpha)
    );
    for (int slot = 1; slot < laneCount; slot++) {
        float v = ls * static_cast<float>(slot);
        capsule(g.at(corner + 2.f, v), g.at(L - corner - 2.f, v), 0.5f, scaled(m_style.separatorColor, alpha));
    }

    ccColor4F line = scaled(m_style.lineColor, alpha);
    float lineR = m_style.rounded ? 1.25f : 1.5f;
    if (m_style.glow == GlowLevel::Strong) {
        capsule(g.at(g.hit, -2.f), g.at(g.hit, T + 2.f), 11.f, withAlpha(line, line.a * 0.06f));
        capsule(g.at(g.hit, -2.f), g.at(g.hit, T + 2.f), 7.f, withAlpha(line, line.a * 0.12f));
        capsule(g.at(g.hit, -2.f), g.at(g.hit, T + 2.f), 3.5f, withAlpha(line, line.a * 0.24f));
    }
    else if (m_style.glow == GlowLevel::Soft) {
        capsule(g.at(g.hit, -2.f), g.at(g.hit, T + 2.f), 6.f, withAlpha(line, line.a * 0.07f));
        capsule(g.at(g.hit, -2.f), g.at(g.hit, T + 2.f), 3.f, withAlpha(line, line.a * 0.16f));
    }
    if (m_style.rounded) capsule(g.at(g.hit, -3.f), g.at(g.hit, T + 3.f), lineR, line);
    else laneRect(g.hit - lineR, g.hit + lineR, -3.f, T + 3.f, line);

    float noteAlpha = m_style.noteOpacity * alpha;
    float glowA = m_style.glow == GlowLevel::Strong ? 0.38f : (m_style.glow == GlowLevel::Soft ? 0.18f : 0.f);
    float glowGrow = m_style.glow == GlowLevel::Strong ? std::max(3.f, ls * 0.14f) : std::max(2.f, ls * 0.09f);
    float farFade = std::min(40.f, L * 0.12f) + 1.f;
    float nearFade = std::min(12.f, g.hit * 0.5f) + 1.f;
    auto edge = [&](float u) {
        return std::clamp((L - u) / farFade, 0.f, 1.f) * std::clamp((u + 1.f) / nearFade, 0.f, 1.f);
    };

    double tNear = now - static_cast<double>(g.hit) / speed;
    double tFar = now + static_cast<double>(L - g.hit) / speed;

    for (int lane = 0; lane < laneCount; lane++) {
        float v = g.laneCenter(lane);
        ccColor4F color = m_style.laneNoteColor(lane);
        auto const& notes = chart.lanes[lane];
        auto const& states = m_states[lane];
        bool statesOk = states.size() == notes.size();

        bool consuming = false;
        int active = m_activeHold[lane];
        if (statesOk && active >= 0 && static_cast<size_t>(active) < notes.size()) {
            consuming = states[static_cast<size_t>(active)].hold == HoldActive && now < notes[static_cast<size_t>(active)].endTime;
        }

        if (m_style.receptors) {
            bool held = m_userHeld[lane];
            if (held || consuming) {
                if (glowA > 0.f) drawHead(g.hit, v, withAlpha(color, noteAlpha * glowA * (consuming ? 1.6f : 1.f)), glowGrow * 1.6f, false);
                drawHead(g.hit, v, withAlpha(mixWhite(color, 0.15f), noteAlpha * 0.62f), 0.f, true);
            }
            else {
                drawHead(g.hit, v, withAlpha(m_style.lineColor, alpha * 0.2f), 0.f, false);
            }
        }

        auto it = std::lower_bound(notes.begin(), notes.end(), tNear, [](Note const& n, double t) {
            return n.endTime < t;
        });
        for (; it != notes.end() && it->startTime <= tFar; ++it) {
            size_t index = static_cast<size_t>(it - notes.begin());
            NoteState st;
            if (statesOk) st = states[index];
            bool isHold = it->endTime - it->startTime >= kHoldSeconds;
            float us = g.hit + static_cast<float>((it->startTime - now) * speed);
            float ue = g.hit + static_cast<float>((it->endTime - now) * speed);
            float a = noteAlpha;
            float bodyFrom = us;
            bool head = true;
            bool bright = false;
            bool tail = isHold;

            if (st.press == PressHit) {
                if (!isHold || st.hold == HoldDone || st.hold == HoldNone) continue;
                if (st.hold == HoldActive) {
                    if (ue <= g.hit) continue;
                    us = g.hit;
                    bodyFrom = g.hit;
                    bright = true;
                }
                else {
                    bodyFrom = std::max(us, g.hit + static_cast<float>((st.cut - now) * speed));
                    head = false;
                    tail = false;
                    a *= 0.3f;
                }
            }
            else if (st.press == PressMissed || st.press == PressSkipped) {
                a *= 0.3f;
            }

            float b0 = std::max(bodyFrom, 0.f);
            float b1 = std::min(ue, L);
            if (b1 - b0 > 0.5f) {
                float bodyA = a * (bright ? 0.62f : 0.42f) * edge(b0);
                drawBody(b0, b1, v, withAlpha(bright ? mixWhite(color, 0.2f) : color, bodyA));
            }
            if (tail && ue >= 0.f && ue <= L) {
                float half = ls * 0.24f;
                capsule(g.at(ue, v - half), g.at(ue, v + half), 1.1f, withAlpha(mixWhite(color, 0.35f), a * 0.8f * edge(ue)));
            }
            if (head && us >= 0.f && us <= L) {
                float e = bright ? 1.f : edge(us);
                if (glowA > 0.f) drawHead(us, v, withAlpha(color, a * glowA * e * (bright ? 1.5f : 1.f)), glowGrow, false);
                drawHead(us, v, withAlpha(bright ? mixWhite(color, 0.2f) : color, a * e), 0.f, true);
            }
        }
    }

    if (m_style.laneLabels) {
        for (int lane = 0; lane < laneCount; lane++) {
            auto const& text = m_style.laneLabel(lane);
            if (text.empty()) continue;
            auto& slot = m_laneLabels[lane];
            float v = g.laneCenter(lane);
            float scale = std::clamp(ls * 0.42f / 32.f, 0.14f, 0.45f);
            ccColor4F color = m_style.laneNoteColor(lane);
            showText(slot, text, {0.f, 0.f}, scale, {0.5f, 0.5f}, toColor3B(mixWhite(color, 0.45f)), 0.9f * alpha, g.vertical ? std::max(8.f, ls - 6.f) : L * 0.3f);
            if (!slot.label->isVisible()) continue;
            auto size = slot.label->getContentSize();
            float tw = size.width * slot.label->getScale();
            float th = size.height * slot.label->getScale() * 0.62f;
            float cw = tw + 8.f;
            float ch = std::min(th + 5.f, ls - 3.f);
            float along = g.vertical ? ch : cw;
            float u = L - std::max(4.f, corner) - along / 2.f;
            auto pos = g.at(u, v);
            slot.label->setPosition(pos);
            if (m_style.chipColor.a > 0.f) {
                fillRoundRect(pos.x - cw / 2.f, pos.y - ch / 2.f, pos.x + cw / 2.f, pos.y + ch / 2.f, ch / 2.f, scaled(m_style.chipColor, alpha), withAlpha(color, 0.18f * alpha));
            }
        }
    }

    drawStats(chart, alpha);
    drawPopups(alpha);

    if (!chart.complete) {
        auto win = CCDirector::get()->getWinSize();
        std::string text = fmt::format("Chart ends at {:.0f}%", chart.reachedPercent);
        if (!session.lastSolverStatus.empty() && !chart.targetReached()) text += " - " + session.lastSolverStatus;
        CCPoint pos;
        CCPoint anchor;
        float maxW;
        if (!g.vertical) {
            bool below = (g.y0 + g.y1) / 2.f > win.height / 2.f;
            pos = CCPoint((g.x0 + g.x1) / 2.f, below ? g.y0 - 9.f : g.y1 + 9.f);
            anchor = CCPoint(0.5f, 0.5f);
            maxW = L * 0.5f;
        }
        else {
            bool right = (g.x0 + g.x1) / 2.f < win.width / 2.f;
            pos = g.at(L - 8.f, 0.f);
            pos.x = right ? g.x1 + 8.f : g.x0 - 8.f;
            anchor = CCPoint(right ? 0.f : 1.f, 0.5f);
            maxW = std::max(60.f, right ? win.width - pos.x - 6.f : pos.x - 6.f);
        }
        showText(m_hint, text, pos, 0.45f, anchor, {210, 214, 228}, 0.8f * alpha, maxW);
    }
}

void RhythmOverlay::drawStats(Chart const&, float alpha) {
    auto const& g = m_geo;
    auto win = CCDirector::get()->getWinSize();
    float base = std::clamp(g.laneSize / 80.f, 0.24f, 0.4f);
    double clock = clockSeconds();

    bool showCombo = m_style.combo && m_combo > 0;
    bool showAccuracy = m_style.accuracy && m_judgeCount > 0;
    if (!showCombo && !showAccuracy) return;

    float pop = 0.f;
    if (m_comboAt >= 0.0) {
        float age = static_cast<float>(clock - m_comboAt);
        if (age >= 0.f && age < 0.12f) pop = 1.f - age / 0.12f;
    }
    std::string combo = fmt::format("{}x", m_combo);
    std::string accuracy = fmt::format("{:.2f}%", m_judgeCount > 0 ? m_judgeScore / m_judgeCount : 100.0);
    ccColor3B comboColor = toColor3B(mixWhite(m_style.p1Color, 0.55f));

    if (!g.vertical) {
        bool below = (g.y0 + g.y1) / 2.f > win.height / 2.f;
        float y = below ? g.y0 - 10.f : g.y1 + 10.f;
        auto hitPos = g.at(g.hit, 0.f);
        auto farPos = g.at(g.length - 4.f, 0.f);
        float towardFar = g.reverse ? 1.f : 0.f;
        if (showCombo) {
            showText(m_comboText, combo, {hitPos.x - (g.reverse ? -4.f : 4.f), y}, base * (1.f + 0.22f * pop), {towardFar, 0.5f}, comboColor, alpha, g.length * 0.3f);
        }
        if (showAccuracy) {
            showText(m_accuracyText, accuracy, {farPos.x, y}, base * 0.78f, {1.f - towardFar, 0.5f}, {225, 230, 242}, 0.8f * alpha, g.length * 0.3f);
        }
    }
    else {
        bool right = (g.x0 + g.x1) / 2.f < win.width / 2.f;
        float x = right ? g.x1 + 8.f : g.x0 - 8.f;
        float ax = right ? 0.f : 1.f;
        float maxW = std::max(40.f, right ? win.width - x - 6.f : x - 6.f);
        auto comboPos = g.at(g.hit + 10.f, 0.f);
        auto accPos = g.at(g.hit + 10.f + std::max(14.f, 34.f * base), 0.f);
        if (showCombo) showText(m_comboText, combo, {x, comboPos.y}, base * (1.f + 0.22f * pop), {ax, 0.5f}, comboColor, alpha, maxW);
        if (showAccuracy) showText(m_accuracyText, accuracy, {x, accPos.y}, base * 0.78f, {ax, 0.5f}, {225, 230, 242}, 0.8f * alpha, maxW);
    }
}

void RhythmOverlay::drawPopups(float alpha) {
    if (!m_style.judgements) return;
    auto const& g = m_geo;
    double clock = clockSeconds();
    for (int lane = 0; lane < kLaneCount; lane++) {
        auto const& popup = m_popups[lane];
        if (popup.shownAt < 0.0) continue;
        double age = clock - popup.shownAt;
        if (age < 0.0 || age > kPopupSeconds) continue;
        int drawLane = lane < g.laneCount ? lane : 0;
        float v = g.laneCenter(drawLane);
        float t = static_cast<float>(age);
        float popT = std::clamp(1.f - t / 0.09f, 0.f, 1.f);
        float scaleMul = 1.f + 0.3f * popT * popT;
        float fade = 1.f - smooth((t - 0.34f) / static_cast<float>(kPopupSeconds - 0.34));
        float opacity = fade * std::max(alpha, 0.6f);
        float base = std::clamp(g.laneSize / 90.f, 0.16f, 0.42f);

        if (!g.vertical) {
            float u = g.hit + std::max(10.f, g.laneSize * 0.45f);
            auto pos = g.at(u, v);
            float ax = g.reverse ? 1.f : 0.f;
            showText(m_judgeWords[lane], popup.word, pos, base * scaleMul, {ax, 0.5f}, popup.color, opacity, g.length * 0.4f);
            if (!popup.sub.empty() && m_judgeWords[lane].label->isVisible()) {
                float w = m_judgeWords[lane].label->getContentSize().width * m_judgeWords[lane].label->getScale();
                CCPoint subPos = {pos.x + (g.reverse ? -(w + 5.f) : w + 5.f), pos.y};
                showText(m_judgeSubs[lane], popup.sub, subPos, std::clamp(g.laneSize / 50.f, 0.35f, 0.55f), {ax, 0.5f}, {225, 230, 242}, opacity * 0.9f, g.length * 0.3f);
            }
        }
        else {
            float maxW = g.laneCount > 1 ? g.laneSize * 1.5f : std::max(g.laneSize * 2.5f, 70.f);
            float u = g.hit + g.laneSize * 0.9f + 12.f;
            showText(m_judgeWords[lane], popup.word, g.at(u, v), base * scaleMul, {0.5f, 0.5f}, popup.color, opacity, maxW * scaleMul);
            if (!popup.sub.empty()) {
                showText(m_judgeSubs[lane], popup.sub, g.at(u - 11.f, v), std::clamp(g.laneSize / 60.f, 0.3f, 0.5f), {0.5f, 0.5f}, {225, 230, 242}, opacity * 0.9f, maxW);
            }
        }
    }
}

void RhythmOverlay::syncJudgeState(Chart const& chart) {
    bool same = m_judgedChart == &chart;
    for (int lane = 0; lane < kLaneCount && same; lane++) {
        same = m_states[lane].size() == chart.lanes[lane].size();
    }
    if (!same) resetJudgeState(chart);
}

void RhythmOverlay::resetJudgeState(Chart const& chart) {
    m_judgedChart = &chart;
    for (int lane = 0; lane < kLaneCount; lane++) {
        m_states[lane].assign(chart.lanes[lane].size(), NoteState{});
        m_missCursor[lane] = 0;
        m_activeHold[lane] = -1;
        m_popups[lane].shownAt = -1.0;
    }
    m_combo = 0;
    m_judgeCount = 0;
    m_judgeScore = 0.0;
    m_comboAt = -1.0;
    m_lastTime = -1.0;
    m_snapFade = true;
}

void RhythmOverlay::rewindJudgeState(Chart const& chart, double now) {
    for (int lane = 0; lane < kLaneCount; lane++) {
        auto const& notes = chart.lanes[lane];
        auto& states = m_states[lane];
        if (states.size() != notes.size()) states.assign(notes.size(), NoteState{});
        for (size_t i = 0; i < notes.size(); i++) {
            auto& st = states[i];
            if (notes[i].startTime >= now - 1e-6) {
                st = NoteState{};
            }
            else if (st.press == PressNone || notes[i].endTime >= now - 1e-6) {
                st.press = PressSkipped;
                st.hold = HoldNone;
            }
        }
        auto it = std::lower_bound(notes.begin(), notes.end(), now - 1e-6, [](Note const& n, double t) {
            return n.startTime < t;
        });
        m_missCursor[lane] = static_cast<size_t>(it - notes.begin());
        m_activeHold[lane] = -1;
        m_userHeld[lane] = false;
        m_popups[lane].shownAt = -1.0;
    }
    m_combo = 0;
    m_judgeCount = 0;
    m_judgeScore = 0.0;
    m_comboAt = -1.0;
    m_attemptStart = now;
    m_snapFade = true;
}

void RhythmOverlay::detectMisses(Chart const& chart, double now, bool counted) {
    double window = m_style.missWindow;
    for (int lane = 0; lane < kLaneCount; lane++) {
        auto const& notes = chart.lanes[lane];
        auto& states = m_states[lane];
        if (states.size() != notes.size()) continue;
        size_t& cursor = m_missCursor[lane];
        while (cursor < notes.size() && notes[cursor].startTime < now - window) {
            auto& st = states[cursor];
            if (st.press == PressNone) {
                if (counted && notes[cursor].startTime >= m_attemptStart - 1e-6) {
                    st.press = PressMissed;
                    record(lane, Judge::Miss, false, 0.0, true);
                }
                else {
                    st.press = PressSkipped;
                }
            }
            cursor++;
        }
        int active = m_activeHold[lane];
        if (active >= 0) {
            auto index = static_cast<size_t>(active);
            if (index >= notes.size()) {
                m_activeHold[lane] = -1;
            }
            else if (now > notes[index].endTime + window) {
                if (states[index].hold == HoldActive) states[index].hold = HoldDone;
                m_activeHold[lane] = -1;
            }
        }
    }
}

void RhythmOverlay::onChartChanged() {
    m_judgedChart = nullptr;
    m_lastTime = -1.0;
    m_snapFade = true;
    for (int lane = 0; lane < kLaneCount; lane++) {
        m_userHeld[lane] = false;
        m_activeHold[lane] = -1;
        m_states[lane].clear();
        m_missCursor[lane] = 0;
        m_popups[lane].shownAt = -1.0;
    }
    m_combo = 0;
    m_judgeCount = 0;
    m_judgeScore = 0.0;
    m_comboAt = -1.0;
}

void RhythmOverlay::onLevelReset() {
    m_lastTime = 1e18;
    m_snapFade = true;
    for (int lane = 0; lane < kLaneCount; lane++) {
        m_userHeld[lane] = false;
        m_activeHold[lane] = -1;
    }
}

void RhythmOverlay::onInput(int lane, bool down, double levelTime) {
    if (lane < 0 || lane >= kLaneCount) return;
    auto s = session();
    if (!s || !s->chart) {
        m_userHeld[lane] = down;
        return;
    }
    auto const& chart = *s->chart;
    if (!chart.twoPlayer) lane = 0;
    m_userHeld[lane] = down;
    if (!m_judgeActive || s->solving()) {
        if (!down) {
            int active = m_activeHold[lane];
            m_activeHold[lane] = -1;
            auto& states = m_states[lane];
            if (active >= 0 && static_cast<size_t>(active) < states.size() && states[static_cast<size_t>(active)].hold == HoldActive) {
                states[static_cast<size_t>(active)].hold = HoldDropped;
                states[static_cast<size_t>(active)].cut = levelTime;
            }
        }
        return;
    }

    syncJudgeState(chart);
    if (down) handlePress(chart, lane, levelTime);
    else handleRelease(chart, lane, levelTime);
}

void RhythmOverlay::handlePress(Chart const& chart, int lane, double time) {
    auto const& notes = chart.lanes[lane];
    auto& states = m_states[lane];
    if (states.size() != notes.size()) return;
    double window = m_style.missWindow;

    auto it = std::lower_bound(notes.begin(), notes.end(), time - window, [](Note const& n, double t) {
        return n.startTime < t;
    });
    int best = -1;
    double bestErr = 1e9;
    for (; it != notes.end() && it->startTime <= time + window; ++it) {
        size_t index = static_cast<size_t>(it - notes.begin());
        if (states[index].press != PressNone) continue;
        if (it->startTime < m_attemptStart - 1e-6) continue;
        double err = time - it->startTime;
        if (std::abs(err) < std::abs(bestErr)) {
            bestErr = err;
            best = static_cast<int>(index);
        }
    }
    if (best < 0) return;

    auto index = static_cast<size_t>(best);
    auto& st = states[index];
    st.press = PressHit;
    double magnitude = std::abs(bestErr);
    Judge judge;
    if (magnitude <= m_style.perfectWindow) judge = Judge::Perfect;
    else if (magnitude <= m_style.greatWindow) judge = Judge::Great;
    else if (magnitude <= m_style.goodWindow) judge = Judge::Good;
    else judge = bestErr < 0.0 ? Judge::Early : Judge::Late;

    auto const& note = notes[index];
    if (note.endTime - note.startTime >= kHoldSeconds) {
        int previous = m_activeHold[lane];
        if (previous >= 0 && static_cast<size_t>(previous) < states.size() && states[static_cast<size_t>(previous)].hold == HoldActive) {
            states[static_cast<size_t>(previous)].hold = HoldDone;
        }
        st.hold = HoldActive;
        st.cut = note.startTime;
        m_activeHold[lane] = best;
    }
    else {
        st.hold = HoldDone;
    }
    record(lane, judge, false, bestErr, true);
}

void RhythmOverlay::handleRelease(Chart const& chart, int lane, double time) {
    int active = m_activeHold[lane];
    m_activeHold[lane] = -1;
    auto const& notes = chart.lanes[lane];
    auto& states = m_states[lane];
    if (active < 0 || states.size() != notes.size()) return;
    auto index = static_cast<size_t>(active);
    if (index >= notes.size()) return;
    auto& st = states[index];
    if (st.hold != HoldActive) return;

    auto const& note = notes[index];
    double err = time - note.endTime;
    if (err < -m_style.goodWindow) {
        st.hold = HoldDropped;
        st.cut = std::max(time, note.startTime);
        if (m_style.judgeHolds) {
            record(lane, Judge::Drop, true, err, true);
        }
        else if (err >= -m_style.missWindow) {
            record(lane, Judge::Early, true, err, false);
        }
        return;
    }

    st.hold = HoldDone;
    double magnitude = std::abs(err);
    Judge judge;
    if (magnitude <= m_style.perfectWindow) judge = Judge::Perfect;
    else if (magnitude <= m_style.greatWindow) judge = Judge::Great;
    else if (magnitude <= m_style.goodWindow) judge = Judge::Good;
    else judge = Judge::Late;
    record(lane, judge, true, err, m_style.judgeHolds);
}

void RhythmOverlay::record(int lane, Judge judge, bool release, double error, bool counted) {
    int j = static_cast<int>(judge);
    if (counted) {
        m_judgeCount++;
        m_judgeScore += judgeWeight(j);
        if (judge == Judge::Miss || judge == Judge::Drop) {
            m_combo = 0;
            m_comboAt = -1.0;
        }
        else {
            m_combo++;
            m_comboAt = clockSeconds();
        }
    }
    if (!m_style.judgements || lane < 0 || lane >= kLaneCount) return;
    auto& popup = m_popups[lane];
    popup.shownAt = clockSeconds();
    popup.word = judgeWord(j);
    popup.color = judgeColor(j);
    if (judge == Judge::Miss || judge == Judge::Drop) popup.sub.clear();
    else popup.sub = fmt::format("{}{:+.0f} ms", release ? "release " : "", error * 1000.0);
}

}
