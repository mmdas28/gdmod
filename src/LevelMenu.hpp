#pragma once

#include <Geode/Geode.hpp>

namespace rp {

struct Session;

class LevelMenu : public geode::Popup {
public:
    static LevelMenu* create();
    void refresh();

protected:
    bool init() override;
    void onClose(cocos2d::CCObject* sender) override;
};

}
