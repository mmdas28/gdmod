#include "Chart.hpp"
#include "Overlay.hpp"
#include "Session.hpp"
#include "Solver.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {

rp::Solver* runningSolver(rp::Session* s) {
    if (!s || !s->solver || !s->solver->running()) return nullptr;
    return s->solver.get();
}

bool skippingVisuals(GJBaseGameLayer* layer) {
    auto solver = runningSolver(rp::sessionFor(layer));
    return solver && solver->skipVisuals();
}

}

class $modify(RPPlayLayer, PlayLayer) {
    struct Fields {
        std::unique_ptr<rp::Session> session;
    };

    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
        (void)self.setHookPriority("PlayLayer::levelComplete", Priority::First);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", Priority::First);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto session = std::make_unique<rp::Session>();
        session->layer = this;
        m_fields->session = std::move(session);
        rp::setSession(m_fields->session.get());
        return true;
    }

    rp::Session* rpSession() {
        auto s = m_fields->session.get();
        return s && s->layer == this ? s : nullptr;
    }

    void onQuit() {
        if (auto s = rpSession()) s->shutdown();
        PlayLayer::onQuit();
    }

    void onExit() {
        if (auto s = rpSession()) {
            if (auto solver = runningSolver(s)) solver->cancel(false);
        }
        PlayLayer::onExit();
    }

    void resetLevel() {
        auto s = rpSession();
        if (auto solver = runningSolver(s)) {
            if (!solver->isInternalReset()) solver->cancel(false);
        }
        PlayLayer::resetLevel();
        if (s && !runningSolver(s)) {
            if (auto overlay = s->getOverlay()) overlay->onLevelReset();
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (auto solver = runningSolver(rpSession())) {
            if (object != m_anticheatSpike) {
                solver->onPlayerDestroyed();
                return;
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (auto solver = runningSolver(rpSession())) {
            solver->onLevelComplete();
            return;
        }
        PlayLayer::levelComplete();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (auto solver = runningSolver(rpSession())) {
            solver->onLevelComplete();
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }

    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        if (runningSolver(rpSession())) return;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        if (runningSolver(rpSession())) return;
        PlayLayer::storeCheckpoint(checkpoint);
    }

    void checkpointActivated(CheckpointGameObject* object) {
        if (runningSolver(rpSession())) return;
        PlayLayer::checkpointActivated(object);
    }

    void processCheckpoints() {
        if (runningSolver(rpSession())) return;
        PlayLayer::processCheckpoints();
    }

    void pauseGame(bool unfocused) {
        if (auto solver = runningSolver(rpSession())) solver->onPause();
        PlayLayer::pauseGame(unfocused);
    }

    void updateVisibility(float dt) {
        if (skippingVisuals(this)) return;
        PlayLayer::updateVisibility(dt);
    }

    void updateProgressbar() {
        if (skippingVisuals(this)) return;
        PlayLayer::updateProgressbar();
    }

    void updateInfoLabel() {
        if (skippingVisuals(this)) return;
        PlayLayer::updateInfoLabel();
    }
};

class $modify(RPBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::processCommands", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::First);
    }

    void update(float dt) {
        auto s = rp::sessionFor(this);
        if (!s) return GJBaseGameLayer::update(dt);

        if (s->solver && s->solver->isStepping()) return GJBaseGameLayer::update(dt);

        rp::tickSession(*s);
        if (auto solver = runningSolver(s)) {
            solver->runFrame();
            return;
        }
        GJBaseGameLayer::update(dt);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto solver = runningSolver(rp::sessionFor(this));
        if (!solver || !solver->isStepping()) return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        solver->beforeStep(isHalfTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        solver->afterStep(isHalfTick);
    }

    void updateParticles(float dt) {
        if (skippingVisuals(this)) return;
        GJBaseGameLayer::updateParticles(dt);
    }

    void updateShaderLayer(float dt) {
        if (skippingVisuals(this)) return;
        GJBaseGameLayer::updateShaderLayer(dt);
    }

    void updateAudioVisualizer() {
        if (skippingVisuals(this)) return;
        GJBaseGameLayer::updateAudioVisualizer();
    }

    void updateDebugDraw() {
        if (skippingVisuals(this)) return;
        GJBaseGameLayer::updateDebugDraw();
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto s = rp::sessionFor(this);
        if (!s) return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (auto solver = runningSolver(s)) {
            if (solver->isInjecting()) GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (button == 1) {
            if (auto overlay = s->getOverlay()) overlay->onInput(isPlayer1 ? 0 : 1, down, m_gameState.m_levelTime);
        }
    }
};

class $modify(RPPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto s = rp::session();
        if (!s || !s->layer) return;

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("menu"_spr);

        const char* solveText = s->solving() ? "Cancel solve" : (s->chart ? "Re-solve" : "Solve");
        auto solveSprite = ButtonSprite::create(solveText, "goldFont.fnt", "GJ_button_01.png", 0.8f);
        solveSprite->setScale(0.6f);
        auto solveButton = CCMenuItemSpriteExtra::create(solveSprite, this, menu_selector(RPPauseLayer::onRhythmSolve));
        solveButton->setID("solve-button"_spr);
        menu->addChild(solveButton);

        bool show = Mod::get()->getSettingValue<bool>("show-overlay");
        auto chartSprite = ButtonSprite::create(show ? "Chart: ON" : "Chart: OFF", "goldFont.fnt", "GJ_button_04.png", 0.8f);
        chartSprite->setScale(0.6f);
        auto chartButton = CCMenuItemSpriteExtra::create(chartSprite, this, menu_selector(RPPauseLayer::onRhythmToggle));
        chartButton->setID("chart-toggle"_spr);
        menu->addChild(chartButton);

        menu->alignItemsVerticallyWithPadding(4.f);
        menu->setPosition({win.width - 62.f, 42.f});
        this->addChild(menu, 20);
    }

    void onRhythmSolve(CCObject*) {
        auto s = rp::session();
        if (!s || !s->layer) return;
        if (auto solver = runningSolver(s)) {
            solver->cancel(false);
            s->lastSolverStatus = "Solver cancelled";
            PauseLayer::onResume(nullptr);
            s->layer->resetLevel();
            return;
        }
        rp::requestSolve(*s);
        PauseLayer::onResume(nullptr);
    }

    void onRhythmToggle(CCObject* sender) {
        bool show = !Mod::get()->getSettingValue<bool>("show-overlay");
        Mod::get()->setSettingValue<bool>("show-overlay", show);
        if (auto item = typeinfo_cast<CCMenuItemSpriteExtra*>(sender)) {
            if (auto sprite = typeinfo_cast<ButtonSprite*>(item->getNormalImage())) {
                sprite->setString(show ? "Chart: ON" : "Chart: OFF");
            }
        }
    }
};
