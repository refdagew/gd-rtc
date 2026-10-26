#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <matjson.hpp>
#include <chrono>
#include <cctype>
#include <random>
#include <string>

using namespace geode::prelude;

namespace {
    constexpr char kDatabaseUrl[] = "https://gd-rtc-default-rtdb.firebaseio.com";
    constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string g_activeRoomCode;
    std::string g_activePeerId;
    bool g_refreshPending = false;

    double nowSeconds() {
        return static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

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

    std::string responseError(geode::utils::web::WebResponse const& response) {
        (void)response;
        return "Connection failed. Please try again.";
    }
}

class $modify(GDRTCMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto bottomMenu = this->getChildByID("bottom-menu");
        if (!bottomMenu) return true;
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

    void setRoomStatus(std::string const& message) {
        if (auto panel = this->getChildByID("gd-rtc-room-panel")) {
            if (auto status = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-room-status"))) {
                status->setString(message.c_str());
            }
        }
    }

    void setRoomMembers(std::string const& message) {
        if (auto panel = this->getChildByID("gd-rtc-room-panel")) {
            if (auto label = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-room-members"))) {
                label->setString(message.c_str());
            }
        }
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
        title->setPosition({win.width / 2, win.height / 2 + 112});
        panel->addChild(title);

        auto subtitle = CCLabelBMFont::create("Play together online", "goldFont.fnt");
        subtitle->setScale(0.42f);
        subtitle->setPosition({win.width / 2, win.height / 2 + 82});
        panel->addChild(subtitle);

        auto input = geode::TextInput::create(190.f, "Room code");
        input->setID("gd-rtc-room-code-input");
        input->setCommonFilter(geode::CommonFilter::Alphanumeric);
        input->setMaxCharCount(6);
        input->setTextAlign(geode::TextInputAlign::Center);
        input->setPosition({win.width / 2, win.height / 2 + 43});
        panel->addChild(input);

        auto members = CCLabelBMFont::create("Players in room:\n—", "goldFont.fnt");
        members->setID("gd-rtc-room-members");
        members->setScale(0.42f);
        members->setPosition({win.width / 2, win.height / 2 - 38});
        panel->addChild(members);

        auto status = CCLabelBMFont::create("Ready", "goldFont.fnt");
        status->setID("gd-rtc-room-status");
        status->setScale(0.42f);
        status->setPosition({win.width / 2, win.height / 2 - 88});
        panel->addChild(status);

        auto menu = CCMenu::create();
        menu->setID("gd-rtc-room-panel-menu");
        menu->setPosition({win.width / 2, win.height / 2 - 3});
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

        auto refreshBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Refresh Players", 115, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .58f),
            this, menu_selector(GDRTCMenuLayer::onRefreshMembers));
        refreshBtn->setPosition({-68, -43});
        menu->addChild(refreshBtn);

        auto leaveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Leave", 75, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onLeaveRoom));
        leaveBtn->setPosition({70, -43});
        menu->addChild(leaveBtn);

        auto closeBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Close", 80, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onCloseRooms));
        closeBtn->setPosition({0, -82});
        menu->addChild(closeBtn);

        this->schedule(schedule_selector(GDRTCMenuLayer::pollRoom), 10.0f);
        if (!g_activeRoomCode.empty()) {
            input->setString(g_activeRoomCode);
            setRoomStatus("Room " + g_activeRoomCode);
            onRefreshMembers(nullptr);
        }
    }

    void onCloseRooms(CCObject*) {
        this->unschedule(schedule_selector(GDRTCMenuLayer::pollRoom));
        if (auto panel = this->getChildByID("gd-rtc-room-panel")) panel->removeFromParent();
        setBackgroundMenusEnabled(true);
    }

