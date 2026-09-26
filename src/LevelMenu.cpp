#include "LevelMenu.hpp"

#include "Session.hpp"

#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/TextInput.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace rp {

void pickImportFile(Session& session);

namespace {

constexpr float kWidth = 400.f;
constexpr float kHeight = 270.f;
constexpr float kPad = 18.f;
constexpr float kRowGap = 6.f;

ccColor3B const kWhite = {255, 255, 255};
ccColor3B const kDim = {215, 205, 190};
ccColor3B const kGreen = {130, 255, 130};
ccColor3B const kYellow = {255, 225, 100};
ccColor3B const kOrange = {255, 150, 60};

bool isPlatformer(PlayLayer* layer) {
    return layer && (layer->m_isPlatformer || (layer->m_levelSettings && layer->m_levelSettings->m_platformerMode));
}

CCLabelBMFont* addLabel(
    CCNode* parent, std::string const& text, char const* font, float scale, CCPoint pos, CCPoint anchor, ccColor3B color,
    std::string const& id, float maxWidth = 0.f
) {
    auto label = CCLabelBMFont::create(text.c_str(), font);
    label->setAnchorPoint(anchor);
    label->setScale(scale);
    float width = label->getContentSize().width;
    if (maxWidth > 0.f && width * scale > maxWidth && width > 0.f) label->setScale(maxWidth / width);
    label->setPosition(pos);
    label->setColor(color);
    label->setID(id);
    parent->addChild(label, 3);
    return label;
}

void addRule(CCNode* parent, float y, std::string const& id) {
    auto rule = CCLayerColor::create({0, 0, 0, 70}, kWidth - kPad * 2.f, 1.f);
    rule->setPosition({kPad, y});
    rule->setID(id);
    parent->addChild(rule, 1);
}

CCMenu* addMenu(CCNode* parent, std::string const& id) {
    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    menu->setID(id);
    parent->addChild(menu, 4);
    return menu;
}

CCMenuItemSpriteExtra* makeButton(
    char const* text, char const* bg, float scale, std::string const& id, geode::Function<void(CCMenuItemSpriteExtra*)> callback
) {
    auto sprite = ButtonSprite::create(text, "goldFont.fnt", bg, .8f);
    sprite->setScale(scale);
    auto button = CCMenuItemExt::createSpriteExtra(sprite, std::move(callback));
    button->setID(id);
    return button;
}

float placeRow(std::vector<CCNode*> const& nodes, float x, float y, float gap, float maxWidth) {
    float total = 0.f;
    for (auto node : nodes) total += node->getScaledContentSize().width;
    if (!nodes.empty()) total += gap * static_cast<float>(nodes.size() - 1);
    float shrink = total > maxWidth && total > 0.f ? maxWidth / total : 1.f;
    for (auto node : nodes) {
        if (shrink < 1.f) {
            node->setScale(node->getScale() * shrink);
            if (auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(node)) item->m_baseScale = node->getScale();
        }
        float w = node->getScaledContentSize().width;
        node->setPosition({x + w / 2.f, y});
        x += w + gap * shrink;
    }
    return x;
}

std::optional<float> parsePercent(std::string const& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return std::nullopt;
    return static_cast<float>(std::clamp(value, 0.0, 100.0));
}

std::string percentText(float value) {
    if (!std::isfinite(value)) value = 0.f;
    if (std::fabs(value - std::round(value)) < 0.05f) return fmt::format("{:.0f}", value);
    return fmt::format("{:.1f}", value);
}

std::string formatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    auto total = static_cast<long long>(seconds);
    if (total < 60) return fmt::format("{}s", total);
    if (total < 3600) return fmt::format("{}m {}s", total / 60, total % 60);
    return fmt::format("{}h {}m", total / 3600, (total / 60) % 60);
}

std::string formatDate(int64_t unixSeconds) {
    if (unixSeconds <= 0) return "";
    std::time_t time = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &time) != 0) return "";
#else
    if (!localtime_r(&time, &tm)) return "";
#endif
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

std::string chartStatus(Chart const& chart, ccColor3B& color) {
    if (chart.complete) {
        color = kGreen;
        return "Complete";
    }
    if (chart.targetReached()) {
        color = kGreen;
        return fmt::format("Target {}% reached", percentText(chart.info.targetPercent));
    }
    color = kYellow;
    return fmt::format("Reaches {:.0f}%", std::floor(std::clamp(chart.reachedPercent, 0.f, 100.f)));
}

