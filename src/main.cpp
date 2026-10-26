#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <matjson.hpp>
#include <chrono>
#include <cctype>
#include <random>
#include <string>

using namespace geode::prelude;

namespace {
    constexpr char kDatabaseUrl[] = "https://gd-rtc-default-rtdb.firebaseio.com";
    constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

    std::string makeRoomCode() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<size_t> pick(0, sizeof(kAlphabet) - 2);
        std::string code;
        for (int i = 0; i < 6; ++i) code += kAlphabet[pick(rng)];
        return code;
    }

    std::string makePeerId() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<unsigned int> pick(0, 15);
        std::string out;
        for (int i = 0; i < 16; ++i) out += "0123456789abcdef"[pick(rng)];
        return out;
    }

    std::string playerName() {
        auto name = GameManager::sharedState()->getPlayerName();
        if (name.empty()) name = "Guest";
        if (name.size() > 32) name.resize(32);
        return name;
    }

    std::string roomUrl(std::string const& code, std::string const& suffix = "") {
        return std::string(kDatabaseUrl) + "/rooms/" + code + suffix + ".json";
    }

    void showMessage(std::string const& title, std::string const& message) {
        FLAlertLayer::create(title.c_str(), message.c_str(), "OK")->show();
    }

    std::string responseError(geode::utils::web::WebResponse const& response) {
        auto body = response.string();
        std::string detail = body ? body.unwrap() : std::string(response.errorMessage());
        if (detail.empty()) detail = "No response body was returned.";
        if (detail.size() > 240) detail.resize(240);
        (void)detail;
        return "Couldn't connect to the room service. Please try again in a moment.";
    }
}