    void onCreateRoom(CCObject*) {
        if (!this->getChildByID("gd-rtc-room-panel")) return;
        setRoomStatus("Creating room...");
        auto code = makeRoomCode();
        auto peer = makePeerId();
        auto now = nowSeconds();

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
        member["lastSeen"] = now;
        room["members"][peer] = member;

        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .bodyJSON(room)
            .put(roomUrl(code)),
            [self, code, peer](geode::utils::web::WebResponse response) {
                if (!response.ok()) {
                    self->setRoomStatus(responseError(response));
                    return;
                }
                g_activeRoomCode = code;
                g_activePeerId = peer;
                if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                    if (auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"))) input->setString(code);
                }
                self->setRoomStatus("Room code: " + code + " — share it with your friend");
                self->onRefreshMembers(nullptr);
            });
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
            setRoomStatus("Enter the six-character room code.");
            return;
        }

        setRoomStatus("Joining room...");
        auto peer = makePeerId();
        auto now = nowSeconds();
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .get(roomUrl(code)),
            [self, code, peer, now](geode::utils::web::WebResponse roomResponse) {
                if (!roomResponse.ok()) {
                    self->setRoomStatus(responseError(roomResponse));
                    return;
                }
                auto parsed = roomResponse.json();
                if (!parsed || parsed.unwrap().isNull()) {
                    self->setRoomStatus("Room not found. Check the code and try again.");
                    return;
                }

                auto existing = parsed.unwrap();
                auto members = existing["members"];
                unsigned int count = 0;
                for (auto const& [id, value] : members) { (void)id; (void)value; ++count; }
                if (count >= 8) {
                    self->setRoomStatus("That room is full (8 players max).");
                    return;
                }

                matjson::Value member = matjson::Value::object();
                member["name"] = playerName();
                member["peerId"] = peer;
                member["joinedAt"] = now;
                member["lastSeen"] = now;

                async::spawn(geode::utils::web::WebRequest()
                    .timeout(std::chrono::seconds(12))
                    .bodyJSON(member)
                    .put(roomUrl(code, "/members/" + peer)),
                    [self, code, peer](geode::utils::web::WebResponse memberResponse) {
                        if (!memberResponse.ok()) {
                            self->setRoomStatus(responseError(memberResponse));
                            return;
                        }
                        g_activeRoomCode = code;
                        g_activePeerId = peer;
                        self->setRoomStatus("Joined room " + code + ".");
                        self->onRefreshMembers(nullptr);
                    });
            });
    }

    void pollRoom(float) {
        if (!this->getChildByID("gd-rtc-room-panel")) {
            this->unschedule(schedule_selector(GDRTCMenuLayer::pollRoom));
            return;
        }
        if (!g_activeRoomCode.empty()) {
            onRefreshMembers(nullptr);
            auto code = g_activeRoomCode;
            auto peer = g_activePeerId;
            auto now = nowSeconds();
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(6))
                .bodyJSON(now)
                .put(roomUrl(code, "/lastSeen")),
                [](geode::utils::web::WebResponse) {});
            async::spawn(geode::utils::web::WebRequest()
                .timeout(std::chrono::seconds(6))
                .bodyJSON(now)
                .put(roomUrl(code, "/members/" + peer + "/lastSeen")),
                [](geode::utils::web::WebResponse) {});
        }
    }

    void onRefreshMembers(CCObject*) {
        if (g_refreshPending) return;
        if (g_activeRoomCode.empty()) {
            setRoomMembers("Players in room:\nCreate or join a room first.");
            return;
        }
        g_refreshPending = true;
        setRoomStatus("Refreshing players...");
        auto code = g_activeRoomCode;
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(10))
            .get(roomUrl(code)),
            [self, code](geode::utils::web::WebResponse response) {
                g_refreshPending = false;
                if (!response.ok()) {
                    self->setRoomStatus(responseError(response));
                    return;
                }
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) {
                    self->setRoomMembers("Room no longer exists.");
                    self->setRoomStatus("Room not found.");
                    return;
                }
                auto data = parsed.unwrap();
                auto members = data["members"];
                std::string names = "Players in room:";
                unsigned int count = 0;
                for (auto const& [id, value] : members) {
                    (void)id;
                    if (count >= 8) break;
                    auto result = value["name"].asString();
                    std::string name = result ? result.unwrap() : "Player";
                    if (name.size() > 32) name.resize(32);
                    names += "\n• " + name;
                    ++count;
                }
                if (!count) names += "\n—";
                self->setRoomMembers(names);
                if (g_activeRoomCode == code) self->setRoomStatus("Room " + code + " · " + std::to_string(count) + "/8 players");
            });
    }

    void onLeaveRoom(CCObject*) {
        if (g_activeRoomCode.empty() || g_activePeerId.empty()) {
            setRoomStatus("You are not in a room.");
            return;
        }
        auto code = g_activeRoomCode;
        auto peer = g_activePeerId;
        setRoomStatus("Leaving room...");
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(8))
            .send("DELETE", roomUrl(code, "/members/" + peer)),
            [self, code, peer](geode::utils::web::WebResponse response) {
                if (!response.ok()) {
                    self->setRoomStatus("Couldn't leave the room. Try again.");
                    return;
                }
                if (g_activeRoomCode == code && g_activePeerId == peer) {
                    g_activeRoomCode.clear();
                    g_activePeerId.clear();
                }
                self->setRoomMembers("Players in room:\n—");
                self->setRoomStatus("You left the room.");

                // Remove the room record once its last member has left.
                async::spawn(geode::utils::web::WebRequest()
                    .timeout(std::chrono::seconds(8))
                    .get(roomUrl(code)),
                    [code](geode::utils::web::WebResponse roomResponse) {
                        if (!roomResponse.ok()) return;
                        auto parsed = roomResponse.json();
                        if (!parsed || parsed.unwrap().isNull()) return;
                        auto members = parsed.unwrap()["members"];
                        bool anyMembers = false;
                        for (auto const& [id, value] : members) {
                            (void)id; (void)value;
                            anyMembers = true;
                            break;
                        }
                        if (!anyMembers) {
                            async::spawn(geode::utils::web::WebRequest()
                                .timeout(std::chrono::seconds(8))
                                .send("DELETE", roomUrl(code)),
                                [](geode::utils::web::WebResponse) {});
                        }
                    });
            });
    }
};
