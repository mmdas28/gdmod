#pragma once

#include "Chart.hpp"
#include "Theme.hpp"

#include <Geode/Geode.hpp>
#include <string>
#include <vector>

namespace rp {

struct Session;
class Solver;
struct SolverView;

class RhythmOverlay : public cocos2d::CCNode {
public:
    static RhythmOverlay* create();

    bool init() override;
    void visit() override;

    void onChartChanged();
    void onLevelReset();
    void onInput(int lane, bool down, double levelTime);
    void onCancelSolve(cocos2d::CCObject* sender);
    void refreshStyleNow();

private:
    static constexpr int kMaxSteps = 8;

    struct Geometry {
        float x0 = 0.f;
        float y0 = 0.f;
        float x1 = 0.f;
        float y1 = 0.f;
        float length = 0.f;
        float thickness = 0.f;
        float laneSize = 0.f;
        float hit = 0.f;
        int laneCount = 1;
        bool vertical = false;
        bool reverse = false;
        bool swap = false;

        cocos2d::CCPoint at(float u, float v) const;
        float laneCenter(int lane) const;
    };

    enum class Judge {
        Perfect,
        Great,
        Good,
        Early,
        Late,
        Miss,
        Drop,
    };

    enum : uint8_t {
        PressNone = 0,
        PressHit = 1,
        PressMissed = 2,
        PressSkipped = 3,
    };

    enum : uint8_t {
        HoldNone = 0,
        HoldActive = 1,
        HoldDone = 2,
        HoldDropped = 3,
    };

    struct NoteState {
        uint8_t press = PressNone;
        uint8_t hold = HoldNone;
        double cut = 0.0;
    };

    struct TextSlot {
        cocos2d::CCLabelBMFont* label = nullptr;
        std::string text;
    };

    struct Popup {
        double shownAt = -1.0;
        std::string word;
        std::string sub;
        cocos2d::ccColor3B color = {255, 255, 255};
    };

    OverlayStyle m_style;
    int m_styleAge = 1 << 20;
    Geometry m_geo;
    float m_bigLineHeight = 32.f;

    cocos2d::CCDrawNode* m_draw = nullptr;
    cocos2d::CCMenu* m_panelMenu = nullptr;
    cocos2d::CCNode* m_cancelButton = nullptr;

    TextSlot m_laneLabels[kLaneCount];
    TextSlot m_judgeWords[kLaneCount];
    TextSlot m_judgeSubs[kLaneCount];
    TextSlot m_comboText;
    TextSlot m_accuracyText;
    TextSlot m_hint;
    TextSlot m_title;
    TextSlot m_elapsed;
    TextSlot m_percent;
    TextSlot m_percentCaption;
    TextSlot m_detail;
    TextSlot m_note;
    TextSlot m_saveHint;
    TextSlot m_steps[kMaxSteps];

    Popup m_popups[kLaneCount];

    Chart const* m_judgedChart = nullptr;
    std::vector<NoteState> m_states[kLaneCount];
    size_t m_missCursor[kLaneCount] = {};
    int m_activeHold[kLaneCount] = {-1, -1};
    bool m_userHeld[kLaneCount] = {};
    double m_lastTime = -1.0;
    double m_attemptStart = 0.0;
    bool m_judgeActive = false;

    int m_combo = 0;
    int m_judgeCount = 0;
    double m_judgeScore = 0.0;
    double m_comboAt = -1.0;

    float m_rangeMix = 1.f;
    float m_idleMix = 0.f;
    bool m_snapFade = true;
    double m_lastClock = -1.0;

    void refreshStyle();
    void computeGeometry(int laneCount);
    void makeText(TextSlot& slot, char const* font, std::string const& id, int z);
    void showText(
        TextSlot& slot, std::string const& text, cocos2d::CCPoint pos, float scale, cocos2d::CCPoint anchor,
        cocos2d::ccColor3B color, float opacity, float maxWidth = 0.f
    );
    void hideAllText();

    void fillRect(float x0, float y0, float x1, float y1, cocos2d::ccColor4F const& color);
    void fillRoundRect(
        float x0, float y0, float x1, float y1, float radius, cocos2d::ccColor4F const& color,
        cocos2d::ccColor4F const& border
    );
    void capsule(cocos2d::CCPoint a, cocos2d::CCPoint b, float radius, cocos2d::ccColor4F const& color);
    void laneRect(float u0, float u1, float v0, float v1, cocos2d::ccColor4F const& color);
    void drawHead(float u, float v, cocos2d::ccColor4F const& color, float grow, bool core);
    void drawBody(float u0, float u1, float v, cocos2d::ccColor4F const& color);

    void drawSolverPanel(SolverView const& view);
    void drawEmptyPill(std::string const& message);
    void drawChart(Session& session, Chart const& chart, double now, float alpha);
    void drawStats(Chart const& chart, float alpha);
    void drawPopups(float alpha);

    bool idleAt(Chart const& chart, double now, int laneCount) const;
    void updateFades(bool rangeVisible, bool idle);
    float fadeAlpha() const;

    void syncJudgeState(Chart const& chart);
    void resetJudgeState(Chart const& chart);
    void rewindJudgeState(Chart const& chart, double now);
    void detectMisses(Chart const& chart, double now, bool counted);
    void handlePress(Chart const& chart, int lane, double time);
    void handleRelease(Chart const& chart, int lane, double time);
    void record(int lane, Judge judge, bool release, double error, bool counted);
};

}
