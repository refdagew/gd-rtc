#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <matjson.hpp>
#include <chrono>
#include <algorithm>
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
    bool g_roomOperationPending = false;

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
        std::string name(GameManager::sharedState()->m_playerName.c_str());
        if (name.empty()) name = "Guest";
        if (name.size() > 32) name.resize(32);
        return name;
    }

    std::string roomsUrl() { return std::string(kDatabaseUrl) + "/rooms.json"; }

    std::string roomUrl(std::string const& code, std::string const& suffix = "") {
        return std::string(kDatabaseUrl) + "/rooms/" + code + suffix + ".json";
    }

    void shareCurrentLevel() {
        if (g_activeRoomCode.empty() || g_activePeerId.empty()) return;
        auto playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) return;
        auto level = playLayer->m_level;
        std::string levelData(level->m_levelString.c_str());
        std::string levelName(level->m_levelName.c_str());
        if (levelData.empty() || levelData.size() > 950000) return;
        if (levelName.empty()) levelName = "Shared level";
        if (levelName.size() > 100) levelName.resize(100);

        auto key = g_activePeerId + "_" + std::to_string(static_cast<long long>(nowSeconds()));
        matjson::Value payload = matjson::Value::object();
        payload["name"] = levelName;
        payload["levelData"] = levelData;
        payload["owner"] = playerName();
        payload["updatedAt"] = nowSeconds();
        payload["sourceLevelId"] = static_cast<int>(level->m_levelID);
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(20))
            .bodyJSON(payload)
            .put(roomUrl(g_activeRoomCode, "/levels/" + key)),
            [](geode::utils::web::WebResponse) {});
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

    void updateRoomControls(bool inRoom) {
        auto panel = this->getChildByID("gd-rtc-room-panel");
        if (!panel) return;
        auto menu = panel->getChildByID("gd-rtc-room-panel-menu");
        if (menu) {
            if (auto item = menu->getChildByID("gd-rtc-create")) item->setVisible(!inRoom);
            if (auto item = menu->getChildByID("gd-rtc-join")) item->setVisible(!inRoom);
            if (auto item = menu->getChildByID("gd-rtc-refresh")) item->setVisible(inRoom);
            if (auto item = menu->getChildByID("gd-rtc-leave")) item->setVisible(inRoom);
            if (auto item = menu->getChildByID("gd-rtc-open-level")) item->setVisible(inRoom);
        }
        if (auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"))) {
            input->setEnabled(!inRoom);
            if (inRoom) input->setString(g_activeRoomCode);
        }
        if (auto subtitle = typeinfo_cast<CCLabelBMFont*>(panel->getChildByID("gd-rtc-subtitle"))) {
            subtitle->setString(inRoom ? "Share the code with friends" : "Create a room or enter a friend's code");
        }
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
        panel->setPosition({win.width / 2, win.height / 2});
        auto uiScale = std::min(1.0f, std::min((win.width - 24.f) / 360.f, (win.height - 24.f) / 390.f));
        panel->setScale(std::max(0.55f, uiScale));
        this->addChild(panel);

        // Full-screen dimmer plus a distinct opaque panel makes the room UI readable
        // regardless of the menu artwork behind it.
        auto shade = CCLayerColor::create({0, 0, 0, 185});
        shade->setContentSize(win);
        shade->setPosition({-win.width / 2, -win.height / 2});
        panel->addChild(shade, -2);

        auto card = CCLayerColor::create({18, 27, 53, 248});
        card->setContentSize({360.f, 390.f});
        card->setPosition({-180.f, -195.f});
        panel->addChild(card, -1);

        auto accent = CCLayerColor::create({65, 190, 255, 255});
        accent->setContentSize({340.f, 3.f});
        accent->setPosition({-170.f, 190.f});
        panel->addChild(accent, -1);

        auto title = CCLabelBMFont::create("GD-RTC Rooms", "bigFont.fnt");
        title->setScale(0.8f);
        title->setPosition({0, 154.f});
        panel->addChild(title);

        auto subtitle = CCLabelBMFont::create("Create a room or enter a friend's code", "goldFont.fnt");
        subtitle->setID("gd-rtc-subtitle");
        subtitle->setScale(0.42f);
        subtitle->setPosition({0, 126.f});
        panel->addChild(subtitle);

        auto input = geode::TextInput::create(190.f, "Room code");
        input->setID("gd-rtc-room-code-input");
        input->setCommonFilter(geode::CommonFilter::Alphanumeric);
        input->setMaxCharCount(6);
        input->setTextAlign(geode::TextInputAlign::Center);
        input->setPosition({0, 88.f});
        panel->addChild(input);

        auto membersBack = CCLayerColor::create({8, 14, 31, 220});
        membersBack->setContentSize({310.f, 104.f});
        membersBack->setPosition({-155.f, -18.f});
        panel->addChild(membersBack, 0);

        auto members = CCLabelBMFont::create("Players in room:\n—", "goldFont.fnt");
        members->setID("gd-rtc-room-members");
        members->setScale(0.42f);
        members->setPosition({0, 30.f});
        panel->addChild(members);

        auto status = CCLabelBMFont::create("Ready", "goldFont.fnt");
        status->setID("gd-rtc-room-status");
        status->setScale(0.42f);
        status->setPosition({0, -182.f});
        panel->addChild(status);

        auto menu = CCMenu::create();
        menu->setID("gd-rtc-room-panel-menu");
        menu->setPosition({0, -94.f});
        panel->addChild(menu);

        auto createBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Create Room", 120, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onCreateRoom));
        createBtn->setID("gd-rtc-create");
        createBtn->setPosition({-75, 0});
        menu->addChild(createBtn);

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join Room", 120, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onJoinRoom));
        joinBtn->setID("gd-rtc-join");
        joinBtn->setPosition({75, 0});
        menu->addChild(joinBtn);

        auto refreshBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Refresh Players", 115, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .58f),
            this, menu_selector(GDRTCMenuLayer::onRefreshMembers));
        refreshBtn->setID("gd-rtc-refresh");
        refreshBtn->setPosition({-68, -40});
        menu->addChild(refreshBtn);

        auto leaveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Leave", 75, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onLeaveRoom));
        leaveBtn->setID("gd-rtc-leave");
        leaveBtn->setPosition({70, -40});
        menu->addChild(leaveBtn);

        auto closeBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Close", 80, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .65f),
            this, menu_selector(GDRTCMenuLayer::onCloseRooms));
        auto openLevelBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Open Shared Level", 150, true, "bigFont.fnt", "GJ_button_02.png", 30.f, .58f),
            this, menu_selector(GDRTCMenuLayer::onOpenSharedLevel));
        openLevelBtn->setID("gd-rtc-open-level");
        openLevelBtn->setPosition({0, -30});
        menu->addChild(openLevelBtn);

        closeBtn->setPosition({0, -63});
        menu->addChild(closeBtn);

        updateRoomControls(!g_activeRoomCode.empty());
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
        if (!this->getChildByID("gd-rtc-room-panel") || g_roomOperationPending) return;
        g_roomOperationPending = true;
        setRoomStatus("Creating room...");
        // Best-effort garbage collection for rooms abandoned for more than a day.
        // Use a generous TTL so a long-running level is not mistaken for a dead room.
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(10))
            .get(roomsUrl()),
            [](geode::utils::web::WebResponse response) {
                if (!response.ok()) return;
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) return;
                auto rooms = parsed.unwrap();
                auto now = nowSeconds();
                for (auto const& [roomCode, room] : rooms) {
                    auto lastSeenResult = room["lastSeen"].asDouble();
                    double lastSeen = lastSeenResult ? lastSeenResult.unwrap() : 0.0;
                    if (lastSeen > 0.0 && now - lastSeen > 86400.0) {
                        async::spawn(geode::utils::web::WebRequest()
                            .timeout(std::chrono::seconds(8))
                            .send("DELETE", roomUrl(roomCode)),
                            [](geode::utils::web::WebResponse) {});
                    }
                }
            });
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
                g_roomOperationPending = false;
                if (!response.ok()) {
                    self->setRoomStatus(responseError(response));
                    return;
                }
                g_activeRoomCode = code;
                g_activePeerId = peer;
                self->updateRoomControls(true);
                if (auto panel = self->getChildByID("gd-rtc-room-panel")) {
                    if (auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"))) input->setString(code);
                }
                self->setRoomStatus("Room code: " + code + " — share it with your friend");
                self->onRefreshMembers(nullptr);
            });
    }

    void onJoinRoom(CCObject*) {
        auto panel = this->getChildByID("gd-rtc-room-panel");
        if (!panel || g_roomOperationPending) return;
        auto input = typeinfo_cast<geode::TextInput*>(panel->getChildByID("gd-rtc-room-code-input"));
        if (!input) return;
        auto rawCode = input->getString();
        std::string code(rawCode.c_str());
        for (auto& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (code.size() != 6) {
            setRoomStatus("Enter the six-character room code.");
            return;
        }

        g_roomOperationPending = true;
        setRoomStatus("Joining room...");
        auto peer = makePeerId();
        auto now = nowSeconds();
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(12))
            .get(roomUrl(code)),
            [self, code, peer, now](geode::utils::web::WebResponse roomResponse) {
                if (!roomResponse.ok()) {
                    g_roomOperationPending = false;
                    self->setRoomStatus(responseError(roomResponse));
                    return;
                }
                auto parsed = roomResponse.json();
                if (!parsed || parsed.unwrap().isNull()) {
                    g_roomOperationPending = false;
                    self->setRoomStatus("Room not found. Check the code and try again.");
                    return;
                }

                auto existing = parsed.unwrap();
                auto members = existing["members"];
                unsigned int count = 0;
                for (auto const& [id, value] : members) { (void)id; (void)value; ++count; }
                if (count >= 8) {
                    g_roomOperationPending = false;
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
                        g_roomOperationPending = false;
                        if (!memberResponse.ok()) {
                            self->setRoomStatus(responseError(memberResponse));
                            return;
                        }
                        g_activeRoomCode = code;
                        g_activePeerId = peer;
                        self->updateRoomControls(true);
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
                auto now = nowSeconds();
                for (auto const& [id, value] : members) {
                    if (count >= 8) break;
                    auto lastSeenValue = value["lastSeen"].asDouble();
                    double lastSeen = lastSeenValue ? lastSeenValue.unwrap() : 0.0;
                    if (lastSeen > 0.0 && now - lastSeen > 45.0) {
                        // Prune stale presence so crashed/disconnected clients don't stay in the room list.
                        async::spawn(geode::utils::web::WebRequest()
                            .timeout(std::chrono::seconds(6))
                            .send("DELETE", roomUrl(code, "/members/" + id)),
                            [](geode::utils::web::WebResponse) {});
                        continue;
                    }
                    auto result = value["name"].asString();
                    std::string name = result ? result.unwrap() : "Player";
                    if (name.size() > 24) name.resize(24);
                    names += "\n• " + name;
                    ++count;
                }
                if (!count) names += "\n—";
                self->setRoomMembers(names);
                if (g_activeRoomCode == code) self->setRoomStatus("Room " + code + " · " + std::to_string(count) + "/8 players");
            });
    }

    void onOpenSharedLevel(CCObject*) {
        if (g_activeRoomCode.empty()) {
            setRoomStatus("Join a room first.");
            return;
        }
        setRoomStatus("Looking for a shared level...");
        auto code = g_activeRoomCode;
        Ref<GDRTCMenuLayer> self = this;
        async::spawn(geode::utils::web::WebRequest()
            .timeout(std::chrono::seconds(15))
            .get(roomUrl(code, "/levels")),
            [self](geode::utils::web::WebResponse response) {
                if (!response.ok()) {
                    self->setRoomStatus("Couldn't load shared levels.");
                    return;
                }
                auto parsed = response.json();
                if (!parsed || parsed.unwrap().isNull()) {
                    self->setRoomStatus("No level has been shared to this room yet.");
                    return;
                }

                matjson::Value latest;
                double latestTime = -1.0;
                for (auto const& [key, candidate] : parsed.unwrap()) {
                    (void)key;
                    auto time = candidate["updatedAt"].asDouble();
                    double timestamp = time ? time.unwrap() : 0.0;
                    if (timestamp > latestTime) {
                        latestTime = timestamp;
                        latest = candidate;
                    }
                }
                auto nameResult = latest["name"].asString();
                auto dataResult = latest["levelData"].asString();
                if (!nameResult || !dataResult || dataResult.unwrap().empty()) {
                    self->setRoomStatus("The shared level data is invalid.");
                    return;
                }
                auto level = GJGameLevel::create();
                level->m_levelName = nameResult.unwrap();
                level->m_levelString = dataResult.unwrap();
                level->m_levelID = 0;
                auto scene = LevelEditorLayer::scene(level, false);
                if (!scene) {
                    self->setRoomStatus("Couldn't open that level.");
                    return;
                }
                self->setRoomStatus("Opening " + std::string(nameResult.unwrap()) + "...");
                CCDirector::sharedDirector()->pushScene(scene);
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
                self->updateRoomControls(false);
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


class $modify(GDRTCPauseLayer, PauseLayer) {
    bool init() {
        if (!PauseLayer::init()) return false;
        if (g_activeRoomCode.empty()) return true;

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("gd-rtc-pause-share-menu");
        menu->setPosition({win.width / 2, win.height * 0.14f});
        auto button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Share Level", 112, true, "bigFont.fnt", "GJ_button_02.png", 30.f, .6f),
            this, menu_selector(GDRTCPauseLayer::onShareLevel));
        button->setID("gd-rtc-pause-share-button");
        menu->addChild(button);
        this->addChild(menu, 100);
        return true;
    }

    void onShareLevel(CCObject*) {
        shareCurrentLevel();
    }
};
