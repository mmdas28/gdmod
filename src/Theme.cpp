#include "Theme.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace rp {

namespace {

struct ThemePreset {
    ccColor4F lanes;
    ccColor4F p1;
    ccColor4F p2;
    ccColor4F line;
    GlowLevel glow;
    bool classic;
};

ccColor4F rgb(int r, int g, int b, float a = 1.f) {
    return {r / 255.f, g / 255.f, b / 255.f, a};
}

ccColor4F toColor4F(ccColor4B c) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f};
}

ThemePreset presetFor(std::string const& name) {
    if (name == "Neon") return {rgb(8, 6, 20, 0.55f), rgb(0, 255, 200), rgb(255, 60, 220), rgb(130, 170, 255), GlowLevel::Strong, false};
    if (name == "Pastel") return {rgb(40, 36, 52, 0.5f), rgb(150, 220, 255), rgb(255, 180, 210), rgb(255, 240, 200), GlowLevel::Soft, false};
    if (name == "Mono") return {rgb(0, 0, 0, 0.5f), rgb(255, 255, 255), rgb(170, 170, 170), rgb(255, 255, 255), GlowLevel::Soft, false};
    if (name == "Classic") return {rgb(5, 5, 13, 0.72f), rgb(60, 220, 90), rgb(60, 220, 90), rgb(30, 150, 255), GlowLevel::Soft, true};
    return {rgb(16, 18, 28, 0.62f), rgb(90, 200, 255), rgb(255, 120, 200), rgb(240, 244, 255), GlowLevel::Soft, false};
}

template <class T>
T setting(std::string_view key) {
    return Mod::get()->getSettingValue<T>(key);
}

float settingFloat(std::string_view key, double lo, double hi) {
    double value = setting<double>(key);
    if (!std::isfinite(value)) value = lo;
    return static_cast<float>(std::clamp(value, lo, hi));
}

std::string cleanLabel(std::string text) {
    std::string out;
    for (char c : text) {
        auto u = static_cast<unsigned char>(c);
        if (u >= 32 && u < 127) out.push_back(c);
        if (out.size() >= 10) break;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}

OverlayStyle loadOverlayStyle() {
    OverlayStyle style;

    style.show = setting<bool>("show-overlay");

    style.orientation = setting<std::string>("orientation") == "Vertical" ? Orientation::Vertical : Orientation::Horizontal;
    auto position = setting<std::string>("overlay-position");
    if (position == "Bottom") style.anchor = OverlayAnchor::Bottom;
    else if (position == "Left") style.anchor = OverlayAnchor::Left;
    else if (position == "Right") style.anchor = OverlayAnchor::Right;
    else if (position == "Center") style.anchor = OverlayAnchor::Center;
    else if (position == "Custom") style.anchor = OverlayAnchor::Custom;
    else style.anchor = OverlayAnchor::Top;
    if (style.orientation == Orientation::Horizontal && (style.anchor == OverlayAnchor::Left || style.anchor == OverlayAnchor::Right)) {
        style.anchor = OverlayAnchor::Top;
    }
    style.customX = settingFloat("custom-x", 0.0, 1.0);
    style.customY = settingFloat("custom-y", 0.0, 1.0);
    style.lengthFraction = settingFloat("overlay-width", 0.1, 1.0);
    style.laneSize = settingFloat("lane-height", 8.0, 120.0);
    style.hitOffset = settingFloat("hit-line-x", 0.0, 2000.0);
    style.scrollSpeed = settingFloat("scroll-speed", 20.0, 5000.0);
    style.reverse = setting<bool>("reverse-scroll");
    style.alwaysShowP2 = setting<bool>("always-show-p2");
    style.swapLanes = setting<bool>("swap-lanes");

    auto preset = presetFor(setting<std::string>("theme"));
    style.laneColor = preset.lanes;
    style.p1Color = preset.p1;
    style.p2Color = preset.p2;
    style.lineColor = preset.line;
    if (setting<bool>("custom-colors")) {
        style.p1Color = toColor4F(setting<ccColor4B>("p1-color"));
        style.p2Color = toColor4F(setting<ccColor4B>("p2-color"));
        style.lineColor = toColor4F(setting<ccColor4B>("line-color"));
        style.laneColor = toColor4F(setting<ccColor4B>("lane-color"));
    }

    auto shape = setting<std::string>("note-style");
    if (shape == "Square") style.shape = NoteShape::Square;
    else if (shape == "Circle") style.shape = NoteShape::Circle;
    else style.shape = NoteShape::Rounded;

    style.glow = setting<bool>("glow") ? preset.glow : GlowLevel::None;
    if (preset.classic) {
        style.rounded = false;
        style.borderColor = {1.f, 1.f, 1.f, 0.8f};
        style.separatorColor = {1.f, 1.f, 1.f, 0.55f};
        style.chipColor = {0.f, 0.f, 0.f, 0.f};
    }

    style.laneOpacity = settingFloat("overlay-opacity", 0.0, 1.0);
    style.noteOpacity = settingFloat("note-opacity", 0.0, 1.0);
    style.receptors = setting<bool>("show-receptors");
    style.laneLabels = setting<bool>("lane-labels");
    style.p1Label = cleanLabel(setting<std::string>("p1-label"));
    style.p2Label = cleanLabel(setting<std::string>("p2-label"));

    style.idleFade = setting<bool>("idle-fade");
    style.idleOpacity = settingFloat("idle-opacity", 0.0, 1.0);
    style.idleSeconds = settingFloat("idle-seconds", 0.1, 60.0);

    style.judgements = setting<bool>("show-judgements");
    style.combo = setting<bool>("show-combo");
    style.accuracy = setting<bool>("show-accuracy");
    style.judgeHolds = setting<bool>("judge-holds");
    style.perfectWindow = settingFloat("perfect-ms", 0.5, 500.0) / 1000.0;
    style.greatWindow = std::max<double>(style.perfectWindow, settingFloat("great-ms", 0.5, 500.0) / 1000.0);
    style.goodWindow = std::max<double>(style.greatWindow, settingFloat("good-ms", 0.5, 500.0) / 1000.0);
    style.missWindow = std::max<double>(style.goodWindow, settingFloat("miss-ms", 1.0, 1000.0) / 1000.0);
    style.visualOffset = settingFloat("visual-offset-ms", -1000.0, 1000.0) / 1000.0;

    return style;
}

}
