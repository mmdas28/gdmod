#pragma once

#include "Chart.hpp"

#include <Geode/Geode.hpp>
#include <string>
#include <vector>

namespace rp {

struct Session;
class Solver;

class RhythmOverlay : public cocos2d::CCNode {
public:
    static RhythmOverlay* create();

    bool init() override;
    void visit() override;

    void onChartChanged();
    void onLevelReset();
    void onInput(int lane, bool down, double levelTime);

private:
    struct Style {
        bool show = true;
        float scrollSpeed = 320.f;
        bool top = true;
        float widthFraction = 0.62f;
        float laneHeight = 26.f;
        float hitLineX = 34.f;
        float backgroundOpacity = 0.72f;
        bool alwaysShowP2 = true;
        bool judgements = true;
        cocos2d::ccColor4F noteColor = {0.24f, 0.86f, 0.35f, 1.f};
        cocos2d::ccColor4F lineColor = {0.12f, 0.59f, 1.f, 1.f};
        double visualOffset = 0.0;
    };

    struct Geometry {
        float x0 = 0.f;
        float y0 = 0.f;
        float width = 0.f;
        float height = 0.f;
        float hitX = 0.f;
        int laneCount = 1;
        float laneBottom(int lane) const;
    };

    Style m_style;
    int m_styleAge = 1 << 20;
    Geometry m_geo;

    cocos2d::CCDrawNode* m_draw = nullptr;
    cocos2d::CCLabelBMFont* m_laneLabels[kLaneCount] = {};
    cocos2d::CCLabelBMFont* m_status = nullptr;
    cocos2d::CCLabelBMFont* m_detail = nullptr;
    cocos2d::CCLabelBMFont* m_hint = nullptr;
    geode::Ref<cocos2d::CCLabelBMFont> m_judgeLabels[kLaneCount];
    std::string m_statusText;
    std::string m_detailText;
    std::string m_hintText;

    Chart const* m_judgedChart = nullptr;
    std::vector<uint8_t> m_pressJudged[kLaneCount];
    std::vector<uint8_t> m_releaseJudged[kLaneCount];
    size_t m_missCursor[kLaneCount] = {};
    bool m_userHeld[kLaneCount] = {};
    double m_lastTime = -1.0;
    double m_attemptStart = 0.0;

    void refreshStyle();
    void computeGeometry(int laneCount);
    void setText(cocos2d::CCLabelBMFont* label, std::string& cache, std::string const& text);
    void hideAllText();

    void drawSolverPanel(Solver const& solver);
    void drawChart(Session& session, Chart const& chart, double now);
    void drawEmptyBox(std::string const& message);
    void drawRect(float x0, float y0, float x1, float y1, cocos2d::ccColor4F const& color);

    void syncJudgeState(Chart const& chart);
    void rewindJudgeState(Chart const& chart, double now);
    void detectMisses(Chart const& chart, double now);
    void showJudgement(int lane, std::string const& text, cocos2d::ccColor3B color);
};

}