class $modify(GDRTCMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto bottomMenu = this->getChildByID("bottom-menu");
        if (!bottomMenu) {
            log::warn("GD-RTC: bottom-menu node was not found; skipping menu button.");
            return true;
        }

        auto icon = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        if (!icon) icon = CCSprite::createWithSpriteFrameName("GJ_achievementsBtn_001.png");
        if (!icon) return true;
        icon->setScale(0.82f);
        auto button = CCMenuItemSpriteExtra::create(icon, this, menu_selector(GDRTCMenuLayer::onGDRTC));
        if (!button) return true;
        button->setID("gd-rtc-main-menu-button"_spr);
        bottomMenu->addChild(button);
        if (auto menu = typeinfo_cast<CCMenu*>(bottomMenu)) menu->updateLayout();
        return true;
    }

    void setBackgroundMenusEnabled(bool enabled) {
        auto children = this->getChildren();
        if (!children) return;
        for (unsigned int i = 0; i < children->count(); ++i) {
            if (auto menu = typeinfo_cast<CCMenu*>(children->objectAtIndex(i))) {
                if (menu->getID() != "gd-rtc-room-panel-menu") menu->setTouchEnabled(enabled);
            }
        }
    }

    void onGDRTC(CCObject*) {
        if (auto existing = this->getChildByID("gd-rtc-room-panel")) {
            existing->removeFromParent();
            setBackgroundMenusEnabled(true);
            return;
        }

        setBackgroundMenusEnabled(false);
        auto win = CCDirector::sharedDirector()->getWinSize();
        auto panel = CCNode::create();
        panel->setID("gd-rtc-room-panel");
        panel->setZOrder(1000);
        this->addChild(panel);

        auto shade = CCLayerColor::create({0, 0, 0, 170});
        shade->setContentSize(win);
        shade->setPosition({0, 0});
        panel->addChild(shade);

        auto title = CCLabelBMFont::create("GD-RTC Rooms", "bigFont.fnt");
        title->setScale(0.8f);
        title->setPosition({win.width / 2, win.height / 2 + 95});
        panel->addChild(title);

        auto subtitle = CCLabelBMFont::create("Create a room or enter a friend's code", "goldFont.fnt");
        subtitle->setScale(0.42f);
        subtitle->setPosition({win.width / 2, win.height / 2 + 63});
        panel->addChild(subtitle);

        auto status = CCLabelBMFont::create("Ready", "goldFont.fnt");
        status->setID("gd-rtc-room-status");
        status->setScale(0.42f);
        status->setPosition({win.width / 2, win.height / 2 - 104});
        panel->addChild(status);

        auto input = geode::TextInput::create(190.f, "Room code");
        input->setID("gd-rtc-room-code-input");
        input->setCommonFilter(geode::CommonFilter::Alphanumeric);
        input->setMaxCharCount(6);
        input->setTextAlign(geode::TextInputAlign::Center);
        input->setPosition({win.width / 2, win.height / 2 + 20});
        panel->addChild(input);

        auto menu = CCMenu::create();
        menu->setID("gd-rtc-room-panel-menu");
        menu->setPosition({win.width / 2, win.height / 2 - 30});
        panel->addChild(menu);

        auto createBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Create Room", 120, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onCreateRoom));
        createBtn->setPosition({-75, 0});
        menu->addChild(createBtn);

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join Room", 120, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onJoinRoom));
        joinBtn->setPosition({75, 0});
        menu->addChild(joinBtn);

        auto closeBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Close", 80, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onCloseRooms));
        closeBtn->setPosition({0, -68});
        menu->addChild(closeBtn);
    }

    void onCloseRooms(CCObject*) {
        if (auto panel = this->getChildByID("gd-rtc-room-panel")) panel->removeFromParent();
        setBackgroundMenusEnabled(true);
    }

    void onCreateRoom(CCObject*) {
        auto code = makeRoomCode();
        auto peer = makePeerId();
        auto now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

        matjson::Value room = matjson::Value::object();
        room["code"] = code;
        room["createdAt"] = now;
        room["lastSeen"] = now;
        room["protocol"] = 1;
        room["members"] = matjson::Value::object();
        matjson::Value member = matjson::Value::object();
        member["name"] = playerName();
        member["peerId"] = peer;
        member["joinedAt"] = now;
        room["members"][peer] = member;

        auto response = geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .bodyJSON(room)
            .putSync(roomUrl(code));
        if (!response.ok()) {
            showMessage("GD-RTC", responseError(response));
            return;
        }

        if (auto panel = this->getChildByID("gd-rtc-room-panel")) {
            if (auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"))) input->setString(code);
        }
        if (auto panel = this->getChildByID("gd-rtc-room-panel")) {
            if (auto status = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-room-status"))) {
                auto text = "Room code: " + code + "  —  share it with your friend";
                status->setString(text.c_str());
            }
        }
    }

    void onJoinRoom(CCObject*) {
        auto panel = this->getChildByID("gd-rtc-room-panel");
        if (!panel) return;
        auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"));
        if (!input) return;
        auto rawCode = input->getString();
        std::string code(rawCode.c_str());
        for (auto& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (code.size() != 6) {
            showMessage("GD-RTC", "Enter the six-character room code first.");
            return;
        }

        auto roomResponse = geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .getSync(roomUrl(code));
        if (!roomResponse.ok()) {
            showMessage("GD-RTC", responseError(roomResponse));
            return;
        }
        auto parsed = roomResponse.json();
        if (!parsed || parsed.unwrap().isNull()) {
            showMessage("GD-RTC", "That room code wasn't found. Check the code and try again.");
            return;
        }

        auto peer = makePeerId();
        auto now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        matjson::Value member = matjson::Value::object();
        member["name"] = playerName();
        member["peerId"] = peer;
        member["joinedAt"] = now;

        auto memberResponse = geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .bodyJSON(member)
            .putSync(roomUrl(code, "/members/" + peer));
        if (!memberResponse.ok()) {
            showMessage("GD-RTC", responseError(memberResponse));
            return;
        }

        if (auto panel = this->getChildByID("gd-rtc-room-panel")) {
            if (auto status = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-room-status"))) {
                auto text = "Joined room " + code;
                status->setString(text.c_str());
            }
        }
    }
};
