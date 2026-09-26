#include "Overlay.hpp"

#include "Session.hpp"
#include "Solver.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace rp {

namespace {

constexpr double kJudgeWindow = 0.135;
constexpr double kMissWindow = 0.135;
constexpr double kTick = 1.0 / 240.0;

ccColor4F toColor4F(ccColor4B c) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}

ccColor4F withAlpha(ccColor4F c, float a) {
    c.a = a;
    return c;
}

}

float RhythmOverlay::Geometry::laneBottom(int lane) const {
    float laneHeight = laneCount > 0 ? height / laneCount : height;
    return y0 + height - (lane + 1) * laneHeight;
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

bool RhythmOverlay::init() {
    if (!CCNode::init()) return false;
    this->setID("overlay"_spr);

    m_draw = CCDrawNode::create();
    m_draw->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
    this->addChild(m_draw, 0);

    for (int i = 0; i < kLaneCount; i++) {
        m_laneLabels[i] = CCLabelBMFont::create(i == 0 ? "P1" : "P2", "bigFont.fnt");
        m_laneLabels[i]->setAnchorPoint({1.f, 0.5f});
        m_laneLabels[i]->setOpacity(190);
        m_laneLabels[i]->setVisible(false);
        this->addChild(m_laneLabels[i], 2);
    }

    m_status = CCLabelBMFont::create("", "bigFont.fnt");
    m_detail = CCLabelBMFont::create("", "chatFont.fnt");
    m_hint = CCLabelBMFont::create("", "chatFont.fnt");
    for (auto label : {m_status, m_detail, m_hint}) {
        label->setVisible(false);
        this->addChild(label, 3);
    }

    auto cancelSprite = ButtonSprite::create("Cancel", "goldFont.fnt", "GJ_button_06.png", 0.8f);
    cancelSprite->setScale(0.7f);
    auto cancelButton = CCMenuItemSpriteExtra::create(cancelSprite, this, menu_selector(RhythmOverlay::onCancelSolve));
    cancelButton->setID("cancel-solve"_spr);
    m_panelMenu = CCMenu::create();
    m_panelMenu->addChild(cancelButton);
    m_panelMenu->setPosition({0.f, 0.f});
    m_panelMenu->setVisible(false);
    this->addChild(m_panelMenu, 5);

    refreshStyle();
    return true;
}

void RhythmOverlay::refreshStyle() {
    auto mod = Mod::get();
    m_style.show = mod->getSettingValue<bool>("show-overlay");
    m_style.scrollSpeed = static_cast<float>(std::max(10.0, mod->getSettingValue<double>("scroll-speed")));
    m_style.top = mod->getSettingValue<std::string>("overlay-position") != "Bottom";
    m_style.widthFraction = static_cast<float>(std::clamp(mod->getSettingValue<double>("overlay-width"), 0.1, 1.0));
    m_style.laneHeight = static_cast<float>(std::clamp(mod->getSettingValue<double>("lane-height"), 6.0, 120.0));
    m_style.hitLineX = static_cast<float>(std::max(0.0, mod->getSettingValue<double>("hit-line-x")));
    m_style.backgroundOpacity = static_cast<float>(std::clamp(mod->getSettingValue<double>("overlay-opacity"), 0.0, 1.0));
    m_style.alwaysShowP2 = mod->getSettingValue<bool>("always-show-p2");
    m_style.judgements = mod->getSettingValue<bool>("show-judgements");
    m_style.noteColor = toColor4F(mod->getSettingValue<ccColor4B>("note-color"));
    m_style.lineColor = toColor4F(mod->getSettingValue<ccColor4B>("line-color"));
    m_style.visualOffset = mod->getSettingValue<double>("visual-offset-ms") / 1000.0;
}

void RhythmOverlay::computeGeometry(int laneCount) {
    auto win = CCDirector::get()->getWinSize();
    m_geo.laneCount = laneCount;
    m_geo.width = std::floor(win.width * m_style.widthFraction);
    m_geo.height = m_style.laneHeight * laneCount;
    m_geo.x0 = std::floor((win.width - m_geo.width) / 2.f);
    m_geo.y0 = m_style.top ? std::floor(win.height - m_geo.height - 24.f) : 14.f;
    m_geo.hitX = m_geo.x0 + std::min(m_style.hitLineX, std::max(0.f, m_geo.width - 20.f));
}

void RhythmOverlay::setText(CCLabelBMFont* label, std::string& cache, std::string const& text) {
    if (cache != text) {
        label->setString(text.c_str());
        cache = text;
    }
    label->setVisible(true);
}

void RhythmOverlay::hideAllText() {
    for (auto label : m_laneLabels) label->setVisible(false);
    m_status->setVisible(false);
    m_detail->setVisible(false);
    m_hint->setVisible(false);
    m_panelMenu->setVisible(false);
}

void RhythmOverlay::onCancelSolve(CCObject*) {
    auto s = session();
    if (!s || !s->solving()) return;
    s->solver->cancel(true);
    s->lastSolverStatus = "Solver cancelled";
}

void RhythmOverlay::drawRect(float x0, float y0, float x1, float y1, ccColor4F const& color) {
    if (x1 <= x0 || y1 <= y0 || color.a <= 0.f) return;
    CCPoint verts[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    m_draw->drawPolygon(verts, 4, color, 0.f, ccColor4F{0.f, 0.f, 0.f, 0.f});
}

void RhythmOverlay::visit() {
    m_draw->clear();
    m_panelMenu->setVisible(false);
    if (++m_styleAge > 30) {
        refreshStyle();
        m_styleAge = 0;
    }

    auto s = session();
    if (!s || !s->layer) {
        hideAllText();
        CCNode::visit();
        return;
    }
    auto layer = s->layer;
    bool paused = layer->m_isPaused;

    if (s->solving()) {
        hideAllText();
        if (!paused) drawSolverPanel(*s->solver);
        CCNode::visit();
        return;
    }

    if (!m_style.show || paused || layer->m_hasCompletedLevel) {
        hideAllText();
        CCNode::visit();
        return;
    }

    if (!s->chart) {
        std::string message;
        if (!s->lastSolverStatus.empty()) message = s->lastSolverStatus;
        else if (s->solveRequested) message = "Starting solver...";
        else message = "No chart yet - pause and press Solve";
        drawEmptyBox(message);
        CCNode::visit();
        return;
    }

    double now = layer->m_gameState.m_levelTime + m_style.visualOffset;
    drawChart(*s, *s->chart, now);
    CCNode::visit();
}

void RhythmOverlay::drawSolverPanel(Solver const& solver) {
    auto win = CCDirector::get()->getWinSize();
    drawRect(0.f, 0.f, win.width, win.height, {0.f, 0.f, 0.f, 0.74f});

    float barWidth = win.width * 0.5f;
    float barX = (win.width - barWidth) / 2.f;
    float barY = win.height * 0.46f;
    float barH = 8.f;
    drawRect(barX - 1.f, barY - 1.f, barX + barWidth + 1.f, barY + barH + 1.f, {1.f, 1.f, 1.f, 0.55f});
    drawRect(barX, barY, barX + barWidth, barY + barH, {0.08f, 0.08f, 0.1f, 1.f});
    drawRect(barX, barY, barX + barWidth * std::clamp(solver.progress(), 0.f, 1.f), barY + barH, withAlpha(m_style.noteColor, 1.f));

    setText(m_status, m_statusText, solver.statusLine());
    m_status->setScale(0.5f);
    m_status->setPosition({win.width / 2.f, win.height * 0.58f});

    setText(m_detail, m_detailText, solver.detailLine());
    m_detail->setScale(0.6f);
    m_detail->setPosition({win.width / 2.f, win.height * 0.40f});

    setText(m_hint, m_hintText, "Rhythm Path is simulating the level in the background.");
    m_hint->setScale(0.55f);
    m_hint->setPosition({win.width / 2.f, win.height * 0.34f});

    m_panelMenu->setVisible(true);
    if (auto button = m_panelMenu->getChildByID("cancel-solve"_spr)) {
        button->setPosition({win.width / 2.f, win.height * 0.22f});
    }
}

void RhythmOverlay::drawEmptyBox(std::string const& message) {
    computeGeometry(m_style.alwaysShowP2 ? 2 : 1);
    auto const& g = m_geo;
    drawRect(g.x0, g.y0, g.x0 + g.width, g.y0 + g.height, {0.02f, 0.02f, 0.05f, m_style.backgroundOpacity});
    drawRect(g.hitX - 1.5f, g.y0 - 3.f, g.hitX + 1.5f, g.y0 + g.height + 3.f, m_style.lineColor);

    for (auto label : m_laneLabels) label->setVisible(false);
    m_status->setVisible(false);
    m_detail->setVisible(false);
    setText(m_hint, m_hintText, message);
    m_hint->setScale(std::clamp(m_style.laneHeight / 40.f, 0.35f, 0.6f));
    m_hint->setPosition({g.x0 + g.width / 2.f, g.y0 + g.height / 2.f});
}

void RhythmOverlay::drawChart(Session& session, Chart const& chart, double now) {
    syncJudgeState(chart);
    if (m_lastTime < 0.0 || now + 1e-9 < m_lastTime) rewindJudgeState(chart, now);
    m_lastTime = now;

    int laneCount = (chart.twoPlayer || m_style.alwaysShowP2) ? 2 : 1;
    computeGeometry(laneCount);
    auto const& g = m_geo;
    float laneHeight = m_style.laneHeight;
    float right = g.x0 + g.width;
    float top = g.y0 + g.height;

    drawRect(g.x0, g.y0, right, top, {0.02f, 0.02f, 0.05f, m_style.backgroundOpacity});
    ccColor4F border = {1.f, 1.f, 1.f, 0.8f};
    drawRect(g.x0 - 1.f, g.y0 - 1.f, right + 1.f, g.y0, border);
    drawRect(g.x0 - 1.f, top, right + 1.f, top + 1.f, border);
    drawRect(g.x0 - 1.f, g.y0, g.x0, top, border);
    drawRect(right, g.y0, right + 1.f, top, border);
    for (int lane = 1; lane < laneCount; lane++) {
        float y = g.laneBottom(lane - 1);
        drawRect(g.x0, y - 0.5f, right, y + 0.5f, {1.f, 1.f, 1.f, 0.55f});
    }

    float speed = m_style.scrollSpeed;
    double tLeft = now - (g.hitX - g.x0) / speed;
    double tRight = now + (right - g.hitX) / speed;
    ccColor4F note = m_style.noteColor;

    for (int lane = 0; lane < laneCount; lane++) {
        auto const& notes = chart.lanes[lane];
        float yb = g.laneBottom(lane);
        float cy = yb + laneHeight / 2.f;
        bool active = false;

        auto it = std::lower_bound(notes.begin(), notes.end(), tLeft, [](Note const& n, double t) {
            return n.endTime < t;
        });
        for (; it != notes.end() && it->startTime <= tRight; ++it) {
            float xs = g.hitX + static_cast<float>((it->startTime - now) * speed);
            float xe = g.hitX + static_cast<float>((it->endTime - now) * speed);
            bool passed = it->startTime < now;
            if (it->startTime <= now && now < it->endTime) active = true;

            float bodyHalf = laneHeight * 0.17f;
            drawRect(std::max(xs, g.x0), cy - bodyHalf, std::min(xe, right), cy + bodyHalf, withAlpha(note, passed ? 0.28f : 0.55f));

            if (xs >= g.x0 - 3.f && xs <= right + 3.f) {
                float headHalfW = 2.5f;
                float headHalfH = laneHeight * 0.4f;
                drawRect(
                    std::max(xs - headHalfW, g.x0), cy - headHalfH,
                    std::min(xs + headHalfW, right), cy + headHalfH,
                    withAlpha(note, passed ? 0.35f : 1.f)
                );
            }
            if (it->endTick - it->startTick >= 6 && xe >= g.x0 && xe <= right) {
                float tailHalfH = laneHeight * 0.26f;
                drawRect(xe - 1.f, cy - tailHalfH, std::min(xe + 1.f, right), cy + tailHalfH, withAlpha(note, passed ? 0.3f : 0.85f));
            }
        }

        if (active) {
            drawRect(g.hitX - 7.f, yb + 1.f, g.hitX + 7.f, yb + laneHeight - 1.f, withAlpha(note, 0.35f));
        }
        if (m_userHeld[lane]) {
            drawRect(g.hitX - 3.f, yb + 1.f, g.hitX + 3.f, yb + laneHeight - 1.f, {1.f, 1.f, 1.f, 0.4f});
        }

        auto label = m_laneLabels[lane];
        label->setVisible(true);
        label->setScale(std::clamp(laneHeight / 60.f, 0.2f, 0.5f));
        label->setPosition({right - 4.f, cy});
    }
    for (int lane = laneCount; lane < kLaneCount; lane++) m_laneLabels[lane]->setVisible(false);

    drawRect(g.hitX - 1.5f, g.y0 - 3.f, g.hitX + 1.5f, top + 3.f, m_style.lineColor);

    m_status->setVisible(false);
    m_detail->setVisible(false);
    if (!chart.complete) {
        std::string why = session.lastSolverStatus.empty() ? std::string("partial chart") : session.lastSolverStatus;
        setText(m_hint, m_hintText, fmt::format("Chart only reaches {:.0f}% ({})", chart.reachedPercent, why));
        m_hint->setScale(0.45f);
        m_hint->setPosition({g.x0 + g.width / 2.f, m_style.top ? g.y0 - 8.f : top + 8.f});
    }
    else {
        m_hint->setVisible(false);
    }

    if (m_style.judgements && !session.layer->m_player1->m_isDead) detectMisses(chart, now);
}

void RhythmOverlay::syncJudgeState(Chart const& chart) {
    bool same = m_judgedChart == &chart;
    for (int lane = 0; lane < kLaneCount && same; lane++) {
        same = m_pressJudged[lane].size() == chart.lanes[lane].size();
    }
    if (same) return;
    m_judgedChart = &chart;
    for (int lane = 0; lane < kLaneCount; lane++) {
        m_pressJudged[lane].assign(chart.lanes[lane].size(), 0);
        m_releaseJudged[lane].assign(chart.lanes[lane].size(), 0);
        m_missCursor[lane] = 0;
    }
    m_lastTime = -1.0;
}

void RhythmOverlay::rewindJudgeState(Chart const& chart, double now) {
    for (int lane = 0; lane < kLaneCount; lane++) {
        auto const& notes = chart.lanes[lane];
        for (size_t i = 0; i < notes.size(); i++) {
            if (notes[i].startTime >= now - 1e-6) m_pressJudged[lane][i] = 0;
            if (notes[i].endTime >= now - 1e-6) m_releaseJudged[lane][i] = 0;
        }
        auto it = std::lower_bound(notes.begin(), notes.end(), now - 1e-6, [](Note const& n, double t) {
            return n.startTime < t;
        });
        m_missCursor[lane] = static_cast<size_t>(it - notes.begin());
        m_userHeld[lane] = false;
    }
    m_attemptStart = now;
}

void RhythmOverlay::detectMisses(Chart const& chart, double now) {
    for (int lane = 0; lane < kLaneCount; lane++) {
        auto const& notes = chart.lanes[lane];
        size_t& cursor = m_missCursor[lane];
        while (cursor < notes.size() && notes[cursor].startTime < now - kMissWindow) {
            if (!m_pressJudged[lane][cursor]) {
                m_pressJudged[lane][cursor] = 1;
                if (notes[cursor].startTime >= m_attemptStart - 1e-6) {
                    showJudgement(lane, "MISS", {255, 80, 80});
                }
            }
            cursor++;
        }
    }
}

void RhythmOverlay::onChartChanged() {
    m_judgedChart = nullptr;
    m_lastTime = -1.0;
    for (auto& held : m_userHeld) held = false;
}

void RhythmOverlay::onLevelReset() {
    m_lastTime = 1e18;
    for (auto& held : m_userHeld) held = false;
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
    if (!m_style.judgements || !m_style.show) return;

    syncJudgeState(chart);
    auto const& notes = chart.lanes[lane];
    auto& flags = down ? m_pressJudged[lane] : m_releaseJudged[lane];

    auto it = std::lower_bound(notes.begin(), notes.end(), levelTime - kJudgeWindow, [down](Note const& n, double t) {
        return (down ? n.startTime : n.endTime) < t;
    });
    int best = -1;
    double bestErr = 1e9;
    for (; it != notes.end(); ++it) {
        double target = down ? it->startTime : it->endTime;
        if (target > levelTime + kJudgeWindow) break;
        size_t index = static_cast<size_t>(it - notes.begin());
        if (flags[index]) continue;
        if (!down && it->endTime - it->startTime < 0.1) continue;
        double err = levelTime - target;
        if (std::abs(err) < std::abs(bestErr)) {
            bestErr = err;
            best = static_cast<int>(index);
        }
    }
    if (best < 0) return;
    flags[best] = 1;

    double ticks = std::abs(bestErr) / kTick;
    std::string word;
    ccColor3B color;
    if (ticks <= 1.5) {
        word = "PERFECT";
        color = {90, 230, 255};
    }
    else if (ticks <= 4.5) {
        word = "GREAT";
        color = {110, 255, 120};
    }
    else if (ticks <= 9.5) {
        word = "GOOD";
        color = {255, 230, 90};
    }
    else {
        word = bestErr < 0 ? "EARLY" : "LATE";
        color = {255, 120, 90};
    }
    showJudgement(lane, fmt::format("{}{} {:+.0f}ms", down ? "" : "release ", word, bestErr * 1000.0), color);
}

void RhythmOverlay::showJudgement(int lane, std::string const& text, ccColor3B color) {
    if (lane >= m_geo.laneCount) lane = 0;
    if (m_judgeLabels[lane]) {
        m_judgeLabels[lane]->stopAllActions();
        m_judgeLabels[lane]->removeFromParent();
        m_judgeLabels[lane] = nullptr;
    }
    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setColor(color);
    label->setAnchorPoint({0.f, 0.5f});
    label->setScale(std::clamp(m_style.laneHeight / 90.f, 0.16f, 0.42f));
    label->setPosition({m_geo.hitX + 8.f, m_geo.laneBottom(lane) + m_style.laneHeight / 2.f});
    this->addChild(label, 4);
    label->runAction(CCSequence::create(
        CCDelayTime::create(0.25f),
        CCFadeOut::create(0.35f),
        CCRemoveSelf::create(),
        nullptr
    ));
    m_judgeLabels[lane] = label;
}

}