std::string chartSource(Chart const& chart) {
    auto const& info = chart.info;
    std::string out;
    if (info.source.starts_with("import:")) {
        out = info.sourceFile.empty() ? "Imported" : "Imported: " + info.sourceFile;
    }
    else {
        out = "Solved";
        if (info.solveSeconds > 0.0) out += " in " + formatDuration(info.solveSeconds);
    }
    auto date = formatDate(info.savedAt);
    if (!date.empty()) out += "  |  saved " + date;
    return out;
}

Session* menuSession(LevelMenu* menu) {
    auto s = session();
    if (!s || !s->layer || s->getMenu() != menu) return nullptr;
    return s;
}

}

LevelMenu* LevelMenu::create() {
    auto ret = new LevelMenu();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool LevelMenu::init() {
    if (!Popup::init(kWidth, kHeight)) return false;
    auto s = session();
    if (!s || !s->layer) return false;

    this->setID("level-menu"_spr);
    this->setTitle("Rhythm Path");
    if (m_title) m_title->setID("title"_spr);

    auto const& ref = s->ref;
    std::string subtitle = ref.levelName.empty() ? std::string("Unnamed level") : ref.levelName;
    subtitle += ref.levelID > 0 ? fmt::format("  |  ID {}", ref.levelID) : std::string("  |  Local level");
    subtitle += "  |  " + ref.describe();
    addLabel(m_mainLayer, subtitle, "chatFont.fnt", .65f, {kWidth / 2.f, 232.f}, {.5f, .5f}, kDim, "subtitle"_spr, kWidth - kPad * 2.f);

    addRule(m_mainLayer, 221.f, "rule-top"_spr);
    addRule(m_mainLayer, 148.f, "rule-chart"_spr);
    addRule(m_mainLayer, 84.f, "rule-solve"_spr);
    addRule(m_mainLayer, 46.f, "rule-range"_spr);

    addLabel(m_mainLayer, "Up to", "chatFont.fnt", .6f, {326.f, 100.f}, {1.f, .5f}, kWhite, "target-label"_spr);
    auto target = TextInput::create(60.f, "100");
    target->setCommonFilter(CommonFilter::Float);
    target->setMaxCharCount(5);
    target->setScale(.55f);
    target->setPosition({347.f, 100.f});
    target->setID("target-input"_spr);
    target->setString(percentText(s->prefs.targetPercent));
    target->setCallback([this](std::string const& text) {
        auto s = menuSession(this);
        if (!s) return;
        auto value = parsePercent(text);
        if (!value || *value < 1.f) return;
        auto prefs = s->prefs;
        prefs.targetPercent = *value;
        setLevelPrefs(*s, prefs);
    });
    m_mainLayer->addChild(target, 4);
    addLabel(m_mainLayer, "%", "chatFont.fnt", .6f, {366.f, 100.f}, {0.f, .5f}, kWhite, "target-unit"_spr);

    float rowY = 65.f;
    auto fromLabel = addLabel(m_mainLayer, "Show chart from", "chatFont.fnt", .6f, {kPad, rowY}, {0.f, .5f}, kWhite, "range-label"_spr);
    float x = kPad + fromLabel->getScaledContentSize().width + 4.f;

    auto makeRangeInput = [this](std::string const& id) {
        auto input = TextInput::create(60.f, "0");
        input->setCommonFilter(CommonFilter::Float);
        input->setMaxCharCount(5);
        input->setScale(.55f);
        input->setID(id);
        m_mainLayer->addChild(input, 4);
        return input;
    };
    auto fromInput = makeRangeInput("range-from-input"_spr);
    fromInput->setPosition({x + 16.5f, rowY});
    x += 33.f + 3.f;
    auto toLabel = addLabel(m_mainLayer, "% to", "chatFont.fnt", .6f, {x, rowY}, {0.f, .5f}, kWhite, "range-to-label"_spr);
    x += toLabel->getScaledContentSize().width + 4.f;
    auto toInput = makeRangeInput("range-to-input"_spr);
    toInput->setPosition({x + 16.5f, rowY});
    x += 33.f + 3.f;
    addLabel(m_mainLayer, "%", "chatFont.fnt", .6f, {x, rowY}, {0.f, .5f}, kWhite, "range-unit"_spr);

    fromInput->setString(percentText(effectiveRangeFrom(*s)));
    toInput->setString(percentText(effectiveRangeTo(*s)));

    auto staticMenu = addMenu(m_mainLayer, "static-menu"_spr);
    auto rangeToggle = CCMenuItemExt::createTogglerWithStandardSprites(.55f, [this](CCMenuItemToggler* toggler) {
        auto s = menuSession(this);
        if (!s) return;
        bool on = !toggler->isToggled();
        auto prefs = s->prefs;
        auto from = typeinfo_cast<TextInput*>(m_mainLayer->getChildByID("range-from-input"_spr));
        auto to = typeinfo_cast<TextInput*>(m_mainLayer->getChildByID("range-to-input"_spr));
        if (on) {
            std::optional<float> a;
            std::optional<float> b;
            if (from) a = parsePercent(from->getString());
            if (to) b = parsePercent(to->getString());
            prefs.customRange = true;
            prefs.rangeFrom = a.value_or(effectiveRangeFrom(*s));
            prefs.rangeTo = b.value_or(effectiveRangeTo(*s));
            setLevelPrefs(*s, prefs);
        }
        else {
            prefs.customRange = false;
            setLevelPrefs(*s, prefs);
            if (from) from->setString(percentText(effectiveRangeFrom(*s)));
            if (to) to->setString(percentText(effectiveRangeTo(*s)));
        }
    });
    rangeToggle->setID("range-toggle"_spr);
    rangeToggle->toggle(s->prefs.customRange);
    rangeToggle->setPosition({268.f, rowY});
    staticMenu->addChild(rangeToggle);
    addLabel(m_mainLayer, "This level only", "chatFont.fnt", .6f, {282.f, rowY}, {0.f, .5f}, kWhite, "range-toggle-label"_spr, kWidth - kPad - 282.f);

    auto onRangeEdit = [this](std::string const&) {
        auto s = menuSession(this);
        if (!s) return;
        auto from = typeinfo_cast<TextInput*>(m_mainLayer->getChildByID("range-from-input"_spr));
        auto to = typeinfo_cast<TextInput*>(m_mainLayer->getChildByID("range-to-input"_spr));
        if (!from || !to) return;
        auto a = parsePercent(from->getString());
        auto b = parsePercent(to->getString());
        if (!a || !b) return;
        auto prefs = s->prefs;
        prefs.customRange = true;
        prefs.rangeFrom = *a;
        prefs.rangeTo = *b;
        setLevelPrefs(*s, prefs);
        if (auto staticMenu = m_mainLayer->getChildByID("static-menu"_spr)) {
            if (auto toggle = typeinfo_cast<CCMenuItemToggler*>(staticMenu->getChildByID("range-toggle"_spr))) toggle->toggle(true);
        }
    };
    fromInput->setCallback(onRangeEdit);
    toInput->setCallback(onRangeEdit);

    float footY = 23.f;
    auto customize = makeButton("Customize", "GJ_button_04.png", .55f, "customize-button"_spr, [](CCMenuItemSpriteExtra*) {
        openSettingsPopup(Mod::get(), false);
    });
    auto noChart = makeButton("Play without chart", "GJ_button_04.png", .55f, "play-without-chart-button"_spr, [this](CCMenuItemSpriteExtra*) {
        auto s = menuSession(this);
        if (!s) return;
        playWithoutChart(*s);
    });
    staticMenu->addChild(customize);
    staticMenu->addChild(noChart);
    placeRow({customize, noChart}, kPad, footY, kRowGap, 200.f);

    auto openToggle = CCMenuItemExt::createTogglerWithStandardSprites(.55f, [](CCMenuItemToggler* toggler) {
        Mod::get()->setSettingValue<bool>("show-level-menu", !toggler->isToggled());
    });
    openToggle->setID("show-on-open-toggle"_spr);
    openToggle->toggle(Mod::get()->getSettingValue<bool>("show-level-menu"));
    openToggle->setPosition({268.f, footY});
    staticMenu->addChild(openToggle);
    addLabel(m_mainLayer, "Show on level open", "chatFont.fnt", .6f, {282.f, footY}, {0.f, .5f}, kWhite, "show-on-open-label"_spr, kWidth - kPad - 282.f);

    this->refresh();
    return true;
}

void LevelMenu::refresh() {
    if (!m_mainLayer) return;
    if (auto old = m_mainLayer->getChildByID("content"_spr)) old->removeFromParentAndCleanup(true);

    auto s = session();
    if (!s || !s->layer) return;

    auto content = CCNode::create();
    content->setID("content"_spr);
    content->setAnchorPoint({0.f, 0.f});
    content->setPosition({0.f, 0.f});
    content->setContentSize({kWidth, kHeight});
    m_mainLayer->addChild(content, 4);

    auto menu = addMenu(content, "content-menu"_spr);

    addLabel(content, "Saved chart", "goldFont.fnt", .5f, {kPad, 207.f}, {0.f, .5f}, kWhite, "chart-heading"_spr);
    float textWidth = 262.f;
    if (s->chart) {
        auto const& chart = *s->chart;
        ccColor3B statusColor = kWhite;
        auto status = chartStatus(chart, statusColor);
        auto statusLabel = addLabel(content, status, "chatFont.fnt", .65f, {kPad, 189.f}, {0.f, .5f}, statusColor, "chart-status"_spr, 120.f);
        std::string details = fmt::format("{} notes", chart.noteCount());
        if (chart.twoPlayer) details += "  |  2-player";
        if (chart.info.optimized) details += "  |  cleaned up";
        addLabel(
            content, "  |  " + details, "chatFont.fnt", .65f, {kPad + statusLabel->getScaledContentSize().width, 189.f}, {0.f, .5f},
            kWhite, "chart-details"_spr, textWidth - statusLabel->getScaledContentSize().width
        );
        addLabel(content, chartSource(chart), "chatFont.fnt", .6f, {kPad, 174.f}, {0.f, .5f}, kDim, "chart-source"_spr, textWidth);
        if (s->chartOutdated) {
            addLabel(
                content, "The level changed since this chart was made", "chatFont.fnt", .6f, {kPad, 159.f}, {0.f, .5f}, kOrange,
                "chart-outdated"_spr, textWidth
            );
        }
        else if (chart.info.timingRelaxed) {
            addLabel(
                content, "Some inputs are faster than the timing limits", "chatFont.fnt", .6f, {kPad, 159.f}, {0.f, .5f}, kDim,
                "chart-relaxed"_spr, textWidth
            );
        }

        auto play = makeButton("Play", "GJ_button_01.png", .75f, "play-button"_spr, [this](CCMenuItemSpriteExtra*) {
            auto s = menuSession(this);
            if (!s) return;
            playChart(*s);
        });
        play->setPosition({334.f, 194.f});
        menu->addChild(play);

        auto exportButton = makeButton("Export", "GJ_button_04.png", .45f, "export-button"_spr, [this](CCMenuItemSpriteExtra*) {
            auto s = menuSession(this);
            if (!s) return;
            exportSavedChart(*s);
        });
        auto deleteButton = makeButton("Delete", "GJ_button_06.png", .45f, "delete-button"_spr, [this](CCMenuItemSpriteExtra*) {
            if (!menuSession(this)) return;
            createQuickPopup(
                "Delete chart", "Delete the saved chart for this level? This can't be undone.", "Cancel", "Delete",
                [](FLAlertLayer*, bool confirmed) {
                    if (!confirmed) return;
                    auto s = session();
                    if (!s || !s->layer) return;
                    deleteSavedChart(*s);
                }
            );
        });
        menu->addChild(exportButton);
        menu->addChild(deleteButton);
        float w = exportButton->getScaledContentSize().width + deleteButton->getScaledContentSize().width + 4.f;
        placeRow({exportButton, deleteButton}, 334.f - w / 2.f, 163.f, 4.f, 110.f);
    }
    else {
        addLabel(content, "No saved chart for this level yet.", "chatFont.fnt", .65f, {kPad, 182.f}, {0.f, .5f}, kDim, "chart-empty"_spr, textWidth);
    }

    addLabel(content, "Solve", "goldFont.fnt", .5f, {kPad, 135.f}, {0.f, .5f}, kWhite, "solve-heading"_spr);

    bool platformer = isPlatformer(s->layer);
    bool progressOutdated = s->progress && s->progress->levelHash != s->ref.levelHash;
    std::string info;
    ccColor3B infoColor = kDim;
    if (platformer) {
        info = "Platformer levels can't be solved or imported";
        infoColor = kOrange;
    }
    else if (s->importBusy) {
        info = s->importLoading ? "Importing..." : "Choose a file to import...";
    }
    else if (progressOutdated) {
        info = "Saved progress is for an older version of this level";
        infoColor = kOrange;
    }
    else if (s->progress) {
        auto const& p = *s->progress;
        info = fmt::format("Saved progress: {:.0f}% after {}", std::floor(std::clamp(p.percent, 0.f, 100.f)), formatDuration(p.elapsedSeconds));
        if (p.pathFound) info += ", path found";
        if (p.targetPercent < 100.f) info += fmt::format(" (up to {}%)", percentText(p.targetPercent));
    }
    else if (!s->lastSolverStatus.empty()) {
        info = "Last run: " + s->lastSolverStatus;
    }
    else if (s->chart) {
        info = "Solve again to replace the chart, or import inputs";
    }
    else {
        info = "Find a path automatically, or import a macro (.gdr) or chart";
    }
    addLabel(content, info, "chatFont.fnt", .6f, {kPad, 119.f}, {0.f, .5f}, infoColor, "solve-info"_spr, kWidth - kPad * 2.f);

    if (!platformer) {
        std::vector<CCNode*> row;
        if (s->progress) {
            auto const& p = *s->progress;
            if (!progressOutdated) {
                auto label = fmt::format("Resume ({:.0f}%, {}", std::floor(std::clamp(p.percent, 0.f, 100.f)), formatDuration(p.elapsedSeconds));
                if (std::isfinite(p.targetPercent) && p.targetPercent >= 1.f && p.targetPercent < 100.f) {
                    label += fmt::format(", up to {}%", percentText(p.targetPercent));
                }
                label += ")";
                auto resume = makeButton(label.c_str(), "GJ_button_01.png", .55f, "resume-button"_spr, [this](CCMenuItemSpriteExtra*) {
                    auto s = menuSession(this);
                    if (!s) return;
                    requestSolve(*s, SolveKind::Resume);
                });
                row.push_back(resume);
            }
            auto restart = makeButton("Start over", progressOutdated ? "GJ_button_01.png" : "GJ_button_04.png", .55f, "start-over-button"_spr, [this](CCMenuItemSpriteExtra*) {
                auto s = menuSession(this);
                if (!s) return;
                deleteSavedProgress(*s);
                requestSolve(*s, SolveKind::Fresh);
            });
            row.push_back(restart);
        }
        else {
            auto solve = makeButton(
                s->chart ? "Re-solve" : "Solve", s->chart ? "GJ_button_04.png" : "GJ_button_01.png", .55f, "solve-button"_spr,
                [this](CCMenuItemSpriteExtra*) {
                    auto s = menuSession(this);
                    if (!s) return;
                    requestSolve(*s, SolveKind::Fresh);
                }
            );
            row.push_back(solve);
        }
        auto import = makeButton(
            s->importBusy ? "Importing..." : "Import", "GJ_button_02.png", .55f, "import-button"_spr,
            [this](CCMenuItemSpriteExtra*) {
                auto s = menuSession(this);
                if (!s || s->importBusy) return;
                pickImportFile(*s);
            }
        );
        import->setEnabled(!s->importBusy);
        if (s->importBusy) {
            if (auto sprite = typeinfo_cast<ButtonSprite*>(import->getNormalImage())) sprite->setOpacity(150);
        }
        row.push_back(import);
        for (auto node : row) menu->addChild(node);
        placeRow(row, kPad, 100.f, kRowGap, 270.f);
    }

    for (std::string id : {"target-label"_spr, "target-input"_spr, "target-unit"_spr}) {
        if (auto node = m_mainLayer->getChildByID(id)) node->setVisible(!platformer);
    }
}

void LevelMenu::onClose(CCObject* sender) {
    Ref<LevelMenu> self(this);
    auto s = session();
    if (!s || !s->layer || s->getMenu() != this) {
        Popup::onClose(sender);
        return;
    }
    if (s->chart) playChart(*s);
    else playWithoutChart(*s);
}

}
