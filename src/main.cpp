#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

namespace {
    class GDRTCInfoPopup : public FLAlertLayer {
    public:
        void showInfo() {
            FLAlertLayer::create(
                "GD-RTC",
                "The multiplayer foundation is being built.\n\n"
                "This early build adds the GD-RTC entry point. "
                "Room signaling and WebRTC gameplay sync are not implemented yet.",
                "OK"
            )->show();
        }
    };
}

$modify(GDRTCMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        auto mainMenu = this->getChildByID("main-menu");
        if (!mainMenu) {
            log::warn("GD-RTC: main-menu node was not found; skipping menu button.");
            return true;
        }

        auto buttonSprite = ButtonSprite::create(
            "GD-RTC",
            90,
            true,
            "bigFont.fnt",
            "GJ_button_01.png",
            30.0f,
            0.8f
        );
        if (!buttonSprite) {
            log::warn("GD-RTC: failed to create menu button sprite.");
            return true;
        }

        auto button = CCMenuItemSpriteExtra::create(
            buttonSprite,
            this,
            menu_selector(GDRTCMenuLayer::onGDRTC)
        );
        if (!button) {
            log::warn("GD-RTC: failed to create menu button.");
            return true;
        }

        button->setID("gd-rtc-main-menu-button");
        mainMenu->addChild(button);
        if (auto menu = typeinfo_cast<CCMenu*>(mainMenu)) {
            menu->updateLayout();
        }
        return true;
    }

    void onGDRTC(CCObject*) {
        FLAlertLayer::create(
            "GD-RTC",
            "Welcome to GD-RTC!\n\n"
            "This is an early development build. Room creation, room joining, "
            "player presence, and WebRTC gameplay synchronization are not "
            "available in this version yet.",
            "OK"
        )->show();
    }
};
