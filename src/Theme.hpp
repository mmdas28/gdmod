#pragma once

#include <Geode/Geode.hpp>
#include <string>

namespace rp {

enum class Orientation {
    Horizontal,
    Vertical,
};

enum class OverlayAnchor {
    Top,
    Bottom,
    Left,
    Right,
    Center,
    Custom,
};

enum class NoteShape {
    Rounded,
    Square,
    Circle,
};

enum class GlowLevel {
    None,
    Soft,
    Strong,
};

struct OverlayStyle {
    bool show = true;

    Orientation orientation = Orientation::Horizontal;
    OverlayAnchor anchor = OverlayAnchor::Top;
    float customX = 0.5f;
    float customY = 0.85f;
    float lengthFraction = 0.62f;
    float laneSize = 26.f;
    float hitOffset = 34.f;
    float scrollSpeed = 320.f;
    bool reverse = false;
    bool alwaysShowP2 = false;
    bool swapLanes = false;

    NoteShape shape = NoteShape::Rounded;
    GlowLevel glow = GlowLevel::Soft;
    bool rounded = true;
    cocos2d::ccColor4F laneColor = {16.f / 255.f, 18.f / 255.f, 28.f / 255.f, 0.62f};
    cocos2d::ccColor4F borderColor = {1.f, 1.f, 1.f, 0.1f};
    cocos2d::ccColor4F separatorColor = {1.f, 1.f, 1.f, 0.07f};
    cocos2d::ccColor4F p1Color = {90.f / 255.f, 200.f / 255.f, 1.f, 1.f};
    cocos2d::ccColor4F p2Color = {1.f, 120.f / 255.f, 200.f / 255.f, 1.f};
    cocos2d::ccColor4F lineColor = {240.f / 255.f, 244.f / 255.f, 1.f, 1.f};
    cocos2d::ccColor4F chipColor = {0.f, 0.f, 0.f, 0.45f};
    cocos2d::ccColor3B textColor = {240, 244, 255};

    float laneOpacity = 1.f;
    float noteOpacity = 1.f;
    bool receptors = true;
    bool laneLabels = true;
    std::string p1Label = "P1";
    std::string p2Label = "P2";

    bool idleFade = true;
    float idleOpacity = 0.3f;
    float idleSeconds = 1.5f;

    bool judgements = true;
    bool combo = true;
    bool accuracy = true;
    bool judgeHolds = true;
    double perfectWindow = 0.007;
    double greatWindow = 0.019;
    double goodWindow = 0.040;
    double missWindow = 0.135;
    double visualOffset = 0.0;

    cocos2d::ccColor4F laneNoteColor(int lane) const { return lane == 0 ? p1Color : p2Color; }
    std::string const& laneLabel(int lane) const { return lane == 0 ? p1Label : p2Label; }
};

OverlayStyle loadOverlayStyle();

}
